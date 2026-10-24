#pragma once
/*
 * app_h264enc —— ESP32-P4 硬件 H.264 编码（/dev/video11 V4L2 M2M）
 *
 * M0 形态：ISP 直接输出 YUV420，本模块把 camera_task 的 mmap 帧以 USERPTR
 * 零拷贝喂给硬编，编码结果按三档 bitrate/QP 扫描，经 JCLINK 0x07 发给
 * pc_host/h264_capture.py（可选同时落 SD 卡）。
 *
 * 三个对外函数里只有 Register 需要显式调用，且必须在 app_camera_init() 之前。
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_h264enc_register(void);

/* app_camera_frame_cb_t 的实现：在 camera_task 内同步完成一帧编码 */
void app_h264enc_on_frame(const uint8_t *data, int w, int h, int stride, uint32_t fourcc);

void app_h264enc_deinit(void);

/* M2 最小命令通道（由 main 在 FACEAI 关闭时注册给 app_usb_link）：
   {"cmd":"stream","value":"on|off","bitrate":N,"qp_min":N,"qp_max":N,"i_period":N}
   回 0x02 {"ack":"stream", ...}。 */
void app_h264enc_cmd_cb(const char *json, uint32_t len);

/* FACEAI 构建里由 faceai 的 0x03 回调先调用：抢走 H.264 专属命令
   （stream / detect / ai_dump，以及 FACEAI 关闭时的 cam）。返回 true=已处理并已 ack。 */
bool app_h264enc_cmd_try(const char *json, uint32_t len);

#ifdef __cplusplus
}
#endif
