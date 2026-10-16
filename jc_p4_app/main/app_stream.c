/*
 * app_stream.c —— 图像回传（P4 硬件 JPEG 编码 -> JCLINK 0x05）
 *
 * 设计：独立任务 + 限速取帧。app_imgpipe 的"每帧只交付一次"语义意味着本任务
 * 每 1/fps 秒才取一帧，所以对 app_faceai 的帧率影响很小（10fps 回传 ≈ 只吃掉
 * 摄像头的 10/50 帧）。取到的帧编码完立即 release，不长时间占槽。
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"

#include "app_stream.h"
#include "app_imgpipe.h"
#include "app_usb_link.h"
#include "app_status.h"

#define TAG "stream"

#define STREAM_TASK_STACK   4096
#define STREAM_TASK_PRIO    4
#define STREAM_ACQ_TIMEOUT  50      /* 取帧超时 ms */
#define STREAM_OUT_EXTRA    4096    /* JPEG 输出缓冲余量 */

static struct {
    jpeg_encoder_handle_t enc;
    uint8_t  *in_buf;          /* 重打包用（stride 与宽不匹配时） */
    size_t    in_cap;
    uint8_t  *out_buf;         /* JPEG 编码输出：必须 jpeg_alloc_encoder_mem 分配（对齐） */
    size_t    out_cap;
    uint8_t  *raw_buf;         /* raw 模式整帧缓冲：与 out_buf **分开**，否则会破坏 JPEG 对齐 */
    size_t    raw_cap;
    bool      enabled;
    int       fmt;              /* APP_STREAM_FMT_JPEG / _RGB565 */
    int       sub;              /* JPEG 子采样 0=420 1=422 2=444 */
    bool      reverse;          /* jpeg_encode_cfg.pixel_reverse（RGB565 字节序） */
    uint32_t  fps;
    uint32_t  quality;
    int64_t   last_send;
    uint32_t  seq;
    app_stream_stats_t st;
    int64_t   win;
    uint32_t  win_cnt;
} s_st = {
    .fps = APP_STREAM_DEF_FPS,
    .quality = APP_STREAM_DEF_QUALITY,
    /* P4 硬件 JPEG 编码器读 RGB565 需要反转字节序：不设的话 0x05 回传是彩色噪声
       （实测 reverse=false -> 蓝紫噪点；true -> 正常的中性灰绿画面） */
    .reverse = true,
};

static esp_err_t stream_encode(const img_frame_t *fr, uint8_t **out, uint32_t *out_len)
{
    int bpp = (fr->fmt == IMG_FMT_RGB565) ? 2 : 3;
    const uint8_t *in = fr->data;
    uint32_t in_size = (uint32_t)fr->stride * (uint32_t)fr->height;

    /* 硬件 JPEG 编码器的 RGB 通道序与标准相反：编码出的 JPEG 红蓝对调
     * （实测同一场景 raw 直出颜色正确、JPEG 错；PIL 与模拟器两个独立解码器
     * 一致——故问题在编码器对输入的解读，且无配置项可改）。
     * JPEG 是标准 JFIF，改解码端会留下非标准文件，所以在编码前把 R/B 换回：
     * RGB565 位域交换 / RGB888 首尾字节交换，拷进私有缓冲（顺带完成 stride 重排）。
     * 只影响 0x05 JPEG 链路：AI / raw / 相机路径都不经过这里。无条件走交换拷贝。 */
    {
        size_t need = (size_t)fr->width * fr->height * bpp;
        if (s_st.in_cap < need) {
            if (s_st.in_buf) heap_caps_free(s_st.in_buf);
            s_st.in_buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_st.in_cap = s_st.in_buf ? need : 0;
        }
        ESP_RETURN_ON_FALSE(s_st.in_buf, ESP_ERR_NO_MEM, TAG, "in buf %u B", (unsigned)need);
        if (bpp == 2) {
            for (int y = 0; y < fr->height; y++) {
                const uint16_t *src = (const uint16_t *)(fr->data + (size_t)y * fr->stride);
                uint16_t *dst = (uint16_t *)(s_st.in_buf + (size_t)y * fr->width * 2);
                for (int x = 0; x < fr->width; x++) {
                    uint16_t v = src[x];
                    dst[x] = (uint16_t)(((v & 0x001F) << 11) | (v & 0x07E0) | (v >> 11));
                }
            }
        } else {
            for (int y = 0; y < fr->height; y++) {
                const uint8_t *src = fr->data + (size_t)y * fr->stride;
                uint8_t *dst = s_st.in_buf + (size_t)y * fr->width * 3;
                for (int x = 0; x < fr->width; x++) {
                    dst[x * 3] = src[x * 3 + 2];
                    dst[x * 3 + 1] = src[x * 3 + 1];
                    dst[x * 3 + 2] = src[x * 3];
                }
            }
        }
        in = s_st.in_buf;
        in_size = (uint32_t)need;
    }

    size_t need_out = (size_t)fr->width * fr->height * bpp / 2 + STREAM_OUT_EXTRA;
    if (s_st.out_cap < need_out) {
        if (s_st.out_buf) heap_caps_free(s_st.out_buf);
        /* 必须用 jpeg_alloc_encoder_mem：硬件要求输出比特流缓冲按 cache 行对齐，
           否则 jpeg_encoder_process 直接报 "bit stream is not aligned" */
        jpeg_encode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        void *p = jpeg_alloc_encoder_mem(need_out, &mem_cfg, &got);
        s_st.out_buf = p;
        s_st.out_cap = p ? got : 0;
    }
    ESP_RETURN_ON_FALSE(s_st.out_buf, ESP_ERR_NO_MEM, TAG, "out buf %u B", (unsigned)need_out);

    if (!s_st.enc) {
        jpeg_encode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 200 };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_st.enc), TAG, "encoder engine");
    }

    jpeg_down_sampling_type_t sub = JPEG_DOWN_SAMPLING_YUV420;
    if (s_st.sub == 1) {
        sub = JPEG_DOWN_SAMPLING_YUV422;
    } else if (s_st.sub == 2) {
        sub = JPEG_DOWN_SAMPLING_YUV444;
    }
    jpeg_encode_cfg_t cfg = {
        .height = (uint32_t)fr->height,
        .width = (uint32_t)fr->width,
        .src_type = (bpp == 2) ? JPEG_ENCODE_IN_FORMAT_RGB565 : JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = sub,
        .image_quality = s_st.quality,
        .pixel_reverse = s_st.reverse,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_encoder_process(s_st.enc, &cfg, in, in_size,
                                         s_st.out_buf, s_st.out_cap, &out_size);
    ESP_RETURN_ON_ERROR(err, TAG, "jpeg encode %dx%d", fr->width, fr->height);
    ESP_RETURN_ON_FALSE(out_size > 0, ESP_FAIL, TAG, "encode produced 0 B");

    *out = s_st.out_buf;
    *out_len = out_size;
    return ESP_OK;
}

/* 8 字节头 + 数据：**两段式直接发送**（2026-10-16，USB 视频流）
 * 不再为"头 + 大帧"分配整块 PSRAM 并 memcpy —— raw 614KB/帧 @10fps 时那是 6MB/s 的
 * 无谓 PSRAM 往返（也省掉一次 malloc/free 抖动）。 */
static esp_err_t stream_send(const img_frame_t *fr, int fmt, const uint8_t *data, uint32_t len)
{
    static uint8_t hdr[APP_STREAM_HDR_LEN];
    hdr[0] = (uint8_t)fmt;
    hdr[1] = 0;
    hdr[2] = (uint8_t)(fr->width & 0xFF);
    hdr[3] = (uint8_t)((fr->width >> 8) & 0xFF);
    hdr[4] = (uint8_t)(fr->height & 0xFF);
    hdr[5] = (uint8_t)((fr->height >> 8) & 0xFF);
    hdr[6] = (uint8_t)(s_st.seq & 0xFF);
    hdr[7] = (uint8_t)((s_st.seq >> 8) & 0xFF);

    return app_usb_link_send2(JCL_TYPE_IMAGE, hdr, APP_STREAM_HDR_LEN, data, len);
}

static void stream_task(void *arg)
{
    (void)arg;
    img_frame_t fr;

    while (1) {
        if (!s_st.enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ⚠️ 源模式守卫（2026-10-06 晚）：PC 送图模式下不取帧。0x05 回传对
         * PC 源没有意义（发送方手里就有原图），而且 stream 与 AI 在帧池里
         * 抢帧——每帧只交付一次，stream 抢到 = AI 无结果 = 该帧不回 0x02，
         * PC 端读超时。上线后从没暴露：直连调试从不开 stream；取景中枢
         * 上线后其保活把 stream 钉在开，才炸出这条竞态（经中枢跑回归实测：
         * 帧结果随机丢失，注册/判定全乱）。 */
        if (app_imgpipe_get_src() != APP_IMG_SRC_CAM) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t interval = 1000000LL / (int64_t)(s_st.fps ? s_st.fps : 1);
        if (now - s_st.last_send < interval) {
            vTaskDelay(1);              /* HZ=100 -> 10ms，够 30fps 粒度 */
            continue;
        }

        if (app_imgpipe_get_frame(&fr, STREAM_ACQ_TIMEOUT) != ESP_OK) {
            continue;
        }

        s_st.last_send = esp_timer_get_time();
        uint8_t *jpeg = NULL;
        uint32_t jpeg_len = 0;
        uint32_t enc_ms = 0;
        esp_err_t err = ESP_OK;
        int64_t t0 = esp_timer_get_time();

        if (s_st.fmt == APP_STREAM_FMT_RGB565) {
            /* 原始像素直传（不经 JPEG 编码），用于排查编码环节。
               2026-10-16：**直接发帧池缓冲**（发送是拷贝语义，且此处仍在 release 之前），
               不再 memcpy 到自备 raw_buf —— 省 614KB/帧 的 PSRAM 往返。 */
            jpeg = fr.data;
            jpeg_len = (uint32_t)((size_t)fr.stride * (uint32_t)fr.height);
        } else {
            err = stream_encode(&fr, &jpeg, &jpeg_len);
        }
        enc_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (err == ESP_OK) {
            err = stream_send(&fr, s_st.fmt, jpeg, jpeg_len);
        }
        app_imgpipe_release(&fr);

        if (err != ESP_OK) {
            s_st.st.drop++;
            ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        s_st.seq++;
        s_st.st.frames++;
        s_st.st.bytes = jpeg_len;
        s_st.st.enc_ms = enc_ms;
        s_st.win_cnt++;

        now = esp_timer_get_time();
        if (now - s_st.win >= 1000000) {
            s_st.st.fps = (float)s_st.win_cnt * 1000000.0f / (float)(now - s_st.win);
            s_st.win = now;
            s_st.win_cnt = 0;
            app_status_set(APP_MOD_STREAM, true, "%ux%u %.0ffps %ums %uKB",
                           (unsigned)fr.width, (unsigned)fr.height, (double)s_st.st.fps,
                           (unsigned)s_st.st.enc_ms, (unsigned)(s_st.st.bytes / 1024));
        }
    }
}

void app_stream_set_enable(bool on)
{
    if (s_st.enabled == on) {
        return;
    }
    s_st.enabled = on;
    s_st.last_send = 0;                 /* 立即发第一帧 */
    app_status_set(APP_MOD_STREAM, on, on ? "waiting frame" : "off");
    ESP_LOGW(TAG, "image stream %s (fps=%"PRIu32" q=%"PRIu32")", on ? "ON" : "OFF",
             s_st.fps, s_st.quality);
    app_usb_link_log("stream: %s fps=%"PRIu32" q=%"PRIu32, on ? "on" : "off", s_st.fps, s_st.quality);
}

bool app_stream_get_enable(void)
{
    return s_st.enabled;
}

void app_stream_set_fps(uint32_t fps)
{
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    s_st.fps = fps;
}

uint32_t app_stream_get_fps(void)
{
    return s_st.fps;
}

void app_stream_set_quality(uint32_t quality)
{
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    s_st.quality = quality;
}

uint32_t app_stream_get_quality(void)
{
    return s_st.quality;
}

void app_stream_set_format(int fmt)
{
    s_st.fmt = (fmt == APP_STREAM_FMT_RGB565) ? APP_STREAM_FMT_RGB565 : APP_STREAM_FMT_JPEG;
}

int app_stream_get_format(void)
{
    return s_st.fmt;
}

void app_stream_set_subsample(int sub)
{
    if (sub < 0) sub = 0;
    if (sub > 2) sub = 2;
    s_st.sub = sub;
}

int app_stream_get_subsample(void)
{
    return s_st.sub;
}

void app_stream_set_reverse(bool on)
{
    s_st.reverse = on;
}

bool app_stream_get_reverse(void)
{
    return s_st.reverse;
}

void app_stream_get_stats(app_stream_stats_t *out)
{
    if (out) *out = s_st.st;
}

esp_err_t app_stream_init(void)
{
    if (s_st.win) return ESP_OK;
    s_st.win = esp_timer_get_time();

    /* 钉 CPU0（与 imgpipe/faceai 同核）：get_frame 的 M2C 失效必须与随后的像素
     * 读取同核才有效——未绑核任务在两核间迁移时，另一核 L1 的陈旧行会让 swap/
     * 编码读到旧像素（实测开机窗口 JPEG 横带混拼，HANDOVER 廿一节） */
    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "stream", STREAM_TASK_STACK,
                                            NULL, STREAM_TASK_PRIO, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");

    app_status_set(APP_MOD_STREAM, true, "off");
    ESP_LOGI(TAG, "up: hw jpeg encode -> JCLINK 0x05, fps=%"PRIu32" q=%"PRIu32" (off by default)",
             s_st.fps, s_st.quality);
    return ESP_OK;
}
