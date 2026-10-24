#pragma once
/*
 * app_camauto —— app 级自动曝光(AE)/自动白平衡(AWB)闭环引擎
 *
 * 背景：P4 的 ISP 硬件只做统计、没有"硬件直接改 sensor 曝光"的 HW AE；
 * Espressif 的 IPA 闭环（闭源 libesp_ipa.a）实测不抬增益、AWB 校正不足、
 * 还会用初始 metadata 把手动 exp/gain 压回默认值（已把
 * CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER 关掉）。本模块在 app 层
 * 自己做闭环：
 *   AE  ：imgpipe 的帧内稀疏亮度采样 -> 目标亮度闭环 -> exp（先）/gain（后）
 *   AWB ：WB 前的 R/G/B 通道均值 -> 灰世界 -> 现有软件白平衡 kr/kb（纯变量，零阻塞）
 *
 * 并发纪律（务必遵守，见 HANDOVER.md 的重启坑）：
 *   - 所有 sensor 写操作走 app_camera 的写操作信箱（camera 任务串行执行），本模块
 *     自身不直接 ioctl；
 *   - tick 由 faceai 主循环每帧调用（内部按时间节流），当前 exp/gain/fps 用 faceai
 *     的缓存传入，也不 ioctl。
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_camauto_init(void);

/* --- YUV420/H.264 路径（M2）---
 * 没有 imgpipe 时由相机帧回调直接喂 Y 平面亮度。register 必须赶在
 * app_camera_init() 之前调用；AWB/夜视增强在 YUV420 下自动关闭（无 RGB 统计）。 */
void app_camauto_register(void);
void app_camauto_feed_luma(int luma);
int  app_camauto_get_luma(void);
bool app_camauto_get_state(int *exp, int *gain, int *luma, bool *ae);
/* 50Hz 抗频闪：曝光量化到 500 行（10ms）整数倍。M2 默认关闭（当前场景
 * 短曝光即可到目标亮度，强制 500 行会过曝）。 */
void app_camauto_set_flicker50(bool on);
bool app_camauto_get_flicker50(void);

/* --- 开关（默认值来自 Kconfig APP_CAM_AE / APP_CAM_AWB）--- */
void app_camauto_set_ae(bool on);
void app_camauto_set_awb(bool on);
bool app_camauto_get_ae(void);
bool app_camauto_get_awb(void);

/* AE 目标亮度 0..255（默认 CONFIG_APP_CAM_AE_TARGET）*/
void app_camauto_set_target(int target);
int  app_camauto_get_target(void);

/* --- 白平衡统一入口（本模块是 kr/kb 的唯一权威）---
 * ISP 硬件 WB 可用时走 camera 任务信箱写 /dev/video20（WBG 乘进 CCM，零 CPU），
 * 否则写 imgpipe 软件白平衡（LUT）。手动命令 / UI 按钮 / AWB 引擎都走这里，
 * 保证引擎视角与实际增益一致。kr/kb 千分比，1000 = 不校正。 */
void app_camauto_wb_apply(int kr, int kb, bool log);
void app_camauto_wb_get(int *kr, int *kb);

/* --- faceai 通知接口 --- */
/* 用户手动改了 exp/gain 或 fps（cam 命令/Night 预设/滑块）：AE 让位（关闭）*/
void app_camauto_note_manual(void);
/* 用户手动改了 wb_kr/wb_kb：AWB 让位（关闭）*/
void app_camauto_note_manual_wb(void);
/* auto_fps 切了帧率：AE 冷却片刻再介入（切换联动里的 exp/gain 拉满优先），
   exp/gain 传切换联动后的值（引擎视角同步）*/
void app_camauto_note_fps_switch(int fps, int exp, int gain);
/* 自动路径（auto_fps boost 等）改了 exp/gain：引擎同步视角，不退出自动 */
void app_camauto_adopt(int exp, int gain);

/* --- 每帧 tick（faceai 主循环调用；cur_fps/cur_gain/exp_max/gain_max 来自 faceai 缓存）---
 * exp_cap_lines：AE 曝光上限（行数，1ms=50 行；0 = 不设上限，用 fps 档位最大值）。
 * 暗光下超过上限转提增益保锐度（2026-10-06 实测：100ms 模糊帧毁掉检测与特征，
 * 20ms 锐利帧 win-max 0.753 vs 0.638）；夜视档 tier>=1 时 faceai 传 0 解除
 * （夜视要亮度，且那时人脸识别已门控关闭）。 */
void app_camauto_tick(int cur_fps, int cur_gain, int exp_max, int gain_max,
                      int exp_cap_lines);

/* --- 夜视增强（gamma 提亮 + BF 降噪；默认 CONFIG_APP_CAM_NIGHT_ENH，自适应）---
 * gamma 随实测亮度自适应（亮时线性、暗时提到 GAMMA_MIN），BF 随增益分档；
 * 手动设置后自适应让位（同 AE/AWB 手动优先规则）。走 camera 任务信箱。 */
void app_camauto_enh_set(bool on);            /* 总开关（关=gamma/BF 模块禁用） */
void app_camauto_enh_manual_gamma(int gamma_permille);  /* 手动 gamma（0=禁用） */
void app_camauto_enh_manual_bf(int bf_level);           /* 手动 BF（0=禁用） */
void app_camauto_enh_resume_auto(void);       /* 手动后恢复自适应 */
bool app_camauto_enh_on(void);
bool app_camauto_enh_manual(void);
void app_camauto_get_enh(int *gamma_permille, int *bf_level);

/* 引擎视角的当前 exp/gain（AE 开启时它是权威：AE 通过信箱改参数后保存在这里，
   faceai 在 cam_state_tick 里用它刷新结果帧的 exp/gain 字段）。返回 false = 视角未初始化 */
bool app_camauto_get_exp_gain(int *exp, int *gain);

#ifdef __cplusplus
}
#endif
