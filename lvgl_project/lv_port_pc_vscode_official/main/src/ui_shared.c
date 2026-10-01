/*
 * ui_shared.c —— 同源 UI 实现（纯 LVGL v9）
 *
 * 布局（弹性列）：
 *   [状态栏 36] [图像 480x360 + 检测框] [结果 30] [名单 grow] [按钮 2x4] [日志 68]
 * JPEG 解码用 lvgl 内置的 tjpgd（直接解码到自管 RGB888 缓冲），
 * 不走 LVGL 的 FS/MEMFS 解码路径 —— 避免依赖 lv_conf 的 FS 开关。
 */
#include "ui_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl/src/libs/tjpgd/tjpgd.h"

#define IMG_W           480
#define IMG_H           300        /* 让出 60px 给第 4 行"帧率档位"按钮 */
#define MAX_BOXES       6
#define LOG_LINES       3
#define UI_NAME_MAX     24

#define C_BG            0x0E1116
#define C_PANEL         0x1A2029
#define C_ACCENT        0x22C1A4
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
    lv_obj_t *no_signal;

    /* 状态栏 / 结果 / 名单 / 日志 */
    lv_obj_t *lbl_link, *lbl_stream, *lbl_result, *lbl_event, *lbl_list, *lbl_log;

    /* 摄像头参数（滑块 + 快捷按钮） */
    lv_obj_t *sl_gain, *sl_exp, *lbl_gain, *lbl_exp, *btn_mirror, *lbl_mirror, *lbl_wb;
    bool      wb_on;
    int       gain, gain_max, exp_val, exp_max;
    bool      dragging;        /* 拖动中：不要用结果帧回写滑块位置 */
    bool      mirror_on;

    /* 时域降噪档（循环按钮） */
    lv_obj_t *lbl_dn;
    int       dn;

    /* 按钮 */
    lv_obj_t *btn_src, *lbl_src, *btn_stream, *lbl_stream_btn;

    /* 弹窗输入 */
    lv_obj_t *kb;
    lv_obj_t *mbox;

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

/* boxes: [[x1,y1,x2,y2],...] -> 归一化到 0..1 */
static int json_boxes(const char *js, float out[][4], int max)
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
            n++;
        }
        while (*q && *q != ']') q++;
        if (*q == ']') q++;
        p = q;
    }
    return n;
}

/* lib: ["a","b"] -> "a, b" */
static void json_lib(const char *js, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *p = strstr(js ? js : "", "\"lib\":[");
    if (!p) return;
    p += 7;
    size_t used = 0;
    while (*p && *p != ']') {
        if (*p == '"') {
            p++;
            const char *e = strchr(p, '"');
            if (!e) break;
            size_t n = (size_t)(e - p);
            if (used + n + 3 < out_sz) {
                if (used) {
                    out[used++] = ',';
                    out[used++] = ' ';
                }
                memcpy(out + used, p, n);
                used += n;
                out[used] = '\0';
            }
            p = e + 1;
        } else {
            p++;
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

/* 检测框：按显示比例摆放（板端 box 是原图像素坐标）；name 只标在最大的一帧首个框上 */
static void update_boxes(const float box[][4], int n, const char *name)
{
    for (int i = 0; i < MAX_BOXES; i++) {
        if (i >= n || s_ui.rgb_w <= 0) {
            lv_obj_add_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float sx = (float)IMG_W / (float)s_ui.rgb_w;
        float sy = (float)IMG_H / (float)s_ui.rgb_h;
        int x1 = (int)(box[i][0] * sx);
        int y1 = (int)(box[i][1] * sy);
        int x2 = (int)(box[i][2] * sx);
        int y2 = (int)(box[i][3] * sy);
        if (x2 <= x1 || y2 <= y1) {
            lv_obj_add_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ui.box_label[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui.box[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_ui.box[i], x1, y1);
        lv_obj_set_size(s_ui.box[i], x2 - x1, y2 - y1);

        if (name && name[0]) {
            lv_label_set_text(s_ui.box_label[i], name);
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
        lv_obj_delete(s_ui.mbox);
        s_ui.mbox = NULL;
    }
}

static void input_ok_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    const char *cmd = (const char *)lv_obj_get_user_data(lv_event_get_target(e));
    const char *name = lv_textarea_get_text(ta);
    if (name && name[0]) {
        char js[128];
        snprintf(js, sizeof(js), "{\"cmd\":\"%s\",\"id\":\"%s\"}", cmd, name);
        send_cmd(js);
        ui_shared_add_log(cmd[0] == 'r' ? "register: 请正对摄像头" : "delete sent");
    }
    close_input();
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    close_input();
}

static void open_input(const char *title, const char *cmd)
{
    close_input();
    s_ui.mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_ui.mbox, title);
    lv_obj_t *ta = lv_textarea_create(s_ui.mbox);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "name");
    lv_obj_set_width(ta, lv_pct(80));

    lv_obj_t *ok = lv_msgbox_add_footer_button(s_ui.mbox, "OK");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(s_ui.mbox, "Cancel");
    lv_obj_set_user_data(ok, (void *)cmd);
    lv_obj_add_event_cb(ok, input_ok_cb, LV_EVENT_CLICKED, ta);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

    s_ui.kb = lv_keyboard_create(lv_screen_active());
    lv_keyboard_set_textarea(s_ui.kb, ta);
    lv_obj_set_size(s_ui.kb, lv_pct(100), 240);
    lv_obj_align(s_ui.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(s_ui.mbox);
}

/* ---------------- 增益/曝光滑块（松手才发命令，避免拖动时刷屏） ---------------- */
static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    bool is_gain = (sl == s_ui.sl_gain);
    lv_label_set_text_fmt(is_gain ? s_ui.lbl_gain : s_ui.lbl_exp,
                          is_gain ? "gain %d" : "exp %d", (int)lv_slider_get_value(sl));
}

static void slider_pressed_cb(lv_event_t *e)
{
    (void)e;
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
    lv_obj_set_height(row, 36);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text_fmt(lbl, "%s %d", label_text, val);
    lv_obj_set_width(lbl, 92);
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
        ui_shared_add_log(to_pc ? "src -> pc (PC 送帧)" : "src -> cam (板载摄像头)");
    } else if (!strncmp(id, "fps", 3)) {
        /* 帧率档位：板端降 VTS 换更长曝光，并自动把 exp/gain 拉到新上限 */
        int f = atoi(id + 3);
        char js[96];
        snprintf(js, sizeof(js), "{\"cmd\":\"cam\",\"fps\":%d,\"auto_exp\":true}", f);
        send_cmd(js);
        ui_shared_add_log("fps -> night shift (exp/gain auto max)");
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
    lv_obj_set_height(b, 46);
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

    /* --- 状态栏 --- */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 34);
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
    s_ui.lbl_stream = lv_label_create(bar);
    lv_label_set_text(s_ui.lbl_stream, "-");
    lv_obj_set_style_text_color(s_ui.lbl_stream, lv_color_hex(C_DIM), 0);

    /* --- 图像区 --- */
    s_ui.img_holder = lv_obj_create(root);
    lv_obj_set_size(s_ui.img_holder, IMG_W, IMG_H);
    lv_obj_set_style_bg_color(s_ui.img_holder, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_ui.img_holder, 0, 0);
    lv_obj_set_style_radius(s_ui.img_holder, 4, 0);
    lv_obj_set_style_pad_all(s_ui.img_holder, 0, 0);
    lv_obj_remove_flag(s_ui.img_holder, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.img = lv_image_create(s_ui.img_holder);
    lv_obj_center(s_ui.img);

    s_ui.no_signal = lv_label_create(s_ui.img_holder);
    lv_label_set_text(s_ui.no_signal, "waiting for image...\n(按 Preview ON + Camera)");
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

    /* --- 摄像头参数：增益 / 曝光 滑块 --- */
    s_ui.sl_gain = make_slider_row(root, "gain", 0, s_ui.gain_max, s_ui.gain, &s_ui.lbl_gain);
    s_ui.sl_exp = make_slider_row(root, "exp", 8, s_ui.exp_max, s_ui.exp_val, &s_ui.lbl_exp);

    /* --- 名单 --- */
    s_ui.lbl_list = lv_label_create(root);
    lv_obj_set_width(s_ui.lbl_list, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.lbl_list, 1);
    lv_label_set_long_mode(s_ui.lbl_list, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.lbl_list, "lib: -");
    lv_obj_set_style_text_color(s_ui.lbl_list, lv_color_hex(C_DIM), 0);

    /* --- 按钮 --- */
    for (int row = 0; row < 4; row++) {
        lv_obj_t *r = lv_obj_create(root);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_height(r, 46);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_set_style_pad_column(r, 4, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        if (row == 0) {
            make_btn(r, "Start", "start", NULL);
            make_btn(r, "Stop", "stop", NULL);
            make_btn(r, "Register", "register", NULL);
            make_btn(r, "Delete", "delete", NULL);
        } else if (row == 1) {
            make_btn(r, "List", "list", NULL);
            make_btn(r, "Clear", "clear", NULL);
            make_btn(r, "Camera", "src", &s_ui.lbl_src);
            make_btn(r, "Preview ON", "stream", &s_ui.lbl_stream_btn);
        } else if (row == 2) {
            /* Night Hi = 10fps + exp max + gain max（最亮）；Night Lo = 同上但增益 ~62%（低噪）*/
            make_btn(r, "Night Hi", "night", NULL);
            make_btn(r, "Night Lo", "night_lo", NULL);
            make_btn(r, "Day", "day", NULL);
            make_btn(r, "Mirror ON", "mirror", &s_ui.lbl_mirror);
            make_btn(r, "WB ON", "wb", &s_ui.lbl_wb);
        } else {
            /* 帧率档位：帧率越低 -> 曝光上限越高（夜视长曝光）。
               板端收到后会顺手把曝光/增益拉到新上限（auto_exp）。 */
            make_btn(r, "50fps", "fps50", NULL);
            make_btn(r, "25fps", "fps25", NULL);
            make_btn(r, "15fps", "fps15", NULL);
            make_btn(r, "10fps", "fps10", NULL);
            /* 时域降噪循环档：点击在 Off → Low → Mid → High 之间循环 */
            make_btn(r, "DN Off", "dn", &s_ui.lbl_dn);
        }
    }

    /* --- 日志 --- */
    s_ui.lbl_log = lv_label_create(root);
    lv_obj_set_width(s_ui.lbl_log, lv_pct(100));
    lv_obj_set_height(s_ui.lbl_log, 62);
    lv_label_set_long_mode(s_ui.lbl_log, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.lbl_log, "");
    lv_obj_set_style_text_color(s_ui.lbl_log, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_font(s_ui.lbl_log, &lv_font_montserrat_14, 0);
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

    /* 按宽度适配显示区（640x480 -> 480x360 恰好 0.75x） */
    int32_t scale = (int32_t)((IMG_W * 256) / (s_ui.rgb_w ? s_ui.rgb_w : IMG_W));
    lv_image_set_src(s_ui.img, &s_ui.dsc);
    lv_image_set_scale(s_ui.img, (uint32_t)scale);
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

    char who[64];
    if (has_match) {
        snprintf(who, sizeof(who), "%s %.2f", match, (double)sim);
    } else {
        snprintf(who, sizeof(who), "-");
    }
    /* 板端 sensor 帧率（点了下面的档位按钮就会变，直接显示出来当反馈）*/
    int cfps = json_int(json, "cam_fps", 0);
    char cf[16] = "";
    if (cfps > 0) {
        snprintf(cf, sizeof(cf), "  cam%dfps", cfps);
    }
    /* 时域降噪档：非 0 才显示；同时把按钮文字与板端对齐（例如从命令行改的）*/
    int dn = json_int(json, "dn", -1);
    char cdn[16] = "";
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
    if (libn >= 0) {
        lv_label_set_text_fmt(s_ui.lbl_result, "faces %d   %s   %.1f fps  %d ms  lib %d%s%s",
                              faces, who, (double)fps, infer, libn, cf, cdn);
    } else {
        lv_label_set_text_fmt(s_ui.lbl_result, "faces %d   %s   %.1f fps  %d ms%s%s",
                              faces, who, (double)fps, infer, cf, cdn);
    }

    lv_label_set_text_fmt(s_ui.lbl_stream, "src %s", src[0] ? src : "-");
    if (src[0]) {
        s_ui.src_mode = !strcmp(src, "pc") ? "pc" : "cam";
        lv_label_set_text(s_ui.lbl_src, s_ui.src_mode[0] == 'p' ? "PC frame" : "Camera");
    }

    /* 事件优先级：registered / match / deleted / error 等直接显示 */
    if (msg[0]) {
        snprintf(s_ui.ev_text, sizeof(s_ui.ev_text), "%s: %s", event, msg);
    } else {
        snprintf(s_ui.ev_text, sizeof(s_ui.ev_text), "%s", event);
    }
    lv_label_set_text(s_ui.lbl_event, s_ui.ev_text);
    lv_obj_set_style_text_color(s_ui.lbl_event,
                                lv_color_hex(!strcmp(event, "error") ? C_WARN : C_ACCENT), 0);

    /* 检测框 */
    float box[MAX_BOXES][4];
    int n = json_boxes(json, box, MAX_BOXES);
    bool show_name = has_match && (!strcmp(event, "match") || !strcmp(event, "registered") ||
                                   !strcmp(event, "none"));
    update_boxes((const float(*)[4])box, n, show_name ? match : NULL);

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
        if (!s_ui.dragging && s_ui.sl_gain) {
            lv_slider_set_value(s_ui.sl_gain, g, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_ui.lbl_gain, "gain %d", g);
        }
    }
    int ex = json_int(json, "exp", -1);
    if (ex >= 0) {
        s_ui.exp_val = ex;
        if (!s_ui.dragging && s_ui.sl_exp) {
            lv_slider_set_value(s_ui.sl_exp, ex, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_ui.lbl_exp, "exp %d", ex);
        }
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
