#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "app_usb_link.h"
#include "sdkconfig.h"

#define TAG "usb_link"

#define RING_SIZE      (128 * 1024)   /* rx ring buffer (PSRAM) */
#define TX_CHUNK       (4096)         /* max bytes per write_queue call */
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
    const jcl_transport_t *tp;   /* 当前发送后端，NULL = 内置 USB CDC */
    volatile bool cdc_up;        /* USB 主机在不在（tinyusb ATTACHED/DETACHED） */
    TickType_t send_fail_tick;   /* 发送失败日志限流 */
    void    (*cmd_cb)(const char *json, uint32_t len);
    uint32_t  dropped;

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

static esp_err_t cdc_write(const uint8_t *data, uint32_t len)
{
    if (!s_lk.cdc_up) {
        /* 没插 USB 主机（比如 PC 只走网线）时直接失败：否则 flush 会白等 100ms
           并且每帧刷一条 "Flush failed"，把串口/CPU 拖死 */
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = data;
    uint32_t left = len;
    while (left > 0) {
        uint32_t n = left > TX_CHUNK ? TX_CHUNK : left;
        esp_err_t err = tinyusb_cdcacm_write_queue(CDC_ITF, p, n);
        if (err != ESP_OK) return err;
        p += n;
        left -= n;
    }
    return ESP_OK;
}

static esp_err_t cdc_flush(void)
{
    if (!s_lk.cdc_up) {
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_cdcacm_write_flush(CDC_ITF, 100);
}

static void tusb_event_cb(tinyusb_event_t *ev, void *arg)
{
    (void)arg;
    if (ev->id == TINYUSB_EVENT_ATTACHED) {
        s_lk.cdc_up = true;
        ESP_LOGI(TAG, "USB host attached -> CDC link up");
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

void app_usb_link_set_transport(const jcl_transport_t *tp)
{
    s_lk.tp = tp;
    ESP_LOGI(TAG, "tx backend -> %s", tp ? tp->name : s_cdc_tp.name);
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

esp_err_t app_usb_link_send(uint8_t type, const void *payload, uint32_t len)
{
    if (!s_lk.tx_lock) return ESP_ERR_INVALID_STATE;
    if (len > JCL_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;

    static uint8_t seq;
    uint8_t hdr[8] = { JCL_MAGIC0, JCL_MAGIC1, type, 0, 0, 0, 0, 0 };
    hdr[3] = ++seq;
    memcpy(&hdr[4], &len, 4);
    uint16_t crc = crc16_ccitt(&hdr[2], 6, 0xFFFF);
    crc = crc16_ccitt(payload, len, crc);
    uint8_t crc_b[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

    xSemaphoreTake(s_lk.tx_lock, portMAX_DELAY);
    const jcl_transport_t *tp = s_lk.tp ? s_lk.tp : &s_cdc_tp;
    esp_err_t err = tp->write(hdr, sizeof(hdr));
    if (err == ESP_OK && len) {
        err = tp->write((const uint8_t *)payload, len);
    }
    if (err == ESP_OK) {
        err = tp->write(crc_b, sizeof(crc_b));
    }
    if (err == ESP_OK && tp->flush) {
        err = tp->flush();
    }
    xSemaphoreGive(s_lk.tx_lock);
    if (err != ESP_OK) {
        /* 限流：断链时每帧都失败，不能每帧刷一条（会拖慢串口/CPU） */
        TickType_t now = xTaskGetTickCount();
        if (now - s_lk.send_fail_tick > pdMS_TO_TICKS(5000)) {
            s_lk.send_fail_tick = now;
            ESP_LOGW(TAG, "send type=%u len=%u via %s failed: %s (cdc_up=%d)",
                     type, (unsigned)len, tp->name, esp_err_to_name(err), (int)s_lk.cdc_up);
        }
    }
    return err;
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
    return ESP_OK;
}
