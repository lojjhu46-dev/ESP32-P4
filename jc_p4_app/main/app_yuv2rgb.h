#pragma once
/*
 * app_yuv2rgb —— YUV420(H.264) → RGB565 → imgpipe 送帧桥
 *
 * camera_task 每 200ms 拷一帧到 PSRAM staging；任务用 PPA 按
 * O_UYY_E_VYY 打包布局转 RGB565，再 app_imgpipe_publish_raw() 给
 * app_faceai（检测 + MFN 特征 + 1:N）。src=pc 时自动停手。
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 须在 app_camera_init() 之前调用 */
esp_err_t app_yuv2rgb_register(void);

/* 须在 app_imgpipe_init() 之后调用：armed + 关掉 imgpipe 的二次镜像/白平衡 */
void app_yuv2rgb_start(void);

#ifdef __cplusplus
}
#endif
