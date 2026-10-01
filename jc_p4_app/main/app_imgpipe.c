/*
 * app_imgpipe.c —— 图像管道（任务四）
 *
 * USB(JCLINK 0x01 JPEG) -> esp_driver_jpeg 硬解 -> RGB888 帧池 -> app_faceai / ack JSON
 */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"

#include "app_imgpipe.h"
#include "sdkconfig.h"
#include "app_usb_link.h"
#include "app_camera.h"
#include "app_status.h"

#define TAG "imgpipe"

#define PIPE_RX_BUF        (256 * 1024)   /* JPEG 接收缓冲（PSRAM） */
#define PIPE_SLOTS         2              /* 帧池槽数（双缓冲） */
#define PIPE_TASK_STACK    4096
#define PIPE_TASK_PRIO     5
#define PIPE_ACQ_TIMEOUT   200            /* 源取帧超时 ms */
#define PIPE_MAX_DEC_W     1920
#define PIPE_MAX_DEC_H     1088

static void pipe_report(void);   /* 前向声明（pipe_publish 里用） */

typedef struct {
    img_frame_t   frame;      /* 发布给消费者的描述 */
    size_t        cap;        /* frame.data 容量 */
    int           pad_w;      /* 解码对齐后的宽（决定 stride） */
    int           pad_h;
    volatile bool busy;       /* 消费者持有 */
    volatile bool consumed;   /* 已被取走过（每帧只交付一次） */
} pipe_slot_t;

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t evt;      /* 新帧事件 */
    pipe_slot_t       slot[PIPE_SLOTS];
    volatile int      latest;   /* 最新帧槽号，-1 = 无 */
    jpeg_decoder_handle_t dec;
    uint8_t          *rx_buf;
    frame_source_t   *src;
    app_img_src_t     src_mode;   /* PC(拉 JPEG) / CAM(摄像头推 raw) */
    bool              hflip;      /* 摄像头帧左右镜像 */
    int               kr, kb;     /* 软件白平衡：R/B 增益千分比（1000 = 不校正） */
    /* 时域降噪（仅摄像头 raw 路径）*/
    int               dn;         /* 档位 0=关 1=低 2=中 3=高 */
    uint8_t          *prev;       /* 上一帧（未镜像、未 WB 的混合结果），PSRAM */
    size_t            prev_cap;
    int               prev_stride, prev_h;
    bool              has_prev;   /* prev 是否可用（首帧/失效后为 false） */
                                  /* 降噪耗时统计见 s_pipe.st.dn_ms */
    app_imgpipe_sink_t sink;
    bool              auto_ack;
    bool              ready;
    app_imgpipe_stats_t st;
    int64_t           fps_win;
    uint32_t          fps_cnt;
} s_pipe;

/* ---------------- USB (JCLINK) 图像源 ---------------- */

static esp_err_t usb_src_start(frame_source_t *self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t usb_src_acquire(frame_source_t *self, src_frame_t *out, uint32_t timeout_ms)
{
    (void)self;
    if (!out) return ESP_ERR_INVALID_ARG;
    int64_t deadline = (timeout_ms == APP_IMG_WAIT_FOREVER)
                       ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        uint8_t type = 0;
        int n = app_usb_link_recv(&type, s_pipe.rx_buf, PIPE_RX_BUF);
        if (n > 0) {
            out->data = s_pipe.rx_buf;
            out->len = (uint32_t)n;
            out->fmt = IMG_FMT_JPEG;
            out->seq = s_pipe.st.rx_frames + 1;
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);              /* 1 tick（HZ=100 -> 10ms），不能用 pdMS_TO_TICKS(2)=0 */
    }
}

/* recv() 已把 payload 拷进 rx_buf，源侧无需引用计数 */
static void usb_src_release(frame_source_t *self, const src_frame_t *f)
{
    (void)self;
    (void)f;
}

static frame_source_t s_usb_src = {
    .name = "usb-jclink",
    .start = usb_src_start,
    .acquire = usb_src_acquire,
    .release = usb_src_release,
    .priv = NULL,
};

frame_source_t *app_imgpipe_usb_source(void)
{
    return &s_usb_src;
}

/* ---------------- 帧池 ---------------- */

static int pipe_take_slot(void)
{
    int slot = -1;
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    for (int i = 0; i < PIPE_SLOTS; i++) {
        if (i != s_pipe.latest && !s_pipe.slot[i].busy) {
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_pipe.lock);
    return slot;
}

/* MCU 对齐尺寸：JPEG 解码输出按采样方式对齐（4:2:0 -> 16x16, 4:2:2 -> 16x8, 4:4:4/灰度 -> 8x8） */
static void pipe_mcu_size(jpeg_down_sampling_type_t m, int *mcux, int *mcuy)
{
    switch (m) {
    case JPEG_DOWN_SAMPLING_YUV420:
        *mcux = 16; *mcuy = 16;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *mcux = 16; *mcuy = 8;
        break;
    default:            /* YUV444 / GRAY */
        *mcux = 8;  *mcuy = 8;
        break;
    }
}

/* 硬解 JPEG 到指定槽（只写无人持有的槽） */
static esp_err_t pipe_decode_into(int idx, const uint8_t *jpeg, uint32_t len)
{
    jpeg_decode_picture_info_t info = { 0 };
    esp_err_t err = jpeg_decoder_get_info(jpeg, len, &info);
    ESP_RETURN_ON_ERROR(err, TAG, "jpeg header parse");
    if (info.width == 0 || info.height == 0 ||
        info.width > PIPE_MAX_DEC_W || info.height > PIPE_MAX_DEC_H) {
        ESP_LOGE(TAG, "unsupported picture %"PRIu32"x%"PRIu32, info.width, info.height);
        return ESP_ERR_INVALID_SIZE;
    }

    int mcux = 8, mcuy = 8;
    pipe_mcu_size(info.sample_method, &mcux, &mcuy);
    int pad_w = (int)((info.width + mcux - 1) / mcux * mcux);
    int pad_h = (int)((info.height + mcuy - 1) / mcuy * mcuy);
    size_t need = (size_t)pad_w * pad_h * 3;

    pipe_slot_t *sl = &s_pipe.slot[idx];
    if (sl->frame.data && (sl->cap < need || sl->pad_w != pad_w || sl->pad_h != pad_h)) {
        heap_caps_free(sl->frame.data);
        sl->frame.data = NULL;
        sl->cap = 0;
    }
    if (!sl->frame.data) {
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        void *p = jpeg_alloc_decoder_mem(need, &mem_cfg, &got);
        ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "dec buf %u B", (unsigned)need);
        sl->frame.data = p;
        sl->cap = got;
        sl->pad_w = pad_w;
        sl->pad_h = pad_h;
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = APP_IMG_JPEG_RGB_ORDER,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    int64_t t0 = esp_timer_get_time();
    uint32_t out_size = 0;
    err = jpeg_decoder_process(s_pipe.dec, &cfg, jpeg, len, sl->frame.data, sl->cap, &out_size);
    ESP_RETURN_ON_ERROR(err, TAG, "jpeg decode %"PRIu32" B", len);

    sl->frame.width = (int)info.width;
    sl->frame.height = (int)info.height;
    sl->frame.stride = pad_w * 3;          /* 解码输出行距 = MCU 对齐宽 * 3 */
    sl->frame.fmt = IMG_FMT_RGB888;
    sl->frame.dec_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    sl->frame.seq = s_pipe.st.rx_frames;
    return ESP_OK;
}

/*
 * 摄像头等 raw 帧：拷进槽（palette 与 JPEG 路径共用 pad_w/pad_h 作为
 * "缓冲失效判断键"：raw 路径存 stride/height，JPEG 路径存 MCU 对齐宽高）
 */
/* ---------------- 时域降噪（Temporal Denoise，仅摄像头 raw 路径） ----------------
 *
 * 弱光噪声以散粒噪声为主（实测：只降增益几乎不改善信噪比），而相邻帧的随机噪声
 * 互不相关、场景内容高度相关 —— 对"没动的地方"做多帧加权平均，噪声按 √N 下降。
 *
 * 做法（性能红线：camera 任务 prio 6，逐像素乘除会触发 task_wdt，历史踩过）：
 *   - 16x16 分块，每块用"下采样 SAD"（4x4=16 个采样点的 G 通道差）判断运动强度
 *   - 静止块强混合、运动块减弱或跳过（避免拖影）
 *   - 像素混合用整数移位：out = cur + ((prev - cur) * wq) >> 8，每像素 1 次乘法
 *     （ESP32 硬件乘法 1 周期；全程无除法/浮点），混合结果同时写回 prev（IIR，
 *     等效多帧平均，省掉一次 600KB memcpy）
 *   - dn=0 时完全不进这里（零开销）；高帧率由守卫跳过（CONFIG_APP_CAM_DN_MAX_FPS）
 *
 * ⚠️ 必须在镜像之前调用：prev 始终保存"未镜像"内容，否则 cur/prev 左右错位会出鬼影。
 */

#define DN_BLOCK     16      /* 块边长 */
#define DN_SAD_STEP  4       /* SAD 采样步长（每块 4x4=16 点） */

/* 档位参数：wq = 静止块混合权重（0..256，越大越依赖历史）；sad_lo/hi = 运动门限
 * （G 通道 6bit 差的采样均值：<=lo 全混、>=hi 不混，中间线性衰减） */
static const struct {
    int wq;
    int sad_lo;
    int sad_hi;
} s_dn_tab[4] = {
    {   0,   0,   0 },       /* 0 = 关（不会走到这里） */
    { 128,   6,  18 },       /* 1 = 低：静止块混 50% */
    { 192,   8,  24 },       /* 2 = 中：混 75% */
    { 236,  10,  30 },       /* 3 = 高：混 92%（留 8% 新帧，避免画面"冻住"） */
};

/* 按当前尺寸准备 prev 缓冲（沿用槽的 cap/stride/h 失效键思路） */
static bool pipe_dn_ensure(int stride, int h)
{
    size_t need = (size_t)stride * h;
    if (s_pipe.prev && s_pipe.prev_cap >= need &&
        s_pipe.prev_stride == stride && s_pipe.prev_h == h) {
        return true;
    }
    if (s_pipe.prev) {
        heap_caps_free(s_pipe.prev);
        s_pipe.prev = NULL;
        s_pipe.prev_cap = 0;
        s_pipe.has_prev = false;
    }
    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "denoise prev buf %u B alloc failed", (unsigned)need);
        return false;
    }
    s_pipe.prev = p;
    s_pipe.prev_cap = need;
    s_pipe.prev_stride = stride;
    s_pipe.prev_h = h;
    return true;
}

static void pipe_dn_apply(uint8_t *data, int w, int h, int stride)
{
    if (!pipe_dn_ensure(stride, h)) {
        return;
    }
    if (!s_pipe.has_prev) {
        memcpy(s_pipe.prev, data, (size_t)stride * h);
        s_pipe.has_prev = true;
        return;                                   /* 首帧：只建立历史，不混合 */
    }

    const int wq0 = s_dn_tab[s_pipe.dn].wq;
    const int sad_lo = s_dn_tab[s_pipe.dn].sad_lo;
    const int sad_hi = s_dn_tab[s_pipe.dn].sad_hi;
    uint32_t nblk = 0, nsad = 0, ntot = 0;

    for (int y0 = 0; y0 < h; y0 += DN_BLOCK) {
        int bh = (h - y0 < DN_BLOCK) ? (h - y0) : DN_BLOCK;
        for (int x0 = 0; x0 < w; x0 += DN_BLOCK) {
            int bw = (w - x0 < DN_BLOCK) ? (w - x0) : DN_BLOCK;

            /* 块 SAD（下采样、只看 G 通道，均值尺度 0..63） */
            int sad = 0;
            for (int by = 0; by < bh; by += DN_SAD_STEP) {
                const uint16_t *pc = (const uint16_t *)(data + (size_t)(y0 + by) * stride) + x0;
                const uint16_t *pp = (const uint16_t *)(s_pipe.prev + (size_t)(y0 + by) * stride) + x0;
                for (int bx = 0; bx < bw; bx += DN_SAD_STEP) {
                    int d = (int)((pc[bx] >> 5) & 0x3F) - (int)((pp[bx] >> 5) & 0x3F);
                    sad += d < 0 ? -d : d;
                }
            }
            sad >>= 4;
            nsad += (uint32_t)sad;
            ntot++;

            int wq = wq0;
            if (sad >= sad_hi) {
                wq = 0;                           /* 运动块：保留新帧（无拖影） */
            } else if (sad > sad_lo) {
                wq = wq0 * (sad_hi - sad) / (sad_hi - sad_lo);   /* 块级除法，每块仅一次 */
            }
            if (wq <= 0) {
                continue;
            }
            nblk++;

            /* 像素混合：整包 16bit 线性插值（RGB565 分量间最多 ±1 LSB 串位，肉眼不可见） */
            for (int y = 0; y < bh; y++) {
                uint16_t *pc = (uint16_t *)(data + (size_t)(y0 + y) * stride) + x0;
                uint16_t *pp = (uint16_t *)(s_pipe.prev + (size_t)(y0 + y) * stride) + x0;
                for (int x = 0; x < bw; x++) {
                    /* ⚠️ 必须按 R/G/B 分量分别混合：RGB565 是位打包格式，
                       对整包做线性插值会跨分量借位（实测噪声反而涨 2.6 倍、出彩色伪影） */
                    int c = pc[x];
                    int p = pp[x];
                    int r = (c >> 11) & 0x1F;
                    int g = (c >> 5) & 0x3F;
                    int b = c & 0x1F;
                    r += ((((p >> 11) & 0x1F) - r) * wq) >> 8;
                    g += ((((p >> 5) & 0x3F) - g) * wq) >> 8;
                    b += (((p & 0x1F) - b) * wq) >> 8;
                    int o = (r << 11) | (g << 5) | b;
                    pc[x] = (uint16_t)o;
                    pp[x] = (uint16_t)o;          /* prev 存混合结果（IIR） */
                }
            }
        }
    }

    s_pipe.st.dn_blocks = nblk;                   /* 调参用：混合块数 / 全帧平均 SAD */
    s_pipe.st.dn_sad = ntot ? (nsad / ntot) : 0;
}

/*
 * 帧后处理 = 软件白平衡(R/B 增益) + 行内镜像，**合并成一次内存遍历**。
 * 白平衡用查表（5bit/8bit LUT）而不是每像素乘除 —— 逐像素乘除实测会把
 * camera 任务(prio 6)占死到触发 task_wdt、把 faceai 饿死（0x02 全停）。
 */
static void pipe_postprocess(uint8_t *data, int w, int h, int stride, img_fmt_t fmt,
                             int kr, int kb, bool hflip)
{
    bool wb = (kr != 1000 || kb != 1000);
    if (!wb && !hflip) {
        return;
    }

    static uint8_t lut_r5[32], lut_b5[32], lut_r8[256], lut_b8[256];
    static int last_kr = -9999, last_kb = -9999;
    if (wb && (kr != last_kr || kb != last_kb)) {
        for (int i = 0; i < 32; i++) {
            int v = i * kr / 1000;
            lut_r5[i] = (uint8_t)(v > 31 ? 31 : v);
            v = i * kb / 1000;
            lut_b5[i] = (uint8_t)(v > 31 ? 31 : v);
        }
        for (int i = 0; i < 256; i++) {
            int v = i * kr / 1000;
            lut_r8[i] = (uint8_t)(v > 255 ? 255 : v);
            v = i * kb / 1000;
            lut_b8[i] = (uint8_t)(v > 255 ? 255 : v);
        }
        last_kr = kr;
        last_kb = kb;
    }

    for (int y = 0; y < h; y++) {
        uint8_t *row = data + (size_t)y * stride;
        if (fmt == IMG_FMT_RGB565) {
            uint16_t *p = (uint16_t *)row;
            if (hflip) {
                for (int i = 0, j = w - 1; i < j; i++, j--) {
                    uint16_t t = p[i];
                    p[i] = p[j];
                    p[j] = t;
                }
            }
            if (wb) {
                for (int x = 0; x < w; x++) {
                    uint16_t v = p[x];
                    p[x] = (uint16_t)(((uint16_t)lut_r5[(v >> 11) & 0x1F] << 11) |
                                      (v & 0x07E0) | lut_b5[v & 0x1F]);
                }
            }
        } else {                    /* RGB888 */
            if (hflip) {
                for (int i = 0, j = w - 1; i < j; i++, j--) {
                    for (int k = 0; k < 3; k++) {
                        uint8_t t = row[i * 3 + k];
                        row[i * 3 + k] = row[j * 3 + k];
                        row[j * 3 + k] = t;
                    }
                }
            }
            if (wb) {
                for (int x = 0; x < w; x++) {
                    row[x * 3] = lut_r8[row[x * 3]];
                    row[x * 3 + 2] = lut_b8[row[x * 3 + 2]];
                }
            }
        }
    }
}

static esp_err_t pipe_copy_into(int idx, const uint8_t *data, int w, int h, int stride, img_fmt_t fmt)
{
    int bpp = (fmt == IMG_FMT_RGB565) ? 2 : 3;
    if (stride <= 0) stride = w * bpp;
    if (w <= 0 || h <= 0 || stride < w * bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t need = (size_t)stride * h;

    pipe_slot_t *sl = &s_pipe.slot[idx];
    if (sl->frame.data && (sl->cap < need || sl->pad_w != stride || sl->pad_h != h)) {
        heap_caps_free(sl->frame.data);
        sl->frame.data = NULL;
        sl->cap = 0;
    }
    if (!sl->frame.data) {
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        void *p = jpeg_alloc_decoder_mem(need, &mem_cfg, &got);
        ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "raw buf %u B", (unsigned)need);
        sl->frame.data = p;
        sl->cap = got;
        sl->pad_w = stride;
        sl->pad_h = h;
    }

    int64_t t0 = esp_timer_get_time();
    memcpy(sl->frame.data, data, need);
    sl->frame.width = w;
    sl->frame.height = h;
    sl->frame.stride = stride;
    sl->frame.fmt = fmt;
    sl->frame.dec_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);   /* raw 帧=拷贝耗时 */
    sl->frame.seq = s_pipe.st.rx_frames + 1;
    return ESP_OK;
}

/* 发布某一槽为最新帧并通知消费者/sink */
static void pipe_publish(int slot)
{
    img_frame_t fr = s_pipe.slot[slot].frame;
    s_pipe.st.dec_frames++;
    s_pipe.st.dec_ms = fr.dec_ms;
    s_pipe.st.width = fr.width;
    s_pipe.st.height = fr.height;

    s_pipe.slot[slot].frame.ts_us = esp_timer_get_time();
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    s_pipe.latest = slot;
    s_pipe.slot[slot].consumed = false;
    xSemaphoreGive(s_pipe.lock);
    xSemaphoreGive(s_pipe.evt);

    pipe_report();

    if (s_pipe.sink) {
        s_pipe.sink(&fr);                   /* faceai 自己回 0x02 */
    }
}

/* ---------------- 0x02 ack（sink 未注册时的最小结果帧） ---------------- */

static void pipe_send_ack(const char *event, const char *msg, int w, int h, uint32_t dec_ms)
{
    char js[288];
    int n = snprintf(js, sizeof(js),
                     "{\"faces\":0,\"boxes\":[],\"event\":\"%s\","
                     "\"fps\":%.1f,\"infer_ms\":%u,\"dropped\":%"PRIu32","
                     "\"pipe\":{\"w\":%d,\"h\":%d,\"jpeg\":%"PRIu32",\"dec_ms\":%u}%s%s%s}",
                     event, (double)s_pipe.st.fps, (unsigned)dec_ms, s_pipe.st.dropped,
                     w, h, s_pipe.st.jpeg_bytes, (unsigned)dec_ms,
                     msg ? ",\"msg\":\"" : "", msg ? msg : "", msg ? "\"" : "");
    if (n > 0 && n < (int)sizeof(js)) {
        app_usb_link_send(JCL_TYPE_RESULT, js, (uint32_t)n);
    }
}

/* ---------------- 解码任务 ---------------- */

static void pipe_report(void)
{
    int64_t now = esp_timer_get_time();
    s_pipe.fps_cnt++;
    int64_t win = now - s_pipe.fps_win;
    if (win < 1000000) {
        return;
    }
    s_pipe.st.fps = (float)s_pipe.fps_cnt * 1000000.0f / (float)win;
    s_pipe.fps_win = now;
    s_pipe.fps_cnt = 0;
    app_status_set(APP_MOD_PIPE, true, "%dx%d %.1ffps %ums",
                   s_pipe.st.width, s_pipe.st.height, (double)s_pipe.st.fps, (unsigned)s_pipe.st.dec_ms);
    if (s_pipe.st.dec_frames) {
        app_usb_link_log("imgpipe: %dx%d %.1ffps dec=%ums jpeg=%"PRIu32"B rx=%"PRIu32" drop=%"PRIu32,
                         s_pipe.st.width, s_pipe.st.height, (double)s_pipe.st.fps,
                         (unsigned)s_pipe.st.dec_ms, s_pipe.st.jpeg_bytes,
                         s_pipe.st.rx_frames, s_pipe.st.dropped);
    }
}

static void imgpipe_task(void *arg)
{
    (void)arg;
    src_frame_t sf;

    while (1) {
        frame_source_t *src = s_pipe.src;
        if (!src) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool have = false;
        if (s_pipe.src_mode == APP_IMG_SRC_PC) {
            have = (src->acquire(src, &sf, PIPE_ACQ_TIMEOUT) == ESP_OK);
        } else {
            /* CAM 模式：摄像头帧由 camera 任务直接 publish_raw 推进来，这里只
               瞄一眼 PC 有没有在发帧（发了就自动切回 PC 源，PC 侧不用先发命令） */
            if (src->acquire(src, &sf, 0) == ESP_OK) {
                app_imgpipe_select_src(APP_IMG_SRC_PC);
                have = true;
            } else {
                vTaskDelay(1);
            }
        }
        if (!have) {
            continue;
        }

        s_pipe.st.rx_frames++;
        s_pipe.st.jpeg_bytes = sf.len;

        int slot = pipe_take_slot();
        if (slot < 0) {
            s_pipe.st.dropped++;
            ESP_LOGW(TAG, "no free frame slot, drop %"PRIu32" B", sf.len);
            src->release(src, &sf);
            continue;
        }

        esp_err_t err = pipe_decode_into(slot, sf.data, sf.len);
        src->release(src, &sf);
        if (err != ESP_OK) {
            s_pipe.st.dropped++;
            ESP_LOGE(TAG, "decode failed (%"PRIu32" B): %s", sf.len, esp_err_to_name(err));
            pipe_send_ack("error", "jpeg decode failed", 0, 0, 0);
            continue;
        }

        bool want_ack = (s_pipe.sink == NULL) && s_pipe.auto_ack;
        img_frame_t fr = s_pipe.slot[slot].frame;
        pipe_publish(slot);
        if (want_ack) {
            pipe_send_ack("none", NULL, fr.width, fr.height, fr.dec_ms);
        }
    }
}

/* ---------------- 源模式 / raw 帧推送 ---------------- */

const char *app_imgpipe_src_str(app_img_src_t src)
{
    return src == APP_IMG_SRC_CAM ? "cam" : "pc";
}

app_img_src_t app_imgpipe_get_src(void)
{
    return s_pipe.src_mode;
}

void app_imgpipe_set_hflip(bool on)
{
    if (s_pipe.hflip == on) {
        return;
    }
    s_pipe.hflip = on;
    ESP_LOGW(TAG, "camera hflip -> %s", on ? "ON" : "OFF");
    app_usb_link_log("imgpipe: hflip %s", on ? "on" : "off");
}

bool app_imgpipe_get_hflip(void)
{
    return s_pipe.hflip;
}

void app_imgpipe_set_wb_gain(int kr, int kb)
{
    if (kr < 100) kr = 100;
    if (kr > 4000) kr = 4000;
    if (kb < 100) kb = 100;
    if (kb > 4000) kb = 4000;
    s_pipe.kr = kr;
    s_pipe.kb = kb;
    ESP_LOGW(TAG, "software WB: R*%.3f B*%.3f", kr / 1000.0, kb / 1000.0);
    app_usb_link_log("imgpipe: wb kr=%d kb=%d", kr, kb);
}

void app_imgpipe_get_wb_gain(int *kr, int *kb)
{
    if (kr) *kr = s_pipe.kr;
    if (kb) *kb = s_pipe.kb;
}

void app_imgpipe_set_dn(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    if (s_pipe.dn == level) {
        return;
    }
    s_pipe.dn = level;
    s_pipe.has_prev = false;       /* 换档丢弃历史，避免策略切换瞬间的残影 */
    ESP_LOGW(TAG, "temporal denoise -> %d (%s)", level,
             level == 0 ? "off" : level == 1 ? "low" : level == 2 ? "mid" : "high");
    app_usb_link_log("imgpipe: denoise -> %d", level);
}

int app_imgpipe_get_dn(void)
{
    return s_pipe.dn;
}

void app_imgpipe_reset_prev(void)
{
    s_pipe.has_prev = false;
}

void app_imgpipe_select_src(app_img_src_t src)
{
    if (s_pipe.src_mode == src) {
        return;
    }
    s_pipe.src_mode = src;
    ESP_LOGW(TAG, "active image source -> %s", app_imgpipe_src_str(src));
    app_usb_link_log("imgpipe: image source -> %s", app_imgpipe_src_str(src));
}

esp_err_t app_imgpipe_publish_raw(const uint8_t *data, int w, int h, int stride, img_fmt_t fmt)
{
    ESP_RETURN_ON_FALSE(data && s_pipe.lock, ESP_ERR_INVALID_STATE, TAG, "not inited");
    if (s_pipe.src_mode != APP_IMG_SRC_CAM) {
        return ESP_ERR_INVALID_STATE;        /* PC 模式：摄像头帧丢弃，不做拷贝 */
    }

    int slot = pipe_take_slot();
    if (slot < 0) {
        s_pipe.st.dropped++;
        return ESP_ERR_NO_MEM;               /* 消费者还没放槽，丢帧 */
    }
    esp_err_t err = pipe_copy_into(slot, data, w, h, stride, fmt);
    if (err != ESP_OK) {
        s_pipe.st.dropped++;
        return err;
    }
    uint8_t *slot_data = s_pipe.slot[slot].frame.data;

    /* 时域降噪（必须在镜像之前，见 pipe_dn_apply 注释）；高帧率下跳过以保证链路稳定 */
    if (s_pipe.dn > 0 && fmt == IMG_FMT_RGB565) {
        if (CONFIG_APP_CAM_DN_MAX_FPS > 0 &&
            app_camera_get_fps() > (uint32_t)CONFIG_APP_CAM_DN_MAX_FPS) {
            s_pipe.st.dn_skip++;
            s_pipe.has_prev = false;      /* 回到低帧率时不要复用陈旧历史（会残影） */
        } else {
            int64_t t_dn = esp_timer_get_time();
            pipe_dn_apply(slot_data, w, h, stride);
            s_pipe.st.dn_ms = (uint32_t)((esp_timer_get_time() - t_dn) / 1000);
        }
    }

    /* 只处理槽里的副本（白平衡 + 镜像一次遍历），不动摄像头 mmap 缓冲 */
    pipe_postprocess(slot_data, w, h, stride, fmt,
                     s_pipe.kr, s_pipe.kb, s_pipe.hflip);
    int bpp = (fmt == IMG_FMT_RGB565) ? 2 : 3;
    s_pipe.st.rx_frames++;
    s_pipe.st.jpeg_bytes = (uint32_t)(stride > 0 ? stride : w * bpp) * (uint32_t)h;
    pipe_publish(slot);
    return ESP_OK;
}

/* ---------------- 公共 API ---------------- */

esp_err_t app_imgpipe_get_frame(img_frame_t *out, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(out && s_pipe.lock, ESP_ERR_INVALID_STATE, TAG, "not inited");
    int64_t deadline = (timeout_ms == APP_IMG_WAIT_FOREVER)
                       ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
        int idx = s_pipe.latest;
        if (idx >= 0 && !s_pipe.slot[idx].busy && !s_pipe.slot[idx].consumed) {
            s_pipe.slot[idx].busy = true;
            s_pipe.slot[idx].consumed = true;
            *out = s_pipe.slot[idx].frame;
            xSemaphoreGive(s_pipe.lock);
            return ESP_OK;
        }
        xSemaphoreGive(s_pipe.lock);
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_NOT_FOUND;
        }
        xSemaphoreTake(s_pipe.evt, 1);   /* 仅作唤醒(1 tick)，超时后重查；0 tick 会空转 */
    }
}

void app_imgpipe_release(const img_frame_t *frame)
{
    if (!frame || !s_pipe.lock) return;
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    for (int i = 0; i < PIPE_SLOTS; i++) {
        if (s_pipe.slot[i].frame.data == frame->data) {
            s_pipe.slot[i].busy = false;
            break;
        }
    }
    xSemaphoreGive(s_pipe.lock);
}

void app_imgpipe_set_sink(app_imgpipe_sink_t cb)
{
    s_pipe.sink = cb;
}

void app_imgpipe_set_auto_ack(bool enable)
{
    s_pipe.auto_ack = enable;
    ESP_LOGI(TAG, "auto ack %s", enable ? "on" : "off (consumer replies)");
}

frame_source_t *app_imgpipe_source(void)
{
    return s_pipe.src;
}

void app_imgpipe_set_source(frame_source_t *src)
{
    s_pipe.src = src ? src : &s_usb_src;
    ESP_LOGI(TAG, "source -> %s", s_pipe.src->name);
}

/* ---------------- 工具：缓冲 / 缩放 ---------------- */

esp_err_t app_imgpipe_alloc_rgb888(img_frame_t *frame, int w, int h)
{
    ESP_RETURN_ON_FALSE(frame && w > 0 && h > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");
    memset(frame, 0, sizeof(*frame));
    size_t size = (size_t)w * h * 3;
    uint8_t *p = heap_caps_aligned_calloc(64, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "rgb888 %dx%d", w, h);
    frame->data = p;
    frame->width = w;
    frame->height = h;
    frame->stride = w * 3;
    frame->fmt = IMG_FMT_RGB888;
    return ESP_OK;
}

void app_imgpipe_free_frame(img_frame_t *frame)
{
    if (!frame) return;
    if (frame->data) heap_caps_free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

esp_err_t app_imgpipe_resize_rgb888(const img_frame_t *src, img_frame_t *dst)
{
    ESP_RETURN_ON_FALSE(src && dst && src->data && dst->data, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(src->fmt == IMG_FMT_RGB888 && dst->fmt == IMG_FMT_RGB888,
                        ESP_ERR_NOT_SUPPORTED, TAG, "only RGB888");
    const int sw = src->width, sh = src->height, dw = dst->width, dh = dst->height;
    ESP_RETURN_ON_FALSE(sw >= 2 && sh >= 2 && dw > 0 && dh > 0 && dst->stride >= dw * 3,
                        ESP_ERR_INVALID_ARG, TAG, "%dx%d -> %dx%d", sw, sh, dw, dh);

    /* 16.16 定点步进，dst 边缘映射到 src 边缘 */
    const uint32_t xstep = (dw > 1) ? (uint32_t)(((uint64_t)(sw - 1) << 16) / (dw - 1)) : 0;
    const uint32_t ystep = (dh > 1) ? (uint32_t)(((uint64_t)(sh - 1) << 16) / (dh - 1)) : 0;

    uint32_t yfp = 0;
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)(yfp >> 16);
        uint32_t fy = (yfp >> 8) & 0xFF;
        if (sy0 > sh - 2) sy0 = sh - 2;
        const uint8_t *row0 = src->data + (size_t)sy0 * src->stride;
        const uint8_t *row1 = row0 + src->stride;
        uint8_t *drow = dst->data + (size_t)y * dst->stride;

        uint32_t xfp = 0;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)(xfp >> 16);
            uint32_t fx = (xfp >> 8) & 0xFF;
            if (sx0 > sw - 2) sx0 = sw - 2;
            const uint8_t *p00 = row0 + (size_t)sx0 * 3;
            const uint8_t *p01 = row1 + (size_t)sx0 * 3;

            uint32_t w00 = (256 - fx) * (256 - fy);
            uint32_t w10 = fx * (256 - fy);
            uint32_t w01 = (256 - fx) * fy;
            uint32_t w11 = fx * fy;
            for (int c = 0; c < 3; c++) {
                uint32_t v = p00[c] * w00 + p00[3 + c] * w10 + p01[c] * w01 + p01[3 + c] * w11;
                drow[x * 3 + c] = (uint8_t)(v >> 16);
            }
            xfp += xstep;
        }
        yfp += ystep;
    }
    return ESP_OK;
}

void app_imgpipe_get_stats(app_imgpipe_stats_t *out)
{
    if (out) *out = s_pipe.st;
}

esp_err_t app_imgpipe_init(void)
{
    if (s_pipe.ready) return ESP_OK;

    s_pipe.lock = xSemaphoreCreateMutex();
    s_pipe.evt = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_pipe.lock && s_pipe.evt, ESP_ERR_NO_MEM, TAG, "sync primitives");

    s_pipe.latest = -1;
    s_pipe.auto_ack = true;
    s_pipe.hflip = CONFIG_APP_CAM_HFLIP;   /* 摄像头模组安装方向导致的镜像，默认翻回来 */
    s_pipe.kr = CONFIG_APP_CAM_WB_KR;      /* 软件白平衡（默认按本机模组实测偏绿量校正） */
    s_pipe.kb = CONFIG_APP_CAM_WB_KB;
    s_pipe.dn = CONFIG_APP_CAM_DN;         /* 时域降噪档（0=关，运行时可用 cam 命令改） */
    s_pipe.fps_win = esp_timer_get_time();
    s_pipe.rx_buf = heap_caps_malloc(PIPE_RX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_pipe.rx_buf, ESP_ERR_NO_MEM, TAG, "rx buf %d B", PIPE_RX_BUF);

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&eng_cfg, &s_pipe.dec), TAG, "jpeg decoder engine");

    s_pipe.src = app_imgpipe_usb_source();
    if (s_pipe.src->start) {
        ESP_RETURN_ON_ERROR(s_pipe.src->start(s_pipe.src), TAG, "source start");
    }

    /* 摄像头已经出图就默认用它（板载源优先），否则等 PC 送帧 */
    s_pipe.src_mode = app_camera_ready() ? APP_IMG_SRC_CAM : APP_IMG_SRC_PC;

    BaseType_t ok = xTaskCreate(imgpipe_task, "imgpipe", PIPE_TASK_STACK, NULL, PIPE_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");

    s_pipe.ready = true;
    app_status_set(APP_MOD_PIPE, true, s_pipe.src_mode == APP_IMG_SRC_CAM ? "camera src"
                                                                          : "waiting jpeg");
    ESP_LOGI(TAG, "up: src=%s(%s), hw jpeg decode -> RGB888, slots=%d",
             app_imgpipe_src_str(s_pipe.src_mode),
             s_pipe.src_mode == APP_IMG_SRC_CAM ? "onboard camera" : s_pipe.src->name, PIPE_SLOTS);
    app_usb_link_log("imgpipe ready: image source=%s", app_imgpipe_src_str(s_pipe.src_mode));
    return ESP_OK;
}
