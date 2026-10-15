/*
 * ui_shared.c —— 同源 UI 实现（纯 LVGL v9）
 *
 * 布局（弹性列，480x800 竖屏）：
 *   [状态栏 30 + 模式徽标] [预览 flex_grow（等比 letterbox + 检测框）]
 *   [结果 16] [事件 16] [按钮 5 行 x38：识别x2 / 模式 / 帧率 / 画质]
 *   [gain/exp 滑块 28x2] [名单 14] [日志 56]
 * JPEG 解码用 lvgl 内置的 tjpgd（直接解码到自管 RGB888 缓冲），
 * 不走 LVGL 的 FS/MEMFS 解码路径 —— 避免依赖 lv_conf 的 FS 开关。
 */
#include "ui_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl/src/libs/tjpgd/tjpgd.h"

#define MAX_BOXES       6
#define LOG_LINES       3
#define UI_NAME_MAX     24

#define C_BG            0x0E1116
#define C_PANEL         0x1A2029
#define C_ACCENT        0x22C1A4
#define C_NIGHT         0xE8A33D    /* 夜视档徽标（琥珀） */
#define C_WARN          0xE5534B
#define C_TEXT          0xE6EDF3
#define C_DIM           0x8B949E

static struct {
    ui_shared_action_cb_t action;
    void *arg;

    /* 图像 */
    uint8_t  *rgb;
    size_t    rgb_cap;
    int       rgb_w, rgb_h, rgb_stride;
    lv_image_dsc_t dsc;
    lv_obj_t *img;
    lv_obj_t *img_holder;
    lv_obj_t *box[MAX_BOXES];
    lv_obj_t *box_label[MAX_BOXES];
    char      box_name[MAX_BOXES][UI_NAME_MAX]; /* F7/F8: 每框名字（boxes 第 5 位） */
    float     box_sim[MAX_BOXES];               /* F7/F8: 每框相似度（boxes 第 6 位） */
    lv_obj_t *no_signal;
    float     disp_scale;      /* 预览统一缩放（原图 px -> 显示 px），检测框定位用 */
    float     disp_ox, disp_oy; /* letterbox 偏移 */

    /* 状态栏 / 结果 / 名单 / 日志 */
    lv_obj_t *lbl_link, *lbl_mode, *lbl_stream, *lbl_result, *lbl_event, *lbl_list, *lbl_log;
    int       night_on, night_tier;   /* 板端夜视阶梯状态（徽标显示用；未收到字段前 0=不显示） */

    /* 摄像头参数（滑块 + 快捷按钮） */
    lv_obj_t *sl_gain, *sl_exp, *lbl_gain, *lbl_exp, *btn_mirror, *lbl_mirror, *lbl_wb;
    bool      wb_on;
    int       gain, gain_max, exp_val, exp_max;
    bool      dragging;        /* 拖动中：不要用结果帧回写滑块位置 */
    bool      mirror_on;

    /* 时域降噪档（循环按钮） */
    lv_obj_t *lbl_dn;
    int       dn;

    /* 自动帧率（运动自适应 15/50fps）*/
    lv_obj_t *lbl_auto;
    bool      auto_on;

    /* AE/AWB 自动模式（板端 app_camauto 闭环） */
    lv_obj_t *lbl_ae, *lbl_awb;
    bool      ae_on, awb_on;

    /* 按钮 */
    lv_obj_t *btn_src, *lbl_src, *btn_stream, *lbl_stream_btn;
    lv_obj_t *btn_autoall;     /* "Auto All"：一键回全自动，全自动时高亮 */

    /* 弹窗输入 */
    lv_obj_t *kb;
    lv_obj_t *mbox;
    /* F8：注册弹窗（shots 选择 + 采集进度态） */
    lv_obj_t *ta_name;             /* 名字输入框 */
    lv_obj_t *btn_ok, *btn_cancel; /* 弹窗按钮（注册中要隐藏 OK / Cancel 改发 stop） */
    lv_obj_t *row_shots;           /* shots 选择行（1/3/5） */
    lv_obj_t *lbl_prog;            /* 采集进度标签 */
    lv_obj_t *bar_prog;            /* 采集进度条 */
    int       reg_shots;           /* 选中的采集帧数（1/3/5，默认 3） */
    char      reg_cmd[16];         /* 弹窗命令（register/delete） */
    bool      enrolling;           /* 弹窗处于@@Q@@注册中@@Q@@态（进度显示，Cancel=stop） */

    bool connected;
    bool stream_on;
    const char *src_mode;      /* "cam" / "pc" */
    char lib_text[256];
    char log_text[512];
    char ev_text[128];
} s_ui = {
    .src_mode = "cam",
    .stream_on = true,
    .gain = 72,  .gain_max = 192,
    .exp_val = 519, .exp_max = 994,
};

/* ---------------- 极简 JSON 取值（板端输出是 cJSON 紧凑格式） ---------------- */
static const char *json_value(const char *js, const char *key)
{
    if (!js) return NULL;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? p + strlen(pat) : NULL;
}

static int json_int(const char *js, const char *key, int def)
{
    const char *v = json_value(js, key);
    return v ? atoi(v) : def;
}

static float json_float(const char *js, const char *key, float def)
{
    const char *v = json_value(js, key);
    return v ? (float)atof(v) : def;
}

static bool json_str(const char *js, const char *key, char *out, size_t out_sz)
{
    const char *v = json_value(js, key);
    if (!v || *v != '"') return false;
    v++;
    const char *e = strchr(v, '"');
    if (!e) return false;
    size_t n = (size_t)(e - v);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

/* boxes: [[x1,y1,x2,y2],...] 或 F7 扩展 [[x1,y1,x2,y2,\"name\",sim],...] -> 归一化到 0..1
 * （names/sims 可为 NULL：调用方不关心每框名字/相似度时只取坐标） */
static int json_boxes(const char *js, float out[][4], char names[][UI_NAME_MAX],
                      float sims[], int max)
{
    const char *p = json_value(js, "boxes");
    if (!p || *p != '[') return 0;
    p++;
    int n = 0;
    while (*p && n < max) {
        if (*p != '[') {
            p++;
            continue;
        }
        float v[4] = { 0, 0, 0, 0 };
        int k = 0;
        const char *q = p + 1;
        while (*q && *q != ']' && k < 4) {
            v[k++] = (float)atof(q);
            while (*q && *q != ',' && *q != ']') q++;
            if (*q == ',') q++;
        }
        if (k == 4) {
            memcpy(out[n], v, sizeof(v));
            if (names) names[n][0] = '\0';
            if (sims) sims[n] = 0;
            /* F7 扩展位：第 5 位名字（字符串）、第 6 位相似度（数字） */
            while (*q && *q != ']') {
                if (*q == '\"') {
                    q++;
                    const char *e = strchr(q, '\"');
                    if (!e) break;
                    size_t len = (size_t)(e - q);
                    if (names) {
                        if (len >= UI_NAME_MAX) len = UI_NAME_MAX - 1;
                        memcpy(names[n], q, len);
                        names[n][len] = '\0';
                    }
                    q = e + 1;
                } else if (*q == ',') {
                    q++;
                    if (*q == '\"') continue;
                    if (sims) sims[n] = (float)atof(q);
                    while (*q && *q != ',' && *q != ']') q++;
                } else {
                    q++;
                }
            }
            n++;
        }
        while (*q && *q != ']') q++;
        if (*q == ']') q++;
        p = q;
    }
    return n;
}

/* lib: ["a","b"] + lib_feats:[3,2]（F8 平行数组）-> "a(3), b(2)"（无则退化 "a, b"） */
static void json_lib(const char *js, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *p = strstr(js ? js : "", "\"lib\":[");
    if (!p) return;
    p += 7;
    char names[16][32];
    int  feats[16];
    int  n = 0;
    while (*p && *p != ']' && n < 16) {
        if (*p == '"') {
            p++;
            const char *e = strchr(p, '"');
            if (!e) break;
            size_t len = (size_t)(e - p);
            if (len >= sizeof(names[0])) len = sizeof(names[0]) - 1;
            memcpy(names[n], p, len);
            names[n][len] = '\0';
            feats[n] = -1;                       /* 无 lib_feats 时 -1 = 不显示计数 */
            n++;
            p = e + 1;
        } else {
            p++;
        }
    }
    const char *pf = strstr(js, "\"lib_feats\":[");
    if (pf) {
        pf += strlen("\"lib_feats\":[");  /* 14：模式串长度；偏移少加 2 会 atoi 到 :[ 得 0 */
        for (int i = 0; i < n && *pf && *pf != ']'; i++) {
            while (*pf == ',' || *pf == ' ') pf++;
            if (*pf == ']' || !*pf) break;
            feats[i] = atoi(pf);
            while (*pf && *pf != ',' && *pf != ']') pf++;
        }
    }
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        char item[44];
        int m = (feats[i] >= 0) ? snprintf(item, sizeof(item), "%s(%d)", names[i], feats[i])
                                : snprintf(item, sizeof(item), "%s", names[i]);
        if (used + (size_t)m + 3 < out_sz) {
            if (used) {
                out[used++] = ',';
                out[used++] = ' ';
            }
            memcpy(out + used, item, (size_t)m);
            used += (size_t)m;
            out[used] = '\0';
        }
    }
}

/* ---------------- 图像：tjpgd 解到自管 RGB888 ---------------- */
typedef struct {
    const uint8_t *data;
    size_t len, pos;
} jpg_src_t;

static struct {
    uint8_t *dst;
    int      stride;
} s_dec;

static size_t tjpgd_in(JDEC *jd, uint8_t *buf, size_t len)
{
    jpg_src_t *s = (jpg_src_t *)jd->device;
    size_t n = s->len - s->pos;
    if (n > len) n = len;
    if (buf) memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return n;
}

static int tjpgd_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const int w = rect->right - rect->left + 1;
    const uint8_t *src = (const uint8_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        uint8_t *dst = s_dec.dst + (size_t)y * s_dec.stride + (size_t)rect->left * 3;
        memcpy(dst, src, (size_t)w * 3);
        src += (size_t)w * 3;
    }
    return 1;
}

static bool ensure_rgb(int w, int h)
{
    size_t need = (size_t)w * h * 3;
    if (s_ui.rgb_cap >= need) return true;
    uint8_t *p = (uint8_t *)realloc(s_ui.rgb, need);
    if (!p) return false;
    s_ui.rgb = p;
    s_ui.rgb_cap = need;
    return true;
}

static bool decode_jpeg(const uint8_t *data, uint32_t len)
{
    static JDEC jd;
    static uint8_t pool[4096];
    jpg_src_t src = { data, len, 0 };

    if (jd_prepare(&jd, tjpgd_in, pool, sizeof(pool), &src) != JDR_OK) {
        return false;
    }
    if (!ensure_rgb(jd.width, jd.height)) {
        return false;
    }
    s_dec.dst = s_ui.rgb;
    s_dec.stride = jd.width * 3;
    if (jd_decomp(&jd, tjpgd_out, 0) != JDR_OK) {
        return false;
    }
    s_ui.rgb_w = jd.width;
    s_ui.rgb_h = jd.height;
    s_ui.rgb_stride = s_dec.stride;
    return true;
}

/* 检测框：按预览的统一缩放 + letterbox 偏移摆放（板端 box 是原图像素坐标）。
 * F8：有每框名字（F7 boxes 第 5/6 位）就画在该框上并带相似度；无则首框回退顶层 match。 */
static void update_boxes(const float box[][4], int n, const char *name)
{
    for (int i = 0; i < MAX_BOXES; i++) {
        if (i >= n || s_ui.rgb_w <= 0 || s_ui.disp_scale <= 0) {
            lv_obj_add_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int x1 = (int)(box[i][0] * s_ui.disp_scale + s_ui.disp_ox);
        int y1 = (int)(box[i][1] * s_ui.disp_scale + s_ui.disp_oy);
        int x2 = (int)(box[i][2] * s_ui.disp_scale + s_ui.disp_ox);
        int y2 = (int)(box[i][3] * s_ui.disp_scale + s_ui.disp_oy);
        if (x2 <= x1 || y2 <= y1) {
            lv_obj_add_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_ui.box[i], x1, y1);
        lv_obj_set_size(s_ui.box[i], x2 - x1, y2 - y1);

        const char *lbl = NULL;
        char buf[UI_NAME_MAX + 10];
        if (s_ui.box_name[i][0]) {
            if (s_ui.box_sim[i] > 0) {
                snprintf(buf, sizeof(buf), "%s %.2f", s_ui.box_name[i], s_ui.box_sim[i]);
                lbl = buf;
            } else {
                lbl = s_ui.box_name[i];
            }
        } else if (i == 0 && name && name[0]) {
            lbl = name;
        }
        if (lbl) {
            lv_label_set_text(s_ui.box_label[i], lbl);
            lv_obj_set_pos(s_ui.box_label[i], x1, y1 > 18 ? y1 - 18 : y1 + 2);
            lv_obj_remove_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---------------- 发送命令 ---------------- */
static void send_cmd(const char *json)
{
    if (s_ui.action) {
        s_ui.action(json, s_ui.arg);
    }
}

/* ---------------- 弹窗输入（注册/删除用） ---------------- */
static void close_input(void)
{
    if (s_ui.kb) {
        lv_obj_delete(s_ui.kb);
        s_ui.kb = NULL;
    }
    if (s_ui.mbox) {
        /* 必须用 lv_msgbox_close：lv_msgbox_create(NULL) 会在 lv_layer_top() 上自动建一块
         * 100%x100% 的可点击背板当 mbox 的父对象，直接 lv_obj_delete(mbox) 只删 mbox，
         * 全屏遮罩会永久残留（主 UI 被盖住 + 点不动）。lv_msgbox_close 会连父背板一起删。 */
        lv_msgbox_close(s_ui.mbox);
        s_ui.mbox = NULL;
    }
    /* 这些控件都是 mbox 的子对象，随 mbox 一起销毁 → 指针必须清空，别留着悬空 */
    s_ui.ta_name = NULL;
    s_ui.row_shots = NULL;
    s_ui.lbl_prog = NULL;
    s_ui.bar_prog = NULL;
    s_ui.btn_ok = NULL;
    s_ui.btn_cancel = NULL;
}

static void input_ok_cb(lv_event_t *e);
static void input_cancel_cb(lv_event_t *e);

/* F8：shots 单选（1/3/5）——点选即高亮并记入 s_ui.reg_shots */
static void shots_sel_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    s_ui.reg_shots = (int)(intptr_t)lv_obj_get_user_data(btn);
    uint32_t n = lv_obj_get_child_count(s_ui.row_shots);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(s_ui.row_shots, i);
        /* 只有三个选项按钮带 user_data；"Shots:" 标题标签没有 → 跳过 */
        if (!b || !lv_obj_get_user_data(b)) continue;
        bool on = (b == btn);
        lv_obj_set_style_bg_color(b, lv_color_hex(on ? C_ACCENT : C_PANEL), 0);
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(on ? C_BG : C_TEXT), 0);
        }
    }
}

/* 名字为空/纯空白？—— OK 置灰与提交都走这一个判据。
   用 > ' ' 判非空白，免 include ctype.h（本文件要与板端同源） */
static bool name_blank(const char *s)
{
    if (!s) return true;
    for (; *s; s++) {
        if ((unsigned char)*s > ' ') return false;
    }
    return true;
}

/* 输入框实时校验：空/纯空白 → OK 置灰不可点，有内容才放行（F8b） */
static void ta_changed_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *ok = (lv_obj_t *)lv_event_get_user_data(e);
    if (!ok) return;
    if (name_blank(lv_textarea_get_text(ta))) {
        lv_obj_add_state(ok, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ok, LV_STATE_DISABLED);
    }
}

/* 提交（OK 按钮与键盘右下 ✓ 共用）。名字为空/纯空白时什么都不做、弹窗保持打开 */
static void do_input_ok(const char *cmd, lv_obj_t *ta)
{
    const char *name = ta ? lv_textarea_get_text(ta) : NULL;
    if (name_blank(name)) return;

    char js[160];
    if (!strcmp(cmd, "register")) {
        /* F1：多帧注册（shots 由弹窗选择，1=旧行为） */
        snprintf(js, sizeof(js), "{\"cmd\":\"register\",\"id\":\"%s\",\"shots\":%d}",
                 name, s_ui.reg_shots);
    } else {
        snprintf(js, sizeof(js), "{\"cmd\":\"%s\",\"id\":\"%s\"}", cmd, name);
    }
    send_cmd(js);
    ui_shared_add_log(cmd[0] == 'r' ? "register: face the camera" : "delete sent");
    /* F8：多帧注册时弹窗转"注册中"态（收键盘/输入，显示进度条），
     * registered/error 事件自动关闭；Cancel 改发 stop（取消采集）。 */
    if (!strcmp(cmd, "register") && s_ui.reg_shots > 1 && s_ui.mbox) {
        s_ui.enrolling = true;
        if (s_ui.kb) {
            lv_obj_delete(s_ui.kb);
            s_ui.kb = NULL;
        }
        if (s_ui.ta_name) lv_obj_add_flag(s_ui.ta_name, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.row_shots) lv_obj_add_flag(s_ui.row_shots, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.btn_ok) lv_obj_add_flag(s_ui.btn_ok, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.lbl_prog) {
            lv_label_set_text_fmt(s_ui.lbl_prog, "collected 0/%d - stay in view", s_ui.reg_shots);
            lv_obj_remove_flag(s_ui.lbl_prog, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ui.bar_prog) {
            lv_bar_set_range(s_ui.bar_prog, 0, s_ui.reg_shots);
            lv_bar_set_value(s_ui.bar_prog, 0, LV_ANIM_OFF);
            lv_obj_remove_flag(s_ui.bar_prog, LV_OBJ_FLAG_HIDDEN);
        }
        return;                      /* 弹窗保持打开（进度态） */
    }
    close_input();
}

static void input_ok_cb(lv_event_t *e)
{
    do_input_ok((const char *)lv_obj_get_user_data(lv_event_get_target(e)),
                (lv_obj_t *)lv_event_get_user_data(e));
}

/* 键盘右下角的 ✓ 键 = OK（键盘自身不带 target 的 cmd，用弹窗记录的 reg_cmd） */
static void kb_ready_cb(lv_event_t *e)
{
    do_input_ok(s_ui.reg_cmd, (lv_obj_t *)lv_event_get_user_data(e));
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_ui.enrolling) {
        send_cmd("{\"cmd\":\"stop\"}");   /* 板端：stop 取消未完成的注册采集 */
        ui_shared_add_log("register: cancelled");
    }
    s_ui.enrolling = false;
    close_input();
}

static void open_input(const char *title, const char *cmd)
{
    close_input();
    s_ui.enrolling = false;
    s_ui.reg_shots = 3;
    snprintf(s_ui.reg_cmd, sizeof(s_ui.reg_cmd), "%s", cmd);
    s_ui.mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_ui.mbox, title);
    lv_obj_t *ta = lv_textarea_create(s_ui.mbox);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "name");
    lv_obj_set_width(ta, lv_pct(80));
    s_ui.ta_name = ta;

    lv_obj_t *ok = lv_msgbox_add_footer_button(s_ui.mbox, "OK");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(s_ui.mbox, "Cancel");
    lv_obj_set_user_data(ok, (void *)cmd);
    lv_obj_add_event_cb(ok, input_ok_cb, LV_EVENT_CLICKED, ta);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);
    /* F8b：输入框为空/纯空白时 OK 置灰（点不动 = 提交不了空名字） */
    lv_obj_add_state(ok, LV_STATE_DISABLED);
    lv_obj_add_event_cb(ta, ta_changed_cb, LV_EVENT_VALUE_CHANGED, ok);
    s_ui.btn_ok = ok;
    s_ui.btn_cancel = cancel;

    /* F8：shots 选择行（1/3/5，仅注册；默认 3 高亮）+ 进度控件（初始隐藏） */
    s_ui.row_shots = NULL;
    s_ui.lbl_prog = NULL;
    s_ui.bar_prog = NULL;
    if (!strcmp(cmd, "register")) {
        static const char *lbls[3] = { "1", "3", "5" };
        static const int vals[3] = { 1, 3, 5 };
        s_ui.row_shots = lv_obj_create(s_ui.mbox);
        lv_obj_set_size(s_ui.row_shots, lv_pct(100), 36);
        lv_obj_set_flex_flow(s_ui.row_shots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_ui.row_shots, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_ui.row_shots, 6, 0);
        lv_obj_set_style_border_width(s_ui.row_shots, 0, 0);
        lv_obj_set_style_bg_opa(s_ui.row_shots, LV_OPA_TRANSP, 0);
        /* F8b：这组按钮 = 注册时采集的人脸帧数（直接发给板端的 shots 参数）——
           裸数字"1/3/5"看不出含义，故加英文标题 */
        lv_obj_t *l_cap = lv_label_create(s_ui.row_shots);
        lv_label_set_text(l_cap, "Shots:");
        lv_obj_set_style_text_color(l_cap, lv_color_hex(C_DIM), 0);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = lv_button_create(s_ui.row_shots);
            lv_obj_set_size(b, 50, 30);
            lv_obj_set_user_data(b, (void *)(intptr_t)vals[i]);
            lv_obj_add_event_cb(b, shots_sel_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, lbls[i]);
            lv_obj_center(l);
            if (vals[i] == s_ui.reg_shots) {
                lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
                lv_obj_set_style_text_color(l, lv_color_hex(C_BG), 0);
            }
        }
        s_ui.lbl_prog = lv_label_create(s_ui.mbox);
        lv_label_set_text(s_ui.lbl_prog, "");
        lv_obj_add_flag(s_ui.lbl_prog, LV_OBJ_FLAG_HIDDEN);
        s_ui.bar_prog = lv_bar_create(s_ui.mbox);
        lv_obj_set_size(s_ui.bar_prog, lv_pct(80), 10);
        lv_obj_add_flag(s_ui.bar_prog, LV_OBJ_FLAG_HIDDEN);
    }

    /* 键盘必须建在 lv_layer_top()（与 msgbox 自动背板同层、且创建在后）：
       msgbox_create(NULL) 的背板是全屏可点击对象，建在屏幕层的键盘会被它整个吞掉点击 */
    s_ui.kb = lv_keyboard_create(lv_layer_top());
    lv_keyboard_set_textarea(s_ui.kb, ta);
    lv_obj_set_size(s_ui.kb, lv_pct(100), 240);
    lv_obj_align(s_ui.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(s_ui.kb);
    lv_obj_add_event_cb(s_ui.kb, kb_ready_cb, LV_EVENT_READY, ta);  /* ✓ 键 = OK */
}

/* ---------------- 增益/曝光滑块（松手才发命令，避免拖动时刷屏） ---------------- */
static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    bool is_gain = (sl == s_ui.sl_gain);
    lv_label_set_text_fmt(is_gain ? s_ui.lbl_gain : s_ui.lbl_exp,
                          is_gain ? "gain %d" : "exp %d", (int)lv_slider_get_value(sl));
}

/* 滑块标签统一刷新：AE 开着时带 "(auto)" 后缀（三个写标签的地方都走这里，
   否则结果帧回写会把后缀抹掉） */
static void slider_label_refresh(void)
{
    const char *sfx = s_ui.ae_on ? " (auto)" : "";
    if (s_ui.lbl_gain) {
        lv_label_set_text_fmt(s_ui.lbl_gain, "gain %d%s", s_ui.gain, sfx);
    }
    if (s_ui.lbl_exp) {
        lv_label_set_text_fmt(s_ui.lbl_exp, "exp %d%s", s_ui.exp_val, sfx);
    }
}

/* AE 自动曝光开着 -> 滑块置灰（视觉提示"自动中"，仍可点按） */
static void sliders_auto_hint(bool ae_on)
{
    lv_obj_t *sls[2] = { s_ui.sl_gain, s_ui.sl_exp };
    for (int i = 0; i < 2; i++) {
        if (!sls[i]) continue;
        if (ae_on) {
            lv_obj_add_state(sls[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(sls[i], LV_STATE_DISABLED);
        }
    }
    slider_label_refresh();
}

static void slider_pressed_cb(lv_event_t *e)
{
    (void)e;
    /* AE 开着时用户动滑块 = 要手动接管：先退 AE（板端手动优先本来也会退，
       这里让 UI 的 AE 按钮与置灰状态立即跟上），再正常拖动 */
    if (s_ui.ae_on) {
        s_ui.ae_on = false;
        if (s_ui.lbl_ae) {
            lv_label_set_text(s_ui.lbl_ae, "AE OFF");
        }
        sliders_auto_hint(false);
        send_cmd("{\"cmd\":\"cam\",\"ae\":false}");
        ui_shared_add_log("manual slider -> AE off");
    }
    s_ui.dragging = true;
}

static void slider_released_cb(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    s_ui.dragging = false;
    int v = (int)lv_slider_get_value(sl);
    char js[64];
    if (sl == s_ui.sl_gain) {
        s_ui.gain = v;
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"gain\":%d}", v);
    } else {
        s_ui.exp_val = v;
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"exp\":%d}", v);
    }
    send_cmd(js);
}

/* 造一行：[标签] [滑块]，返回滑块对象 */
static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *label_text, int min, int max,
                                 int val, lv_obj_t **out_lbl)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text_fmt(lbl, "%s %d", label_text, val);
    lv_obj_set_width(lbl, 120);     /* 容得下 "exp 500 (auto)" 不换行 */
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), 0);
    if (out_lbl) *out_lbl = lbl;

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_flex_grow(sl, 1);
    lv_obj_set_height(sl, 14);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, slider_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(sl, slider_released_cb, LV_EVENT_RELEASED, NULL);
    return sl;
}

/* ---------------- 按钮回调 ---------------- */
static void btn_event_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
    if (!id) return;

    if (!strcmp(id, "start")) {
        send_cmd("{\"cmd\":\"start\"}");
    } else if (!strcmp(id, "stop")) {
        send_cmd("{\"cmd\":\"stop\"}");
    } else if (!strcmp(id, "list")) {
        send_cmd("{\"cmd\":\"list\"}");
    } else if (!strcmp(id, "clear")) {
        send_cmd("{\"cmd\":\"clear\"}");
        lv_label_set_text(s_ui.lbl_list, "lib: (cleared)");
    } else if (!strcmp(id, "register")) {
        open_input("Register face", "register");
    } else if (!strcmp(id, "delete")) {
        open_input("Delete person", "delete");
    } else if (!strcmp(id, "src")) {
        bool to_pc = !strcmp(s_ui.src_mode, "cam");
        const char *v = to_pc ? "pc" : "cam";
        char js[64];
        snprintf(js, sizeof(js), "{\"cmd\":\"src\",\"value\":\"%s\"}", v);
        send_cmd(js);
        s_ui.src_mode = v;
        lv_label_set_text(s_ui.lbl_src, to_pc ? "PC frame" : "Camera");
        ui_shared_add_log(to_pc ? "src -> pc (PC frames)" : "src -> cam (onboard camera)");
    } else if (!strncmp(id, "fps", 3)) {
        /* 帧率档位：板端降 VTS 换更长曝光，并自动把 exp/gain 拉到新上限；
           手动改帧率时板端会自动退出自动模式，这里同步按钮状态 */
        int f = atoi(id + 3);
        char js[112];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"fps\":%d,\"auto_exp\":true,\"auto_fps\":false}", f);
        send_cmd(js);
        if (s_ui.auto_on) {
            s_ui.auto_on = false;
            if (s_ui.lbl_auto) {
                lv_label_set_text(s_ui.lbl_auto, "AUTO OFF");
            }
        }
        ui_shared_add_log("fps -> night shift (exp/gain auto max, auto off)");
    } else if (!strcmp(id, "auto")) {
        /* 自动帧率开关（夜视档专用）：运动自动切 50fps，静止回落夜视阶梯档；
           白天（tier0）恒定 50fps，此开关在白天没有效果（低光照逻辑不上身） */
        s_ui.auto_on = !s_ui.auto_on;
        char js[96];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"auto_fps\":%s}", s_ui.auto_on ? "true" : "false");
        send_cmd(js);
        if (s_ui.lbl_auto) {
            lv_label_set_text(s_ui.lbl_auto, s_ui.auto_on ? "AUTO ON" : "AUTO OFF");
        }
        ui_shared_add_log(s_ui.auto_on ? "auto-fps ON (night: motion 50fps <-> ladder idle; day: fixed 50)"
                                       : "auto-fps OFF");
    } else if (!strcmp(id, "dn")) {
        /* 时域降噪循环档：Off -> Low -> Mid -> High -> Off
           （板端对静止区域做多帧加权平均，运动区域自动跳过；高帧率下会被守卫跳过）*/
        static const char *nm[4] = { "Off", "Low", "Mid", "High" };
        s_ui.dn = (s_ui.dn + 1) & 3;
        char js[64];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"dn\":%d}", s_ui.dn);
        send_cmd(js);
        if (s_ui.lbl_dn) {
            lv_label_set_text_fmt(s_ui.lbl_dn, "DN %s", nm[s_ui.dn]);
        }
        char lg[80];
        snprintf(lg, sizeof(lg), "denoise -> %s (temporal, static areas)", nm[s_ui.dn]);
        ui_shared_add_log(lg);
    } else if (!strcmp(id, "night_lo")) {
        /* 夜视·低噪：降帧率换 100ms 曝光上限 + 曝光拉满，但增益只给 ~2/3
           （满增益会把暗部噪声一起放大；这条档位是"亮且相对干净"） */
        int g = s_ui.gain_max > 0 ? (int)(s_ui.gain_max * 0.62f) : 120;
        char js[112];
        snprintf(js, sizeof(js),
                 "{\"cmd\":\"cam\",\"fps\":10,\"auto_exp\":true,\"gain\":%d}", g);
        send_cmd(js);
        ui_shared_add_log("night(low noise): 10fps, exp max, gain ~62%");
    } else if (!strcmp(id, "night")) {
        char js[80];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"gain\":%d,\"exp\":%d}",
                 s_ui.gain_max, s_ui.exp_max);
        send_cmd(js);
        ui_shared_add_log("night mode: gain/exp = max");
    } else if (!strcmp(id, "day")) {
        send_cmd("{\"cmd\":\"cam\",\"gain\":72,\"exp\":519}");
        ui_shared_add_log("day mode: gain 72 / exp 519");
    } else if (!strcmp(id, "autoall")) {
        /* 一键回全自动（光照双模式的默认工作方式）：夜视阶梯 + 运动自适应 + AE/AWB 全开。
           按钮高亮由结果帧的 night/auto/ae/awb 驱动（见 ui_shared_set_result），
           这里不手动置色，避免与板端实际状态不一致。 */
        send_cmd("{\"cmd\":\"cam\",\"night\":true,\"auto_fps\":true,\"ae\":true,\"awb\":true}");
        ui_shared_add_log("auto all: night ladder + auto-fps + AE/AWB on");
    } else if (!strcmp(id, "ae")) {
        /* 自动曝光开关（板端 app 级闭环：帧亮度 -> exp/gain；手动拖滑块会自动退出） */
        s_ui.ae_on = !s_ui.ae_on;
        char js[72];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"ae\":%s}", s_ui.ae_on ? "true" : "false");
        send_cmd(js);
        if (s_ui.lbl_ae) {
            lv_label_set_text(s_ui.lbl_ae, s_ui.ae_on ? "AE ON" : "AE OFF");
        }
        ui_shared_add_log(s_ui.ae_on ? "auto exposure ON (target luma loop)"
                                     : "auto exposure OFF");
    } else if (!strcmp(id, "awb")) {
        /* 自动白平衡开关（板端灰世界 -> 软件白平衡 kr/kb；手动 WB 会自动退出） */
        s_ui.awb_on = !s_ui.awb_on;
        char js[76];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"awb\":%s}", s_ui.awb_on ? "true" : "false");
        send_cmd(js);
        if (s_ui.lbl_awb) {
            lv_label_set_text(s_ui.lbl_awb, s_ui.awb_on ? "AWB ON" : "AWB OFF");
        }
        ui_shared_add_log(s_ui.awb_on ? "auto white balance ON (gray world)"
                                      : "auto white balance OFF");
    } else if (!strcmp(id, "wb")) {
        /* 软件白平衡开关（预设值与板端 Kconfig 默认一致：1320/1645） */
        s_ui.wb_on = !s_ui.wb_on;
        char js[96];
        if (s_ui.wb_on) {
            snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"wb_kr\":1320,\"wb_kb\":1645}");
        } else {
            snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"wb_kr\":1000,\"wb_kb\":1000}");
        }
        send_cmd(js);
        lv_label_set_text(s_ui.lbl_wb, s_ui.wb_on ? "WB ON" : "WB OFF");
        ui_shared_add_log(s_ui.wb_on ? "software WB on (1320/1645)" : "software WB off");
    } else if (!strcmp(id, "mirror")) {
        s_ui.mirror_on = !s_ui.mirror_on;
        char js[64];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"mirror\":%s}", s_ui.mirror_on ? "true" : "false");
        send_cmd(js);
        lv_label_set_text(s_ui.lbl_mirror, s_ui.mirror_on ? "Mirror ON" : "Mirror OFF");
        ui_shared_add_log(s_ui.mirror_on ? "mirror on" : "mirror off");
    } else if (!strcmp(id, "stream")) {
        s_ui.stream_on = !s_ui.stream_on;
        char js[80];
        snprintf(js, sizeof(js), "{\"cmd\":\"stream\",\"value\":\"%s\",\"fps\":10}",
                 s_ui.stream_on ? "on" : "off");
        send_cmd(js);
        lv_label_set_text(s_ui.lbl_stream_btn, s_ui.stream_on ? "Preview ON" : "Preview OFF");
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, const char *id, lv_obj_t **out_lbl)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 38);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_center(l);
    lv_obj_set_user_data(b, (void *)id);
    lv_obj_add_event_cb(b, btn_event_cb, LV_EVENT_CLICKED, NULL);
    if (out_lbl) *out_lbl = l;
    return b;
}

/* ---------------- 初始化 ---------------- */
void ui_shared_init(void)
{
    s_ui.mirror_on = true;      /* 与板端 CONFIG_APP_CAM_HFLIP=y 的默认一致 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 6, 0);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 4, 0);

    /* --- 状态栏：链路 | 模式徽标 | 图像源 --- */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    s_ui.lbl_link = lv_label_create(bar);
    lv_label_set_text(s_ui.lbl_link, "offline");
    lv_obj_set_style_text_color(s_ui.lbl_link, lv_color_hex(C_WARN), 0);
    s_ui.lbl_mode = lv_label_create(bar);
    lv_label_set_text(s_ui.lbl_mode, "");
    lv_obj_set_style_text_color(s_ui.lbl_mode, lv_color_hex(C_ACCENT), 0);
    s_ui.lbl_stream = lv_label_create(bar);
    lv_label_set_text(s_ui.lbl_stream, "-");
    lv_obj_set_style_text_color(s_ui.lbl_stream, lv_color_hex(C_DIM), 0);

    /* --- 预览区：弹性占满剩余高度，图像等比 letterbox 居中 --- */
    s_ui.img_holder = lv_obj_create(root);
    lv_obj_set_width(s_ui.img_holder, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.img_holder, 1);
    lv_obj_set_style_bg_color(s_ui.img_holder, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_ui.img_holder, 0, 0);
    lv_obj_set_style_radius(s_ui.img_holder, 4, 0);
    lv_obj_set_style_pad_all(s_ui.img_holder, 0, 0);
    lv_obj_remove_flag(s_ui.img_holder, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.img = lv_image_create(s_ui.img_holder);
    lv_obj_center(s_ui.img);

    s_ui.no_signal = lv_label_create(s_ui.img_holder);
    lv_label_set_text(s_ui.no_signal, "waiting for image...\n(press Preview ON + Camera)");
    lv_obj_set_style_text_align(s_ui.no_signal, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.no_signal, lv_color_hex(C_DIM), 0);
    lv_obj_center(s_ui.no_signal);

    for (int i = 0; i < MAX_BOXES; i++) {
        s_ui.box[i] = lv_obj_create(s_ui.img_holder);
        lv_obj_set_style_bg_opa(s_ui.box[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_ui.box[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_width(s_ui.box[i], 2, 0);
        lv_obj_set_style_radius(s_ui.box[i], 2, 0);
        lv_obj_set_style_pad_all(s_ui.box[i], 0, 0);
        lv_obj_add_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.box[i], LV_OBJ_FLAG_SCROLLABLE);

        s_ui.box_label[i] = lv_label_create(s_ui.img_holder);
        lv_label_set_text(s_ui.box_label[i], "");
        lv_obj_set_style_text_color(s_ui.box_label[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_color(s_ui.box_label[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_ui.box_label[i], LV_OPA_60, 0);
        lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* --- 结果行 --- */
    s_ui.lbl_result = lv_label_create(root);
    lv_label_set_text(s_ui.lbl_result, "faces -");
    lv_obj_set_style_text_color(s_ui.lbl_result, lv_color_hex(C_TEXT), 0);
    s_ui.lbl_event = lv_label_create(root);
    lv_label_set_text(s_ui.lbl_event, "");
    lv_obj_set_style_text_color(s_ui.lbl_event, lv_color_hex(C_ACCENT), 0);

    /* --- 按钮：按功能分组（识别 x2 / 模式 / 帧率 / 画质） --- */
    for (int row = 0; row < 5; row++) {
        lv_obj_t *r = lv_obj_create(root);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, 38);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_set_style_pad_column(r, 4, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        switch (row) {
        case 0:                                    /* 识别操作 */
            make_btn(r, "Start", "start", NULL);
            make_btn(r, "Stop", "stop", NULL);
            make_btn(r, "Register", "register", NULL);
            make_btn(r, "Delete", "delete", NULL);
            break;
        case 1:
            make_btn(r, "List", "list", NULL);
            make_btn(r, "Clear", "clear", NULL);
            make_btn(r, "Camera", "src", &s_ui.lbl_src);
            make_btn(r, "Preview ON", "stream", &s_ui.lbl_stream_btn);
            break;
        case 2:                                    /* 模式：一键全自动 + 手动预设 */
            /* Auto All = 夜视阶梯 + 运动自适应 + AE/AWB 全开（光照双模式的默认工作方式） */
            s_ui.btn_autoall = make_btn(r, "Auto All", "autoall", NULL);
            /* Night Hi = 10fps + exp max + gain max（最亮）；Night Lo = 同上但增益 ~62%（低噪）*/
            make_btn(r, "Night Hi", "night", NULL);
            make_btn(r, "Night Lo", "night_lo", NULL);
            make_btn(r, "Day", "day", NULL);
            break;
        case 3:                                    /* 帧率：帧率越低 -> 曝光上限越高（夜视长曝光） */
            make_btn(r, "50fps", "fps50", NULL);
            make_btn(r, "25fps", "fps25", NULL);
            make_btn(r, "15fps", "fps15", NULL);
            /* 自动帧率开关（运动自适应）：静止由夜视阶梯决定，检测到运动自动切 50fps */
            make_btn(r, "AUTO OFF", "auto", &s_ui.lbl_auto);
            break;
        default:                                   /* 画质：镜像/白平衡/自动曝光/自动白平衡/降噪 */
            make_btn(r, "Mirror ON", "mirror", &s_ui.lbl_mirror);
            make_btn(r, "WB ON", "wb", &s_ui.lbl_wb);
            make_btn(r, "AE OFF", "ae", &s_ui.lbl_ae);
            make_btn(r, "AWB OFF", "awb", &s_ui.lbl_awb);
            /* 时域降噪循环档：点击在 Off -> Low -> Mid -> High 之间循环 */
            make_btn(r, "DN Off", "dn", &s_ui.lbl_dn);
            break;
        }
    }

    /* --- 摄像头参数：增益 / 曝光 滑块（AE 开时置灰提示"自动中"） --- */
    s_ui.sl_gain = make_slider_row(root, "gain", 0, s_ui.gain_max, s_ui.gain, &s_ui.lbl_gain);
    s_ui.sl_exp = make_slider_row(root, "exp", 8, s_ui.exp_max, s_ui.exp_val, &s_ui.lbl_exp);

    /* --- 名单 --- */
    s_ui.lbl_list = lv_label_create(root);
    lv_obj_set_width(s_ui.lbl_list, lv_pct(100));
    lv_obj_set_height(s_ui.lbl_list, 14);
    lv_label_set_long_mode(s_ui.lbl_list, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_ui.lbl_list, "lib: -");
    lv_obj_set_style_text_color(s_ui.lbl_list, lv_color_hex(C_DIM), 0);

    /* --- 日志 --- */
    s_ui.lbl_log = lv_label_create(root);
    lv_obj_set_width(s_ui.lbl_log, lv_pct(100));
    lv_obj_set_height(s_ui.lbl_log, 56);
    lv_label_set_long_mode(s_ui.lbl_log, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.lbl_log, "");
    lv_obj_set_style_text_color(s_ui.lbl_log, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(s_ui.lbl_log, &lv_font_montserrat_14, 0);

    sliders_auto_hint(s_ui.ae_on);      /* AE 默认开：滑块初始即置灰提示 */
}

void ui_shared_set_action_cb(ui_shared_action_cb_t cb, void *arg)
{
    s_ui.action = cb;
    s_ui.arg = arg;
}

/* ---------------- 与链路对接 ---------------- */
void ui_shared_set_link(bool connected, const char *detail)
{
    bool was = s_ui.connected;
    s_ui.connected = connected;
    lv_label_set_text_fmt(s_ui.lbl_link, "%s %s", connected ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                          detail ? detail : "");
    lv_obj_set_style_text_color(s_ui.lbl_link, lv_color_hex(connected ? C_ACCENT : C_WARN), 0);

    if (connected && !was) {
        /* 刚连上：自动开检测 + 板载摄像头 + 画面回传（相当于开机默认界面） */
        ui_shared_add_log("connected -> auto start / cam / stream on");
        send_cmd("{\"cmd\":\"start\"}");
        send_cmd("{\"cmd\":\"src\",\"value\":\"cam\"}");
        /* 明确要 JPEG：raw(614KB/帧) 是排查用的，UI 用不上且很吃带宽 */
        send_cmd("{\"cmd\":\"stream\",\"value\":\"on\",\"fps\":10,\"fmt\":\"jpeg\"}");
        /* 立刻同步一次板端状态：ack 会带 gain/exp/mirror/wb/ae/awb/auto/dn/night/ntier，
           滑块位置、开关按钮、模式徽标不用等用户手动发命令就对齐 */
        send_cmd("{\"cmd\":\"cam\"}");
        lv_label_set_text(s_ui.lbl_src, "Camera");
        lv_label_set_text(s_ui.lbl_stream_btn, "Preview ON");
        s_ui.stream_on = true;
        s_ui.src_mode = "cam";
    }
}

void ui_shared_set_image(const uint8_t *data, uint32_t len, int fmt, int width, int height)
{
    if (!data || len == 0) return;

    if (fmt == 0) {                     /* JPEG */
        if (!decode_jpeg(data, len)) {
            return;
        }
    } else {                            /* RGB565 裸帧（预留） */
        int w = width > 0 ? width : 320;
        int h = height > 0 ? height : 240;
        if (!ensure_rgb(w, h)) return;
        const uint16_t *src = (const uint16_t *)data;
        for (int y = 0; y < h; y++) {
            uint8_t *d = s_ui.rgb + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                uint16_t v = src[(size_t)y * w + x];
                d[0] = (uint8_t)((v >> 11) << 3);
                d[1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                d[2] = (uint8_t)((v & 0x1F) << 3);
                d += 3;
            }
        }
        s_ui.rgb_w = w;
        s_ui.rgb_h = h;
        s_ui.rgb_stride = w * 3;
    }

    s_ui.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ui.dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_ui.dsc.header.w = (uint32_t)s_ui.rgb_w;
    s_ui.dsc.header.h = (uint32_t)s_ui.rgb_h;
    s_ui.dsc.header.stride = (uint32_t)s_ui.rgb_stride;
    s_ui.dsc.data_size = (uint32_t)((size_t)s_ui.rgb_stride * s_ui.rgb_h);
    s_ui.dsc.data = s_ui.rgb;

    /* 等比缩放到预览区内接（letterbox 居中）：缩放系数同时记录下来给检测框定位，
       修掉旧版"按宽缩放 + 框 Y 按容器高压缩"造成的框/画面错位 */
    int32_t hold_w = lv_obj_get_width(s_ui.img_holder);
    int32_t hold_h = lv_obj_get_height(s_ui.img_holder);
    if (hold_w < 8 || hold_h < 8) {          /* 布局还没算好（首帧过早）时的兜底 */
        hold_w = 468;                        /* 480 - 2*6 边距 */
        hold_h = 366;
    }
    float sc = (float)hold_w / (float)(s_ui.rgb_w ? s_ui.rgb_w : hold_w);
    float sc_h = (float)hold_h / (float)(s_ui.rgb_h ? s_ui.rgb_h : hold_h);
    if (sc_h < sc) sc = sc_h;
    s_ui.disp_scale = sc;
    s_ui.disp_ox = ((float)hold_w - (float)s_ui.rgb_w * sc) * 0.5f;
    s_ui.disp_oy = ((float)hold_h - (float)s_ui.rgb_h * sc) * 0.5f;
    lv_image_set_src(s_ui.img, &s_ui.dsc);
    lv_image_set_scale(s_ui.img, (uint32_t)(sc * 256.0f));
    lv_obj_center(s_ui.img);
    lv_obj_invalidate(s_ui.img);
    lv_obj_add_flag(s_ui.no_signal, LV_OBJ_FLAG_HIDDEN);
}

void ui_shared_set_result(const char *json)
{
    if (!json) return;

    int faces = json_int(json, "faces", 0);
    float sim = json_float(json, "sim", 0);
    float fps = json_float(json, "fps", 0);
    int infer = json_int(json, "infer_ms", 0);
    int libn = json_int(json, "lib_n", -1);
    char match[UI_NAME_MAX] = { 0 };
    char src[8] = { 0 };
    char event[48] = { 0 };
    char msg[96] = { 0 };
    bool has_match = json_str(json, "match", match, sizeof(match));
    json_str(json, "src", src, sizeof(src));
    json_str(json, "event", event, sizeof(event));
    json_str(json, "msg", msg, sizeof(msg));

    /* F8：boxes 先行解析（结果行要脸宽；每框名字/相似度填进 s_ui 供检测框标签用） */
    float box[MAX_BOXES][4];
    char  box_names[MAX_BOXES][UI_NAME_MAX];
    float box_sims[MAX_BOXES];
    int nb = json_boxes(json, box, box_names, box_sims, MAX_BOXES);
    memset(s_ui.box_name, 0, sizeof(s_ui.box_name));
    for (int i = 0; i < nb; i++) {
        snprintf(s_ui.box_name[i], UI_NAME_MAX, "%s", box_names[i]);
        s_ui.box_sim[i] = box_sims[i];
    }
    int face_w = (nb > 0) ? (int)(box[0][2] - box[0][0]) : -1;

    /* F8：结果行稳定态（ASCII：LVGL 内置字体无 ✓） */
    int stable = json_int(json, "stable", -1);
    char who[80];
    if (!strcmp(event, "enrolling")) {
        snprintf(who, sizeof(who), "enrolling");
    } else if (has_match && stable == 1) {
        snprintf(who, sizeof(who), "OK %s %.2f", match, (double)sim);
    } else if (!strcmp(event, "match")) {
        snprintf(who, sizeof(who), "OK %s %.2f", match, (double)sim);
    } else if (!strcmp(event, "tracking")) {
        snprintf(who, sizeof(who), ".. %s", has_match ? match : "confirming");
    } else if (!strcmp(event, "reject") && stable == 1) {
        snprintf(who, sizeof(who), "?? stranger");
    } else if (has_match) {
        snprintf(who, sizeof(who), "%s %.2f", match, (double)sim);
    } else {
        snprintf(who, sizeof(who), "-");
    }
    char cw[16] = "";
    if (face_w > 0) {
        snprintf(cw, sizeof(cw), " w%d", face_w);   /* F8：检出反馈（脸宽 px） */
    }
    /* 板端 sensor 帧率（自动模式下会实时变，直接显示出来当反馈）*/
    int cfps = json_int(json, "cam_fps", 0);
    char cf[28] = "";
    if (cfps > 0) {
        snprintf(cf, sizeof(cf), "  cam%dfps%s", cfps, s_ui.auto_on ? " auto" : "");
    }
    /* 自动帧率状态：与板端对齐（命令行改的也能同步按钮文字）。
       板端结果帧字段是 "auto"（旧代码找 "auto_fps" 永远匹配不到，同步失效） */
    int au = json_int(json, "auto", -1);
    if (au >= 0 && (au != 0) != s_ui.auto_on) {
        s_ui.auto_on = au != 0;
        if (s_ui.lbl_auto) {
            lv_label_set_text(s_ui.lbl_auto, s_ui.auto_on ? "AUTO ON" : "AUTO OFF");
        }
    }
    /* 实时运动占比：优先运动估计器的 mv，DN 关着没有时回退降噪的 dn_mv；
       mvNN% 接近 0 说明画面静止，越高说明越"动" */
    int mv = json_int(json, "mv", json_int(json, "dn_mv", -1));
    char cmv[16] = "";
    if (mv >= 0) {
        snprintf(cmv, sizeof(cmv), " mv%d%%", mv);
    }
    /* 时域降噪档：非 0 才显示；同时把按钮文字与板端对齐（例如从命令行改的）*/
    int dn = json_int(json, "dn", -1);
    char cdn[20] = "";
    if (dn > 0) {
        snprintf(cdn, sizeof(cdn), "  dn%d", dn);
    }
    if (dn >= 0 && dn != s_ui.dn) {
        static const char *dnm[4] = { "Off", "Low", "Mid", "High" };
        s_ui.dn = dn;
        if (s_ui.lbl_dn) {
            lv_label_set_text_fmt(s_ui.lbl_dn, "DN %s", dnm[dn]);
        }
    }
    /* 夜视阶梯档位：tier>=1 = 低光照夜视模式（板端此档不做人脸识别，
       AI 让出帧池给预览，结果帧 event="night" 2Hz 低频上报） */
    int nti = json_int(json, "ntier", -1);
    int non = json_int(json, "night", -1);
    if (non >= 0 && nti >= 0) {
        s_ui.night_on = non;
        s_ui.night_tier = nti;
    }
    char cnight[16] = "";
    if (s_ui.night_on > 0 && s_ui.night_tier >= 1) {
        snprintf(cnight, sizeof(cnight), " night%d", s_ui.night_tier);
    }
    /* 状态栏模式徽标：阶梯开着才显示（关着 = 手动模式，不标） */
    if (s_ui.lbl_mode) {
        if (s_ui.night_on > 0 && s_ui.night_tier >= 1) {
            lv_obj_set_style_text_color(s_ui.lbl_mode, lv_color_hex(C_NIGHT), 0);
            lv_label_set_text_fmt(s_ui.lbl_mode, "NIGHT %d", s_ui.night_tier);
        } else if (s_ui.night_on > 0) {
            lv_obj_set_style_text_color(s_ui.lbl_mode, lv_color_hex(C_ACCENT), 0);
            lv_label_set_text(s_ui.lbl_mode, "DAY");
        } else if (s_ui.night_on == 0) {
            lv_label_set_text(s_ui.lbl_mode, "");
        }
    }
    /* AE/AWB 自动模式状态（板端被手动操作退出时按钮文字跟着变） */
    int ae = json_int(json, "ae", -1);
    if (ae >= 0 && (ae != 0) != s_ui.ae_on) {
        s_ui.ae_on = ae != 0;
        if (s_ui.lbl_ae) {
            lv_label_set_text(s_ui.lbl_ae, s_ui.ae_on ? "AE ON" : "AE OFF");
        }
        sliders_auto_hint(s_ui.ae_on);      /* AE 开关联动滑块置灰/(auto) 提示 */
    }
    int awb = json_int(json, "awb", -1);
    if (awb >= 0 && (awb != 0) != s_ui.awb_on) {
        s_ui.awb_on = awb != 0;
        if (s_ui.lbl_awb) {
            lv_label_set_text(s_ui.lbl_awb, s_ui.awb_on ? "AWB ON" : "AWB OFF");
        }
    }
    /* Auto All 高亮 = 板端确实处于全自动（夜视阶梯 + 运动自适应 + AE/AWB 全开） */
    if (s_ui.btn_autoall) {
        bool all = (s_ui.night_on > 0) && s_ui.auto_on && s_ui.ae_on && s_ui.awb_on;
        lv_obj_set_style_bg_color(s_ui.btn_autoall, lv_color_hex(all ? C_ACCENT : C_PANEL), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_ui.btn_autoall, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(all ? C_BG : C_TEXT), 0);
        }
    }
    /* AE 开着时显示实时亮度（收敛到 ae_tgt 附近即正常） */
    int luma = json_int(json, "luma", -1);
    char cluma[16] = "";
    if (luma >= 0 && s_ui.ae_on) {
        snprintf(cluma, sizeof(cluma), " L%d", luma);
    }
    if (libn >= 0) {
        lv_label_set_text_fmt(s_ui.lbl_result, "faces %d   %s   %.1f fps  %d ms  lib %d%s%s%s%s%s%s",
                              faces, who, (double)fps, infer, libn, cf, cdn, cmv, cw, cluma, cnight);
    } else {
        lv_label_set_text_fmt(s_ui.lbl_result, "faces %d   %s   %.1f fps  %d ms%s%s%s%s%s%s",
                              faces, who, (double)fps, infer, cf, cdn, cmv, cw, cluma, cnight);
    }

    lv_label_set_text_fmt(s_ui.lbl_stream, "src %s", src[0] ? src : "-");
    if (src[0]) {
        s_ui.src_mode = !strcmp(src, "pc") ? "pc" : "cam";
        lv_label_set_text(s_ui.lbl_src, s_ui.src_mode[0] == 'p' ? "PC frame" : "Camera");
    }

    /* 事件优先级：registered / match / deleted / error 等直接显示 */
    /* F8：注册采集进度（弹窗进度态）；registered/error 自动关闭弹窗 */
    if (s_ui.enrolling) {
        int cN = 0, cM = 0;
        if (!strcmp(event, "enrolling") && sscanf(msg, "collected %d/%d", &cN, &cM) == 2) {
            if (s_ui.lbl_prog) {
                lv_label_set_text_fmt(s_ui.lbl_prog, "collected %d/%d - stay in view", cN, cM);
            }
            if (s_ui.bar_prog) {
                lv_bar_set_value(s_ui.bar_prog, cN, LV_ANIM_OFF);
            }
        }
        if (!strcmp(event, "registered") || !strcmp(event, "error")) {
            s_ui.enrolling = false;
            close_input();
        }
    }

    if (msg[0]) {
        snprintf(s_ui.ev_text, sizeof(s_ui.ev_text), "%s: %s", event, msg);
    } else {
        snprintf(s_ui.ev_text, sizeof(s_ui.ev_text), "%s", event);
    }
    lv_label_set_text(s_ui.lbl_event, s_ui.ev_text);
    lv_obj_set_style_text_color(s_ui.lbl_event,
                                lv_color_hex(!strcmp(event, "error") ? C_WARN : C_ACCENT), 0);

    /* 检测框（box/nb/每框名字已在上方解析；update_boxes 内部优先用每框名字） */
    bool show_name = has_match && (!strcmp(event, "match") || !strcmp(event, "registered") ||
                                   !strcmp(event, "none"));
    update_boxes((const float(*)[4])box, nb, show_name ? match : NULL);

    /* 摄像头参数：同步滑块/按钮的真实值（板端每条结果帧都带 gain/exp/mirror） */
    int gmax = json_int(json, "gain_max", -1);
    if (gmax > 0 && gmax != s_ui.gain_max && s_ui.sl_gain) {
        s_ui.gain_max = gmax;
        lv_slider_set_range(s_ui.sl_gain, 0, gmax);
    }
    int emax = json_int(json, "exp_max", -1);
    if (emax > 0 && emax != s_ui.exp_max && s_ui.sl_exp) {
        s_ui.exp_max = emax;
        lv_slider_set_range(s_ui.sl_exp, 8, emax);
    }
    int g = json_int(json, "gain", -1);
    if (g >= 0) {
        s_ui.gain = g;
    }
    int ex = json_int(json, "exp", -1);
    if (ex >= 0) {
        s_ui.exp_val = ex;
    }
    if ((g >= 0 || ex >= 0) && !s_ui.dragging) {
        /* 拖动中不用结果帧回写；否则滑块位置与标签（含 AE 的 "(auto)" 后缀）同步 */
        if (g >= 0 && s_ui.sl_gain) {
            lv_slider_set_value(s_ui.sl_gain, g, LV_ANIM_OFF);
        }
        if (ex >= 0 && s_ui.sl_exp) {
            lv_slider_set_value(s_ui.sl_exp, ex, LV_ANIM_OFF);
        }
        slider_label_refresh();
    }
    int wkr = json_int(json, "wb_kr", -1);
    if (wkr >= 0) {
        s_ui.wb_on = wkr > 1100;      /* >1000 视为开了软件白平衡 */
        if (s_ui.lbl_wb) {
            lv_label_set_text(s_ui.lbl_wb, s_ui.wb_on ? "WB ON" : "WB OFF");
        }
    }
    int mir = json_int(json, "mirror", -1);
    if (mir >= 0) {
        s_ui.mirror_on = mir != 0;
        if (s_ui.lbl_mirror) {
            lv_label_set_text(s_ui.lbl_mirror, s_ui.mirror_on ? "Mirror ON" : "Mirror OFF");
        }
    }

    /* 名单（lib 只在 start/list/delete/register 的回复里带） */
    char lib[256];
    json_lib(json, lib, sizeof(lib));
    if (lib[0]) {
        snprintf(s_ui.lib_text, sizeof(s_ui.lib_text), "lib: %s", lib);
        lv_label_set_text(s_ui.lbl_list, s_ui.lib_text);
    }
}

void ui_shared_add_log(const char *line)
{
    if (!line) return;
    char tail[sizeof(s_ui.log_text)];
    tail[0] = '\0';
    /* 保留最后 LOG_LINES-1 行 */
    int nl = 0;
    for (const char *p = s_ui.log_text; *p; p++) {
        if (*p == '\n') nl++;
    }
    if (nl >= LOG_LINES) {
        const char *p = s_ui.log_text;
        while (nl >= LOG_LINES && *p) {
            if (*p == '\n') nl--;
            p++;
        }
        snprintf(tail, sizeof(tail), "%s", p);
    } else {
        snprintf(tail, sizeof(tail), "%s", s_ui.log_text);
    }
    snprintf(s_ui.log_text, sizeof(s_ui.log_text), "%s%s%s", tail, tail[0] ? "\n" : "", line);
    lv_label_set_text(s_ui.lbl_log, s_ui.log_text);
}

void ui_shared_tick(void)
{
    /* 目前无周期性动作（图像/结果都是事件驱动），留接口给后续加动画/心跳 */
}
