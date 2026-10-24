/*
 * app_h264enc.c —— ESP32-P4 硬件 H.264 编码支路（M0 验证）
 *
 *   camera_task（CPU1）同步回调 -> /dev/video11 M2M OUTPUT(USERPTR 零拷贝)
 *   -> 硬编 -> CAPTURE mmap 取 NAL（含 SPS/PPS）-> /sdcard/h264_*.h264
 *
 * M0 只回答 4 个问题：ISP 能否出 YUV420、码流能否解码、三档 bitrate/QP 的
 * 实际码率与单帧长度、单帧编码耗时。编码留在 camera 回调（≈5ms），SD/链路等
 * 慢 I/O 全部交给低优先级 sender_task；每个访问单元按 JCLINK 0x07 发给
 * pc_host/h264_capture.py（send stream_id=阶段号），可选同时落 SD。
 *
 * ⚠️ esp_video H.264 设备的硬约束（见 esp_video_h264_device.c）：
 *   - 必须先 S_FMT(OUTPUT) 再 S_FMT(CAPTURE)，否则 CAPTURE 宽高被写成 0
 *   - 四个控件必须在 STREAMON(CAPTURE) 前设置（编码器此时才创建）
 *   - I_PERIOD 同时被当作编码器 fps
 *   - 输入格式只认 V4L2_PIX_FMT_YUV420 -> O_UYY_E_VYY，stride == width
 *   - qp_min == qp_max 会静默关闭码率控制
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "sdkconfig.h"
#include "cJSON.h"

#include "app_camera.h"
#include "app_h264enc.h"
#include "app_usb_link.h"
#include "app_camauto.h"
#include "app_ai_detect.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#if CONFIG_APP_CAM_OUT_FMT_YUV420

#define TAG             "h264enc"
#define H264_DEV        ESP_VIDEO_H264_DEVICE_NAME      /* "/dev/video11" */
#define H264_ALIGN      64
#define PHASE_US        ((int64_t)CONFIG_APP_H264_DUMP_SECONDS * 1000000LL)
#define FEED_INTERVAL   (1000000LL / CONFIG_APP_H264_FEED_FPS)
#define LINK_Q_DEPTH    16
#define AU_HDR_LEN      20      /* 0x07 载荷头，见 app_usb_link.h */

typedef struct {
    uint8_t *data;          /* PSRAM，发送任务负责 free */
    uint32_t len;
    uint32_t pts_us;
    uint32_t frame_seq;
    uint16_t stream_id;
    bool     key;
} h264_pkt_t;

typedef struct {
    uint32_t    bitrate;
    uint8_t     qp_min;
    uint8_t     qp_max;
    uint8_t     gop;
    const char *tag;
} h264_phase_t;

/* M0 参数表：同一 GOP 下比码率；M2 默认只跑单档稳定流 */
#if CONFIG_APP_H264_SWEEP
static const h264_phase_t s_phases[] = {
    {  500000, 25, 35, CONFIG_APP_H264_GOP, "b500_qp25-35"  },
    { 1000000, 25, 30, CONFIG_APP_H264_GOP, "b1000_qp25-30" },
    { 2000000, 20, 30, CONFIG_APP_H264_GOP, "b2000_qp20-30" },
};
#else
static const h264_phase_t s_phases[] = {
    { CONFIG_APP_H264_LIVE_BITRATE, CONFIG_APP_H264_LIVE_QP_MIN,
      CONFIG_APP_H264_LIVE_QP_MAX, CONFIG_APP_H264_LIVE_GOP, "live" },
};
#endif
#define PHASE_COUNT (sizeof(s_phases) / sizeof(s_phases[0]))

static struct {
    bool     registered;
    bool     done;
    int      fd;
    int      w, h;
    uint8_t *cap_buf;
    size_t   cap_len;
    FILE    *fp;
    int      phase;
    int64_t  phase_start;
    int64_t  next_frame;
    int64_t  win_start;
    int64_t  last_stats;
    /* 阶段累计 */
    uint32_t frames;
    uint32_t errors;
    uint32_t idr;
    uint32_t enc_sum_us;
    uint32_t enc_max_us;
    uint64_t bytes;
    /* 1s 窗口 */
    uint32_t win_frames;
    uint32_t win_enc_sum;
    uint32_t win_enc_max;
    uint64_t win_bytes;
    bool     warned_fmt;
    bool     warned_stride;
    bool     warned_align;
    bool     phase_y_logged;
    QueueHandle_t     q;
    SemaphoreHandle_t fp_mux;
    bool     sd_ok;
    uint32_t seq;               /* 本 phase 内的码流帧号 */
    uint32_t link_sent;
    uint32_t link_fail;
    uint32_t q_full;
    /* M2 运行时参数 */
    volatile bool enabled;
    volatile bool cfg_dirty;
    portMUX_TYPE cfg_mux;
    uint32_t cfg_bitrate;
    uint8_t  cfg_qp_min;
    uint8_t  cfg_qp_max;
    uint8_t  cfg_gop;
} s_h = { .fd = -1, .phase = -1 };

/* 抽样 Y 均值（O_UYY_E_VYY 打包布局：每行 u y y / v y y，行字节 = w*3/2） */
static uint32_t y_plane_mean(const uint8_t *p, int w, int h)
{
    uint64_t sum = 0;
    uint32_t n = 0;
    const size_t line_bytes = (size_t)w * 3 / 2;
    for (int y = 0; y < h; y += 8) {
        const uint8_t *row = p + (size_t)y * line_bytes;
        for (int pair = 0; pair < w / 2; pair += 4) {
            sum += row[pair * 3 + 1] + row[pair * 3 + 2];
            n += 2;
        }
    }
    return n ? (uint32_t)(sum / n) : 0;
}

static bool nal_has_idr(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1f) == 5) {
                return true;
            }
            i += 3;
        }
    }
    return false;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* 阶段切换前等队列排空，保证旧 stream_id 的包写进旧文件/旧流 */
static void q_drain(void)
{
    if (!s_h.q) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    while (uxQueueMessagesWaiting(s_h.q) > 0 &&
            esp_timer_get_time() - t0 < 1500000LL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* 低优先级发送任务：SD 写卡 + JCLINK 0x07 都在这里，camera 回调只编码/入队 */
static void sender_task(void *arg)
{
    (void)arg;
    /* 预热本任务的 newlib stdio 锁（失败日志在运行期才打，届时堆已碎片化） */
    ESP_LOGI(TAG, "sender task started");
    h264_pkt_t pkt;
    for (;;) {
        if (xQueueReceive(s_h.q, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
#if CONFIG_APP_H264_SINK_SD
        if (s_h.fp_mux) {
            xSemaphoreTake(s_h.fp_mux, portMAX_DELAY);
        }
        if (s_h.fp) {
            if (fwrite(pkt.data, 1, pkt.len, s_h.fp) != pkt.len) {
                ESP_LOGE(TAG, "fwrite failed: errno %d", errno);
                fclose(s_h.fp);
                s_h.fp = NULL;
                s_h.sd_ok = false;
            } else if (pkt.key || (pkt.frame_seq % 30) == 0) {
                fflush(s_h.fp);
            }
        }
        if (s_h.fp_mux) {
            xSemaphoreGive(s_h.fp_mux);
        }
#endif
#if CONFIG_APP_H264_SINK_LINK
        uint8_t hdr[AU_HDR_LEN] = { 0 };
        hdr[0] = 1;                                             /* codec: H.264 */
        hdr[1] = (uint8_t)((pkt.key ? 0x03 : 0x00) | 0x04);     /* key|spspps|last_frag */
        put16(hdr + 2, pkt.stream_id);
        put32(hdr + 4, pkt.pts_us);
        put32(hdr + 8, pkt.frame_seq);
        put16(hdr + 12, (uint16_t)s_h.w);
        put16(hdr + 14, (uint16_t)s_h.h);
        put32(hdr + 16, pkt.len);
        if (app_usb_link_send2(JCL_TYPE_H264, hdr, AU_HDR_LEN, pkt.data, pkt.len) == ESP_OK) {
            s_h.link_sent++;
        } else {
            s_h.link_fail++;
        }
#endif
        heap_caps_free(pkt.data);
    }
}

static void emit_packet(const uint8_t *data, uint32_t len, bool key, uint32_t pts_us)
{
    if (!s_h.q) {
        return;
    }
    h264_pkt_t pkt = {
        .len = len,
        .pts_us = pts_us,
        .frame_seq = ++s_h.seq,
        .stream_id = (uint16_t)(s_h.phase + 1),
        .key = key,
    };
    pkt.data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pkt.data) {
        s_h.errors++;
        return;
    }
    memcpy(pkt.data, data, len);
    if (xQueueSend(s_h.q, &pkt, 0) != pdTRUE) {
        heap_caps_free(pkt.data);
        s_h.q_full++;
    }
}

static void enc_close(void)
{
    int type;
    if (s_h.fd >= 0) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_h.fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(s_h.fd, VIDIOC_STREAMOFF, &type);
        if (s_h.cap_buf) {
            munmap(s_h.cap_buf, s_h.cap_len);
        }
        close(s_h.fd);
    }
    q_drain();
#if CONFIG_APP_H264_SINK_SD
    if (s_h.fp_mux) {
        xSemaphoreTake(s_h.fp_mux, portMAX_DELAY);
    }
    if (s_h.fp) {
        fflush(s_h.fp);
        fclose(s_h.fp);
    }
    s_h.fp = NULL;
    s_h.sd_ok = false;
    if (s_h.fp_mux) {
        xSemaphoreGive(s_h.fp_mux);
    }
#endif
    s_h.fd = -1;
    s_h.cap_buf = NULL;
    s_h.cap_len = 0;
}

static esp_err_t enc_open(int w, int h, const h264_phase_t *p)
{
    char path[96];

    s_h.w = w;
    s_h.h = h;
    s_h.fd = open(H264_DEV, O_RDWR);
    if (s_h.fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno %d (H.264 device enabled in sdkconfig?)",
                 H264_DEV, errno);
        return ESP_ERR_NOT_FOUND;
    }

    /* 1) OUTPUT 必须最先设：CAPTURE 的宽高从 OUTPUT 继承 */
    struct v4l2_format fo = { 0 };
    fo.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fo.fmt.pix.width = w;
    fo.fmt.pix.height = h;
    fo.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    if (ioctl(s_h.fd, VIDIOC_S_FMT, &fo) != 0) {
        ESP_LOGE(TAG, "S_FMT(OUTPUT,YUV420) failed: errno %d", errno);
        goto fail;
    }

    struct v4l2_requestbuffers rqo = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(s_h.fd, VIDIOC_REQBUFS, &rqo) != 0) {
        ESP_LOGE(TAG, "REQBUFS(OUTPUT) failed: errno %d", errno);
        goto fail;
    }

    /* 2) CAPTURE：H.264 Annex-B，缓冲固定 4*W*H（由 esp_video 在 PSRAM 分配） */
    struct v4l2_format fc = { 0 };
    fc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fc.fmt.pix.width = w;
    fc.fmt.pix.height = h;
    fc.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    if (ioctl(s_h.fd, VIDIOC_S_FMT, &fc) != 0) {
        ESP_LOGE(TAG, "S_FMT(CAPTURE,H264) failed: errno %d", errno);
        goto fail;
    }

    struct v4l2_requestbuffers rqc = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_h.fd, VIDIOC_REQBUFS, &rqc) != 0) {
        ESP_LOGE(TAG, "REQBUFS(CAPTURE) failed: errno %d", errno);
        goto fail;
    }

    struct v4l2_buffer cb = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = 0,
    };
    if (ioctl(s_h.fd, VIDIOC_QUERYBUF, &cb) != 0) {
        ESP_LOGE(TAG, "QUERYBUF(CAPTURE) failed: errno %d", errno);
        goto fail;
    }
    s_h.cap_buf = mmap(NULL, cb.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_h.fd, cb.m.offset);
    if (s_h.cap_buf == MAP_FAILED) {
        s_h.cap_buf = NULL;
        ESP_LOGE(TAG, "mmap(CAPTURE) failed: errno %d", errno);
        goto fail;
    }
    s_h.cap_len = cb.length;
    if (ioctl(s_h.fd, VIDIOC_QBUF, &cb) != 0) {
        ESP_LOGE(TAG, "QBUF(CAPTURE) failed: errno %d", errno);
        goto fail;
    }

    /* 3) 四个控件必须在 STREAMON(CAPTURE) 之前 */
    struct v4l2_ext_control ctl[4] = {
        { .id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, .value = p->gop },
        { .id = V4L2_CID_MPEG_VIDEO_BITRATE,       .value = (int32_t)p->bitrate },
        { .id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP,   .value = p->qp_min },
        { .id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP,   .value = p->qp_max },
    };
    struct v4l2_ext_controls ctls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 4,
        .controls = ctl,
    };
    if (ioctl(s_h.fd, VIDIOC_S_EXT_CTRLS, &ctls) != 0) {
        ESP_LOGE(TAG, "S_EXT_CTRLS failed: errno %d", errno);
        goto fail;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_h.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON(CAPTURE) failed: errno %d", errno);
        goto fail;
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(s_h.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON(OUTPUT) failed: errno %d", errno);
        goto fail;
    }

#if CONFIG_APP_H264_SINK_SD
    snprintf(path, sizeof(path), "/sdcard/h264_%s.h264", p->tag);
    if (s_h.fp_mux) {
        xSemaphoreTake(s_h.fp_mux, portMAX_DELAY);
    }
    s_h.fp = fopen(path, "wb");
    s_h.sd_ok = (s_h.fp != NULL);
    if (s_h.fp_mux) {
        xSemaphoreGive(s_h.fp_mux);
    }
    if (s_h.sd_ok) {
        ESP_LOGI(TAG, "phase %d/%u %" PRIu32 "bps qp%u-%u gop%u -> %s (cap buf %u B)",
                 s_h.phase + 1, (unsigned)PHASE_COUNT, p->bitrate, p->qp_min, p->qp_max,
                 p->gop, path, (unsigned)s_h.cap_len);
    } else {
        ESP_LOGW(TAG, "fopen(%s) failed: SD unavailable -> link only", path);
    }
#else
    (void)path;
    ESP_LOGI(TAG, "phase %d/%u %" PRIu32 "bps qp%u-%u gop%u -> JCLINK 0x07 (cap buf %u B)",
             s_h.phase + 1, (unsigned)PHASE_COUNT, p->bitrate, p->qp_min, p->qp_max,
             p->gop, (unsigned)s_h.cap_len);
#endif
    return ESP_OK;

fail:
    enc_close();
    return ESP_FAIL;
}

static void phase_report(void)
{
    int64_t dur = esp_timer_get_time() - s_h.phase_start;
    uint32_t fps10 = dur > 0 ? (uint32_t)((uint64_t)s_h.frames * 10000000ULL / (uint64_t)dur) : 0;
    uint32_t kbps = dur > 0 ? (uint32_t)(s_h.bytes * 8 * 1000000ULL / (uint64_t)dur / 1000ULL) : 0;
    uint32_t enc_avg = s_h.frames ? s_h.enc_sum_us / s_h.frames : 0;

    ESP_LOGI(TAG, "phase %d done: frames=%" PRIu32 " bytes=%" PRIu64 " (%" PRIu32 " kbps) "
             "avg=%" PRIu32 "B fps=%" PRIu32 ".%" PRIu32 " enc_avg=%" PRIu32 "us "
             "enc_max=%" PRIu32 "us idr=%" PRIu32 " err=%" PRIu32 " sink=%s link=%" PRIu32
             "/%" PRIu32 " qfull=%" PRIu32,
             s_h.phase + 1, s_h.frames, s_h.bytes, kbps,
             s_h.frames ? (uint32_t)(s_h.bytes / s_h.frames) : 0,
             fps10 / 10, fps10 % 10, enc_avg, s_h.enc_max_us,
             s_h.idr, s_h.errors, s_h.sd_ok ? "sd" : "link",
             s_h.link_sent, s_h.link_fail, s_h.q_full);
    /* CPU 占空比（编码同步跑在 camera_task 里，单核口径）：enc_ms * fps / 10 */
    if (fps10) {
        ESP_LOGI(TAG, "phase %d cpu: enc_duty=%" PRIu32 "%% of one core",
                 s_h.phase + 1, (uint32_t)((uint64_t)enc_avg * fps10 / 100000ULL));
    }
}

static void phase_reset_stats(void)
{
    s_h.frames = 0;
    s_h.errors = 0;
    s_h.idr = 0;
    s_h.enc_sum_us = 0;
    s_h.enc_max_us = 0;
    s_h.bytes = 0;
    s_h.win_frames = 0;
    s_h.win_enc_sum = 0;
    s_h.win_enc_max = 0;
    s_h.win_bytes = 0;
    s_h.phase_y_logged = false;
    s_h.seq = 0;
    s_h.link_sent = 0;
    s_h.link_fail = 0;
    s_h.q_full = 0;
}

static void phase_start(int idx, int w, int h, int64_t now)
{
    phase_reset_stats();
    s_h.phase = idx;
    s_h.phase_start = now;
    s_h.win_start = now;
    s_h.last_stats = now;
    s_h.next_frame = now;
#if CONFIG_APP_H264_SWEEP
    portENTER_CRITICAL(&s_h.cfg_mux);
    s_h.cfg_bitrate = s_phases[idx].bitrate;
    s_h.cfg_qp_min = s_phases[idx].qp_min;
    s_h.cfg_qp_max = s_phases[idx].qp_max;
    s_h.cfg_gop = s_phases[idx].gop;
    s_h.cfg_dirty = false;
    portEXIT_CRITICAL(&s_h.cfg_mux);
#endif
    h264_phase_t cfg = {
        .bitrate = s_h.cfg_bitrate,
        .qp_min = s_h.cfg_qp_min,
        .qp_max = s_h.cfg_qp_max,
        .gop = s_h.cfg_gop,
        .tag = s_phases[idx].tag,
    };
    if (enc_open(w, h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "phase %d open failed -> M0 aborted", idx + 1);
        s_h.done = true;
        s_h.phase = -1;
    }
}

/* 运行时参数变化：关流 → 用新参数重建（esp_video 的 4 个控件只在 STREAMON 生效） */
static void apply_cfg(int w, int h)
{
    h264_phase_t cfg;
    portENTER_CRITICAL(&s_h.cfg_mux);
    cfg = (h264_phase_t){
        .bitrate = s_h.cfg_bitrate,
        .qp_min = s_h.cfg_qp_min,
        .qp_max = s_h.cfg_qp_max,
        .gop = s_h.cfg_gop,
        .tag = "live",
    };
    s_h.cfg_dirty = false;
    portEXIT_CRITICAL(&s_h.cfg_mux);

    ESP_LOGI(TAG, "apply params: %" PRIu32 "bps qp%u-%u gop%u",
             cfg.bitrate, cfg.qp_min, cfg.qp_max, cfg.gop);
    if (s_h.fd >= 0) {
        phase_report();
        enc_close();
    }
    phase_reset_stats();
    int64_t now = esp_timer_get_time();
    s_h.phase = 0;
    s_h.phase_start = now;
    s_h.win_start = now;
    s_h.last_stats = now;
    s_h.next_frame = now;
    if (enc_open(w, h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "reopen with new params failed -> stream off");
        s_h.enabled = false;
    }
}

#if CONFIG_APP_H264_SWEEP
static void phase_next(int w, int h, int64_t now)
{
    phase_report();
    enc_close();
    if (s_h.phase + 1 < (int)PHASE_COUNT) {
        phase_start(s_h.phase + 1, w, h, now);
    } else if (CONFIG_APP_H264_LOOP) {
        ESP_LOGI(TAG, "sweep loop: back to phase 1");
        phase_start(0, w, h, now);
    } else {
        s_h.done = true;
        ESP_LOGI(TAG, "M0 sweep complete (%u phases). Camera keeps streaming without encoding.",
                 (unsigned)PHASE_COUNT);
    }
}
#endif

static void encode_one(const uint8_t *src, int w, int h, int64_t now)
{
    size_t need = (size_t)w * h * 3 / 2;
    int64_t t0 = esp_timer_get_time();

    struct v4l2_buffer ob = {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ob.m.userptr = (unsigned long)src;
    ob.length = (uint32_t)need;
    if (ioctl(s_h.fd, VIDIOC_QBUF, &ob) != 0) {
        ESP_LOGW(TAG, "QBUF(OUTPUT) failed: errno %d", errno);
        s_h.errors++;
        return;
    }

    struct v4l2_buffer cb = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_h.fd, VIDIOC_DQBUF, &cb) != 0) {
        ESP_LOGW(TAG, "DQBUF(CAPTURE) failed: errno %d", errno);
        s_h.errors++;
        /* 输入缓冲已入队，不回收的话下一帧 QBUF 会一直 EBUSY */
        struct v4l2_buffer tmp = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR };
        ioctl(s_h.fd, VIDIOC_DQBUF, &tmp);
        return;
    }
    uint32_t enc_us = (uint32_t)(esp_timer_get_time() - t0);

    bool ok = !(cb.flags & V4L2_BUF_FLAG_ERROR) && cb.bytesused > 0;
    if (ok) {
        bool idr = nal_has_idr(s_h.cap_buf, cb.bytesused);
        s_h.frames++;
        s_h.bytes += cb.bytesused;
        s_h.enc_sum_us += enc_us;
        if (enc_us > s_h.enc_max_us) {
            s_h.enc_max_us = enc_us;
        }
        s_h.win_frames++;
        s_h.win_bytes += cb.bytesused;
        s_h.win_enc_sum += enc_us;
        if (enc_us > s_h.win_enc_max) {
            s_h.win_enc_max = enc_us;
        }
        if (idr) {
            s_h.idr++;
        }
#if CONFIG_APP_H264_SINK_LINK || CONFIG_APP_H264_SINK_SD
        /* 只拷贝入队，SD/链路 I/O 全部交给 sender_task */
        emit_packet(s_h.cap_buf, cb.bytesused, idr, (uint32_t)t0);
#endif
    } else {
        s_h.errors++;
        ESP_LOGW(TAG, "encode error: flags=0x%08" PRIx32 " bytesused=%" PRIu32,
                 (uint32_t)cb.flags, cb.bytesused);
    }

    if (ioctl(s_h.fd, VIDIOC_QBUF, &cb) != 0) {
        ESP_LOGW(TAG, "QBUF(CAPTURE) failed: errno %d", errno);
    }
    struct v4l2_buffer done_out = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(s_h.fd, VIDIOC_DQBUF, &done_out) != 0) {
        ESP_LOGW(TAG, "DQBUF(OUTPUT) failed: errno %d", errno);
    }

    if (now - s_h.last_stats >= 1000000LL && s_h.win_frames) {
        int64_t span = now - s_h.win_start;
        ESP_LOGI(TAG, "%" PRIu32 " fps, avg=%" PRIu32 " B enc_avg=%" PRIu32 "us "
                 "enc_max=%" PRIu32 "us idr=%" PRIu32 " err=%" PRIu32,
                 span > 0 ? (uint32_t)((uint64_t)s_h.win_frames * 1000000ULL / (uint64_t)span) : 0,
                 (uint32_t)(s_h.win_bytes / s_h.win_frames),
                 s_h.win_enc_sum / s_h.win_frames, s_h.win_enc_max,
                 s_h.idr, s_h.errors);
        s_h.last_stats = now;
        s_h.win_start = now;
        s_h.win_frames = 0;
        s_h.win_bytes = 0;
        s_h.win_enc_sum = 0;
        s_h.win_enc_max = 0;
    }
}

esp_err_t app_h264enc_register(void)
{
    if (s_h.registered) {
        return ESP_OK;
    }
    portMUX_INITIALIZE(&s_h.cfg_mux);
    s_h.enabled = true;
    s_h.cfg_bitrate = s_phases[0].bitrate;
    s_h.cfg_qp_min = s_phases[0].qp_min;
    s_h.cfg_qp_max = s_phases[0].qp_max;
    s_h.cfg_gop = s_phases[0].gop;
    s_h.cfg_dirty = false;
    s_h.q = xQueueCreate(LINK_Q_DEPTH, sizeof(h264_pkt_t));
    s_h.fp_mux = xSemaphoreCreateMutex();
    if (!s_h.q || !s_h.fp_mux) {
        ESP_LOGE(TAG, "queue/mutex create failed");
        return ESP_ERR_NO_MEM;
    }
    /* 发送任务优先级 4（低于 camera 6），钉 CPU0，慢 I/O 不占 CPU1 的采集链 */
    if (xTaskCreatePinnedToCore(sender_task, "h264_tx", 4096, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "sender task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_h.registered = true;
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t int_max = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "registered (%s %" PRIu32 "bps qp%u-%u gop%u, feed %d fps); "
             "heap int=%u/%u psram=%u",
#if CONFIG_APP_H264_SWEEP
             "sweep",
#else
             "live",
#endif
             s_h.cfg_bitrate, s_h.cfg_qp_min, s_h.cfg_qp_max, s_h.cfg_gop,
             CONFIG_APP_H264_FEED_FPS,
             (unsigned)int_free, (unsigned)int_max, (unsigned)ps_free);
    app_camera_set_frame_cb(app_h264enc_on_frame);
    return ESP_OK;
}

void app_h264enc_on_frame(const uint8_t *data, int w, int h, int stride, uint32_t fourcc)
{
    if (!s_h.registered || s_h.done || !data || w <= 0 || h <= 0) {
        return;
    }
    if (fourcc != V4L2_PIX_FMT_YUV420) {
        if (!s_h.warned_fmt) {
            s_h.warned_fmt = true;
            ESP_LOGE(TAG, "unexpected fourcc %08" PRIx32 " (need YUV420)", fourcc);
        }
        return;
    }
    if (stride != w) {
        if (!s_h.warned_stride) {
            s_h.warned_stride = true;
            ESP_LOGE(TAG, "stride %d != width %d, encoder needs packed YUV420", stride, w);
        }
        return;
    }
    if (((uintptr_t)data) & (H264_ALIGN - 1)) {
        if (!s_h.warned_align) {
            s_h.warned_align = true;
            ESP_LOGE(TAG, "frame %p is not %d-byte aligned; USERPTR rejected",
                     (const void *)data, H264_ALIGN);
        }
        return;
    }

    int64_t now = esp_timer_get_time();
    if (!s_h.enabled) {
        /* 停流只暂停送帧：不要 enc_close()。faceai/imgpipe 起来后内部 RAM
           已放不下编码器的 46KB 参考帧，重开必然失败（实测 fd=-1 后
           esp_h264_enc_hw_new: No memory for reference frame）。 */
        return;
    }
    if (s_h.cfg_dirty) {
        apply_cfg(w, h);
        if (!s_h.enabled) {
            return;
        }
    }
    if (s_h.phase < 0) {
        phase_start(0, w, h, now);
    }
    if (s_h.done || s_h.fd < 0) {
        return;
    }
    if (!s_h.phase_y_logged) {
        s_h.phase_y_logged = true;
        ESP_LOGI(TAG, "phase %d Y mean=%" PRIu32 " (target ~120; 250+ = overexposed)",
                 s_h.phase + 1, y_plane_mean(data, w, h));
    }
    if (now < s_h.next_frame) {
        return;
    }
    s_h.next_frame = now + FEED_INTERVAL;

    encode_one(data, w, h, now);

#if CONFIG_APP_H264_SWEEP
    if (now - s_h.phase_start >= PHASE_US) {
        phase_next(w, h, now);
    }
#endif
}

void app_h264enc_deinit(void)
{
    if (s_h.phase >= 0 && !s_h.done) {
        phase_report();
    }
    enc_close();
    s_h.done = true;
    s_h.phase = -1;
}

/* M2 命令通道：FACEAI 构建里由 faceai 的 cmd_cb 先调用 cmd_try 抢走 H.264 专属命令。
   {"cmd":"stream","value":"on|off","bitrate":N,"qp_min":N,"qp_max":N,"i_period":N}
   {"cmd":"cam","ae":0|1,"gain":N,"exp":N,"flicker50":0|1,"ae_target":N}  (FACEAI 关闭时)
   {"cmd":"detect","value":"on|off"} / {"cmd":"ai_dump"}                 (AI_DETECT 时) */
bool app_h264enc_cmd_try(const char *json, uint32_t len)
{
    if (!json || len == 0) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return false;
    }
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *name = cJSON_IsString(cmd) ? cmd->valuestring : "";

    bool handled = false;
    bool off_ignored = false;
#if !CONFIG_APP_ENABLE_FACEAI
    int cam_exp = 0, cam_gain = 0, cam_luma = -1;
    bool cam_ae = false;
#endif

    if (strcmp(name, "stream") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        const cJSON *br = cJSON_GetObjectItemCaseSensitive(root, "bitrate");
        const cJSON *qmin = cJSON_GetObjectItemCaseSensitive(root, "qp_min");
        const cJSON *qmax = cJSON_GetObjectItemCaseSensitive(root, "qp_max");
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "i_period");

        portENTER_CRITICAL(&s_h.cfg_mux);
        if (cJSON_IsString(v)) {
            const bool on = (strcmp(v->valuestring, "on") == 0) ||
                            (strcmp(v->valuestring, "1") == 0);
            if (!on) {
                /* M2 常开固件：off 只回 ack，不停流。停流路径在 faceai 起来后
                   会触发内部 RAM 碎片化 abort；产品也没有停流入口。 */
                off_ignored = true;
            } else if (!s_h.enabled) {
                s_h.enabled = true;
            }
        }
        if (cJSON_IsNumber(br)) {
            uint32_t x = (uint32_t)br->valuedouble;
            x = x < 25000 ? 25000 : (x > 2500000 ? 2500000 : x);
            if (x != s_h.cfg_bitrate) {
                s_h.cfg_bitrate = x;
                s_h.cfg_dirty = true;
            }
        }
        if (cJSON_IsNumber(qmin) && cJSON_IsNumber(qmax)) {
            int lo = (int)qmin->valuedouble, hi = (int)qmax->valuedouble;
            lo = lo < 0 ? 0 : (lo > 51 ? 51 : lo);
            hi = hi < 0 ? 0 : (hi > 51 ? 51 : hi);
            if (lo < hi && (lo != s_h.cfg_qp_min || hi != s_h.cfg_qp_max)) {
                s_h.cfg_qp_min = (uint8_t)lo;
                s_h.cfg_qp_max = (uint8_t)hi;
                s_h.cfg_dirty = true;
            }
        }
        if (cJSON_IsNumber(ip)) {
            int g = (int)ip->valuedouble;
            g = g < 1 ? 1 : (g > 120 ? 120 : g);
            if (g != s_h.cfg_gop) {
                s_h.cfg_gop = (uint8_t)g;
                s_h.cfg_dirty = true;
            }
        }
        portEXIT_CRITICAL(&s_h.cfg_mux);
        handled = true;
#if !CONFIG_APP_ENABLE_FACEAI
    } else if (strcmp(name, "cam") == 0) {
        const cJSON *jae = cJSON_GetObjectItemCaseSensitive(root, "ae");
        const cJSON *jg = cJSON_GetObjectItemCaseSensitive(root, "gain");
        const cJSON *je = cJSON_GetObjectItemCaseSensitive(root, "exp");
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(root, "flicker50");
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "ae_target");

        if (!app_camauto_get_state(&cam_exp, &cam_gain, &cam_luma, &cam_ae)) {
            cam_exp = CONFIG_APP_H264_EXP_LINES;
            cam_gain = CONFIG_APP_CAM_DEF_GAIN;
        }
        bool manual = false;
        if (cJSON_IsNumber(je)) {
            int e = (int)je->valuedouble;
            cam_exp = e < 8 ? 8 : (e > 994 ? 994 : e);
            manual = true;
        }
        if (cJSON_IsNumber(jg)) {
            int g = (int)jg->valuedouble;
            cam_gain = g < 0 ? 0 : (g > 192 ? 192 : g);
            manual = true;
        }
        if (manual) {
            app_camera_post_exp_gain(cam_exp, cam_gain);   /* 相机任务信箱 */
            app_camauto_adopt(cam_exp, cam_gain);
            app_camauto_note_manual();                     /* 手动参数 -> AE 让位 */
        }
        if (cJSON_IsBool(jae)) {
            cam_ae = cJSON_IsTrue(jae);
            app_camauto_set_ae(cam_ae);
        } else if (cJSON_IsNumber(jae)) {
            cam_ae = jae->valueint != 0;
            app_camauto_set_ae(cam_ae);
        }
        if (cJSON_IsNumber(jf)) {
            app_camauto_set_flicker50(jf->valueint != 0);
        }
        if (cJSON_IsNumber(jt)) {
            app_camauto_set_target((int)jt->valuedouble);
        }
        app_camauto_get_state(&cam_exp, &cam_gain, &cam_luma, &cam_ae);
        handled = true;
#endif
#if CONFIG_APP_AI_DETECT
    } else if (strcmp(name, "detect") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        bool on = app_ai_detect_enabled();
        if (cJSON_IsString(v)) {
            on = (strcmp(v->valuestring, "on") == 0) || (strcmp(v->valuestring, "1") == 0);
        } else if (cJSON_IsBool(v)) {
            on = cJSON_IsTrue(v);
        } else if (cJSON_IsNumber(v)) {
            on = v->valueint != 0;
        }
        app_ai_detect_set_enabled(on);
        handled = true;
    } else if (strcmp(name, "ai_dump") == 0) {
        app_ai_detect_request_dump();
        handled = true;
#endif
    }

    if (handled) {
        /* 任何命令都回 0x02，避免上位机 sendCmdWait 3s 超时 */
        cJSON *ack = cJSON_CreateObject();
        if (ack) {
            cJSON_AddStringToObject(ack, "ack", name);
            if (strcmp(name, "stream") == 0) {
                cJSON_AddBoolToObject(ack, "value", s_h.enabled);
                cJSON_AddStringToObject(ack, "fmt", "h264");
                cJSON_AddNumberToObject(ack, "bitrate", s_h.cfg_bitrate);
                cJSON_AddNumberToObject(ack, "qp_min", s_h.cfg_qp_min);
                cJSON_AddNumberToObject(ack, "qp_max", s_h.cfg_qp_max);
                cJSON_AddNumberToObject(ack, "i_period", s_h.cfg_gop);
                if (off_ignored) {
                    cJSON_AddStringToObject(ack, "msg", "stream off ignored (always-on)");
                }
#if !CONFIG_APP_ENABLE_FACEAI
            } else if (strcmp(name, "cam") == 0) {
                cJSON_AddNumberToObject(ack, "gain", cam_gain);
                cJSON_AddNumberToObject(ack, "gain_max", 192);
                cJSON_AddNumberToObject(ack, "exp", cam_exp);
                cJSON_AddNumberToObject(ack, "exp_max", 994);
                cJSON_AddNumberToObject(ack, "ae", cam_ae ? 1 : 0);
                cJSON_AddNumberToObject(ack, "awb", 0);
                cJSON_AddNumberToObject(ack, "mirror", 1);
                cJSON_AddNumberToObject(ack, "cam_fps", 50);
                cJSON_AddNumberToObject(ack, "luma", cam_luma);
                char m[80];
                snprintf(m, sizeof(m), "cam ae=%d gain=%d exp=%d luma=%d",
                         cam_ae ? 1 : 0, cam_gain, cam_exp, cam_luma);
                cJSON_AddStringToObject(ack, "msg", m);
#endif
#if CONFIG_APP_AI_DETECT
            } else if (strcmp(name, "detect") == 0) {
                cJSON_AddBoolToObject(ack, "enabled", app_ai_detect_enabled());
                cJSON_AddNumberToObject(ack, "fps", app_ai_detect_fps());
            } else if (strcmp(name, "ai_dump") == 0) {
                cJSON_AddBoolToObject(ack, "queued", true);
#endif
            }
            char *s = cJSON_PrintUnformatted(ack);
            if (s) {
                esp_err_t e = app_usb_link_send(JCL_TYPE_RESULT, s, (uint32_t)strlen(s));
                (void)e;
                cJSON_free(s);
            }
            cJSON_Delete(ack);
        }
    }
    cJSON_Delete(root);
    return handled;
}

/* FACEAI 关闭时由 main 注册：未处理的命令仍回 h264-only */
void app_h264enc_cmd_cb(const char *json, uint32_t len)
{
    if (app_h264enc_cmd_try(json, len)) {
        return;
    }
    if (!json || len == 0) {
        return;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return;
    }
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *name = cJSON_IsString(cmd) ? cmd->valuestring : "";
    cJSON *ack = cJSON_CreateObject();
    if (ack) {
        cJSON_AddStringToObject(ack, "ack", name);
        cJSON_AddStringToObject(ack, "msg", "h264-only");
        char *s = cJSON_PrintUnformatted(ack);
        if (s) {
            app_usb_link_send(JCL_TYPE_RESULT, s, (uint32_t)strlen(s));
            cJSON_free(s);
        }
        cJSON_Delete(ack);
    }
    cJSON_Delete(root);
}

#else  /* !CONFIG_APP_CAM_OUT_FMT_YUV420 */

esp_err_t app_h264enc_register(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void app_h264enc_on_frame(const uint8_t *data, int w, int h, int stride, uint32_t fourcc)
{
    (void)data;
    (void)w;
    (void)h;
    (void)stride;
    (void)fourcc;
}

void app_h264enc_deinit(void)
{
}

void app_h264enc_cmd_cb(const char *json, uint32_t len)
{
    (void)json;
    (void)len;
}

bool app_h264enc_cmd_try(const char *json, uint32_t len)
{
    (void)json;
    (void)len;
    return false;
}

#endif /* CONFIG_APP_CAM_OUT_FMT_YUV420 */
