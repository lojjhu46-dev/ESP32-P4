/*
 * ui_shared —— 与板端同源的人机界面（板子触摸屏的 PC 替代品）
 *
 * 只依赖 LVGL（不含任何 socket / 平台代码）：PC 模拟器里由 net_link 驱动，
 * 将来板端 JD9365 屏点亮后，同一份代码接 esp_lvgl_port 即可复用。
 *
 * 画面：状态栏 | 摄像头预览(含检测框) | 结果 | 名单 | 操作按钮 | 日志
 * 交互：按钮 -> action 回调（发出 JCLINK 命令 JSON）-> 板端执行并回 0x02
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 用户点了按钮/提交了输入 -> 把这条 JSON 命令发给板端 */
typedef void (*ui_shared_action_cb_t)(const char *cmd_json, void *arg);

void ui_shared_init(void);
void ui_shared_set_action_cb(ui_shared_action_cb_t cb, void *arg);

/* 链路状态（顶部状态栏 + 首次连上时自动 start/src/stream） */
void ui_shared_set_link(bool connected, const char *detail);

/* 0x05 图像帧（fmt: 0=JPEG 1=RGB565） */
void ui_shared_set_image(const uint8_t *data, uint32_t len, int fmt, int width, int height);

/* 0x02 结果 JSON（faces/boxes/match/sim/fps/infer_ms/lib/event/src/msg/ack） */
void ui_shared_set_result(const char *json);

/* 0x04 日志行 */
void ui_shared_add_log(const char *line);

/* 供主循环周期调用（默认图像/回传开关等） */
void ui_shared_tick(void);

#ifdef __cplusplus
}
#endif
