#pragma once
/*
 * app_ai_detect —— YUV420/H.264 并行的人脸检测（只检测，不做识别/名单）
 *
 * 相机帧回调每 N 帧把整帧 YUV420 拷到 PSRAM staging；AI 任务用 PPA 转
 * RGB565（失败/布局不符则回退 CPU 转换）→ esp-dl MSRMNP → 0x02 结果帧
 * （faces/boxes/infer_ms）。不挂 /storage，避免与 HW_H264 抢内部 RAM。
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 须在 app_camera_init() 之前调用；内部创建任务/缓冲/PPA client */
void app_ai_detect_register(void);

void app_ai_detect_set_enabled(bool on);
bool app_ai_detect_enabled(void);
int  app_ai_detect_fps(void);

/* 调试：把下一帧送检的 RGB565 通过 0x05 发回 PC（验证转换/画质） */
void app_ai_detect_request_dump(void);

#ifdef __cplusplus
}
#endif
