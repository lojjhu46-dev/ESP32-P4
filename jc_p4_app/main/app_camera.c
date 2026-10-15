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
#include <math.h>
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

static esp_err_t cam_ctrl_set(uint32_t id, int32_t value);   /* 信箱执行用（实现见下） */

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
    bool     sensor_hflip;              /* 传感器 HMIRROR 已生效（软件镜像就不用做了） */
    uint32_t frames;
    uint32_t dropped;
} s_cam = { .fd = -1 };

/* ISP 设备（/dev/video20）：只用来设硬件白平衡（WBG 乘进 CCM）。
   打开失败则白平衡退回 imgpipe 软件路径。 */
static struct {
    int fd;
} s_isp = { .fd = -1 };

/* ---------------- 写操作信箱 ----------------
 * faceai（自动路径）投递 -> camera_task 在 QBUF 后执行。自旋锁保护合并字段：
 * P4 双核，投递者与执行者可能真的并行。锁内只有几条赋值，无 ioctl。 */
#define CAM_MREQ_FPS   0x1u
#define CAM_MREQ_EXP   0x2u
#define CAM_MREQ_GAIN  0x4u
#define CAM_MREQ_WB    0x8u    /* ISP 硬件白平衡（/dev/video20，WBG 乘进 CCM） */
#define CAM_MREQ_GAMMA 0x10u   /* ISP 硬件 gamma 曲线（低光提亮；0=禁用模块） */
#define CAM_MREQ_BF    0x20u   /* ISP 硬件 BF 拜耳域降噪（level 0=禁用） */
#define CAM_MREQ_CCM   0x40u   /* ISP 硬件 CCM 颜色校正（mix 千分比：0=禁用模块） */

typedef struct {
    int32_t fps, exp, gain;
    int32_t wb_kr, wb_kb;       /* 千分比 */
    int32_t gamma;              /* 千分比 gamma 参数（1000=线性，0=禁用模块） */
    int32_t bf;                 /* BF level（0=禁用） */
    int32_t ccm_mix;            /* CCM 混合千分比（0=禁用，1000=完整厂商标定矩阵） */
} cam_mreq_args_t;

static portMUX_TYPE s_mreq_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t mask;              /* 待执行的请求位；0 = 空 */
    cam_mreq_args_t arg;
} s_mreq;

static void cam_mreq_post(uint32_t add_mask, const cam_mreq_args_t *a)
{
    if (!s_cam.streaming) {
        return;                  /* 任务没在跑：丢弃（自动路径对失败无感知需求）*/
    }
    portENTER_CRITICAL(&s_mreq_mux);
    if (add_mask & CAM_MREQ_FPS)  s_mreq.arg.fps = a->fps;
    if (add_mask & CAM_MREQ_EXP)  s_mreq.arg.exp = a->exp;
    if (add_mask & CAM_MREQ_GAIN) s_mreq.arg.gain = a->gain;
    if (add_mask & CAM_MREQ_WB) {
        s_mreq.arg.wb_kr = a->wb_kr;
        s_mreq.arg.wb_kb = a->wb_kb;
    }
    if (add_mask & CAM_MREQ_GAMMA) s_mreq.arg.gamma = a->gamma;
    if (add_mask & CAM_MREQ_BF)    s_mreq.arg.bf = a->bf;
    if (add_mask & CAM_MREQ_CCM)   s_mreq.arg.ccm_mix = a->ccm_mix;
    s_mreq.mask |= add_mask;
    portEXIT_CRITICAL(&s_mreq_mux);
}

/* 在 /dev/video20 上设一个 esp_video ISP 自定义 control（p_u8 payload） */
static int isp_set_ctrl(uint32_t id, const void *payload, uint8_t size)
{
    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = id;
    ec.p_u8 = (uint8_t *)payload;
    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.ctrl_class = V4L2_CID_USER_CLASS;
    ecs.count = 1;
    ecs.controls = &ec;
    return ioctl(s_isp.fd, VIDIOC_S_EXT_CTRLS, &ecs);
}

/* gamma 曲线的 16 个采样点 x：首点必须 >0、相邻差全为 2 的幂、末点=255
   （IDF 驱动硬校验，esp_driver_isp/src/isp_gamma.c——x[0]=0 会被判 INVALID_ARG），
   分布前密后疏——提亮主要作用于暗部 */
static const uint8_t s_gamma_x[ISP_GAMMA_CURVE_POINTS_NUM] = {
    1, 2, 4, 8, 16, 32, 64, 96, 128, 192, 224, 240, 248, 252, 254, 255
};

/* gammapermille: 0=禁用模块；1000=线性（模块开着但无提亮）；<1000=暗部提亮 */
static void isp_apply_gamma(int gamma_permille)
{
    esp_video_isp_gamma_t g;
    memset(&g, 0, sizeof(g));
    if (gamma_permille <= 0) {
        g.enable = false;
    } else {
        float gp = gamma_permille / 1000.0f;
        if (gp > 1.0f) gp = 1.0f;            /* >1 是压暗，没意义 */
        if (gp < 0.30f) gp = 0.30f;          /* 过强会丢暗部层次 */
        g.enable = true;
        /* 曲线语义（受控 A/B 实测确认，2026-10-01）：pt.x=输入位置、pt.y=输出值，
           线性曲线完全透明。要实现 out = in^gp 的暗部提亮，直接 y = 255*(x/255)^gp。
           （注意 x 网格首点必须 >0、相邻差为 2 的幂、末点=255，是驱动硬校验。） */
        int prev_y = -1;
        for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
            g.points[i].x = s_gamma_x[i];
            int y = (int)(255.0f * powf((float)s_gamma_x[i] / 255.0f, gp) + 0.5f);
            if (y <= prev_y) {
                y = prev_y + 1;              /* 保持单调 */
            }
            if (y > 255) {
                y = 255;
            }
            if (i == ISP_GAMMA_CURVE_POINTS_NUM - 1) {
                y = 255;                     /* 末点顶格 */
            }
            g.points[i].y = (uint8_t)y;
            prev_y = y;
        }
    }
    if (isp_set_ctrl(V4L2_CID_USER_ESP_ISP_GAMMA, &g, sizeof(g)) != 0) {
        ESP_LOGW(TAG, "isp gamma %d set failed (errno %d)", gamma_permille, errno);
    }
}

/* bf_level: 0=禁用；2..20（用 3..6 档）。矩阵取 IPA sc2336 调优的 level-3 模板 */
static void isp_apply_bf(int bf_level)
{
    esp_video_isp_bf_t bf;
    memset(&bf, 0, sizeof(bf));
    if (bf_level <= 0) {
        bf.enable = false;
    } else {
        if (bf_level < 2) bf_level = 2;
        if (bf_level > 20) bf_level = 20;
        static const uint8_t m[3][3] = { { 2, 4, 2 }, { 4, 5, 4 }, { 2, 4, 2 } };
        bf.enable = true;
        bf.level = (uint8_t)bf_level;
        memcpy(bf.matrix, m, sizeof(m));
    }
    if (isp_set_ctrl(V4L2_CID_USER_ESP_ISP_BF, &bf, sizeof(bf)) != 0) {
        ESP_LOGW(TAG, "isp bf %d set failed (errno %d)", bf_level, errno);
    }
}

/* 厂商标定 CCM（esp_cam_sensor/sensors/sc2336/cfg/sc2336_default.json acc/ccm 原值）。
 * 三行系数和均为 1.0 = 亮度守恒（灰/白不动，只校色相与饱和度），
 * 对 AE/夜视阶梯的亮度判定无扰动。IPA 控制器关闭后这套参数从未下发过，
 * 是白天颜色与人眼差距大的主要软件来源。 */
static const float s_ccm_vendor[3][3] = {
    { 1.408f, -0.094f, -0.314f },
    { -0.130f, 1.280f, -0.150f },
    { -0.072f, -0.173f, 1.245f },
};

/* ccm_mix 千分比：0=禁用 CCM 模块；1000=完整厂商标定矩阵；其他 = 与单位阵插值
   （>1000 为过饱和方向，可用于逼近厂商 saturation 138 的意图）。
   驱动要求矩阵元素范围 (-4, 4)，钳位兜底。独立硬件模块，与 WBG 白平衡互不影响。 */
static void isp_apply_ccm(int mix)
{
    esp_video_isp_ccm_t ccm;
    memset(&ccm, 0, sizeof(ccm));
    if (mix <= 0) {
        ccm.enable = false;
    } else {
        ccm.enable = true;
        float k = mix / 1000.0f;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                float v = (i == j ? 1.0f : 0.0f) + k * (s_ccm_vendor[i][j] - (i == j ? 1.0f : 0.0f));
                if (v > 3.99f) v = 3.99f;
                if (v < -3.99f) v = -3.99f;
                ccm.matrix[i][j] = v;
            }
        }
    }
    if (isp_set_ctrl(V4L2_CID_USER_ESP_ISP_CCM, &ccm, sizeof(ccm)) != 0) {
        ESP_LOGW(TAG, "isp ccm %d set failed (errno %d)", mix, errno);
    }
}

/* 在 camera_task 的安全点调用（QBUF 之后）：此时没有别的 ioctl 在途 */
static void camera_mailbox_run(void)
{
    uint32_t mask;
    cam_mreq_args_t a;
    portENTER_CRITICAL(&s_mreq_mux);
    mask = s_mreq.mask;
    a = s_mreq.arg;
    s_mreq.mask = 0;
    portEXIT_CRITICAL(&s_mreq_mux);

    if (mask & CAM_MREQ_FPS) {
        cam_ctrl_set(APP_CAM_CTL_FPS, a.fps);
    }
    if (mask & CAM_MREQ_EXP) {
        cam_ctrl_set(V4L2_CID_EXPOSURE, a.exp);
    }
    if (mask & CAM_MREQ_GAIN) {
        cam_ctrl_set(V4L2_CID_GAIN, a.gain);
    }
    if ((mask & CAM_MREQ_WB) && s_isp.fd >= 0) {
        /* ISP 硬件白平衡：WBG 增益乘进 CCM 对角元，零 CPU 成本（原先软件 WB
           是每帧 614KB 的 LUT 内存遍历，把 camera 任务占满过）。ISP 已在
           streaming（csi streamon 时启动），esp_video 会 live 重配 CCM。 */
        esp_video_isp_wb_t wb = {
            .enable = true,
            .red_gain = a.wb_kr / 1000.0f,
            .blue_gain = a.wb_kb / 1000.0f,
        };
        if (isp_set_ctrl(V4L2_CID_USER_ESP_ISP_WB, &wb, sizeof(wb)) != 0) {
            ESP_LOGW(TAG, "isp wb %d/%d set failed (errno %d)", (int)a.wb_kr, (int)a.wb_kb, errno);
        }
    }
    if ((mask & CAM_MREQ_GAMMA) && s_isp.fd >= 0) {
        isp_apply_gamma(a.gamma);
    }
    if ((mask & CAM_MREQ_BF) && s_isp.fd >= 0) {
        isp_apply_bf(a.bf);
    }
    if ((mask & CAM_MREQ_CCM) && s_isp.fd >= 0) {
        isp_apply_ccm(a.ccm_mix);
    }
}

void app_camera_post_exp_gain(int32_t exp, int32_t gain)
{
    cam_mreq_args_t a = { .exp = exp, .gain = gain };
    cam_mreq_post(CAM_MREQ_EXP | CAM_MREQ_GAIN, &a);
}

void app_camera_post_fps(int32_t fps)
{
    cam_mreq_args_t a = { .fps = fps };
    cam_mreq_post(CAM_MREQ_FPS, &a);
}

bool app_camera_post_wb(int32_t wkr, int32_t wkb)
{
    if (s_isp.fd < 0) {
        return false;
    }
    cam_mreq_args_t a = { .wb_kr = wkr, .wb_kb = wkb };
    cam_mreq_post(CAM_MREQ_WB, &a);
    return true;
}

/* gamma_permille: 1000=线性 / <1000=提亮 / 0=禁用；返回 false = ISP 通道不可用 */
bool app_camera_post_gamma(int32_t gamma_permille)
{
    if (s_isp.fd < 0) {
        return false;
    }
    cam_mreq_args_t a = { .gamma = gamma_permille };
    cam_mreq_post(CAM_MREQ_GAMMA, &a);
    return true;
}

/* bf_level: 0=禁用，2..20（app 用 3..6） */
bool app_camera_post_bf(int32_t bf_level)
{
    if (s_isp.fd < 0) {
        return false;
    }
    cam_mreq_args_t a = { .bf = bf_level };
    cam_mreq_post(CAM_MREQ_BF, &a);
    return true;
}

/* ccm_mix: 0=禁用 CCM 模块，1000=完整厂商标定矩阵（isp_apply_ccm 注释） */
bool app_camera_post_ccm(int32_t ccm_mix)
{
    if (s_isp.fd < 0) {
        return false;
    }
    cam_mreq_args_t a = { .ccm_mix = ccm_mix };
    cam_mreq_post(CAM_MREQ_CCM, &a);
    return true;
}

bool app_camera_wb_available(void)
{
    return s_isp.fd >= 0;
}

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

        /* 自动路径的写请求（AE 调节 / auto_fps 切换）在这里串行执行：
           与取流同任务、与运动估计/降噪的大数据处理不同时，无跨任务 ioctl 并发 */
        camera_mailbox_run();

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

/* 传感器 HMIRROR 是否已生效（imgpipe 据此跳过软件镜像，省一次全帧内存遍历） */
bool app_camera_sensor_hflip(void)
{
    return s_cam.sensor_hflip;
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

    /* 镜像走传感器 HMIRROR（本地 patch 后 set_ext_ctrls_to_sensor 已接通，sc2336 驱动
       有 set 分支）：在数据源头翻转，AI/回传天然一致，且省掉 imgpipe 每帧一次的
       软件行内镜像内存遍历（640x480 约省 8~10ms/帧）。
       ⚠️ 只信 SET 的返回值——sc2336 驱动的 get 分支没实现（get 必失败），拿 GET
       验证会把 SET 成功也误判成不可用。 */
    s_cam.sensor_hflip = false;
#if CONFIG_APP_CAM_HFLIP
    if (app_camera_set_sensor(APP_CAM_CTL_HMIRROR, 1) == ESP_OK) {
        s_cam.sensor_hflip = true;
        ESP_LOGI(TAG, "sensor hmirror ON (software hflip disabled)");
    } else {
        ESP_LOGW(TAG, "sensor hmirror unavailable, software hflip stays");
    }
#endif

    /* ISP 设备：硬件白平衡入口（esp_video_init 已无条件创建 /dev/video20） */
    s_isp.fd = open("/dev/video20", O_RDWR);
    if (s_isp.fd < 0) {
        ESP_LOGW(TAG, "open /dev/video20 failed (%d), WB stays software", errno);
    }

    s_cam.streaming = true;
    /* 钉到 CPU1：camera 任务每帧 ~600KB 拷贝 + 后处理（约 15~24ms CPU），之前与
       faceai(推理 ~20ms/帧)、imgpipe、isp_task(IPA 控制器开时 prio 11) 全挤在 CPU0，
       实测 CPU0 饱和 -> 关 IPA 控制器后 faceai 被饿到模型加载卡死 + IDLE0 触发 WDT。
       CPU1 基本空闲，摄像头整条采集/后处理链挪过去后 AI 独占 CPU0。 */
    if (xTaskCreatePinnedToCore(camera_task, "camera", CAM_TASK_STACK, NULL,
                                CAM_TASK_PRIO, NULL, CONFIG_APP_CAM_CORE) != pdPASS) {
        s_cam.streaming = false;
        app_status_set(APP_MOD_CAM, false, "task fail");
        return ESP_FAIL;
    }

    /* 初始白平衡：ISP 硬件可用就走 WBG（软件路径保持 1000=不二次校正），
       否则维持 imgpipe 的软件默认值（init 里已设 CONFIG 默认） */
    if (s_isp.fd >= 0) {
        cam_mreq_args_t a = { .wb_kr = CONFIG_APP_CAM_WB_KR, .wb_kb = CONFIG_APP_CAM_WB_KB };
        cam_mreq_post(CAM_MREQ_WB, &a);
        ESP_LOGI(TAG, "white balance -> ISP hardware WBG (%d/%d permille)",
                 CONFIG_APP_CAM_WB_KR, CONFIG_APP_CAM_WB_KB);
        /* 初始 CCM：IPA 控制器关闭后厂商标定矩阵从未下发（白天色偏主因），
           随 WB 一起在 camera_task 安全点下发；mix=0 时 CCM 模块保持禁用 */
#if CONFIG_APP_CAM_CCM_MIX > 0
        cam_mreq_args_t c = { .ccm_mix = CONFIG_APP_CAM_CCM_MIX };
        cam_mreq_post(CAM_MREQ_CCM, &c);
        ESP_LOGI(TAG, "CCM -> ISP hardware (mix %d permille)", CONFIG_APP_CAM_CCM_MIX);
#endif
    }

    app_status_set(APP_MOD_CAM, true, "%dx%d streaming", s_cam.width, s_cam.height);
    return ESP_OK;
}
