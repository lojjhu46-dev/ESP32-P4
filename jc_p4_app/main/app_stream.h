#pragma once
/*
 * app_stream —— 把图像管道里的帧回传给 PC/模拟器（任务七：LCD 替代 UI 的画面来源）
 *
 *   摄像头/PC 帧 -> app_imgpipe 帧池 -> 本模块按限速取帧 -> P4 硬件 JPEG 编码
 *        -> JCLINK 0x05 图像帧 -> PC(host_app.py / LVGL 模拟器) 显示
 *
 * 0x05 载荷（小端）：
 *   off 0  fmt    u8    0=JPEG 1=RGB565(裸，备用)
 *   off 1  flags  u8    预留
 *   off 2  width  u16
 *   off 4  height u16
 *   off 6  seq    u16    帧序号（UI 可据此丢旧帧）
 *   off 8  data[]        JPEG 比特流 / RGB565 像素
 *
 * 默认关闭，PC 端发 {"cmd":"stream","value":"on"} 打开（省 CPU/带宽）。
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_STREAM_HDR_LEN      8
#define APP_STREAM_FMT_JPEG     0
#define APP_STREAM_FMT_RGB565   1

#define APP_STREAM_DEF_FPS      10     /* 回传帧率上限 */
#define APP_STREAM_DEF_QUALITY  75     /* JPEG 质量 1~100 */

esp_err_t app_stream_init(void);

/* 开关（同时把 JPEG 编码器惰性创建） */
void app_stream_set_enable(bool on);
bool app_stream_get_enable(void);

/* 回传帧率上限（1~30）与 JPEG 质量（1~100） */
void     app_stream_set_fps(uint32_t fps);
uint32_t app_stream_get_fps(void);
void     app_stream_set_quality(uint32_t quality);
uint32_t app_stream_get_quality(void);

/* 回传格式：APP_STREAM_FMT_JPEG（默认，省带宽）/ APP_STREAM_FMT_RGB565（原始像素，
 * 640x480 一帧 614KB，用于排查"JPEG 编码是否把画面搞花了"） */
void     app_stream_set_format(int fmt);
int      app_stream_get_format(void);

/* JPEG 子采样：0=4:2:0(默认,最小) 1=4:2:2 2=4:4:4(色度不降采样,颜色最好) */
void     app_stream_set_subsample(int sub);
int      app_stream_get_subsample(void);

/* RGB565 字节序：编码器若把像素读反，图像会变成彩色噪声 —— 用这个开关矫正 */
void     app_stream_set_reverse(bool on);
bool     app_stream_get_reverse(void);

typedef struct {
    uint32_t frames;     /* 已回传帧数 */
    uint32_t bytes;      /* 最近一帧字节数 */
    uint32_t enc_ms;     /* 最近一帧编码耗时 */
    uint32_t drop;       /* 因限速/无帧丢掉的次数 */
    float    fps;        /* 最近 1s 实际回传帧率 */
} app_stream_stats_t;

void app_stream_get_stats(app_stream_stats_t *out);

#ifdef __cplusplus
}
#endif
