#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "app_usb_link.h"
#include "sdkconfig.h"

#define TAG "usb_link"

#define RING_SIZE      (128 * 1024)   /* rx ring buffer (PSRAM) */
#define TX_CHUNK       (8192)         /* 每次 write_queue 的块大小（与
                                         CONFIG_TINYUSB_CDC_TX_BUFSIZE=8KB 对齐）：
                                         块太小 → 频繁等排水，USB HS 吞吐上不去 */
#define TX_BUDGET_US   (500000)       /* 单帧发送总预算（与 TCP 后端一致）：超预算即丢帧
                                         （返回 ESP_ERR_TIMEOUT），绝不把 AI/stream 拖死 */
#define CDC_ITF        TINYUSB_CDC_ACM_0
#define JCL_MAGIC0     0x4A
#define JCL_MAGIC1     0x43

/* ---- frame parser states ---- */
enum { ST_MAGIC0, ST_MAGIC1, ST_TYPE, ST_SEQ, ST_LEN, ST_PAYLOAD, ST_CRC0, ST_CRC1 };

typedef struct {
    uint8_t  *ring;              /* rx ring buffer (PSRAM) */
    volatile uint32_t head;      /* written by CDC callback (single producer) */
    volatile uint32_t tail;      /* consumed by parser task (single consumer) */
    uint8_t   state;
    uint8_t   f_type;
    uint8_t   f_seq;
    uint32_t  f_len;
    uint32_t  f_got;
    uint16_t  crc_calc;
    uint16_t  crc_rx;
    uint8_t  *f_buf;             /* current frame payload buffer */
    SemaphoreHandle_t tx_lock;
    SemaphoreHandle_t rx_lock;   /* 串行化多个后端的字节注入 */
    const jcl_transport_t *tp;   /* 当前**生效**的发送后端，NULL = 内置 USB CDC */
    const jcl_transport_t *net_tp; /* 外部（TCP）后端是否存在：由 app_netlink 注册 */
    uint8_t   forced;            /* 强制态：0=auto 1=usb 2=tcp（2026-10-16） */
    volatile bool cdc_up;        /* USB 主机在不在（tinyusb ATTACHED/DETACHED） */
    TickType_t send_fail_tick;   /* 发送失败日志限流 */
    void    (*cmd_cb)(const char *json, uint32_t len);
    void    (*blob_cb)(const uint8_t *data, uint32_t len);   /* 0x06 块数据（F10） */
    uint32_t  dropped;

    /* 发送统计（2026-10-16，link 命令与上位机状态栏用） */
    app_usb_link_stats_t tx;

    /* latest complete JPEG frame slot (window=1 protocol) */
    uint8_t  *jpeg_buf;
    uint32_t  jpeg_len;
    uint8_t   jpeg_seq;
    SemaphoreHandle_t jpeg_sem;
} jclink_t;

static jclink_t s_lk;

/* CRC-16/CCITT-FALSE */
static uint16_t crc16_ccitt(const uint8_t *data, uint32_t len, uint16_t crc)
{
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint32_t ring_used(void) { return s_lk.head - s_lk.tail; }
static inline uint32_t ring_free(void) { return RING_SIZE - ring_used(); }

/* called from tinyusb task context: push received chunk into ring */
static void ring_push(const uint8_t *data, uint32_t len)
{
    if (ring_free() < len) {
        /* drop oldest bytes to make room (keep newest data) */
        uint32_t need = len - ring_free();
        s_lk.tail += need;
        s_lk.dropped++;
    }
    uint32_t h = s_lk.head & (RING_SIZE - 1);
    uint32_t first = RING_SIZE - h;
    if (first > len) first = len;
    memcpy(s_lk.ring + h, data, first);
    if (len > first) {
        memcpy(s_lk.ring, data + first, len - first);
    }
    s_lk.head += len;
}

static uint32_t ring_pop(uint8_t *dst, uint32_t len)
{
    uint32_t avail = ring_used();
    if (len > avail) len = avail;
    if (len == 0) return 0;
    uint32_t t = s_lk.tail & (RING_SIZE - 1);
    uint32_t first = RING_SIZE - t;
    if (first > len) first = len;
    memcpy(dst, s_lk.ring + t, first);
    if (len > first) {
        memcpy(dst + first, s_lk.ring, len - first);
    }
    s_lk.tail += len;
    return len;
}

/* CDC RX callback (tinyusb task): move chunk into ring buffer */
static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    static uint8_t chunk[1024];
    size_t rx_size = 0;
    /* drain everything available in this event */
    while (ESP_OK == tinyusb_cdcacm_read(itf, chunk, sizeof(chunk), &rx_size) && rx_size > 0) {
        ring_push(chunk, rx_size);
        if (rx_size < sizeof(chunk)) break;
    }
}

static void frame_reset(void)
{
    s_lk.state = ST_MAGIC0;
    if (s_lk.f_buf) {
        heap_caps_free(s_lk.f_buf);
        s_lk.f_buf = NULL;
    }
}

/* consume one byte in the parser state machine */
static void parser_feed(uint8_t b)
{
    switch (s_lk.state) {
    case ST_MAGIC0:
        if (b == JCL_MAGIC0) s_lk.state = ST_MAGIC1;
        break;
    case ST_MAGIC1:
        s_lk.state = (b == JCL_MAGIC1) ? ST_TYPE : ST_MAGIC0;
        break;
    case ST_TYPE:
        s_lk.f_type = b;
        s_lk.crc_calc = crc16_ccitt(&b, 1, 0xFFFF);
        s_lk.state = ST_SEQ;
        break;
    case ST_SEQ:
        s_lk.f_seq = b;
        s_lk.crc_calc = crc16_ccitt(&b, 1, s_lk.crc_calc);
        s_lk.f_got = 0;
        s_lk.state = ST_LEN;
        break;
    case ST_LEN:
        ((uint8_t *)&s_lk.f_len)[s_lk.f_got++] = b;
        s_lk.crc_calc = crc16_ccitt(&b, 1, s_lk.crc_calc);
        if (s_lk.f_got == 4) {
            if (s_lk.f_len == 0 || s_lk.f_len > JCL_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "bad frame len %u", (unsigned)s_lk.f_len);
                s_lk.dropped++;
                frame_reset();
                return;
            }
            s_lk.f_buf = heap_caps_malloc(s_lk.f_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
            if (!s_lk.f_buf) {
                ESP_LOGE(TAG, "no mem for %u-byte frame", (unsigned)s_lk.f_len);
                s_lk.dropped++;
                frame_reset();
                return;
            }
            s_lk.f_got = 0;
            s_lk.state = ST_PAYLOAD;
        }
        break;
    case ST_PAYLOAD:
        s_lk.f_buf[s_lk.f_got++] = b;
        s_lk.crc_calc = crc16_ccitt(&b, 1, s_lk.crc_calc);
        if (s_lk.f_got == s_lk.f_len) {
            s_lk.state = ST_CRC0;
        }
        break;
    case ST_CRC0:
        ((uint8_t *)&s_lk.crc_rx)[0] = b;
        s_lk.state = ST_CRC1;
        break;
    case ST_CRC1:
        ((uint8_t *)&s_lk.crc_rx)[1] = b;
        if (s_lk.crc_rx == s_lk.crc_calc) {
            if (s_lk.f_type == JCL_TYPE_CMD && s_lk.cmd_cb) {
                s_lk.cmd_cb((const char *)s_lk.f_buf, s_lk.f_len);
            } else if (s_lk.f_type == JCL_TYPE_BLOB && s_lk.blob_cb) {
                /* F10：块数据回调（rx 任务上下文，payload 返回后即释放） */
                s_lk.blob_cb(s_lk.f_buf, s_lk.f_len);
            } else if (s_lk.f_type == JCL_TYPE_JPEG) {
                if (s_lk.jpeg_buf) heap_caps_free(s_lk.jpeg_buf);
                s_lk.jpeg_buf = s_lk.f_buf;
                s_lk.jpeg_len = s_lk.f_len;
                s_lk.jpeg_seq = s_lk.f_seq;
                s_lk.f_buf = NULL;         /* ownership moved */
                xSemaphoreGive(s_lk.jpeg_sem);
            }
        } else {
            ESP_LOGW(TAG, "crc mismatch type=%u len=%u", s_lk.f_type, (unsigned)s_lk.f_len);
            s_lk.dropped++;
        }
        frame_reset();
        break;
    default:
        frame_reset();
        break;
    }
}

static void usb_rx_task(void *arg)
{
    static uint8_t buf[512];
    /* 预热 newlib stdio 锁：本任务平时不打印，首次打印若赶上内部 RAM 碎片化
       会因 xQueueCreateMutex 失败直接 abort（实测）。启动时内存宽裕。 */
    ESP_LOGI(TAG, "usb_rx task started");
    while (1) {
        uint32_t n = ring_pop(buf, sizeof(buf));
        if (n == 0) {
            vTaskDelay(1);          /* HZ=100：pdMS_TO_TICKS(2) 会算成 0 tick，空转饿死 IDLE -> TWDT */
            continue;
        }
        app_usb_link_feed(buf, n);
    }
}

/* ---------------- 传输层后端 ---------------- */

/* USB CDC 写入（2026-10-16 重写）。
 * ⚠️ 关键坑：esp_tinyusb v2 的 `tinyusb_cdcacm_write_queue()` 返回 **size_t 已入队字节数**，
 *   不是 esp_err_t！旧代码 `esp_err_t err = tinyusb_cdcacm_write_queue(...); if (err != ESP_OK)
 *   return err;` 把"成功写入 N 字节"当成错误码返回 → **USB 侧每一次 send 都被判为失败**
 *   （TCP 走另一个后端，所以一直没暴露；这也是 USB 视频流从没跑起来的原因）。
 *   正确用法：按返回值推进，返回 0 = FIFO 满。
 * 策略与 TCP 后端一致：非阻塞推进 + 总预算（TX_BUDGET_US），超预算丢帧不阻塞调用者。 */
/* 当前帧的发送预算（tx_lock 串行化，写入者唯一）：视频帧用短预算快速丢帧，
   控制帧/ack 保持长预算，避免 H.264 连续发送把 ack 饿死。 */
static volatile uint32_t s_tx_budget_us = TX_BUDGET_US;

static esp_err_t cdc_write(const uint8_t *data, uint32_t len)
{
    if (!s_lk.cdc_up) {
        /* 没插 USB 主机（比如 PC 只走网线）时直接失败，避免白等 */
        return ESP_ERR_INVALID_STATE;
    }
    int64_t t0 = esp_timer_get_time();
    const uint8_t *p = data;
    uint32_t left = len;
    int spins = 0;
    while (left > 0) {
        uint32_t n = left > TX_CHUNK ? TX_CHUNK : left;
        size_t queued = tinyusb_cdcacm_write_queue(CDC_ITF, p, n);
        if (queued == 0) {
            /* FIFO 满：等 tinyusb 把 FIFO 抽干。
             * ⚠️ 2026-10-16 实测教训：这里原先用 `vTaskDelay(1)`（= 1 个 tick）——
             * 512B FIFO 每抽一次就等一整 tick，吞吐被死死钉在 ~170KB/s
             * （USB 下 JPEG 只能到 10.5fps，raw 614KB 帧直接超预算被截断）。
             * 改成 **μs 级忙等**（tinyusb 任务优先级更高会抢占，或跑在另一核）：
             * 连续忙等 64 次后再让出 1 tick，兼顾吞吐与不饿死其它任务。 */
            if (esp_timer_get_time() - t0 > (int64_t)s_tx_budget_us) {
                return ESP_ERR_TIMEOUT;
            }
            if (++spins < 64) {
                esp_rom_delay_us(50);
            } else {
                spins = 0;
                vTaskDelay(1);
            }
            continue;
        }
        /* 主动泵一次端点：缩短 FIFO 占用时间（非阻塞，未冲完也返回） */
        tinyusb_cdcacm_write_flush(CDC_ITF, 0);
        p += queued;
        left -= (uint32_t)queued;
    }
    return ESP_OK;
}

/* 帧尾 flush：非阻塞泵一次即可。数据已拷进 tinyusb 软件 FIFO（tud_cdc_n_write 是拷贝
   语义），调用方缓冲可立即复用；未冲完不算错误，后台任务会继续排。
   旧实现每帧 `write_flush(100)` 白等 100ms —— raw 10fps（614KB/帧）下会直接把吞吐
   压到 6MB/s 以下。 */
static esp_err_t cdc_flush(void)
{
    if (!s_lk.cdc_up) {
        return ESP_ERR_INVALID_STATE;
    }
    tinyusb_cdcacm_write_flush(CDC_ITF, 0);
    return ESP_OK;
}

static void tusb_event_cb(tinyusb_event_t *ev, void *arg)
{
    (void)arg;
    if (ev->id == TINYUSB_EVENT_ATTACHED) {
        s_lk.cdc_up = true;
        ESP_LOGI(TAG, "USB host attached -> CDC link up");
        /* 2026-10-16：把协商速率打出来——USB 视频流的天花板直接取决于 FS/HS
           （FS 理论 1.216MB/s；实测 raw 只到 1.36MB/s，需先判断是硬件 FS 受限
           还是 512B FIFO 的每包软件开销受限）。 */
        const tusb_speed_t sp = tud_speed_get();
        ESP_LOGW(TAG, "USB negotiated speed = %s", sp == TUSB_SPEED_HIGH ? "HIGH (480Mbps, 上限~60MB/s)"
                                    : sp == TUSB_SPEED_FULL ? "FULL (12Mbps, 上限~1.2MB/s)" : "UNKNOWN");
    } else if (ev->id == TINYUSB_EVENT_DETACHED) {
        s_lk.cdc_up = false;
        ESP_LOGW(TAG, "USB host detached -> CDC link down");
    }
}

static const jcl_transport_t s_cdc_tp = {
    .name = "usb-cdc",
    .write = cdc_write,
    .flush = cdc_flush,
};

/* 依据"强制态 + 是否有外部后端"重算生效后端（2026-10-16）：
   auto（默认）TCP 优先级与历史行为一致：客户端连上抢占、断开由 set_transport(NULL) 回落。 */
static void apply_backend(void)
{
    const jcl_transport_t *want = NULL;         /* NULL = 内置 USB CDC */
    if (s_lk.forced == 1) {                     /* 强制 usb */
        want = NULL;
    } else {                                    /* auto / 强制 tcp：都用外部后端（没连上则是 CDC） */
        want = s_lk.net_tp;
    }
    s_lk.tp = want;
}

bool app_usb_link_force_backend(const char *name)
{
    uint8_t want;
    if (!name || !strcmp(name, "auto")) {
        want = 0;
    } else if (!strcmp(name, "usb")) {
        want = 1;
    } else if (!strcmp(name, "tcp")) {
        want = 2;
    } else {
        return false;
    }
    s_lk.forced = want;
    apply_backend();
    ESP_LOGW(TAG, "link forced=%s -> active=%s", app_usb_link_forced_name(),
             app_usb_link_active_name());
    app_usb_link_log("usblink: link=%s active=%s", app_usb_link_forced_name(),
                     app_usb_link_active_name());
    return true;
}

const char *app_usb_link_active_name(void)
{
    return s_lk.tp ? s_lk.tp->name : s_cdc_tp.name;
}

const char *app_usb_link_forced_name(void)
{
    switch (s_lk.forced) {
    case 1:  return "usb";
    case 2:  return "tcp";
    default: return "auto";
    }
}

bool app_usb_link_usb_forced(void)
{
    return s_lk.forced == 1;
}

void app_usb_link_set_transport(const jcl_transport_t *tp)
{
    s_lk.net_tp = tp;
    apply_backend();
    ESP_LOGI(TAG, "tx backend -> %s (forced=%s)", app_usb_link_active_name(),
             app_usb_link_forced_name());
}

void app_usb_link_feed(const uint8_t *data, uint32_t len)
{
    if (!s_lk.rx_lock) return;
    xSemaphoreTake(s_lk.rx_lock, portMAX_DELAY);
    for (uint32_t i = 0; i < len; i++) {
        parser_feed(data[i]);
    }
    xSemaphoreGive(s_lk.rx_lock);
}

void app_usb_link_rx_reset(void)
{
    if (!s_lk.rx_lock) return;
    xSemaphoreTake(s_lk.rx_lock, portMAX_DELAY);
    frame_reset();
    xSemaphoreGive(s_lk.rx_lock);
}

bool app_usb_link_ready(void)
{
    return s_lk.tx_lock != NULL;
}

/* ---- public API ---- */

/* 组帧发送公共实现：帧 = hdr | payload，帧长 = hdr_len + payload_len（两段之间无间隙）。
   0x05 大帧走这里可以"8B 头 + 614KB 数据"分两次写，省掉一整块 PSRAM 与一次 memcpy。 */
static esp_err_t send_vect(uint8_t type,
                           const void *hdr, uint32_t hdr_len,
                           const void *payload, uint32_t payload_len)
{
    if (!s_lk.tx_lock) return ESP_ERR_INVALID_STATE;
    uint64_t len64 = (uint64_t)hdr_len + (uint64_t)payload_len;
    if (len64 > JCL_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;

    uint32_t len = (uint32_t)len64;
    static uint8_t seq;
    uint8_t h[8] = { JCL_MAGIC0, JCL_MAGIC1, type, 0, 0, 0, 0, 0 };
    h[3] = ++seq;
    memcpy(&h[4], &len, 4);
    uint16_t crc = crc16_ccitt(&h[2], 6, 0xFFFF);
    crc = crc16_ccitt(hdr, hdr_len, crc);
    crc = crc16_ccitt(payload, payload_len, crc);
    uint8_t crc_b[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lk.tx_lock, portMAX_DELAY);
    /* 视频帧拥塞时快速丢帧（50ms），把 tx_lock 尽快让给 0x02 ack/日志 */
    s_tx_budget_us = (type == JCL_TYPE_H264) ? 50000 : TX_BUDGET_US;
    const jcl_transport_t *tp = s_lk.tp ? s_lk.tp : &s_cdc_tp;
    esp_err_t err = tp->write(h, sizeof(h));
    if (err == ESP_OK && hdr_len) {
        err = tp->write(hdr, hdr_len);
    }
    if (err == ESP_OK && payload_len) {
        err = tp->write(payload, payload_len);
    }
    if (err == ESP_OK) {
        err = tp->write(crc_b, sizeof(crc_b));
    }
    if (err == ESP_OK && tp->flush) {
        err = tp->flush();
    }
    xSemaphoreGive(s_lk.tx_lock);

    if (err == ESP_OK) {
        s_lk.tx.tx_frames++;
        s_lk.tx.tx_bytes += (uint64_t)sizeof(h) + len + sizeof(crc_b);
        s_lk.tx.tx_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    } else {
        s_lk.tx.tx_drop++;
        /* 不在这里打日志：本函数可能在 usb_rx/驱动任务里，运行期首次 stdio
           锁分配在内部 RAM 碎片化后会 abort（实测）。失败只看 tx_drop 计数。 */
    }
    return err;
}

esp_err_t app_usb_link_send(uint8_t type, const void *payload, uint32_t len)
{
    return send_vect(type, NULL, 0, payload, len);
}

esp_err_t app_usb_link_send2(uint8_t type,
                             const void *hdr, uint32_t hdr_len,
                             const void *payload, uint32_t payload_len)
{
    return send_vect(type, hdr, hdr_len, payload, payload_len);
}

void app_usb_link_get_stats(app_usb_link_stats_t *out)
{
    if (out) *out = s_lk.tx;
}

void app_usb_link_log(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    if (n > 0) {
        buf[n] = '\n';
        app_usb_link_send(JCL_TYPE_LOG, buf, n + 1);
    }
}

int app_usb_link_recv(uint8_t *type_out, void *payload, uint32_t max_len)
{
    if (xSemaphoreTake(s_lk.jpeg_sem, 0) != pdTRUE) {
        return 0;
    }
    uint32_t n = s_lk.jpeg_len;
    if (n > max_len) n = max_len;
    memcpy(payload, s_lk.jpeg_buf, n);
    heap_caps_free(s_lk.jpeg_buf);
    s_lk.jpeg_buf = NULL;
    *type_out = JCL_TYPE_JPEG;
    return (int)n;
}

void app_usb_link_set_cmd_cb(void (*cb)(const char *json, uint32_t len))
{
    s_lk.cmd_cb = cb;
}

void app_usb_link_set_blob_cb(void (*cb)(const uint8_t *data, uint32_t len))
{
    s_lk.blob_cb = cb;
}

esp_err_t app_usb_link_init(void)
{
    if (s_lk.tx_lock) return ESP_OK;   /* already up */

    memset(&s_lk, 0, sizeof(s_lk));
    s_lk.ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_lk.ring, ESP_ERR_NO_MEM, TAG, "ring alloc failed");
    s_lk.tx_lock = xSemaphoreCreateMutex();
    s_lk.rx_lock = xSemaphoreCreateMutex();
    s_lk.jpeg_sem = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "USB CDC (tinyusb) initializing on USB0 OTG HS");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tusb_event_cb);
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install");

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = CDC_ITF,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "cdcacm init");

    BaseType_t ok = xTaskCreate(usb_rx_task, "usb_rx", 4096, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "rx task create failed");

    ESP_LOGI(TAG, "JCLINK v1 ready: ring=%dKB PSRAM, frame 'JC'|type|seq|len|payload|crc16",
             RING_SIZE / 1024);
    /* 构建标记（2026-10-16）：USB 视频流 + link 强制切换特性，便于确认板端跑的是哪一版 */
    ESP_LOGW(TAG, "link-feature build %s %s (CDC tx=%d rx=%d, TX_CHUNK=%d, budget=%dms)",
             __DATE__, __TIME__, CONFIG_TINYUSB_CDC_TX_BUFSIZE, CONFIG_TINYUSB_CDC_RX_BUFSIZE,
             TX_CHUNK, TX_BUDGET_US / 1000);
    return ESP_OK;
}
