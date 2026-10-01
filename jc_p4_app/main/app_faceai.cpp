/*
 * app_faceai.cpp —— 板端边缘人脸检测 / 特征提取 / 1:N 比对（任务五）
 *
 *   app_imgpipe(RGB888) --> HumanFaceDetect(MSR+MNP) --> HumanFaceFeat(MFN)
 *        --> dl::recognition::DataBase(/storage/face.db) --> 0x02 JSON 回传 PC
 *
 * 说明：
 *   - 特征库用 esp-dl 自带 dl::recognition::DataBase（按 id 存 L2 归一化特征），
 *     落盘在 storage(FAT) 分区；名字 <-> id 映射存 NVS(namespace "faceai")。
 *   - 模型在 AI 任务里加载（几秒），加载期间由 app_imgpipe 内置 ack 兜底，
 *     加载完成后 app_imgpipe_set_auto_ack(false) 接管应答（每帧回一条 0x02）。
 *   - 命令走 app_usb_link 的 0x03 帧：回调只投队列，解析/执行都在 AI 任务里。
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "nvs.h"
#include "cJSON.h"

#include "dl_image_define.hpp"
#include "dl_recognition_database.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

#include "app_faceai.h"
#include "app_imgpipe.h"
#include "app_stream.h"
#include "app_camera.h"
#include "app_usb_link.h"
#include "app_status.h"
#include "sdkconfig.h"

#define TAG "faceai"

#define FACE_DB_MOUNT   "/storage"
#define FACE_DB_PART    "storage"
#define FACE_DB_PATH    "/storage/face.db"
#define FACE_NVS_NS     "faceai"
#define FACE_NVS_KEY    "names"

#define MAX_FEATS       32      /* 特征条数上限（同一个人可以注册多张） */
#define MAX_PERSONS     10      /* 人数上限（lib 里的唯一名字数） */
#define NVS_NAMES_MAX   2048
#define MAX_BOXES       CONFIG_APP_FACEAI_MAX_BOXES
#define CMD_QUEUE_LEN   4
#define CMD_JSON_MAX    256
#define TASK_STACK      8192
#define TASK_PRIO       4

typedef struct {
    uint16_t id;
    char     name[32];
} face_name_t;

typedef struct {
    char json[CMD_JSON_MAX];
} cmd_msg_t;

namespace {

HumanFaceDetect *s_detect = nullptr;
HumanFaceFeat   *s_feat = nullptr;
dl::recognition::DataBase *s_db = nullptr;
wl_handle_t      s_wl = WL_INVALID_HANDLE;
QueueHandle_t    s_cmd_q = nullptr;
bool             s_db_ok = false;

face_name_t      s_names[MAX_FEATS];
int              s_name_cnt = 0;        /* 特征条数（一个名字可能多条） */

bool             s_running = true;
bool             s_mode_detect = true;
bool             s_mode_match = true;
float            s_match_thr = CONFIG_APP_FACEAI_MATCH_THR / 100.0f;
bool             s_reg_pending = false;
int64_t          s_reg_arm_us = 0;      /* 武装注册的时刻，只接受其后发布的帧 */
char             s_reg_name[32] = { 0 };
bool             s_lib_dirty = false;
volatile bool    s_ready = false;

uint8_t         *s_pack_buf = nullptr;   /* 行距不一致时的重打包缓冲 */
size_t           s_pack_cap = 0;

uint32_t         s_frames = 0;
uint32_t         s_drops = 0;
const char      *s_ack_cmd = nullptr;   /* 非空时，send_result 会带上 "ack":<命令名> */

/* 摄像头状态缓存：cam 命令查询/设置后更新，之后每条结果帧都带上，
   方便 UI 同步增益/曝光滑块的真实值（不额外 ioctl） */
bool s_cam_state_valid = false;
int  s_cam_gain = 0, s_cam_gain_max = 0;
int  s_cam_exp = 0, s_cam_exp_max = 0;
int  s_cam_mirror = 1;
int  s_cam_wb_kr = 1000, s_cam_wb_kb = 1000;
int  s_cam_fps = 0;                  /* sensor 当前帧率（降帧率换长曝光） */
int  s_cam_dn = 0;                   /* 时域降噪档位 0..3（0=关） */
int  s_cam_dn_ms = 0;                /* 最近一帧降噪耗时 ms */
int  s_cam_dn_skip = 0;              /* 被帧率守卫跳过的降噪帧数 */
int  s_cam_dn_blk = 0;               /* 参与混合的块数（调参用） */
int  s_cam_dn_sad = 0;               /* 全帧平均 SAD（调参用） */
float            s_fps = 0.0f;
uint32_t         s_fps_cnt = 0;
int64_t          s_fps_win = 0;
int64_t          s_status_ms = 0;

}  // namespace

/* ---------------- 名字表（NVS 持久化） ---------------- */

static void name_sanitize(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    out[0] = 0;
    if (!in) return;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' || c >= 0x80) {
            out[j++] = (char)c;
        } else if (c == ' ') {
            out[j++] = '_';
        }
    }
    out[j] = 0;
}

static bool name_exists(const char *name)
{
    if (!name || !name[0]) return false;
    for (int i = 0; i < s_name_cnt; i++) {
        if (strcmp(s_names[i].name, name) == 0) return true;
    }
    return false;
}

static int name_count_persons(void)
{
    int n = 0;
    for (int i = 0; i < s_name_cnt; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(s_names[j].name, s_names[i].name) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) n++;
    }
    return n;
}

static const char *name_find_name(uint16_t id)
{
    for (int i = 0; i < s_name_cnt; i++) {
        if (s_names[i].id == id) return s_names[i].name;
    }
    return nullptr;
}

/* 追加一条特征->名字（可重复名字，表示同一个人多张特征） */
static bool name_add(uint16_t id, const char *name)
{
    for (int i = 0; i < s_name_cnt; i++) {
        if (s_names[i].id == id) {
            strlcpy(s_names[i].name, name, sizeof(s_names[i].name));
            return true;
        }
    }
    if (s_name_cnt >= MAX_FEATS) return false;
    s_names[s_name_cnt].id = id;
    strlcpy(s_names[s_name_cnt].name, name, sizeof(s_names[s_name_cnt].name));
    s_name_cnt++;
    return true;
}

/* 删除某个名字下的全部特征记录（返回删掉的条数） */
static int name_remove_by_name(const char *name)
{
    int removed = 0;
    for (int i = 0; i < s_name_cnt;) {
        if (strcmp(s_names[i].name, name) == 0) {
            for (int j = i; j < s_name_cnt - 1; j++) {
                s_names[j] = s_names[j + 1];
            }
            s_name_cnt--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

static void names_load(void)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_str(h, FACE_NVS_KEY, nullptr, &len) == ESP_OK && len > 1 && len < NVS_NAMES_MAX) {
        char *buf = (char *)malloc(len);
        if (buf) {
            if (nvs_get_str(h, FACE_NVS_KEY, buf, &len) == ESP_OK) {
                char *save = nullptr;
                for (char *tok = strtok_r(buf, ";", &save);
                     tok && s_name_cnt < MAX_FEATS;
                     tok = strtok_r(nullptr, ";", &save)) {
                    char *sep = strchr(tok, ':');
                    if (!sep) continue;
                    *sep = 0;
                    int id = atoi(tok);
                    if (id <= 0 || id > 0xFFFF) continue;
                    name_sanitize(sep + 1, s_names[s_name_cnt].name, sizeof(s_names[0].name));
                    if (!s_names[s_name_cnt].name[0]) continue;
                    s_names[s_name_cnt].id = (uint16_t)id;
                    s_name_cnt++;
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "names loaded: %d", s_name_cnt);
}

static void names_save(void)
{
    char buf[NVS_NAMES_MAX];
    size_t off = 0;
    buf[0] = 0;
    for (int i = 0; i < s_name_cnt; i++) {
        char item[48];
        int n = snprintf(item, sizeof(item), "%u:%s", (unsigned)s_names[i].id, s_names[i].name);
        if (n <= 0) continue;
        size_t need = (size_t)n + (off ? 1 : 0);
        if (off + need + 1 >= sizeof(buf)) break;
        if (off) buf[off++] = ';';
        memcpy(buf + off, item, (size_t)n);
        off += (size_t)n;
        buf[off] = 0;
    }

    nvs_handle_t h;
    if (nvs_open(FACE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, names not persisted");
        return;
    }
    esp_err_t err = off ? nvs_set_str(h, FACE_NVS_KEY, buf) : nvs_erase_key(h, FACE_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---------------- 0x02 结果帧 ---------------- */

static void send_result(const char *event,
                        const std::list<dl::detect::result_t> *res,
                        float sim,
                        const char *match_name,
                        uint32_t infer_ms,
                        const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "faces", res ? (int)res->size() : 0);
    cJSON *boxes = cJSON_CreateArray();
    if (boxes) {
        int n = 0;
        if (res) {
            for (const auto &r : *res) {
                if (n >= MAX_BOXES) break;
                if (r.box.size() < 4) continue;
                cJSON *b = cJSON_CreateArray();
                if (!b) break;
                for (int k = 0; k < 4; k++) {
                    cJSON_AddItemToArray(b, cJSON_CreateNumber(r.box[k]));
                }
                cJSON_AddItemToArray(boxes, b);
                n++;
            }
        }
        cJSON_AddItemToObject(root, "boxes", boxes);
    }
    if (match_name && match_name[0]) {
        cJSON_AddStringToObject(root, "match", match_name);
    }
    cJSON_AddNumberToObject(root, "sim", sim);
    cJSON_AddNumberToObject(root, "fps", s_fps);
    cJSON_AddNumberToObject(root, "infer_ms", (int)infer_ms);
    cJSON_AddNumberToObject(root, "lib_n", s_name_cnt);
    cJSON_AddNumberToObject(root, "dropped", (int)s_drops);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddStringToObject(root, "src", app_imgpipe_src_str(app_imgpipe_get_src()));
    if (s_ack_cmd) {
        /* 命令回复专用标记：摄像头常驻推流时，host 靠它把 ack 和流式结果区分开 */
        cJSON_AddStringToObject(root, "ack", s_ack_cmd);
    }
    if (s_cam_state_valid) {
        cJSON_AddNumberToObject(root, "gain", s_cam_gain);
        cJSON_AddNumberToObject(root, "gain_max", s_cam_gain_max);
        cJSON_AddNumberToObject(root, "exp", s_cam_exp);
        cJSON_AddNumberToObject(root, "exp_max", s_cam_exp_max);
        cJSON_AddNumberToObject(root, "mirror", s_cam_mirror);
        cJSON_AddNumberToObject(root, "wb_kr", s_cam_wb_kr);
        cJSON_AddNumberToObject(root, "wb_kb", s_cam_wb_kb);
        cJSON_AddNumberToObject(root, "cam_fps", s_cam_fps);
        /* 时域降噪：档位 + 耗时（关的时候不带这两个字段，免得刷屏）*/
        cJSON_AddNumberToObject(root, "dn", s_cam_dn);
        if (s_cam_dn > 0) {
            cJSON_AddNumberToObject(root, "dn_ms", s_cam_dn_ms);
            cJSON_AddNumberToObject(root, "dn_blk", s_cam_dn_blk);
            cJSON_AddNumberToObject(root, "dn_sad", s_cam_dn_sad);
            if (s_cam_dn_skip > 0) {
                cJSON_AddNumberToObject(root, "dn_skip", s_cam_dn_skip);
            }
        }
    }
    if (msg) cJSON_AddStringToObject(root, "msg", msg);
    if (s_lib_dirty) {
        cJSON *lib = cJSON_CreateArray();
        if (lib) {
            for (int i = 0; i < s_name_cnt; i++) {         /* 只列唯一名字 */
                bool dup = false;
                for (int j = 0; j < i; j++) {
                    if (strcmp(s_names[j].name, s_names[i].name) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) cJSON_AddItemToArray(lib, cJSON_CreateString(s_names[i].name));
            }
            cJSON_AddItemToObject(root, "lib", lib);
        }
        s_lib_dirty = false;
    }

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js) {
        app_usb_link_send(JCL_TYPE_RESULT, js, (uint32_t)strlen(js));
        cJSON_free(js);
    }
}

/* ---------------- 单帧处理 ---------------- */

static void handle_frame(const img_frame_t &fr)
{
    /* esp-dl 要求紧密排布（row_step = width*bpp）：JPEG 解码按 MCU 对齐、
       摄像头/ISP 可能给 bytesperline，行距不一致时先重打包 */
    const int bpp = (fr.fmt == IMG_FMT_RGB565) ? 2 : 3;
    const int row_bytes = fr.width * bpp;
    const uint8_t *data = fr.data;
    if (fr.stride != row_bytes) {
        size_t need = (size_t)row_bytes * fr.height;
        if (s_pack_cap < need) {
            if (s_pack_buf) heap_caps_free(s_pack_buf);
            s_pack_buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_pack_cap = s_pack_buf ? need : 0;
        }
        if (!s_pack_buf) {
            send_result("error", nullptr, 0, nullptr, 0, "no mem for repack");
            return;
        }
        for (int y = 0; y < fr.height; y++) {
            memcpy(s_pack_buf + (size_t)y * row_bytes,
                   fr.data + (size_t)y * fr.stride,
                   (size_t)row_bytes);
        }
        data = s_pack_buf;
    }

    dl::image::img_t img = {};
    img.data = (void *)data;
    img.width = (uint16_t)fr.width;
    img.height = (uint16_t)fr.height;
    /* 板载摄像头(ISP)出的是 RGB565LE；PC 送来的 JPEG 硬解成 RGB888 */
    img.pix_type = (fr.fmt == IMG_FMT_RGB565) ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                                              : dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    int64_t t0 = esp_timer_get_time();
    const std::list<dl::detect::result_t> &det = s_detect->run(img);
    uint32_t infer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    /* 最大的人脸用于识别 / 注册 */
    const dl::detect::result_t *big = nullptr;
    for (const auto &r : det) {
        if (!big || r.box_area() > big->box_area()) big = &r;
    }

    const char *event = "none";
    const char *match_name = "";
    float sim = 0.0f;
    const char *msg = nullptr;

    /* register 只对"武装之后发布"的帧生效：命令与帧的到达顺序不确定，
       没有这个时间戳门限会出现"注册命令配上了之前那帧"的错配 */
    const bool reg_active = s_reg_pending && (fr.ts_us > s_reg_arm_us);
    if (reg_active) {
        if (!big) {
            msg = "no face, retry";             /* 保持 pending，等下一帧 */
        } else if (!s_db_ok) {
            event = "error";
            msg = "feature db unavailable";
            s_reg_pending = false;
        } else {
            dl::TensorBase *f = s_feat->run(img, big->keypoint);
            if (f && s_db->enroll_feat(f) == ESP_OK) {
                /* 刚入库的特征与自身相似度=1.0，用它取回新分配的 id */
                std::vector<dl::recognition::result_t> q = s_db->query_feat(f, 0.999f, 1);
                uint16_t id = q.empty() ? 0 : q.front().id;
                if (id && name_add(id, s_reg_name)) {
                    names_save();
                    match_name = s_reg_name;    /* 结果里带上刚注册的名字，host 可核对 */
                    event = "registered";
                    s_lib_dirty = true;
                    s_reg_pending = false;
                    ESP_LOGI(TAG, "registered '%s' as id %u (feats %d, persons %d)",
                             s_reg_name, (unsigned)id, s_name_cnt, name_count_persons());
                } else {
                    event = "error";
                    msg = "lib full";
                }
            } else {
                event = "error";
                msg = "enroll failed";
            }
        }
    } else if (big && s_mode_match && s_db_ok) {
        dl::TensorBase *f = s_feat->run(img, big->keypoint);
        if (f) {
            std::vector<dl::recognition::result_t> q = s_db->query_feat(f, s_match_thr, 1);
            if (!q.empty()) {
                sim = q.front().similarity;
                const char *nm = name_find_name(q.front().id);
                if (nm) {
                    match_name = nm;
                    event = "match";
                } else {
                    event = "reject";
                }
            } else {
                event = "reject";
            }
        } else {
            event = "error";
            msg = "feat failed";
        }
    }

    /* fps 统计（1s 窗口） */
    int64_t now = esp_timer_get_time();
    s_fps_cnt++;
    if (now - s_fps_win >= 1000000) {
        s_fps = (float)s_fps_cnt * 1000000.0f / (float)(now - s_fps_win);
        s_fps_win = now;
        s_fps_cnt = 0;
    }

    if (now - s_status_ms > 500000) {
        s_status_ms = now;
        app_status_set(APP_MOD_FACE, true, "%d face %s", (int)det.size(),
                       match_name[0] ? match_name : event);
    }

    send_result(event, &det, sim, match_name, infer_ms, msg);
}

/* ---------------- 命令处理（AI 任务上下文） ---------------- */

static void handle_cmd(const char *json)
{
    s_ack_cmd = nullptr;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        send_result("error", nullptr, 0, nullptr, 0, "bad json");
        return;
    }

    const cJSON *jc = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(jc)) {
        cJSON_Delete(root);
        send_result("error", nullptr, 0, nullptr, 0, "missing cmd");
        return;
    }
    const char *cmd = jc->valuestring;
    s_ack_cmd = cmd;               /* 本函数内所有 send_result 都会带 ack 字段 */

    if (strcmp(cmd, "start") == 0) {
        s_running = true;
        s_lib_dirty = true;
        send_result("none", nullptr, 0, nullptr, 0, "started");
    } else if (strcmp(cmd, "stop") == 0) {
        s_running = false;
        send_result("none", nullptr, 0, nullptr, 0, "stopped");
    } else if (strcmp(cmd, "mode") == 0) {
        const cJSON *jd = cJSON_GetObjectItem(root, "detect");
        const cJSON *jm = cJSON_GetObjectItem(root, "match");
        const cJSON *jt = cJSON_GetObjectItem(root, "thr");
        if (cJSON_IsBool(jd)) s_mode_detect = cJSON_IsTrue(jd);
        if (cJSON_IsBool(jm)) s_mode_match = cJSON_IsTrue(jm);
        if (cJSON_IsNumber(jt)) {
            float t = (float)jt->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;      /* 允许 70 或 0.7 */
            if (t > 0.0f && t < 1.0f) s_match_thr = t;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "detect=%d match=%d thr=%.2f",
                 (int)s_mode_detect, (int)s_mode_match, s_match_thr);
        send_result("none", nullptr, 0, nullptr, 0, msg);
    } else if (strcmp(cmd, "register") == 0 || strcmp(cmd, "delete") == 0) {
        const cJSON *jn = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsString(jn)) jn = cJSON_GetObjectItem(root, "name");
        char name[32];
        name_sanitize(cJSON_IsString(jn) ? jn->valuestring : nullptr, name, sizeof(name));
        if (!name[0]) {
            send_result("error", nullptr, 0, nullptr, 0, "bad name");
        } else if (strcmp(cmd, "register") == 0) {
            if (!s_db_ok) {
                send_result("error", nullptr, 0, nullptr, 0, "feature db unavailable");
            } else if (!name_exists(name) && name_count_persons() >= MAX_PERSONS) {
                send_result("error", nullptr, 0, nullptr, 0, "lib full");
            } else {
                strlcpy(s_reg_name, name, sizeof(s_reg_name));
                s_reg_arm_us = esp_timer_get_time();
                s_reg_pending = true;
                s_running = true;
                ESP_LOGI(TAG, "register armed for '%s'", s_reg_name);
                /* 立即 ack：host 先等这条 ack，再发帧，注册就不会错配 */
                char ack[80];
                snprintf(ack, sizeof(ack), "register armed for %s, now send a face frame", s_reg_name);
                send_result("none", nullptr, 0, nullptr, 0, ack);
            }
        } else {
            int removed = 0;
            if (s_db_ok) {
                for (int i = 0; i < s_name_cnt; i++) {
                    if (strcmp(s_names[i].name, name) == 0) {
                        s_db->delete_feat(s_names[i].id);
                        removed++;
                    }
                }
            }
            if (removed) {
                name_remove_by_name(name);
                names_save();
                s_lib_dirty = true;
                char m[64];
                snprintf(m, sizeof(m), "deleted %d feat(s) of %s", removed, name);
                send_result("deleted", nullptr, 0, nullptr, 0, m);
            } else {
                send_result("error", nullptr, 0, nullptr, 0, "not found");
            }
        }
    } else if (strcmp(cmd, "src") == 0) {
        /* {"cmd":"src","value":"cam"|"pc"} —— 切板载摄像头 / PC 送帧 */
        const cJSON *jv = cJSON_GetObjectItem(root, "value");
        if (!cJSON_IsString(jv)) jv = cJSON_GetObjectItem(root, "src");
        const char *v = cJSON_IsString(jv) ? jv->valuestring : "";
        if (!strcmp(v, "cam") || !strcmp(v, "camera")) {
            app_imgpipe_select_src(APP_IMG_SRC_CAM);
        } else if (!strcmp(v, "pc") || !strcmp(v, "usb")) {
            app_imgpipe_select_src(APP_IMG_SRC_PC);
        } else {
            send_result("error", nullptr, 0, nullptr, 0, "src must be cam|pc");
            s_ack_cmd = nullptr;
            cJSON_Delete(root);
            return;
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "src=%s", app_imgpipe_src_str(app_imgpipe_get_src()));
        send_result("none", nullptr, 0, nullptr, 0, msg);
    } else if (strcmp(cmd, "stream") == 0) {
        /* {"cmd":"stream","value":"on"|"off","fps":10,"quality":75}
         * 图像回传（0x05）：LVGL 模拟器/上位机用它拿板端画面；value 省略=只查询状态 */
        const cJSON *jv = cJSON_GetObjectItem(root, "value");
        bool on = app_stream_get_enable();
        if (cJSON_IsString(jv)) {
            const char *v = jv->valuestring;
            if (!strcmp(v, "on") || !strcmp(v, "1")) {
                on = true;
            } else if (!strcmp(v, "off") || !strcmp(v, "0")) {
                on = false;
            } else {
                send_result("error", nullptr, 0, nullptr, 0, "stream value must be on|off");
                s_ack_cmd = nullptr;
                cJSON_Delete(root);
                return;
            }
        } else if (cJSON_IsBool(jv)) {
            on = cJSON_IsTrue(jv);
        } else if (jv != nullptr) {
            send_result("error", nullptr, 0, nullptr, 0, "stream value must be on|off");
            s_ack_cmd = nullptr;
            cJSON_Delete(root);
            return;
        }
        const cJSON *jf = cJSON_GetObjectItem(root, "fps");
        if (cJSON_IsNumber(jf)) {
            app_stream_set_fps((uint32_t)jf->valuedouble);
        }
        const cJSON *jq = cJSON_GetObjectItem(root, "quality");
        if (cJSON_IsNumber(jq)) {
            app_stream_set_quality((uint32_t)jq->valuedouble);
        }
        const cJSON *jfmt = cJSON_GetObjectItem(root, "fmt");
        if (cJSON_IsString(jfmt)) {
            if (!strcmp(jfmt->valuestring, "raw") || !strcmp(jfmt->valuestring, "rgb565")) {
                app_stream_set_format(APP_STREAM_FMT_RGB565);
            } else {
                app_stream_set_format(APP_STREAM_FMT_JPEG);
            }
        }
        const cJSON *jrev = cJSON_GetObjectItem(root, "reverse");
        if (cJSON_IsBool(jrev)) {
            app_stream_set_reverse(cJSON_IsTrue(jrev));
        }
        const cJSON *jsub = cJSON_GetObjectItem(root, "subsample");
        if (cJSON_IsNumber(jsub)) {
            app_stream_set_subsample((int)jsub->valueint);
        }
        app_stream_set_enable(on);
        app_stream_stats_t st = {};
        app_stream_get_stats(&st);
        char msg[144];
        snprintf(msg, sizeof(msg), "stream=%s fps=%u q=%u fmt=%s sub=%d sent=%u enc=%ums %uKB drop=%u",
                 app_stream_get_enable() ? "on" : "off",
                 (unsigned)app_stream_get_fps(), (unsigned)app_stream_get_quality(),
                 app_stream_get_format() == APP_STREAM_FMT_RGB565 ? "raw" : "jpeg",
                 app_stream_get_subsample(),
                 (unsigned)st.frames, (unsigned)st.enc_ms, (unsigned)(st.bytes / 1024),
                 (unsigned)st.drop);
        send_result("none", nullptr, 0, nullptr, 0, msg);
    } else if (strcmp(cmd, "cam") == 0) {
        /* 摄像头参数（夜视测试）：查询 / 设置 / 枚举
         *   {"cmd":"cam"}                                         只查询
         *   {"cmd":"cam","enum":true}                             列出全部 controls（走 0x04 日志）
         *   {"cmd":"cam","ae":false,"exp_us":20000,"gain":8}      关 AE + 手动曝光(µs)/增益(索引)
         *   {"cmd":"cam","brightness":128,"sat":64,"contrast":32} ISP 侧
         */
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "enum"))) {
            app_camera_dump_ctrls();
        }
        app_cam_ctrl_t c = {};
        uint32_t mask = 0;
        const cJSON *j;
        j = cJSON_GetObjectItem(root, "ae");
        if (cJSON_IsBool(j)) {
            c.auto_val = cJSON_IsTrue(j) ? 1 : 0;
            mask |= APP_CAM_SET_AUTO;
        }
        j = cJSON_GetObjectItem(root, "exp_us");
        if (cJSON_IsNumber(j)) {
            c.exp_val = (int32_t)(j->valuedouble / 100.0);      /* sc2336: 1 = 100µs */
            mask |= APP_CAM_SET_EXP;
        }
        j = cJSON_GetObjectItem(root, "exp");
        if (cJSON_IsNumber(j)) {
            c.exp_val = j->valueint;
            mask |= APP_CAM_SET_EXP;
        }
        j = cJSON_GetObjectItem(root, "gain");
        if (cJSON_IsNumber(j)) {
            c.gain_val = j->valueint;
            mask |= APP_CAM_SET_GAIN;
        }
        j = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(j)) {
            c.bright_val = j->valueint;
            mask |= APP_CAM_SET_BRIGHT;
        }
        j = cJSON_GetObjectItem(root, "sat");
        if (cJSON_IsNumber(j)) {
            c.sat_val = j->valueint;
            mask |= APP_CAM_SET_SAT;
        }
        j = cJSON_GetObjectItem(root, "contrast");
        if (cJSON_IsNumber(j)) {
            c.contrast_val = j->valueint;
            mask |= APP_CAM_SET_CONTRAST;
        }
        if (mask) {
            app_camera_set_ctrl(&c, mask);
        }
        /* 镜像/翻转（ISP 设备的 0x00980914 / 0x00980915，走 V4L2 路径）*/
        j = cJSON_GetObjectItem(root, "hflip");
        if (cJSON_IsBool(j)) {
            c.hflip_val = cJSON_IsTrue(j) ? 1 : 0;
            mask |= APP_CAM_SET_HFLIP;
        }
        j = cJSON_GetObjectItem(root, "vflip");
        if (cJSON_IsBool(j)) {
            c.vflip_val = cJSON_IsTrue(j) ? 1 : 0;
            mask |= APP_CAM_SET_VFLIP;
        }
        /* 画面镜像（驱动层改不了，管道里对摄像头帧做行内镜像） */
        j = cJSON_GetObjectItem(root, "mirror");
        if (cJSON_IsBool(j)) {
            app_imgpipe_set_hflip(cJSON_IsTrue(j));
        }
        /* 软件白平衡（管道内做，驱动侧 AWB/WB 都不可用）：
         *   {"cmd":"cam","wb_kr":1330,"wb_kb":1630}   —— 千分比，1000=不校正
         *   {"cmd":"cam","wb_kr":1000,"wb_kb":1000}   —— 关掉校正
         */
        /* 帧率（夜视长曝光）：{"cmd":"cam","fps":10} —— 降帧率后曝光上限变大。
         * 带 "auto_exp":true 时会顺手把曝光/增益拉到新的上限（UI 的夜视档一键用）。 */
        j = cJSON_GetObjectItem(root, "fps");
        if (cJSON_IsNumber(j)) {
            app_camera_set_fps((uint32_t)j->valuedouble);
            app_imgpipe_reset_prev();   /* 帧率变 -> 曝光上限变 -> 亮度会跳，丢弃时域历史 */
            if (cJSON_IsTrue(cJSON_GetObjectItem(root, "auto_exp"))) {
                app_cam_ctrl_t t = {};
                if (app_camera_get_ctrl(&t) == ESP_OK && t.have_exp && t.exp_max > 0) {
                    uint32_t mask = APP_CAM_SET_EXP;
                    /* 命令里显式给了 exp/gain 就用它（"夜视低噪"档给 gain≈120：曝光拉满
                       但增益不满，信噪比更好），没给才填满。注意本块在 exp/gain 赋值之后，
                       最终值以这里为准。 */
                    const cJSON *je = cJSON_GetObjectItem(root, "exp");
                    const cJSON *jg = cJSON_GetObjectItem(root, "gain");
                    t.exp_val = cJSON_IsNumber(je) ? (int32_t)je->valuedouble : t.exp_max;
                    if (t.have_gain) {
                        t.gain_val = cJSON_IsNumber(jg) ? (int32_t)jg->valuedouble : t.gain_max;
                        mask |= APP_CAM_SET_GAIN;
                    }
                    app_camera_set_ctrl(&t, mask);
                    app_usb_link_log("cam: night shift -> fps %d, exp %d, gain %d",
                                     (int)j->valueint, (int)t.exp_val, (int)t.gain_val);
                }
            }
        }

        /* 时域降噪档（弱光降噪，仅摄像头 raw 路径）：{"cmd":"cam","dn":0..3}
         * 0=关 1=低 2=中 3=高；高帧率下会被帧率守卫自动跳过（见 Kconfig） */
        j = cJSON_GetObjectItem(root, "dn");
        if (cJSON_IsNumber(j)) {
            app_imgpipe_set_dn(j->valueint);
        }
        /* 曝光/增益被改过就丢弃时域历史：亮度突变混进历史会留下整屏残影 */
        if (mask & (APP_CAM_SET_EXP | APP_CAM_SET_GAIN)) {
            app_imgpipe_reset_prev();
        }

        const cJSON *jkr = cJSON_GetObjectItem(root, "wb_kr");
        const cJSON *jkb = cJSON_GetObjectItem(root, "wb_kb");
        if (cJSON_IsNumber(jkr) || cJSON_IsNumber(jkb)) {
            int kr = 0, kb = 0;
            app_imgpipe_get_wb_gain(&kr, &kb);
            if (cJSON_IsNumber(jkr)) {
                kr = jkr->valueint;
            }
            if (cJSON_IsNumber(jkb)) {
                kb = jkb->valueint;
            }
            app_imgpipe_set_wb_gain(kr, kb);
        }

        /* ⚠️ 不要再调用 app_camera_set_wb()：驱动侧 WB 的 EXT_CTRLS 在 ISP pipeline
           controller 运行时**会阻塞**（实测把整个 faceai 任务卡死 → 0x02 全停）。
           驱动 WB 三条路本来就 FAIL，白平衡统一走软件方案 wb_kr/wb_kb（见上）。 */

        app_cam_ctrl_t now = {};
        app_camera_get_ctrl(&now);
        s_cam_state_valid = true;
        s_cam_gain = (int)now.gain_val;
        s_cam_gain_max = (int)now.gain_max;
        s_cam_exp = (int)now.exp_val;
        s_cam_exp_max = (int)now.exp_max;
        s_cam_mirror = app_imgpipe_get_hflip() ? 1 : 0;
        app_imgpipe_get_wb_gain(&s_cam_wb_kr, &s_cam_wb_kb);
        s_cam_fps = (int)app_camera_get_fps();
        s_cam_dn = app_imgpipe_get_dn();
        {
            app_imgpipe_stats_t pst = {};
            app_imgpipe_get_stats(&pst);
            s_cam_dn_ms = (int)pst.dn_ms;
            s_cam_dn_skip = (int)pst.dn_skip;
            s_cam_dn_blk = (int)pst.dn_blocks;
            s_cam_dn_sad = (int)pst.dn_sad;
        }
        char msg[224];
        /* exp 的单位是"行"（VTS≈1000 @640x480/50fps）：994 行 ≈ 19.9ms（=帧周期 20ms 的上限）。
           夜视想更长曝光只能降帧率（换 lower-fps sensor 格式或改 VTS）。 */
        snprintf(msg, sizeof(msg),
                 "exp=%d/%d | gain=%d/%d | mirror=%d | sat=%d | ae=%d",
                 (int)now.exp_val, (int)now.exp_max, (int)now.gain_val, (int)now.gain_max,
                 (int)app_imgpipe_get_hflip(), (int)now.sat_val, (int)now.auto_val);
        send_result("none", nullptr, 0, nullptr, 0, msg);
    } else if (strcmp(cmd, "list") == 0) {
        s_lib_dirty = true;
        send_result("list", nullptr, 0, nullptr, 0, nullptr);
    } else if (strcmp(cmd, "clear") == 0) {
        if (s_db_ok) s_db->clear_all_feats();
        s_name_cnt = 0;
        names_save();
        s_lib_dirty = true;
        send_result("deleted", nullptr, 0, nullptr, 0, "cleared");
    } else {
        send_result("error", nullptr, 0, nullptr, 0, "unknown cmd");
    }

    s_ack_cmd = nullptr;           /* 之后的帧结果不带 ack */
    cJSON_Delete(root);
}

/* 0x03 回调跑在 usb_rx 任务里：只拷进队列，解析放到 AI 任务 */
static void cmd_cb(const char *json, uint32_t len)
{
    if (!s_cmd_q || len == 0) return;
    cmd_msg_t msg;
    size_t n = len < (CMD_JSON_MAX - 1) ? len : (CMD_JSON_MAX - 1);
    memcpy(msg.json, json, n);
    msg.json[n] = 0;
    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop: %s", msg.json);
    }
}

/* ---------------- AI 任务 ---------------- */

static void faceai_task(void *arg)
{
    (void)arg;
    app_status_set(APP_MOD_FACE, true, "loading model");

    /* 1. 特征库所在分区 storage(FAT) */
    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = true;
    mcfg.max_files = 4;
    mcfg.allocation_unit_size = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FACE_DB_MOUNT, FACE_DB_PART, &mcfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "feature db mount failed: %s (detect only)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "feature db partition '%s' mounted at %s (first boot formats it, ~few s)",
                 FACE_DB_PART, FACE_DB_MOUNT);
    }

    /* 2. 模型加载（几秒；此间 imgpipe 内置 ack 仍在回 PC） */
    int64_t t0 = esp_timer_get_time();
    s_detect = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    float det_thr = CONFIG_APP_FACEAI_DETECT_THR / 100.0f;
    s_detect->set_score_thr(det_thr, 0);
    s_detect->set_score_thr(det_thr, 1);
    s_feat = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    if (err == ESP_OK) {
        s_db = new dl::recognition::DataBase(FACE_DB_PATH, s_feat->get_feat_len());
        s_db_ok = true;
        ESP_LOGI(TAG, "feature db %s: %d feat(s) in lib, name table %d", FACE_DB_PATH,
                 s_db->get_num_feats(), s_name_cnt);
    }
    ESP_LOGI(TAG, "models loaded in %d ms (detect_thr=%.2f match_thr=%.2f feat_len=%d)",
             (int)((esp_timer_get_time() - t0) / 1000), det_thr, s_match_thr,
             s_feat ? s_feat->get_feat_len() : 0);

    /* 3. 接管 0x02 应答 + 起循环 */
    app_imgpipe_set_auto_ack(false);
    s_fps_win = esp_timer_get_time();
    s_ready = true;
    app_status_set(APP_MOD_FACE, true, "ready lib %d", s_name_cnt);
    app_usb_link_log("faceai ready: MSRMNP+MFN, db=%s, lib=%d",
                     s_db_ok ? FACE_DB_PATH : "off", s_name_cnt);

    while (1) {
        cmd_msg_t msg;
        while (xQueueReceive(s_cmd_q, &msg, 0) == pdTRUE) {
            handle_cmd(msg.json);
        }

        img_frame_t fr;
        if (app_imgpipe_get_frame(&fr, 200) != ESP_OK) {
            continue;                       /* 无新帧 */
        }
        s_frames++;
        if (!s_running) {
            send_result("stopped", nullptr, 0, nullptr, 0, "send start");
        } else if (!s_mode_detect) {
            send_result("none", nullptr, 0, nullptr, 0, "detect off");
        } else {
            handle_frame(fr);
        }
        app_imgpipe_release(&fr);
    }
}

/* ---------------- 公共 API ---------------- */

extern "C" esp_err_t app_faceai_init(void)
{
    names_load();

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "cmd queue create");

    app_usb_link_set_cmd_cb(cmd_cb);

    if (xTaskCreate(faceai_task, "faceai", TASK_STACK, nullptr, TASK_PRIO, nullptr) != pdPASS) {
        return ESP_FAIL;
    }
    app_status_set(APP_MOD_FACE, true, "starting");
    ESP_LOGI(TAG, "faceai starting: detect_thr=%.2f match_thr=%.2f, db=%s",
             CONFIG_APP_FACEAI_DETECT_THR / 100.0f, s_match_thr, FACE_DB_PATH);
    return ESP_OK;
}

extern "C" bool app_faceai_ready(void)
{
    return s_ready;
}
