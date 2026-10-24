/*
 * app_yuv2rgb.c —— YUV420(H.264) → RGB565 → imgpipe 送帧桥（5fps）
 *
 * ISP 的 V4L2_PIX_FMT_YUV420 实际是 O_UYY_E_VYY 打包布局（每行 u y y / v y y，
 * 行字节数 = w*3/2），PPA 的 YUV420 输入正好按这个布局解释（实测 delta=0）。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "linux/videodev2.h"

#include "app_camera.h"
#include "app_imgpipe.h"
#include "app_yuv2rgb.h"
#include "sdkconfig.h"

#define TAG        "yuv2rgb"
#define YUV_W      640
#define YUV_H      480
#define YUV_SIZE   ((size_t)YUV_W * YUV_H * 3 / 2)
#define RGB_SIZE   ((size_t)YUV_W * YUV_H * 2)
#define ALIGN_BYTES 128
#define TASK_STACK 4096
#define TASK_PRIO  3
#define FEED_MS    200          /* 5fps 送检 */

static struct {
    bool       armed;
    volatile bool busy;
    int64_t    last_us;
    void      *staging;
    void      *rgb;
    TaskHandle_t task;
    ppa_client_handle_t ppa;
    bool       cpu_fallback;
    uint32_t   frames;
} s;

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static inline uint16_t yuv_to_rgb565(int y, int u, int v)
{
    const int c = y - 16, d = u - 128, e = v - 128;
    int r = clamp255((298 * c + 409 * e + 128) >> 8);
    int g = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
    int b = clamp255((298 * c + 516 * d + 128) >> 8);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* PPA 不可用时的回退：按 O_UYY_E_VYY 打包布局逐像素转 */
static void convert_cpu(void)
{
    const uint8_t *src = (const uint8_t *)s.staging;
    const size_t line_bytes = (size_t)YUV_W * 3 / 2;
    uint16_t *out = (uint16_t *)s.rgb;
    for (int row = 0; row < YUV_H; row++) {
        const uint8_t *lr = src + (size_t)row * line_bytes;
        const uint8_t *er = src + (size_t)(row & ~1) * line_bytes;
        const uint8_t *orow = er + line_bytes;
        uint16_t *dst = out + (size_t)row * YUV_W;
        for (int pair = 0; pair < YUV_W / 2; pair++) {
            const int u = er[pair * 3];
            const int v = orow[pair * 3];
            dst[pair * 2] = yuv_to_rgb565(lr[pair * 3 + 1], u, v);
            dst[pair * 2 + 1] = yuv_to_rgb565(lr[pair * 3 + 2], u, v);
        }
    }
}

static bool convert_ppa(void)
{
    if (!s.ppa) {
        return false;
    }
    esp_cache_msync(s.staging, YUV_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer = s.staging;
    cfg.in.pic_w = YUV_W;
    cfg.in.pic_h = YUV_H;
    cfg.in.block_w = YUV_W;
    cfg.in.block_h = YUV_H;
    cfg.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    cfg.out.buffer = s.rgb;
    cfg.out.buffer_size = RGB_SIZE;
    cfg.out.pic_w = YUV_W;
    cfg.out.pic_h = YUV_H;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = 1.0f;
    cfg.scale_y = 1.0f;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(s.ppa, &cfg) != ESP_OK) {
        return false;
    }
    esp_cache_msync(s.rgb, RGB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return true;
}

static void yuv2rgb_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s.armed) {
            s.busy = false;
            continue;
        }
        if (!s.cpu_fallback) {
            if (!convert_ppa()) {
                ESP_LOGW(TAG, "PPA convert failed -> CPU fallback");
                s.cpu_fallback = true;
            }
        }
        if (s.cpu_fallback) {
            convert_cpu();
        }
        /* 送进 imgpipe（内部会再拷进 614KB 槽；src=pc 时返回 INVALID_STATE） */
        esp_err_t err = app_imgpipe_publish_raw((const uint8_t *)s.rgb, YUV_W, YUV_H,
                                                YUV_W * 2, IMG_FMT_RGB565);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "publish skipped: %s", esp_err_to_name(err));
        } else {
            s.frames++;
        }
        s.busy = false;
    }
}

static void yuv2rgb_frame_cb(const uint8_t *data, int w, int h, int stride, uint32_t fourcc)
{
    if (!s.armed || s.busy || !s.task) {
        return;
    }
    if (!data || fourcc != V4L2_PIX_FMT_YUV420 || w != YUV_W || h != YUV_H || stride != w) {
        return;
    }
    if (app_imgpipe_get_src() != APP_IMG_SRC_CAM) {
        return;                                  /* src=pc：回归期间停手 */
    }
    int64_t now = esp_timer_get_time();
    if (now - s.last_us < FEED_MS * 1000) {
        return;
    }
    s.last_us = now;
    s.busy = true;
    memcpy(s.staging, data, YUV_SIZE);
    xTaskNotifyGive(s.task);
}

esp_err_t app_yuv2rgb_register(void)
{
    if (s.task) {
        return ESP_OK;
    }
    s.staging = heap_caps_aligned_alloc(ALIGN_BYTES, YUV_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    s.rgb = heap_caps_aligned_alloc(ALIGN_BYTES, RGB_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s.staging || !s.rgb) {
        ESP_LOGE(TAG, "buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ppa_client_config_t pcfg = {};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = 1;
    if (ppa_register_client(&pcfg, &s.ppa) != ESP_OK) {
        ESP_LOGW(TAG, "PPA client register failed -> CPU conversion");
        s.ppa = NULL;
        s.cpu_fallback = true;
    }
    if (xTaskCreatePinnedToCore(yuv2rgb_task, "yuv2rgb", TASK_STACK, NULL, TASK_PRIO,
                                &s.task, 1) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(app_camera_add_frame_cb(yuv2rgb_frame_cb), TAG, "frame cb");
    ESP_LOGI(TAG, "registered %dx%d -> RGB565, feed %d ms, convert=%s",
             YUV_W, YUV_H, FEED_MS, s.cpu_fallback ? "cpu" : "ppa");
    return ESP_OK;
}

void app_yuv2rgb_start(void)
{
    s.armed = true;
    ESP_LOGI(TAG, "started (imgpipe hflip/WB/src 已在相机启动前配置)");
}
