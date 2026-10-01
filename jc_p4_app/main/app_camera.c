/*
 * app_camera.c —— 板载 SC2336 (MIPI-CSI) 持续采集 → app_imgpipe
 *
 *   esp_video(V4L2) 打开 /dev/video0，3 个 mmap 缓冲循环 DQBUF/QBUF；
 *   每帧把 ISP 输出（RGB565）推进图像管道：app_imgpipe_publish_raw()。
 *   管道处于 PC 源模式时 publish 返回非 OK，帧直接丢（不影响出流）。
 *
 * 排查备忘（2026-09-30 实测）：
 *   - sccb_config.freq 必须给（0 会 invalid scl frequency，传感器压根不探测）
 *   - ISP 时钟 esp_video 写死 80MHz -> 只能用 640x480@50fps 这类小分辨率
 *   - 排线没插好时 I2C 上 0x30 不应答（app_i2c_scan() 一眼可见）
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_ioctl.h"

#include "board_pins.h"
#include "app_i2c.h"
#include "app_imgpipe.h"
#include "app_usb_link.h"
#include "app_status.h"
#include "app_camera.h"
#include "sdkconfig.h"

#define TAG "camera"

#define CAM_BUF_COUNT     3
#define CAM_TASK_STACK    4096
/* 比 imgpipe/usb(5) 高一级：DQBUF 要按时做，否则 V4L2 三个缓冲满了会整秒取不到帧 */
#define CAM_TASK_PRIO     6
#define CAM_STATUS_US     1000000

static struct {
    int      fd;
    struct {
        void  *start;
        size_t length;
    } buf[CAM_BUF_COUNT];
    int      width;
    int      height;
    int      stride;
    uint32_t fourcc;
    volatile bool streaming;
    uint32_t frames;
    uint32_t dropped;
} s_cam = { .fd = -1 };

/* 持续采集：DQBUF(阻塞) -> publish_raw(拷贝进管道槽) -> QBUF */
static void camera_task(void *arg)
{
    (void)arg;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint32_t win_cnt = 0;
    int64_t win = esp_timer_get_time();

    ESP_LOGI(TAG, "streaming %dx%d fourcc=%08X stride=%d", s_cam.width, s_cam.height,
             s_cam.fourcc, s_cam.stride);

    while (s_cam.streaming) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!(buf.flags & V4L2_BUF_FLAG_ERROR) && buf.bytesused > 0 && buf.index < CAM_BUF_COUNT) {
            s_cam.frames++;
            win_cnt++;
            if (app_imgpipe_publish_raw((const uint8_t *)s_cam.buf[buf.index].start,
                                        s_cam.width, s_cam.height, s_cam.stride,
                                        IMG_FMT_RGB565) != ESP_OK) {
                s_cam.dropped++;        /* 消费者还没放槽 / 当前是 PC 源 */
            }
        } else {
            s_cam.dropped++;
        }

        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed: errno %d", errno);
            break;
        }

        int64_t now = esp_timer_get_time();
        if (now - win >= CAM_STATUS_US) {
            float fps = (float)win_cnt * 1000000.0f / (float)(now - win);
            app_status_set(APP_MOD_CAM, true, "%dx%d %.0ffps", s_cam.width, s_cam.height, (double)fps);
            ESP_LOGI(TAG, "capture %dx%d %.1ffps frames=%u drop=%u src=%s",
                     s_cam.width, s_cam.height, (double)fps,
                     (unsigned)s_cam.frames, (unsigned)s_cam.dropped,
                     app_imgpipe_src_str(app_imgpipe_get_src()));
            win = now;
            win_cnt = 0;
        }
    }

    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    vTaskDelete(NULL);
}

bool app_camera_ready(void)
{
    return s_cam.streaming && s_cam.fd >= 0;
}

/* ---------------- V4L2 controls（曝光/增益/ISP 亮度）---------------- */

/* ⚠️ esp_video 只实现 EXT 版 controls（VIDIOC_QUERY_EXT_CTRL / G|S_EXT_CTRLS），
   旧版 VIDIOC_QUERYCTRL / G_CTRL / S_CTRL 一律失败 —— 必须用 EXT API */
static void cam_ctrl_query(uint32_t id, int32_t *val, int32_t *mn, int32_t *mx, bool *ok)
{
    *ok = false;
    *val = *mn = *mx = 0;
    if (s_cam.fd < 0) {
        return;
    }

    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = id;
    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_ID2CLASS(id);
    ecs.count = 1;
    ecs.controls = &ec;
    if (ioctl(s_cam.fd, VIDIOC_G_EXT_CTRLS, &ecs) != 0) {
        return;                     /* 驱动不支持这个 control */
    }
    *val = ec.value;

    struct v4l2_query_ext_ctrl q;
    memset(&q, 0, sizeof(q));
    q.id = id;
    if (ioctl(s_cam.fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        *mn = (int32_t)q.minimum;
        *mx = (int32_t)q.maximum;
    }
    *ok = true;
}

esp_err_t app_camera_get_ctrl(app_cam_ctrl_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "arg");
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_ERR_INVALID_STATE, TAG, "not open");
    memset(out, 0, sizeof(*out));
    cam_ctrl_query(V4L2_CID_EXPOSURE, &out->exp_val, &out->exp_min, &out->exp_max, &out->have_exp);
    cam_ctrl_query(V4L2_CID_GAIN, &out->gain_val, &out->gain_min, &out->gain_max, &out->have_gain);
    cam_ctrl_query(V4L2_CID_BRIGHTNESS, &out->bright_val, &out->bright_min, &out->bright_max, &out->have_bright);
    cam_ctrl_query(V4L2_CID_SATURATION, &out->sat_val, &out->sat_min, &out->sat_max, &out->have_sat);
    cam_ctrl_query(V4L2_CID_CONTRAST, &out->contrast_val, &out->contrast_min, &out->contrast_max, &out->have_contrast);
    bool have_auto = false;
    int32_t v = 0, mn = 0, mx = 0;
    cam_ctrl_query(V4L2_CID_EXPOSURE_AUTO, &v, &mn, &mx, &have_auto);
    out->have_auto = have_auto;
    out->auto_val = v;
    int32_t hv = 0, fmin = 0, fmax = 0;
    cam_ctrl_query(APP_CAM_CTL_ISP_HFLIP, &hv, &fmin, &fmax, &out->have_hflip);
    out->hflip_val = hv;
    cam_ctrl_query(APP_CAM_CTL_ISP_VFLIP, &hv, &fmin, &fmax, &out->have_vflip);
    out->vflip_val = hv;
    return ESP_OK;
}

static esp_err_t cam_ctrl_set(uint32_t id, int32_t value)
{
    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = id;
    ec.value = value;
    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CTRL_ID2CLASS(id);
    ecs.count = 1;
    ecs.controls = &ec;
    if (ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &ecs) != 0) {
        ESP_LOGW(TAG, "S_EXT_CTRLS id=0x%08x val=%d failed (errno %d)", (unsigned)id, (int)value, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ctrl 0x%08x <- %d", (unsigned)id, (int)value);
    return ESP_OK;
}

esp_err_t app_camera_set_ctrl(const app_cam_ctrl_t *in, uint32_t mask)
{
    ESP_RETURN_ON_FALSE(in && s_cam.fd >= 0, ESP_ERR_INVALID_STATE, TAG, "arg/open");
    esp_err_t err = ESP_OK;
    if (mask & APP_CAM_SET_AUTO) {
        err |= cam_ctrl_set(V4L2_CID_EXPOSURE_AUTO,
                            in->auto_val ? V4L2_EXPOSURE_AUTO : V4L2_EXPOSURE_MANUAL);
    }
    if (mask & APP_CAM_SET_EXP) {
        err |= cam_ctrl_set(V4L2_CID_EXPOSURE, in->exp_val);
    }
    if (mask & APP_CAM_SET_GAIN) {
        err |= cam_ctrl_set(V4L2_CID_GAIN, in->gain_val);
    }
    if (mask & APP_CAM_SET_BRIGHT) {
        err |= cam_ctrl_set(V4L2_CID_BRIGHTNESS, in->bright_val);
    }
    if (mask & APP_CAM_SET_SAT) {
        err |= cam_ctrl_set(V4L2_CID_SATURATION, in->sat_val);
    }
    if (mask & APP_CAM_SET_CONTRAST) {
        err |= cam_ctrl_set(V4L2_CID_CONTRAST, in->contrast_val);
    }
    if (mask & APP_CAM_SET_HFLIP) {
        err |= cam_ctrl_set(APP_CAM_CTL_ISP_HFLIP, in->hflip_val ? 1 : 0);
    }
    if (mask & APP_CAM_SET_VFLIP) {
        err |= cam_ctrl_set(APP_CAM_CTL_ISP_VFLIP, in->vflip_val ? 1 : 0);
    }
    return err;
}

/* sensor class controls：ctrl_class 用 0（让 esp_video 按 id 自己分发） */
static esp_err_t cam_sensor_ctrl(uint32_t id, int32_t *value, bool write)
{
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_ERR_INVALID_STATE, TAG, "not open");
    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = id;
    if (write) {
        ec.value = *value;
    }
    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = 0;
    ecs.count = 1;
    ecs.controls = &ec;
    int req = write ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS;
    if (ioctl(s_cam.fd, req, &ecs) != 0) {
        ESP_LOGW(TAG, "%s sensor ctrl 0x%08x failed (errno %d)", write ? "set" : "get",
                 (unsigned)id, errno);
        return ESP_FAIL;
    }
    if (!write) {
        *value = ec.value;
    } else {
        ESP_LOGI(TAG, "sensor ctrl 0x%08x <- %d", (unsigned)id, (int)*value);
    }
    return ESP_OK;
}

esp_err_t app_camera_set_sensor(uint32_t id, int32_t value)
{
    return cam_sensor_ctrl(id, &value, true);
}

esp_err_t app_camera_get_sensor(uint32_t id, int32_t *value)
{
    ESP_RETURN_ON_FALSE(value, ESP_ERR_INVALID_ARG, TAG, "arg");
    return cam_sensor_ctrl(id, value, false);
}

esp_err_t app_camera_set_wb(bool enable, float red_gain, float blue_gain)
{
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_ERR_INVALID_STATE, TAG, "not open");
    esp_video_isp_wb_t wb = {
        .enable = enable,
        .red_gain = red_gain,
        .blue_gain = blue_gain,
    };
    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = V4L2_CID_USER_ESP_ISP_WB;
    ec.p_u8 = (uint8_t *)&wb;
    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CID_USER_CLASS;
    ecs.count = 1;
    ecs.controls = &ec;
    int r1 = ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &ecs);
    /* 同时试标准 V4L2 的 RED/BLUE_BALANCE（esp_video 内部也用它，value = gain * 1000） */
    esp_err_t e2 = cam_ctrl_set(V4L2_CID_RED_BALANCE, (int32_t)(red_gain * 1000.0f));
    esp_err_t e3 = cam_ctrl_set(V4L2_CID_BLUE_BALANCE, (int32_t)(blue_gain * 1000.0f));

    app_usb_link_log("cam wb: custom(0x%08x)=%s redbal=%s bluebal=%s",
                     (unsigned)V4L2_CID_USER_ESP_ISP_WB,
                     r1 == 0 ? "ok" : "FAIL",
                     e2 == ESP_OK ? "ok" : "FAIL",
                     e3 == ESP_OK ? "ok" : "FAIL");
    ESP_LOGW(TAG, "WB enable=%d red=%.2f blue=%.2f (custom=%d)", (int)enable,
             (double)red_gain, (double)blue_gain, r1);
    return (r1 == 0 || e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_camera_set_fps(uint32_t fps)
{
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_ERR_INVALID_STATE, TAG, "not open");
    if (fps < 1) {
        fps = 1;
    }
    return cam_ctrl_set(APP_CAM_CTL_FPS, (int32_t)fps);
}

uint32_t app_camera_get_fps(void)
{
    int32_t v = 0, mn = 0, mx = 0;
    bool ok = false;
    cam_ctrl_query(APP_CAM_CTL_FPS, &v, &mn, &mx, &ok);
    return ok ? (uint32_t)v : 0;
}

void app_camera_dump_ctrls(void)
{
    if (s_cam.fd < 0) {
        return;
    }
    /* 扫描 V4L2 各 control class 区间，列出驱动里真正实现的项 */
    static const struct {
        const char *name;
        uint32_t    base;
        uint32_t    span;
    } areas[] = {
        { "USER(ISP)",   V4L2_CID_USER_BASE,    0x1000 },  /* 0x0098xxxx */
        { "CAMERA",      V4L2_CID_CAMERA_CLASS, 0x1000 },  /* 0x009Axxxx */
        { "SENSOR-MISC", 0x00020000,            0x0040 },  /* ESP_CAM_SENSOR_* class 2 */
        { "SENSOR-3A",   0x00030000,            0x0040 },  /* class 3 (AE/AWB/gain) */
    };
    for (size_t a = 0; a < sizeof(areas) / sizeof(areas[0]); a++) {
        for (uint32_t id = areas[a].base; id < areas[a].base + areas[a].span; id++) {
            struct v4l2_query_ext_ctrl q;
            memset(&q, 0, sizeof(q));
            q.id = id;
            if (ioctl(s_cam.fd, VIDIOC_QUERY_EXT_CTRL, &q) != 0) {
                continue;
            }
            int32_t val = 0;
            bool got = false;
            struct v4l2_ext_control ec;
            memset(&ec, 0, sizeof(ec));
            ec.id = id;
            struct v4l2_ext_controls ecs;
            memset(&ecs, 0, sizeof(ecs));
            /* V4L2 标准 id 用其 class；sensor class id（0x0002/0x0003xxxx）用 0 让驱动按 id 分发 */
            ecs.ctrl_class = (id >= 0x00980000) ? V4L2_CTRL_ID2CLASS(id) : 0;
            ecs.count = 1;
            ecs.controls = &ec;
            if (ioctl(s_cam.fd, VIDIOC_G_EXT_CTRLS, &ecs) == 0) {
                val = ec.value;
                got = true;
            }
            app_usb_link_log("ctrl[%s] 0x%08x '%s' type=%u min=%d max=%d cur=%d%s",
                             areas[a].name, (unsigned)id, q.name, (unsigned)q.type,
                             (int)q.minimum, (int)q.maximum, (int)val, got ? "" : " (get fail)");
        }
    }
}

esp_err_t app_camera_init(void)
{
    if (s_cam.fd >= 0) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = app_i2c_get_bus();
    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = false,          /* reuse main I2C bus (GPIO7/8) */
                .i2c_handle = bus,
                /* freq 必须给：esp_video_init 会把它写进 sccb_i2c_config.scl_speed_hz，
                   留 0 时 i2c_master_bus_add_device 报 "invalid scl frequency"，
                   SCCB 建不起来 -> 传感器根本没被探测（之前摄像头失败的真正原因） */
                .freq = 100 * 1000,          /* SCCB 100kHz */
            },
            .reset_pin = -1,
            .pwdn_pin  = -1,
        },
    };
    const esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };

    esp_err_t err = esp_video_init(&cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        app_status_set(APP_MOD_CAM, false, "init fail");
        return err;
    }

    s_cam.fd = open("/dev/video0", O_RDWR);
    if (s_cam.fd < 0) {
        ESP_LOGE(TAG, "open /dev/video0 failed (no camera detected?)");
        app_status_set(APP_MOD_CAM, false, "no device");
        return ESP_ERR_NOT_FOUND;
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "G_FMT failed: errno %d", errno);
        app_status_set(APP_MOD_CAM, false, "g_fmt fail");
        close(s_cam.fd);
        s_cam.fd = -1;
        return ESP_FAIL;
    }
    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;
    s_cam.fourcc = fmt.fmt.pix.pixelformat;
    s_cam.stride = fmt.fmt.pix.bytesperline ? (int)fmt.fmt.pix.bytesperline : s_cam.width * 2;
    ESP_LOGI(TAG, "format %dx%d fourcc=%08X stride=%d", s_cam.width, s_cam.height,
             s_cam.fourcc, s_cam.stride);
    if (s_cam.fourcc != V4L2_PIX_FMT_RGB565) {
        ESP_LOGW(TAG, "expected RGB565 (fourcc RGBP), got %08X - publish will treat as RGB565",
                 s_cam.fourcc);
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed: errno %d", errno);
        app_status_set(APP_MOD_CAM, false, "reqbuf fail");
        close(s_cam.fd);
        s_cam.fd = -1;
        return ESP_FAIL;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        uint8_t *p = NULL;
        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) == 0) {
            p = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_cam.fd, buf.m.offset);
        }
        if (!p || ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "buffer %d setup failed: errno %d", i, errno);
            app_status_set(APP_MOD_CAM, false, "buf fail");
            close(s_cam.fd);
            s_cam.fd = -1;
            return ESP_FAIL;
        }
        s_cam.buf[i].start = p;
        s_cam.buf[i].length = buf.length;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed: errno %d", errno);
        app_status_set(APP_MOD_CAM, false, "streamon fail");
        close(s_cam.fd);
        s_cam.fd = -1;
        return ESP_FAIL;
    }

    /* 默认增益：实测（2026-09-30）esp_video 的 AE/AGC 不会动增益（恒为 0 = 最小增益），
       不设的话画面 avg_luma≈30 一片黑；SC2336 增益 0..192，72 ≈ 室内正常亮度(avg≈120)。
       夜视测试：{"cmd":"cam","gain":192}（配合 exp 一起调）。 */
    app_cam_ctrl_t defc = { .gain_val = CONFIG_APP_CAM_DEF_GAIN };
    if (app_camera_set_ctrl(&defc, APP_CAM_SET_GAIN) != ESP_OK) {
        ESP_LOGW(TAG, "set default gain %d failed", CONFIG_APP_CAM_DEF_GAIN);
    }

    s_cam.streaming = true;
    if (xTaskCreate(camera_task, "camera", CAM_TASK_STACK, NULL, CAM_TASK_PRIO, NULL) != pdPASS) {
        s_cam.streaming = false;
        app_status_set(APP_MOD_CAM, false, "task fail");
        return ESP_FAIL;
    }

    app_status_set(APP_MOD_CAM, true, "%dx%d streaming", s_cam.width, s_cam.height);
    return ESP_OK;
}
