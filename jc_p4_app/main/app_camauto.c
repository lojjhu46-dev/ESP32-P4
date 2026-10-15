/*
 * app_camauto.c —— app 级 AE/AWB 闭环（实现说明见 app_camauto.h）
 *
 * 控制律（全部整数运算，无浮点）：
 *   AE ：每 ~250ms 看一次亮度，误差出死区才动作；乘法律（目标/当前，单步限幅
 *        0.5x~2x 防振荡）；太暗先加曝光、曝光到顶再加增益，太亮先降增益再降曝光
 *        （增益产生噪声、优先不动它）；50Hz 抗频闪把曝光量化到 500 行(=10ms)整数倍。
 *   AWB：每 ~500ms 看一次 WB 前的 R/G/B 均值，kr=G/R、kb=G/B（千分比），EMA 平滑
 *        + 死区 + 钳位后写软件白平衡（纯变量）。暗帧噪声大不可信，低于门限跳过。
 */
#include <stdint.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"

#include "app_camauto.h"
#include "app_camera.h"
#include "app_imgpipe.h"
#include "app_usb_link.h"
#include "sdkconfig.h"

/* sdkconfig 对 bool=n 的符号不生成宏（且参数项可能被父开关隐藏），这里统一兜底 */
#ifndef CONFIG_APP_CAM_AE
#define CONFIG_APP_CAM_AE 0
#endif
#ifndef CONFIG_APP_CAM_AWB
#define CONFIG_APP_CAM_AWB 0
#endif
#ifndef CONFIG_APP_CAM_NIGHT_ENH
#define CONFIG_APP_CAM_NIGHT_ENH 0
#endif
#ifndef CONFIG_APP_CAM_AE_TARGET
#define CONFIG_APP_CAM_AE_TARGET 120
#endif
#ifndef CONFIG_APP_CAM_AWB_MIN_LUMA
#define CONFIG_APP_CAM_AWB_MIN_LUMA 40
#endif
#ifndef CONFIG_APP_CAM_GAMMA_MIN
#define CONFIG_APP_CAM_GAMMA_MIN 650
#endif

#define TAG "camauto"

/* ---- AE 参数（调参入口都集中在这；真机标定后如有必要再提升为 Kconfig）---- */
#define AE_TICK_MS        250     /* 采样/判定节流 */
#define AE_MIN_STEP_MS    500     /* 两次实际写 sensor 的最小间隔（留出收敛观察窗） */
#define AE_DEADZONE       6       /* 亮度死区 ±（0..255），防边缘抖动 */
#define AE_PCT_MIN        50      /* 单步最小校正比例（%） */
#define AE_PCT_MAX        200     /* 单步最大校正比例（%） */
#define AE_EXP_MIN_STEP   60      /* 曝光步进下限（行），保证每步有进展 */
#define AE_GAIN_MIN_STEP  2       /* 增益步进下限（索引） */
#define AE_EXP_MIN        100     /* 曝光下限（行）——抗频闪关闭时才用得到 */
#define AE_FLICKER_STEP   500     /* 50Hz 市电：曝光量子 = 10ms = 500 行（0 = 关闭量化） */
#define AE_COOL_MS        500     /* auto_fps 切换后的冷却 */

/* ---- AWB 参数 ---- */
#define AWB_TICK_MS       500     /* AWB 判定周期 */
#define AWB_EMA_SHIFT     3       /* 平滑系数 1/8 */
#define AWB_DEADZONE      30      /* 千分比死区（3%）：小于它不动 */
#define AWB_KR_MIN        800     /* 增益钳位（千分比） */
#define AWB_KR_MAX        2500

/* ---- 夜视增强（gamma 提亮 + BF 降噪，自适应）----
 * gamma 随实测亮度：亮时线性无感、暗时提到 GAMMA_MIN；BF 随增益（对齐 IPA 的
 * adn 增益分档）。gamma 提亮会抬高 AE 测到的亮度 -> AE 降增益 -> 亮度回落，
 * 与 AE 存在耦合，所以决策节流放慢（2s）+ luma EMA + 宽迟滞带压制振荡。 */
#define ENH_TICK_MS       2000    /* 增强决策节流 */
#define ENH_LUMA_EMA_DIV  4       /* luma 平滑 1/4 */
#define ENH_LUMA_DARK     45      /* 低于此 -> 最强提亮（迟滞下沿） */
#define ENH_LUMA_BRIGHT   70      /* 高于此 -> 线性/不提亮（迟滞上沿） */
#define ENH_GAMMA_STEP    50      /* gamma 千分比量化档（变化不足一档不下发） */

static struct {
    bool     ae_on;
    bool     awb_on;
    int      target;
    /* 引擎视角的传感器状态（信箱写入的值也记在这里） */
    bool     view_valid;
    int      cur_exp, cur_gain;
    /* tick 传入的环境 */
    int      fps, exp_max, gain_max;
    /* 节流 */
    int64_t  last_tick_us;
    int64_t  last_step_us;        /* 上次 AE 实际写 sensor */
    int64_t  cool_until_us;       /* fps 切换冷却截止 */
    /* AWB */
    int      kr, kb;              /* 当前校正（千分比） */
    bool     kr_valid;
    int64_t  last_awb_us;
    /* 夜视增强（gamma/BF 自适应） */
    bool     enh_on;
    bool     enh_manual;          /* 手动设置过 gamma/bf：自适应让位 */
    int      gamma;               /* 当前下发的 gamma 千分比（-1=未下发） */
    int      bf;                  /* 当前下发的 BF level（-1=未下发） */
    int      luma_ema;            /* 亮度 EMA（-1=未建立） */
    int64_t  last_enh_us;
} s = {
    .ae_on = CONFIG_APP_CAM_AE,
    .awb_on = CONFIG_APP_CAM_AWB,
    .target = CONFIG_APP_CAM_AE_TARGET,
    .exp_max = 994,
    .gain_max = 192,
    .kr = CONFIG_APP_CAM_WB_KR,
    .kb = CONFIG_APP_CAM_WB_KB,
    .enh_on = CONFIG_APP_CAM_NIGHT_ENH,
    .gamma = -1,
    .bf = -1,
    .luma_ema = -1,
};

static bool wb_via_isp(void);   /* 定义在 wb_apply 附近（init 也要用） */

/* 帧内统计（luma/RGB）只在有任何自动功能需要时开启采样 */
static bool stats_needed(void)
{
    return s.ae_on || s.awb_on || s.enh_on;
}

static void stats_update(void)
{
    app_imgpipe_set_ae_stats(stats_needed());
}

void app_camauto_init(void)
{
    ESP_LOGI(TAG, "AE=%d(target %d) AWB=%d ENH=%d flicker50=%d%s",
             (int)s.ae_on, s.target, (int)s.awb_on, (int)s.enh_on,
             AE_FLICKER_STEP > 0 ? 1 : 0,
             (s.ae_on || s.awb_on || s.enh_on) ? "" : " (all off)");
    stats_update();
    if (wb_via_isp()) {
        /* 硬件 WB 生效后软件层不能再校正：清掉 imgpipe 的默认增益（初始 WBG
           值由 app_camera_init 经信箱下发 CONFIG 默认） */
        app_imgpipe_set_wb_gain_quiet(1000, 1000);
        ESP_LOGI(TAG, "WB path: ISP hardware (software wb off), kr=%d kb=%d", s.kr, s.kb);
    } else {
        ESP_LOGI(TAG, "WB path: imgpipe software kr=%d kb=%d", s.kr, s.kb);
    }
    app_usb_link_log("camauto: AE=%d AWB=%d ENH=%d target=%d wb=%s",
                     (int)s.ae_on, (int)s.awb_on, (int)s.enh_on, s.target,
                     wb_via_isp() ? "isp" : "soft");
}

void app_camauto_set_ae(bool on)
{
    if (s.ae_on == on) {
        return;
    }
    s.ae_on = on;
    ESP_LOGW(TAG, "AE -> %s (target %d)", on ? "ON" : "OFF", s.target);
    app_usb_link_log("camauto: AE %s (target %d)", on ? "on" : "off", s.target);
    stats_update();
    if (on) {
        /* 开启瞬间进冷却：让当前设定先稳定一拍，避免开机即写 */
        s.cool_until_us = esp_timer_get_time() + AE_COOL_MS * 1000;
    }
}

void app_camauto_set_awb(bool on)
{
    if (s.awb_on == on) {
        return;
    }
    s.awb_on = on;
    ESP_LOGW(TAG, "AWB -> %s", on ? "ON" : "OFF");
    app_usb_link_log("camauto: AWB %s", on ? "on" : "off");
    stats_update();
    if (on) {
        /* 引擎视角 s.kr/kb 就是当前生效增益（init 从 CONFIG 初始化、手动 apply 同步），
           从当前值出发做平滑，避免启用瞬间颜色跳变 */
        s.last_awb_us = 0;               /* 下个 tick 立即评估一次 */
    }
}

bool app_camauto_get_ae(void)
{
    return s.ae_on;
}

bool app_camauto_get_awb(void)
{
    return s.awb_on;
}

void app_camauto_set_target(int target)
{
    if (target < 20) target = 20;
    if (target > 240) target = 240;
    if (s.target == target) {
        return;
    }
    s.target = target;
    ESP_LOGW(TAG, "AE target -> %d", target);
    app_usb_link_log("camauto: AE target %d", target);
}

int app_camauto_get_target(void)
{
    return s.target;
}

/* ISP 硬件 WB 可用时，imgpipe 的软件白平衡必须保持 1000（否则双重校正），
 * 帧里的校正效果由 ISP WBG 在上游完成 —— AE/AWB 统计看到的也已是校正后画像 */
static bool wb_via_isp(void)
{
    return app_camera_wb_available();
}

void app_camauto_wb_apply(int kr, int kb, bool log)
{
    if (kr < 100) kr = 100;
    if (kr > 4000) kr = 4000;
    if (kb < 100) kb = 100;
    if (kb > 4000) kb = 4000;
    s.kr = kr;
    s.kb = kb;
    s.kr_valid = true;
    if (wb_via_isp()) {
        app_camera_post_wb(kr, kb);
        if (log) {
            ESP_LOGW(TAG, "ISP WB: R*%.3f B*%.3f", kr / 1000.0, kb / 1000.0);
            app_usb_link_log("camauto: isp wb kr=%d kb=%d", kr, kb);
        }
    } else if (log) {
        app_imgpipe_set_wb_gain(kr, kb);
    } else {
        app_imgpipe_set_wb_gain_quiet(kr, kb);
    }
}

void app_camauto_wb_get(int *kr, int *kb)
{
    if (kr) *kr = s.kr;
    if (kb) *kb = s.kb;
}

void app_camauto_note_manual(void)
{
    if (s.ae_on) {
        ESP_LOGW(TAG, "manual exp/gain/fps -> AE off");
        app_usb_link_log("camauto: manual param, AE off");
        s.ae_on = false;
    }
}

void app_camauto_note_manual_wb(void)
{
    if (s.awb_on) {
        ESP_LOGW(TAG, "manual wb -> AWB off");
        app_usb_link_log("camauto: manual wb, AWB off");
        s.awb_on = false;
    }
}

void app_camauto_note_fps_switch(int fps, int exp, int gain)
{
    s.fps = fps;
    s.cur_exp = exp;
    s.cur_gain = gain;
    s.view_valid = true;
    /* 切换联动（exp/gain 拉满）优先完成，AE 冷却后再接管 */
    s.cool_until_us = esp_timer_get_time() + AE_COOL_MS * 1000;
}

void app_camauto_adopt(int exp, int gain)
{
    s.cur_exp = exp;
    s.cur_gain = gain;
    s.view_valid = true;
}

bool app_camauto_get_exp_gain(int *exp, int *gain)
{
    if (!s.view_valid) {
        return false;
    }
    if (exp) *exp = s.cur_exp;
    if (gain) *gain = s.cur_gain;
    return true;
}

/* ---------------- 夜视增强（gamma 提亮 + BF 降噪，自适应） ---------------- */

/* 变更下发（只在值真的变化时投递，信箱与 ISP set 都省） */
static void enh_apply(int gamma_permille, int bf_level)
{
    if (gamma_permille != s.gamma) {
        if (app_camera_post_gamma(gamma_permille)) {
            s.gamma = gamma_permille;
        }
    }
    if (bf_level != s.bf) {
        if (app_camera_post_bf(bf_level)) {
            s.bf = bf_level;
        }
    }
}

/* gamma 参数由 EMA 亮度决定：>=BRIGHT 线性（无提亮）、<=DARK 最强、中间线性过渡 */
static int enh_gamma_from_luma(int luma)
{
    const int gmin = CONFIG_APP_CAM_GAMMA_MIN;
    if (luma >= ENH_LUMA_BRIGHT) {
        return 1000;
    }
    if (luma <= ENH_LUMA_DARK) {
        return gmin;
    }
    return gmin + (1000 - gmin) * (luma - ENH_LUMA_DARK) / (ENH_LUMA_BRIGHT - ENH_LUMA_DARK);
}

/* BF level 随增益分档（对齐 IPA sc2336_default.json 的 adn 段思路：
   增益越高噪点越大，拜耳域降噪越强） */
static int enh_bf_from_gain(int gain)
{
    if (gain >= 120) {
        return 6;
    }
    if (gain >= 48) {
        return 5;
    }
    if (gain >= 8) {
        return 4;
    }
    return 3;
}

static void enh_step(int64_t now, int luma, int cur_gain)
{
    if (now - s.last_enh_us < ENH_TICK_MS * 1000) {
        return;
    }
    s.last_enh_us = now;

    /* luma EMA：gamma 提亮会抬高实测亮度，不平滑会跟 AE 形成耦合振荡 */
    if (s.luma_ema < 0) {
        s.luma_ema = luma;
    } else {
        s.luma_ema += (luma - s.luma_ema) / ENH_LUMA_EMA_DIV;
    }

    int g = enh_gamma_from_luma(s.luma_ema);
    g = ((g + ENH_GAMMA_STEP / 2) / ENH_GAMMA_STEP) * ENH_GAMMA_STEP;   /* 量化成档 */
    enh_apply(g, enh_bf_from_gain(cur_gain));
    ESP_LOGD(TAG, "ENH: luma %d (ema %d) -> gamma %d, gain %d -> bf %d",
             luma, s.luma_ema, s.gamma, cur_gain, s.bf);
}

void app_camauto_enh_set(bool on)
{
    /* ⚠️ 显式 enh:true 必须能"恢复自适应"，即使当前本来就是开的——手动
       gamma/bf_level 会把 s.enh_manual 置位让自适应永久让位，如果这里在
       "状态没变"时直接早退，就再也没有回到自动的入口了（HANDOVER 承诺
       enh:true 可恢复，实测不成立：画面会一直停在手动 gamma）。 */
    bool was = s.enh_on;
    s.enh_on = on;
    if (on) {
        s.enh_manual = false;
        s.luma_ema = -1;                 /* 重建 EMA，下个 tick 立即按亮度判档 */
        s.last_enh_us = 0;
        stats_update();
        if (!was) {
            ESP_LOGW(TAG, "night enhance -> ON");
            app_usb_link_log("camauto: enhance on");
        } else {
            app_usb_link_log("camauto: enhance back to auto (manual cleared)");
        }
        return;                          /* 不立即下发：等 enh_step 按 EMA 定档 */
    }
    if (!was) {
        return;
    }
    ESP_LOGW(TAG, "night enhance -> OFF");
    app_usb_link_log("camauto: enhance off");
    stats_update();
    enh_apply(0, 0);                     /* gamma/BF 模块都禁用 */
}

/* 手动 gamma（permille，0=禁用模块）：自适应让位 */
void app_camauto_enh_manual_gamma(int gamma_permille)
{
    s.enh_manual = true;
    if (gamma_permille > 0 && gamma_permille < 300) {
        gamma_permille = 300;
    }
    if (gamma_permille > 1000) {
        gamma_permille = 1000;
    }
    enh_apply(gamma_permille, s.bf < 0 ? 3 : s.bf);
    ESP_LOGW(TAG, "manual gamma -> %d (enhance auto off)", gamma_permille);
    app_usb_link_log("camauto: manual gamma %d", gamma_permille);
}

/* 手动 BF level（0=禁用）：自适应让位 */
void app_camauto_enh_manual_bf(int bf_level)
{
    s.enh_manual = true;
    if (bf_level > 20) {
        bf_level = 20;
    }
    enh_apply(s.gamma < 0 ? 1000 : s.gamma, bf_level);
    ESP_LOGW(TAG, "manual bf -> %d (enhance auto off)", bf_level);
    app_usb_link_log("camauto: manual bf %d", bf_level);
}

/* 手动操作后重新允许自适应（比如 enh 总开关重新打开时已带此语义） */
void app_camauto_enh_resume_auto(void)
{
    if (!s.enh_manual) {
        return;
    }
    s.enh_manual = false;
    s.last_enh_us = 0;                   /* 下个 tick 立即评估 */
    app_usb_link_log("camauto: enhance back to auto");
}

bool app_camauto_enh_on(void)
{
    return s.enh_on;
}

bool app_camauto_enh_manual(void)
{
    return s.enh_manual;
}

void app_camauto_get_enh(int *gamma_permille, int *bf_level)
{
    if (gamma_permille) *gamma_permille = s.gamma;
    if (bf_level) *bf_level = s.bf;
}

/* 视角初始化：从真实寄存器读一次（仅在 AE/AWB 开且从未读过时执行一次），
 * 之后所有变化都来自 tick 传入 / adopt / 引擎自己的信箱写入，不再读 sensor */
static void view_seed_once(void)
{
    if (s.view_valid) {
        return;
    }
    app_cam_ctrl_t t = {};
    if (app_camera_get_ctrl(&t) == ESP_OK && t.have_exp && t.have_gain) {
        s.cur_exp = (int)t.exp_val;
        s.cur_gain = (int)t.gain_val;
        if (t.exp_max > 0) s.exp_max = (int)t.exp_max;
        if (t.gain_max > 0) s.gain_max = (int)t.gain_max;
        s.view_valid = true;
        ESP_LOGI(TAG, "view seeded: exp=%d/%d gain=%d/%d", s.cur_exp, s.exp_max,
                 s.cur_gain, s.gain_max);
    }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* AE 单步：返回是否发出了写请求 */
static bool ae_step(int64_t now, int luma)
{
    if (now < s.cool_until_us) {
        return false;
    }
    if (now - s.last_step_us < AE_MIN_STEP_MS * 1000) {
        return false;
    }
    int err = s.target - luma;
    if (err > -AE_DEADZONE && err < AE_DEADZONE) {
        return false;                    /* 死区内不动 */
    }
    int exp_min = (AE_FLICKER_STEP > 0) ? AE_FLICKER_STEP : AE_EXP_MIN;
    if (s.exp_max > 0 && exp_min > s.exp_max) {
        exp_min = s.exp_max;             /* 帧率极高时上限可能低于量子（保险） */
    }

    /* 乘法律：期望的亮度比例 = 目标/当前，单步限幅 0.5x~2x */
    int pct = clampi(s.target * 100 / (luma > 0 ? luma : 1), AE_PCT_MIN, AE_PCT_MAX);

    int exp = s.cur_exp;
    int gain = s.cur_gain;
    if (err > 0) {
        /* 太暗：先加曝光（不增噪声），曝光到顶再加增益 */
        if (s.exp_max <= 0 || exp < s.exp_max) {
            exp = clampi((int)(((int64_t)exp * pct) / 100), exp + AE_EXP_MIN_STEP,
                         s.exp_max > 0 ? s.exp_max : exp + AE_EXP_MIN_STEP);
        } else if (gain < s.gain_max) {
            gain = clampi((int)(((int64_t)gain * pct) / 100), gain + AE_GAIN_MIN_STEP, s.gain_max);
        } else {
            return false;                /* 已到顶：暗就暗吧，不再抖 */
        }
    } else {
        /* 太亮：先降增益（噪声优先），增益到底再降曝光 */
        if (gain > 0) {
            gain = clampi((int)(((int64_t)gain * pct) / 100), 0, gain - AE_GAIN_MIN_STEP);
        } else if (exp > exp_min) {
            exp = clampi((int)(((int64_t)exp * pct) / 100), exp_min, exp - AE_EXP_MIN_STEP);
        } else {
            return false;
        }
    }

    if (AE_FLICKER_STEP > 0 && exp > AE_FLICKER_STEP) {
        exp = ((exp + AE_FLICKER_STEP / 2) / AE_FLICKER_STEP) * AE_FLICKER_STEP;
    }
    exp = clampi(exp, exp_min, s.exp_max > 0 ? s.exp_max : exp);
    gain = clampi(gain, 0, s.gain_max > 0 ? s.gain_max : gain);
    if (exp == s.cur_exp && gain == s.cur_gain) {
        return false;                    /* 限幅后无变化（量化导致），不发请求 */
    }

    app_camera_post_exp_gain(exp, gain);
    app_imgpipe_reset_prev();            /* 亮度跳变 -> 降噪/运动参考帧作废 */
    ESP_LOGI(TAG, "AE %s: luma %d tgt %d -> exp %d gain %d",
             err > 0 ? "up" : "down", luma, s.target, exp, gain);
    app_usb_link_log("camauto: AE %s luma %d -> exp %d gain %d",
                     err > 0 ? "up" : "down", luma, exp, gain);
    s.cur_exp = exp;
    s.cur_gain = gain;
    s.last_step_us = now;
    return true;
}

/* AWB 单步（灰世界）
 * 两种口径：
 *   软件路径：统计在软件 WB 之前采样 -> 目标增益 = G/R、G/B（绝对灰世界）
 *   ISP 路径：硬件 WBG 在统计上游已生效     -> 在当前增益上做相对修正 kr*G/R
 */
static void awb_step(int64_t now, int r, int g, int b, int luma)
{
    if (now - s.last_awb_us < AWB_TICK_MS * 1000) {
        return;
    }
    s.last_awb_us = now;
    if (!s.kr_valid) {
        s.kr_valid = true;               /* s.kr/kb 在 init/apply 已有正确值 */
        return;                          /* 第一拍只建立基准 */
    }
    if (luma < CONFIG_APP_CAM_AWB_MIN_LUMA || r < 8 || b < 8) {
        return;                          /* 暗帧/极端值不可信，保持现状 */
    }
    int kr_t, kb_t;
    if (wb_via_isp()) {
        kr_t = clampi((int)(((int64_t)s.kr * g) / r), AWB_KR_MIN, AWB_KR_MAX);
        kb_t = clampi((int)(((int64_t)s.kb * g) / b), AWB_KR_MIN, AWB_KR_MAX);
    } else {
        kr_t = clampi(g * 1000 / r, AWB_KR_MIN, AWB_KR_MAX);
        kb_t = clampi(g * 1000 / b, AWB_KR_MIN, AWB_KR_MAX);
    }

    /* EMA 平滑后过死区：场景本身带颜色时靠慢速+死区避免颜色 Pumping */
    int kr_n = s.kr + ((kr_t - s.kr) >> AWB_EMA_SHIFT);
    int kb_n = s.kb + ((kb_t - s.kb) >> AWB_EMA_SHIFT);
    if (abs(kr_n - s.kr) < AWB_DEADZONE && abs(kb_n - s.kb) < AWB_DEADZONE) {
        return;
    }
    app_camauto_wb_apply(kr_n, kb_n, false);
    ESP_LOGD(TAG, "AWB -> kr %d kb %d (r%d g%d b%d luma %d)", s.kr, s.kb, r, g, b, luma);
}

void app_camauto_tick(int cur_fps, int cur_gain, int exp_max, int gain_max,
                      int exp_cap_lines)
{
    if (!s.ae_on && !s.awb_on && !s.enh_on) {
        return;
    }
    if (cur_fps > 0) s.fps = cur_fps;
    if (exp_max > 0) s.exp_max = exp_max;
    if (gain_max > 0) s.gain_max = gain_max;
    /* AE 曝光上限（2026-10-06，Kconfig APP_CAM_AE_EXP_MAX_MS / cam 命令
     * ae_exp_max_ms）：钳住引擎的曝光天花板——超过上限转提增益保锐度。
     * 只钳 AE 自己的写入；手动 exp 与夜视 boost（走信箱直写）不经这里。 */
    if (exp_cap_lines > 0 && (s.exp_max <= 0 || s.exp_max > exp_cap_lines)) {
        s.exp_max = exp_cap_lines;
    }

    int64_t now = esp_timer_get_time();
    if (now - s.last_tick_us < AE_TICK_MS * 1000) {
        return;
    }
    s.last_tick_us = now;

    /* 只对板载摄像头闭环；PC 送帧模式没有可控制的曝光 */
    if (app_imgpipe_get_src() != APP_IMG_SRC_CAM) {
        return;
    }
    app_imgpipe_stats_t st;
    app_imgpipe_get_stats(&st);
    if (st.ae_stats_us == 0 || now - st.ae_stats_us > 500 * 1000) {
        return;                          /* 没有新鲜采样（流停了/未开采样），不盲调 */
    }

    /* 夜视增强：不依赖 AE 开关（手动 Night 档 AE 关着也要自适应），
       也不依赖引擎视角（BF 分档只看当前增益缓存） */
    if (s.enh_on && !s.enh_manual) {
        enh_step(now, (int)st.ae_luma, cur_gain);
    }

    if (!s.ae_on && !s.awb_on) {
        return;
    }
    view_seed_once();
    if (!s.view_valid) {
        return;                          /* 读不到就先不闭环 */
    }
    if (s.ae_on) {
        ae_step(now, (int)st.ae_luma);
    }
    if (s.awb_on) {
        awb_step(now, (int)st.ae_r, (int)st.ae_g, (int)st.ae_b, (int)st.ae_luma);
    }
}
