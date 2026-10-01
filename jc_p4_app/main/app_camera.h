#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 板载 SC2336 (MIPI-CSI, WTDK-CAM-2336) 视频采集。
 *
 * 走 esp_video(V4L2)：SCCB 复用主 I2C 总线(GPIO7/8，必须给 sccb_config.freq)，
 * 默认格式来自 sdkconfig 的 CONFIG_CAMERA_SC2336_MIPI_*。
 * 初始化后 camera_task 持续 DQBUF->app_imgpipe_publish_raw()->QBUF，
 * 直接把 RGB565 帧推进图像管道（管道 CAM 源模式）。
 *
 * ⚠️ esp_video 把 ISP 时钟硬编码 80MHz，1080p30 会 "ISP: fifo overflow"，
 *    所以默认用 640x480@50fps。
 */
esp_err_t app_camera_init(void);

/* 摄像头是否已出流（imgpipe 用它决定默认图像源） */
bool app_camera_ready(void);

/* ---------------- 图像参数（曝光/增益/ISP 亮度…）----------------
 * 走 V4L2 controls（esp_video 的 /dev/videoX）：
 *   V4L2_CID_EXPOSURE   曝光，sc2336 单位 100µs（如 200 = 20ms）
 *   V4L2_CID_GAIN       总增益（menu 索引，0 最小；实际倍数见驱动增益表）
 *   V4L2_CID_BRIGHTNESS / CONTRAST / SATURATION / HUE   ISP 侧
 *   V4L2_CID_EXPOSURE_AUTO  自动曝光（V4L2_EXPOSURE_MANUAL=0 / AUTO=1）
 * 夜视测试：先 ae=false 关掉 AE，再把 exp/gain 拉满，观察画面亮度变化。
 */
typedef struct {
    bool    have_exp, have_gain, have_bright, have_sat, have_contrast, have_auto;
    int32_t exp_val, exp_min, exp_max;         /* 100µs 单位 */
    int32_t gain_val, gain_min, gain_max;
    int32_t bright_val, bright_min, bright_max;
    int32_t sat_val, sat_min, sat_max;
    int32_t contrast_val, contrast_min, contrast_max;
    int32_t auto_val;                          /* 0=manual 1=auto */
    bool    have_hflip, have_vflip;
    int32_t hflip_val, vflip_val;              /* 0/1，见 APP_CAM_CTL_ISP_HFLIP */
} app_cam_ctrl_t;

enum {
    APP_CAM_SET_EXP      = 1 << 0,
    APP_CAM_SET_GAIN     = 1 << 1,
    APP_CAM_SET_BRIGHT   = 1 << 2,
    APP_CAM_SET_SAT      = 1 << 3,
    APP_CAM_SET_CONTRAST = 1 << 4,
    APP_CAM_SET_AUTO     = 1 << 5,
    APP_CAM_SET_HFLIP    = 1 << 6,
    APP_CAM_SET_VFLIP    = 1 << 7,
};

/* esp_video 的 ISP 视频设备自己实现的 0/1 控制（dump 枚举出来的 0x00980914/0x00980915），
   实测就是**镜像/翻转**开关 —— 画面镜像时用 {"cmd":"cam","hflip":true} 纠正。
   ⚠️ sensor class 的 ESP_CAM_SENSOR_HMIRROR/VFLIP 走不通：esp_video 的
   esp_video_set_ext_ctrls_to_sensor() 是死代码（没有任何调用点），sensor 侧控制传不进去。 */
#define APP_CAM_CTL_ISP_HFLIP  0x00980914u
#define APP_CAM_CTL_ISP_VFLIP  0x00980915u

esp_err_t app_camera_get_ctrl(app_cam_ctrl_t *out);
esp_err_t app_camera_set_ctrl(const app_cam_ctrl_t *in, uint32_t mask);

/* ---------------- sensor 侧 controls（esp_cam_sensor 的 class<<16|id 体系）----------------
 * 这些 id 不在 V4L2 user class 里，必须用 EXT controls（ctrl_class=0）直连 sensor 驱动。
 */
#define APP_CAM_SENSOR_ID(cls, id)  (((uint32_t)(cls) << 16) | (uint32_t)(id))
#define APP_CAM_CTL_HMIRROR   APP_CAM_SENSOR_ID(2, 0x0a)   /* ESP_CAM_SENSOR_HMIRROR（镜像）*/
#define APP_CAM_CTL_VFLIP     APP_CAM_SENSOR_ID(2, 0x0b)   /* ESP_CAM_SENSOR_VFLIP */
#define APP_CAM_CTL_SHARPNESS APP_CAM_SENSOR_ID(2, 0x0c)
#define APP_CAM_CTL_DENOISE   APP_CAM_SENSOR_ID(2, 0x0d)
#define APP_CAM_CTL_AWB       APP_CAM_SENSOR_ID(3, 0x01)   /* ESP_CAM_SENSOR_AWB（自动白平衡）*/
#define APP_CAM_CTL_AE        APP_CAM_SENSOR_ID(3, 0x05)   /* ESP_CAM_SENSOR_AE_CONTROL */
#define APP_CAM_CTL_AGC       APP_CAM_SENSOR_ID(3, 0x06)   /* ESP_CAM_SENSOR_AGC */
#define APP_CAM_CTL_WB_PRESET APP_CAM_SENSOR_ID(3, 0x20)   /* AUTO_N_PRESET_WB */
#define APP_CAM_CTL_GAIN_ABS  APP_CAM_SENSOR_ID(3, 0x17)   /* 绝对增益（模拟+数字）*/
#define APP_CAM_CTL_EXP_US    APP_CAM_SENSOR_ID(3, 0x1b)   /* 曝光时间(µs) */

esp_err_t app_camera_set_sensor(uint32_t id, int32_t value);
esp_err_t app_camera_get_sensor(uint32_t id, int32_t *value);

/*
 * ISP 白平衡增益（修"画面偏绿"）：走 esp_video 自定义 control
 * `V4L2_CID_USER_ESP_ISP_WB`（0x009819E5），payload 是 esp_video_isp_wb_t{enable,red_gain,blue_gain}。
 * 偏绿 => G 相对过强 => 把 red_gain / blue_gain 提到 1.x。
 */
esp_err_t app_camera_set_wb(bool enable, float red_gain, float blue_gain);

/*
 * 帧率控制（夜视长曝光的前提）：
 * 本地 patch 的 esp_video 把 V4L2_CID_USER_BASE+0x20 映射到 ESP_CAM_SENSOR_FPS，
 * sc2336 收到后按 VTS 换算并写寄存器 —— 帧率越低，单帧可用的曝光行数越多
 * （曝光上限 = VTS - 6），画面才能更亮。
 * 帧率范围 1 ~ 格式标称（640x480 是 50）；改完要重新拉满 exp 才能看到效果。
 */
#define APP_CAM_CTL_FPS   (0x00980900u + 0x20u)
esp_err_t app_camera_set_fps(uint32_t fps);
uint32_t  app_camera_get_fps(void);

/* 把所有支持的 controls 列到 0x04 日志（排查用） */
void app_camera_dump_ctrls(void);

#ifdef __cplusplus
}
#endif
