/*
 * app_ai_detect.cpp —— YUV420/H.264 并行的人脸检测（检测框，不做识别）
 *
 * 数据通路：camera_task 帧回调（QBUF 前）→ memcpy 460KB 到 PSRAM staging →
 * AI 任务 PPA YUV420→RGB565（或 CPU 回退）→ esp-dl MSRMNP → 0x02。
 * 检测按 ~5fps 节流，AI 忙时直接丢帧，不阻塞相机任务；不挂 /storage。
 */
#include <cstring>
#include <cstdlib>
#include <new>
#include <list>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "linux/videodev2.h"
#include "cJSON.h"

#include "app_ai_detect.h"
#include "app_camera.h"
#include "app_usb_link.h"

#include "human_face_detect.hpp"
#include "dl_image_define.hpp"
#include "sdkconfig.h"

#ifndef CONFIG_APP_AI_DETECT_FPS
#define CONFIG_APP_AI_DETECT_FPS 5
#endif
#ifndef CONFIG_APP_AI_DETECT_SCORE
#define CONFIG_APP_AI_DETECT_SCORE 50
#endif
#ifndef CONFIG_APP_AI_DETECT_CONVERT_PPA
#define CONFIG_APP_AI_DETECT_CONVERT_PPA 1
#endif

#define TAG "ai_detect"

#define AI_W        640
#define AI_H        480
#define AI_YUV_SIZE ((size_t)AI_W * AI_H * 3 / 2)
#define AI_RGB_SIZE ((size_t)AI_W * AI_H * 2)
#define AI_ALIGN    128
#define AI_STACK    8192
#define AI_PRIO     3
#define AI_LOG_US   (5 * 1000 * 1000)

static struct {
    bool       enabled;
    volatile bool busy;
    volatile int64_t last_us;
    int        period_us;
    int        fps;
    int        frames;
    int        drops;
    int64_t    fps_t0;
    int64_t    last_log_us;
    void      *staging;
    void      *rgb;
    TaskHandle_t task;
    ppa_client_handle_t ppa;
    bool       convert_cpu;
    bool       probe_done;
    bool       model_ok;
    volatile bool dump_req;
} s = {
    .period_us = 1000000 / CONFIG_APP_AI_DETECT_FPS,
    .fps = CONFIG_APP_AI_DETECT_FPS,
};

static HumanFaceDetect *s_detect;   /* 在 AI 任务里加载 */
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

/* ISP 的 V4L2_PIX_FMT_YUV420 实际是 O_UYY_E_VYY 打包布局
 * （每行 u y y u y y…，偶数行 U、奇数行 V；行字节数 = w*3/2），
 * 不是平面 YU12。硬编按这个布局吃，CPU 也必须按这个布局转。 */
static void convert_cpu(void)
{
    const uint8_t *src = (const uint8_t *)s.staging;
    const size_t line_bytes = (size_t)AI_W * 3 / 2;
    uint16_t *out = (uint16_t *)s.rgb;
    for (int row = 0; row < AI_H; row++) {
        const uint8_t *lr = src + (size_t)row * line_bytes;
        const uint8_t *er = src + (size_t)(row & ~1) * line_bytes;
        const uint8_t *orow = er + line_bytes;
        uint16_t *dst = out + (size_t)row * AI_W;
        for (int pair = 0; pair < AI_W / 2; pair++) {
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
    esp_cache_msync(s.staging, AI_YUV_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer = s.staging;
    cfg.in.pic_w = AI_W;
    cfg.in.pic_h = AI_H;
    cfg.in.block_w = AI_W;
    cfg.in.block_h = AI_H;
    cfg.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    cfg.out.buffer = s.rgb;
    cfg.out.buffer_size = AI_RGB_SIZE;
    cfg.out.pic_w = AI_W;
    cfg.out.pic_h = AI_H;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = 1.0f;
    cfg.scale_y = 1.0f;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(s.ppa, &cfg) != ESP_OK) {
        return false;
    }
    esp_cache_msync(s.rgb, AI_RGB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return true;
}

static uint16_t packed_px(const uint8_t *src, int x, int y)
{
    const size_t line_bytes = (size_t)AI_W * 3 / 2;
    const int pair = x / 2;
    const uint8_t *lr = src + (size_t)y * line_bytes;
    const uint8_t *er = src + (size_t)(y & ~1) * line_bytes;
    const uint8_t *orow = er + line_bytes;
    return yuv_to_rgb565(lr[pair * 3 + 1 + (x & 1)], er[pair * 3], orow[pair * 3]);
}

/* PPA YUV420 输入布局校验：与打包布局的 CPU 结果比对 4 个采样点。
 * 布局不符时颜色会离谱 → 永久切 CPU 转换。 */
static void probe_color(void)
{
    const uint8_t *src = (const uint8_t *)s.staging;
    const uint16_t *rgb = (const uint16_t *)s.rgb;
    static const int pts[4][2] = { { 80, 80 }, { 80, 560 }, { 400, 80 }, { 400, 560 } };
    int worst = 0;
    for (int i = 0; i < 4; i++) {
        int py = pts[i][0], px = pts[i][1];
        uint16_t got = rgb[(size_t)py * AI_W + px];
        uint16_t exp = packed_px(src, px, py);
        int dr = abs((int)((got >> 11) & 0x1F) - (int)((exp >> 11) & 0x1F)) * 8;
        int dg = abs((int)((got >> 5) & 0x3F) - (int)((exp >> 5) & 0x3F)) * 4;
        int db = abs((int)(got & 0x1F) - (int)(exp & 0x1F)) * 8;
        int d = dr > dg ? dr : dg;
        if (db > d) d = db;
        if (d > worst) worst = d;
    }
    ESP_LOGI(TAG, "PPA color probe: max channel delta=%d", worst);
    if (worst > 48) {
        s.convert_cpu = true;
        ESP_LOGW(TAG, "PPA YUV420 layout mismatch -> CPU YUV->RGB565 fallback");
    }
}

/* 调试 dump：4x 降采样成 160x120（38KB），避免 614KB 帧把 USB TX 占满
   （实测会拖到 H.264 发送超时）。先发 Y 灰度（验证 staging 拷贝），
   再发转换后的彩色（验证 YUV 布局）。 */
static void dump_small(const uint16_t *src, int src_stride, int step, const char *tag)
{
    const int dw = 160, dh = 120;
    const size_t bytes = (size_t)dw * dh * 2;
    uint8_t hdr[8] = { 1, 0, (uint8_t)(dw & 0xFF), (uint8_t)(dw >> 8),
                       (uint8_t)(dh & 0xFF), (uint8_t)(dh >> 8), 0, 0 };
    uint16_t *small = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!small) {
        ESP_LOGW(TAG, "dump: no mem");
        return;
    }
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            small[y * dw + x] = src[(size_t)(y * step) * src_stride + x * step];
        }
    }
    esp_cache_msync(small, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_err_t err = app_usb_link_send2(JCL_TYPE_IMAGE, hdr, sizeof(hdr), small, (uint32_t)bytes);
    ESP_LOGI(TAG, "dump %s: %dx%d sent %u B (%s)", tag, dw, dh, (unsigned)bytes,
             esp_err_to_name(err));
    heap_caps_free(small);
}

static void dump_frame(void)
{
    const int dw = 160, dh = 120, step = 4;
    const size_t bytes = (size_t)dw * dh * 2;
    uint16_t *gray = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray) {
        const uint8_t *y = (const uint8_t *)s.staging;
        const size_t line_bytes = (size_t)AI_W * 3 / 2;
        for (int r = 0; r < dh; r++) {
            const uint8_t *lr = y + (size_t)(r * step) * line_bytes;
            for (int c = 0; c < dw; c++) {
                int v = lr[(c * step / 2) * 3 + 1 + ((c * step) & 1)];
                gray[r * dw + c] = yuv_to_rgb565(v, 128, 128);
            }
        }
        dump_small(gray, dw, 1, "Y-gray");
        heap_caps_free(gray);
    }
    vTaskDelay(pdMS_TO_TICKS(30));    /* 两帧之间给 USB TX 一点喘息 */
    dump_small((const uint16_t *)s.rgb, AI_W, 4, "RGB");
}

static void send_result(const std::list<dl::detect::result_t> &res, int infer_ms, int conv_ms)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddNumberToObject(root, "faces", (int)res.size());
    cJSON *boxes = cJSON_CreateArray();
    if (boxes) {
        for (const auto &r : res) {
            if (r.box.size() < 4) {
                continue;
            }
            cJSON *b = cJSON_CreateArray();
            if (!b) {
                break;
            }
            for (int k = 0; k < 4; k++) {
                cJSON_AddItemToArray(b, cJSON_CreateNumber(r.box[k]));
            }
            cJSON_AddItemToArray(boxes, b);
        }
        cJSON_AddItemToObject(root, "boxes", boxes);
    }
    cJSON_AddNumberToObject(root, "infer_ms", infer_ms);
    cJSON_AddNumberToObject(root, "conv_ms", conv_ms);
    cJSON_AddNumberToObject(root, "drops", s.drops);
    cJSON_AddNumberToObject(root, "fps", s.fps);
    cJSON_AddNumberToObject(root, "detect", 1);
    cJSON_AddStringToObject(root, "event", "none");
    cJSON_AddStringToObject(root, "src", "cam");
    char *js = cJSON_PrintUnformatted(root);
    if (js) {
        app_usb_link_send(JCL_TYPE_RESULT, js, (uint32_t)strlen(js));
        cJSON_free(js);
    }
    cJSON_Delete(root);
}

static void ai_task(void *arg)
{
    (void)arg;
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "model load: internal free=%u B", (unsigned)free_int);
    if (free_int < 64 * 1024) {
        ESP_LOGW(TAG, "internal RAM too low for model -> detection disabled");
    } else {
        s_detect = new (std::nothrow) HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
        if (!s_detect) {
            ESP_LOGW(TAG, "detector alloc failed -> detection disabled");
        } else {
            s_detect->set_score_thr(CONFIG_APP_AI_DETECT_SCORE / 100.0f);
            s.model_ok = true;
            ESP_LOGI(TAG, "model loaded (score=%.2f), heap int=%u largest=%u",
                     CONFIG_APP_AI_DETECT_SCORE / 100.0f,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
    s.fps_t0 = esp_timer_get_time();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s.enabled || !s.model_ok) {
            s.busy = false;
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        int64_t tc = t0;
        if (!s.convert_cpu && !convert_ppa()) {
            ESP_LOGW(TAG, "PPA convert failed -> CPU fallback");
            s.convert_cpu = true;
        }
        if (s.convert_cpu) {
            convert_cpu();
        }
        int conv_ms = (int)((esp_timer_get_time() - tc) / 1000);
        (void)t0;
        if (!s.probe_done) {
            s.probe_done = true;
            probe_color();
            if (s.convert_cpu) {
                convert_cpu();        /* 校验失败的那帧重转一次再送模型 */
            }
        }
        if (s.dump_req) {
            s.dump_req = false;
            dump_frame();
        }
        dl::image::img_t img = {};
        img.data = s.rgb;
        img.width = AI_W;
        img.height = AI_H;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
        int64_t ti = esp_timer_get_time();
        std::list<dl::detect::result_t> &res = s_detect->run(img);
        int infer_ms = (int)((esp_timer_get_time() - ti) / 1000);
        send_result(res, infer_ms, conv_ms);
        s.frames++;
        s.busy = false;

        int64_t now = esp_timer_get_time();
        if (now - s.fps_t0 >= AI_LOG_US) {
            s.fps = s.fps_t0 ? (int)((int64_t)s.frames * 1000000 / (now - s.fps_t0)) : 0;
            ESP_LOGI(TAG, "detect %d fps, faces=%d infer=%dms drops=%d heap_int=%u",
                     s.fps, (int)res.size(), infer_ms, s.drops,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s.frames = 0;
            s.fps_t0 = now;
        }
    }
}

static void ai_frame_cb(const uint8_t *data, int w, int h, int stride, uint32_t fourcc)
{
    if (!s.enabled || !s.task || !s.model_ok) {
        return;
    }
    if (s.busy) {
        s.drops++;
        return;
    }
    if (!data || fourcc != V4L2_PIX_FMT_YUV420 || w != AI_W || h != AI_H || stride != w) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (now - s.last_us < s.period_us) {
        return;
    }
    s.last_us = now;
    s.busy = true;
    memcpy(s.staging, data, AI_YUV_SIZE);
    xTaskNotifyGive(s.task);
}

void app_ai_detect_register(void)
{
    if (s.task) {
        return;
    }
    s.enabled = true;
    s.period_us = 1000000 / (CONFIG_APP_AI_DETECT_FPS > 0 ? CONFIG_APP_AI_DETECT_FPS : 5);
    s.staging = heap_caps_aligned_alloc(AI_ALIGN, AI_YUV_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    s.rgb = heap_caps_aligned_alloc(AI_ALIGN, AI_RGB_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s.staging || !s.rgb) {
        ESP_LOGE(TAG, "buffer alloc failed (staging=%p rgb=%p)", s.staging, s.rgb);
        s.enabled = false;
        return;
    }
#if CONFIG_APP_AI_DETECT_CONVERT_PPA
    ppa_client_config_t pcfg = {};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = 1;
    if (ppa_register_client(&pcfg, &s.ppa) != ESP_OK) {
        ESP_LOGW(TAG, "PPA client register failed -> CPU conversion");
        s.ppa = NULL;
        s.convert_cpu = true;
    }
#else
    s.convert_cpu = true;
#endif
    BaseType_t ok = xTaskCreatePinnedToCore(ai_task, "ai_detect", AI_STACK, NULL, AI_PRIO,
                                            &s.task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s.enabled = false;
        s.task = NULL;
        return;
    }
    esp_err_t err = app_camera_add_frame_cb(ai_frame_cb);
    ESP_LOGI(TAG, "registered: %dx%d, target %d fps, convert=%s",
             AI_W, AI_H, CONFIG_APP_AI_DETECT_FPS, s.convert_cpu ? "cpu" : "ppa");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera frame cb register failed (%s)", esp_err_to_name(err));
        s.enabled = false;
    }
}

void app_ai_detect_set_enabled(bool on)
{
    s.enabled = on;
    ESP_LOGW(TAG, "detection -> %s", on ? "ON" : "OFF");
}

bool app_ai_detect_enabled(void)
{
    return s.enabled;
}

int app_ai_detect_fps(void)
{
    return CONFIG_APP_AI_DETECT_FPS;
}

void app_ai_detect_request_dump(void)
{
    s.dump_req = true;
}
