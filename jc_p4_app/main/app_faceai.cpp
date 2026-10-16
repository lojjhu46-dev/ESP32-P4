/*
 * app_faceai.cpp —— 板端边缘人脸检测 / 特征提取 / 1:N 比对（任务五）
 *
 *   app_imgpipe(RGB888) --> HumanFaceDetect(MSR+MNP) --> HumanFaceFeat(MFN)
 *        --> dl::recognition::DataBase(/storage/face.db) --> 0x02 JSON 回传 PC
 *
 * 说明：
 *   - 特征库用 esp-dl 自带 dl::recognition::DataBase（按 id 存 L2 归一化特征），
 *     落盘在 storage(FAT) 分区；名字 <-> id 映射存 /storage/names.json（F9），
 *     NVS(namespace "faceai") 旧格式仅作只读兜底 + 首次开机自动迁移。
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
#include "esp_cache.h"
#include "esp_rom_crc.h"
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
#include "app_camauto.h"
#include "app_wifi.h"
#include "esp_wifi.h"
#include "app_usb_link.h"
#include "app_status.h"
#include "sdkconfig.h"

#define TAG "faceai"

/* sdkconfig 对 bool=n 的符号不生成宏，这里兜底（同 app_camauto.c 的写法） */
#ifndef CONFIG_APP_CAM_NIGHT_FPS
#define CONFIG_APP_CAM_NIGHT_FPS 0
#endif
#ifndef CONFIG_APP_CAM_NIGHT_FPS_LOW
#define CONFIG_APP_CAM_NIGHT_FPS_LOW 5
#endif
#ifndef CONFIG_APP_CAM_NIGHT_FPS_MIN
#define CONFIG_APP_CAM_NIGHT_FPS_MIN 2
#endif
#ifndef CONFIG_APP_CAM_NIGHT_LUMA_DARK
#define CONFIG_APP_CAM_NIGHT_LUMA_DARK 45
#endif
#ifndef CONFIG_APP_CAM_NIGHT_LUMA_DARKER
#define CONFIG_APP_CAM_NIGHT_LUMA_DARKER 25
#endif
#ifndef CONFIG_APP_CAM_NIGHT_DN_LOW
#define CONFIG_APP_CAM_NIGHT_DN_LOW 2
#endif
#ifndef CONFIG_APP_CAM_NIGHT_DN_MIN
#define CONFIG_APP_CAM_NIGHT_DN_MIN 3
#endif
#ifndef CONFIG_APP_CAM_DN
#define CONFIG_APP_CAM_DN 0
#endif
/* 夜视档关人脸（默认开）；bool=n 时 sdkconfig.h 不生成宏，兜底为 0（关），
   改 Kconfig 后必须 idf.py reconfigure（见 HANDOVER 第四节） */
#ifndef CONFIG_APP_FACEAI_NIGHT_OFF
#define CONFIG_APP_FACEAI_NIGHT_OFF 0
#endif
/* 白天曝光余量阶梯（bool=y 默认开；=n 时不生成宏，兜底 0） */
#ifndef CONFIG_APP_CAM_DAY_STEP
#define CONFIG_APP_CAM_DAY_STEP 0
#endif
#ifndef CONFIG_APP_CAM_DAY_FPS_MID
#define CONFIG_APP_CAM_DAY_FPS_MID 20
#endif
#ifndef CONFIG_APP_CAM_DAY_FPS_LOW
#define CONFIG_APP_CAM_DAY_FPS_LOW 10
#endif
#ifndef CONFIG_APP_CAM_DAY_GAIN_GATE
#define CONFIG_APP_CAM_DAY_GAIN_GATE 48
#endif
/* AE 曝光上限（毫秒，0=不设上限）：暗光下 AE 不再把积分拉满帧周期（100ms
 * 模糊帧毁掉检测与特征），超过上限转提增益保锐度。1ms = 50 行。
 * 夜视档（tier>=1）自动解除——夜视要亮度且人脸已门控。运行时
 * {"cmd":"cam","ae_exp_max_ms":N}。2026-10-06 实测依据见 HANDOVER 廿二节 12 节 */
#ifndef CONFIG_APP_CAM_AE_EXP_MAX_MS
#define CONFIG_APP_CAM_AE_EXP_MAX_MS 40
#endif

#define FACE_DB_MOUNT   "/storage"
#define FACE_DB_PART    "storage"
#define FACE_DB_PATH    "/storage/face.db"
#define FACE_NVS_NS     "faceai"
#define FACE_NVS_KEY    "names"

/* F9 容量（2026-10-11）：人数/特征条数上限走 Kconfig，默认 64 人 / 320 条特征
   （一个名字可注册多张特征 = 名表多行）。改 Kconfig 后必须 idf.py reconfigure。 */
#ifndef CONFIG_APP_FACEAI_MAX_PERSONS
#define CONFIG_APP_FACEAI_MAX_PERSONS 64
#endif
#ifndef CONFIG_APP_FACEAI_MAX_FEATS
#define CONFIG_APP_FACEAI_MAX_FEATS 320
#endif
#define MAX_PERSONS     CONFIG_APP_FACEAI_MAX_PERSONS
#define MAX_FEATS       CONFIG_APP_FACEAI_MAX_FEATS
/* NVS 兜底的旧格式字符串上限（"id:name;..."）：IDF NVS 单个 str 值上限约 4000B，
   超过时该次兜底写跳过（主存储是 /storage/names.json，不受此限） */
#define NVS_NAMES_MAX   4000

/* ---- 白天识别质量（规划 F1/F2/F3，2026-10-04）---- */
#ifndef CONFIG_APP_FACEAI_MATCH_THR_HI
#define CONFIG_APP_FACEAI_MATCH_THR_HI 80
#endif
#ifndef CONFIG_APP_FACEAI_MATCH_THR_LO
#define CONFIG_APP_FACEAI_MATCH_THR_LO 70
#endif
/* 深探（同帧重复查询/重复提特征 + 关键点上报 + 推理后指纹复算）：
 * 默认关。⚠️ 开启会让每帧多跑 1~2 次 s_feat->run()，而 esp-dl 的执行是历史相关的，
 * 重复 run 会污染后续帧的特征（真机实测同一人 sim_raw 0.26~0.84 乱跳）——
 * 标定/质量评估必须关掉，只在专门查非确定性时打开（HANDOVER 廿一节）。 */
#ifndef CONFIG_APP_FACEAI_DIAG_DEEP
#define CONFIG_APP_FACEAI_DIAG_DEEP 0
#endif
/* F11：当前检测模型名（只用于日志/ack 显示） */
static const char *detect_model_name(void)
{
#if CONFIG_APP_FACEAI_DETECT_IS_ESPDET
    return "espdet_pico_224+mnp";
#else
    return "msrmnp_s8_v1";
#endif
}
#define FACE_SMOOTH_K            5      /* 平滑窗口：最近 K 帧的 top-1 结果 */
#define FACE_SMOOTH_M            3      /* HI 区确认所需命中帧数；灰区需要 M+1 */
#define FACE_ENROLL_MIN_SCORE    0.60f  /* 注册质量门控：检测分下限 */
#define FACE_ENROLL_MIN_PX       80     /* 注册质量门控：脸宽下限 px（640 宽下约 2.5~3m） */
#define FACE_ENROLL_MAX_SHOTS    5      /* 多帧注册最多采 5 帧（3~5 帧足够） */
/* ---- F4 轻跟踪 + 特征提取节流（规划 P0-F4，2026-10-06）----
 * 同一张脸、位移很小的相邻帧不必每帧都跑 93ms 的 MFN 提特征：沿用上一帧结论、
 * 只更新人脸框（esp-who / esp-box 都是"检测每帧、特征按需"）。
 * 关键：**必须有 HOLD 上限**——完全静止的脸每帧都"没动"，只看位移会一路跳过，
 * 提特征频率塌到 ~0、识别长时间不刷新。150ms ≈ 上限 6.7 次/秒。 */
#ifndef CONFIG_APP_FACEAI_TRACK_HOLD_MS
#define CONFIG_APP_FACEAI_TRACK_HOLD_MS 150
#endif
#ifndef CONFIG_APP_FACEAI_TRACK_IOU
#define CONFIG_APP_FACEAI_TRACK_IOU 40
#endif
#ifndef CONFIG_APP_FACEAI_TRACK_MOVE_PCT
#define CONFIG_APP_FACEAI_TRACK_MOVE_PCT 10
#endif
#define FACE_TRACK_IOU      (CONFIG_APP_FACEAI_TRACK_IOU / 100.0f)
#define FACE_TRACK_MOVE_PCT (CONFIG_APP_FACEAI_TRACK_MOVE_PCT / 100.0f)
/* ---- F6 事件去重 + F7 多人脸（规划 P1-F6/F7，2026-10-06）----
 * bool=n 时 sdkconfig.h 不生成宏，#ifndef 兜底（改 Kconfig 后必须 reconfigure） */
#ifndef CONFIG_APP_FACEAI_EVENT_DEDUP_MS
#define CONFIG_APP_FACEAI_EVENT_DEDUP_MS 5000
#endif
#ifndef CONFIG_APP_FACEAI_MULTI_MAX
#define CONFIG_APP_FACEAI_MULTI_MAX 2
#endif
/* F2 快速通道：本帧裸相似度 >= 此值（百分比）时不等窗口票数直接确认 match。
 * 检出间歇时窗口凑不满 M/M+1 票是命中率真瓶颈（第廿二节第 8 节：53% 过线帧
 * 被票数吃掉 8 个）；离线复算加 0.80 快速通道后命中 2/19 -> 8/19、B 误报 0。
 * 真人陌生人 win-max 上沿 0.587 → 默认 0.80 余量 +0.21。0 = 关闭。 */
#ifndef CONFIG_APP_FACEAI_FAST_THR
#define CONFIG_APP_FACEAI_FAST_THR 80
#endif
#define FACE_ENROLL_INTERVAL_US  (400 * 1000)  /* 相邻采集帧最小间隔：拉开拿自然姿态变化 */
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
/* F11：ESPDet 模式下额外用 MNP 二级补关键点（ESPDet 自身不出 landmark） */
human_face_detect::MNP *s_mnp = nullptr;
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
/* F3 双阈值：sim>=HI 命中 M 帧即确认；LO..HI 灰区需 M+1 帧；<LO 视为未命中 */
float            s_thr_hi = CONFIG_APP_FACEAI_MATCH_THR_HI / 100.0f;
float            s_thr_lo = CONFIG_APP_FACEAI_MATCH_THR_LO / 100.0f;
/* 检测分数阈值（F11 调试用）：运行时可改，{"cmd":"mode","det_thr":0.35}。
 * ⚠️ 只能在 faceai 任务里改（命令在主循环里处理），不能在别的任务直接 ioctl 级调用 */
float            s_det_thr = CONFIG_APP_FACEAI_DETECT_THR / 100.0f;
bool             s_reg_pending = false;
int64_t          s_reg_arm_us = 0;      /* 武装注册的时刻，只接受其后发布的帧 */
char             s_reg_name[32] = { 0 };
/* F1 多帧注册向导：shots=1 即旧的单帧注册 */
int              s_reg_shots = 0;       /* 目标采集帧数 */
int              s_reg_got = 0;         /* 已入库帧数 */
int64_t          s_reg_last_us = 0;     /* 上一帧采集时刻（帧间隔拉开用） */
/* F2 时域平滑：最近 K 帧 top-1 结果环形缓冲（人脸离开即清空） */
struct smooth_rec_t { uint16_t id; float sim; };
smooth_rec_t     s_smooth[FACE_SMOOTH_K];
int              s_smooth_cnt = 0;      /* 已填充帧数 */
int              s_smooth_pos = 0;      /* 下一写位置 */
/* F4 轻跟踪：上一帧"真正提过特征"的脸框 + 那帧的结论（跳过帧沿用）。
 * 只缓存一条（当前只识别最大脸）；F7 多人脸时按脸各存一条。 */
enum face_track_ev_t { TRK_NONE = 0, TRK_MATCH, TRK_REJECT, TRK_TRACKING };
bool             s_trk_ok = false;
int              s_trk_box[4] = { 0, 0, 0, 0 };
int64_t          s_trk_us = 0;          /* 上次提特征的帧时刻（fr.ts_us） */
int              s_trk_ev = TRK_NONE;
char             s_trk_name[32] = { 0 };
float            s_trk_sim = 0.0f;
float            s_trk_raw = 0.0f;
int              s_trk_stable = 0;
int              s_trk_reused = 0;      /* 累计跳过的帧数（诊断） */
/* F4 验收指标：每秒提特征次数（1s 窗口）+ 本帧是否走了节流 */
int              s_dbg_reuse = 0;       /* 结果帧字段 reuse（1=本帧沿用未提特征） */
int              s_feat_cnt = 0;        /* 本秒提特征次数 */
float            s_feat_hz = 0.0f;      /* 每秒提特征次数 */
int64_t          s_feat_win_us = 0;
/* F4 保持窗（运行时可调，照 det_thr 的先例）：{"cmd":"mode","trk_hold_ms":400}。
 * ⚠️ 现场实测（2026-10-06）：AI 处理一帧要 ~244ms（解码+检测+93ms 提特征），
 * 比编译默认的 150ms 还长 → 保持窗**永远等不到触发**（先有鸡先有蛋：只有跳过
 * 提特征才能跑到 150ms 以内，而跑不进 150ms 就永远不跳）。所以必须能现场扫。 */
int              s_trk_hold_ms = CONFIG_APP_FACEAI_TRACK_HOLD_MS;
/* ---- F6 事件去重（规划 P1-F6）：同一人的 match / 陌生人的稳定 reject 在 T ms
 * 内只报一次事件（esp-box 门禁"识别成功欢迎一次"的语义）。窗口期内 event 回落
 * 为 "none"，但 match/sim/stable 字段照常携带——UI 名字显示不闪断，host 侧
 * 开门/日志联动靠事件边沿触发。0 = 关闭去重。 */
int              s_dedup_ms = CONFIG_APP_FACEAI_EVENT_DEDUP_MS;
char             s_evt_match_name[32] = { 0 };   /* 上次 match 事件的名字（去重键） */
int64_t          s_evt_match_us = 0;
int64_t          s_evt_rej_us = 0;               /* 上次稳定 reject 事件时刻 */
/* F2 快速通道：本帧 raw_sim >= 此值直接确认（不等窗口票数）；0 = 关。
 * 只信"本帧自己的强测量"——多人库时名字取本帧 top-1 的 id，不是窗口多数派。 */
float            s_fast_thr = CONFIG_APP_FACEAI_FAST_THR / 100.0f;
/* esp-dl 工作区清零实验（2026-10-06，HANDOVER 廿二节 14 节）：特征非确定性
 * （同输入两次 run 结果不同、跨 boot"抽签"）怀疑是模型读了上一次运行残留的
 * arena 工作区内存（中间张量构建时 calloc 一次后跨 run 永久复用、从不清零）。
 * 0=关 1=每次提特征前清零工作区 2=随机毒化（输出不随垃圾变=残留无害；
 * 随垃圾变=假设证实）。运行时 {"cmd":"mode","feat_ws":0|1|2}。 */
int              s_feat_ws_mode = 0;
/* 逐层二分（工作区实验续）：feat_bisect=K 武装 K 轮；每轮带脸帧跑两次 traced
 * run（每算子后对整个 arena 算 CRC），报告 A/B 同帧分歧级与跨帧首分歧级。
 * （ref 对照基准与 valid 标志定义在 feat_bisect_analyze 前。） */
int              s_feat_bisect_left = 0;
/* ---- F7 多人脸（规划 P1-F7）：按面积前 N 的脸分别提特征比对。
 * s_multi[0] = 主脸（第 1 大，走完整 F2/F4/F6 路径，顶层 match/sim 语义不变）；
 * [1..] = 副脸（每帧裸比对、无平滑窗口，raw_sim>=thr_lo 才报名字）。
 * send_result 按 res 指针把 name/sim 追加到对应 boxes[i]（第 5/6 个元素，
 * 旧解析只读前 4 个坐标，向后兼容）。 */
struct multi_rec_t {
    const dl::detect::result_t *res;   /* 指向本帧 det 列表元素（指针匹配用） */
    const char *name;                  /* "" = 未命中/未提特征 */
    float       sim;
};
multi_rec_t      s_multi[3];
int              s_multi_n = 0;          /* 本帧填了几个条目（0 = 无脸/未走匹配） */
int              s_multi_max = CONFIG_APP_FACEAI_MULTI_MAX;
multi_rec_t      s_multi_cache[2];       /* 副脸结论快照：F4 复用帧不提特征，回放用 */
int              s_multi_cache_n = 0;
/* 结果帧附加诊断（匹配分支每帧刷新；<0 = 结果帧不带该字段） */
int              s_dbg_stable = -1;
float            s_dbg_sim_raw = 0.0f;
/* 缓存一致性诊断（HANDOVER 廿一节）：生产者指纹在 fr.crc32/fr.dec_core，
 * 这里存消费者侧指纹与所在核 */
uint32_t         s_dbg_ai_crc = 0;
int              s_dbg_ai_core = -1;
uint32_t         s_dbg_frm_crc = 0;
int              s_dbg_dec_core = -1;
#if CONFIG_APP_FACEAI_DIAG_DEEP
uint32_t         s_dbg_ai_crc2 = 0;
float            s_dbg_sim_q2 = -1.0f;   /* 同帧第二次查询（诊断） */
float            s_dbg_sim_f2 = -1.0f;   /* 同帧第二次提特征后的查询（诊断） */
#endif
/* 关键点上报（**不额外跑模型**，所以开着也不影响被测对象）：排查
 * "同一张图自匹配却上不去" 时看 kp 在跨帧时是否抖动（对齐错位的直接证据）。
 * 结果帧字段 "kp"（10 个整数）。 */
#ifndef CONFIG_APP_FACEAI_DIAG_KP
#define CONFIG_APP_FACEAI_DIAG_KP 0
#endif
#if CONFIG_APP_FACEAI_DIAG_KP
std::vector<int> s_dbg_kp;
#endif
bool             s_lib_dirty = false;
volatile bool    s_ready = false;

uint8_t         *s_pack_buf = nullptr;   /* 行距不一致时的重打包缓冲 */
size_t           s_pack_cap = 0;

uint32_t         s_frames = 0;
uint32_t         s_drops = 0;
const char      *s_ack_cmd = nullptr;   /* 非空时，send_result 会带上 "ack":<命令名> */
cJSON           *s_extra_obj = nullptr; /* 非空时 send_result 并入 "dbx" 字段并清零（F10）*/

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
int  s_cam_dn_sad = 0;               /* 全帧「块内差均值」的平均（调参用） */
int  s_cam_dn_mmax = 0;              /* 全帧「块内最大差」的平均（当前运动判据，调参用） */
int  s_cam_dn_mv = 0;                /* 运动块占比 %（UI 结果行显示 mvNN%） */
int  s_cam_mv_peak = 0;              /* 运动估计：全帧块最大差峰值 0..63 */
int  s_cam_mv_ratio = 0;             /* 运动估计：运动块占比 % */
int  s_cam_mv_ms = 0;                /* 运动估计耗时 */
int64_t s_cam_mv_us = 0;             /* 运动统计的时刻（判断样本是否新鲜） */
int  s_cam_mv_ready = 0;             /* 该样本是否来自真实比较（0=基准帧，mv 恒 0） */
int  s_cam_luma = 0;                 /* AE 统计：帧内平均亮度 0..255（imgpipe 采样） */
int  s_cam_ccm = CONFIG_APP_CAM_CCM_MIX;   /* CCM 混合千分比（与 app_camera_init 的初始下发一致） */
/* AE 曝光上限（ms，0=不设）：暗光保锐度，夜视档自动解除（见 Kconfig 说明） */
int  s_ae_exp_cap_ms = CONFIG_APP_CAM_AE_EXP_MAX_MS;
bool s_auto_fps = false;             /* 自动帧率模式开关（运动自适应 15/50fps） */
int  s_auto_af_ms = 0;               /* 最近一次自动切帧耗时（结果帧 af_ms） */
/* 夜视阶梯（#1 按暗度降帧率 + #2 自动时域降噪）：与上面的运动自适应正交，
   运动只往高帧率升档、暗度只往低帧率降档，二者共用 auto_fps_apply() 做切换编排 */
bool s_night_fps = CONFIG_APP_CAM_NIGHT_FPS;  /* 总开关；手动 fps 会让位 */
int  s_night_tier = 0;               /* 当前生效档 0=正常 1=暗 2=很暗（结果帧 ntier） */
int  s_night_pend = 0;               /* 待确认档位（未过保持时间） */
int  s_night_luma_ema = -1;          /* 亮度 EMA（-1=未建立） */
int64_t s_night_state_us = 0;        /* 待确认档位的起始时刻（保持时间计时） */
bool s_dn_manual = false;            /* 手动调过 dn：自动 DN 让位 */
int  s_dn_user = CONFIG_APP_CAM_DN;  /* 用户手动 dn 值（回 tier0 时恢复） */
/* 光照双模式（低光照夜视 / 白天人脸识别）：夜视阶梯 tier>=1（暗）时 AI 不取帧
   不推理 —— 帧池整个让给 0x05 夜视预览；光照回 tier0 自动恢复识别。
   PC 送帧（src=pc）永不门控：阶梯 luma 只在摄像头路径采样，且窗口=1 的
   每帧应答语义必须保持。运行时 {"cmd":"cam","face_night":bool} 覆盖。 */
bool s_face_night_off = CONFIG_APP_FACEAI_NIGHT_OFF;
bool s_face_gated = false;           /* 上一轮门控状态（边沿检测，发模式切换日志） */
int64_t s_night_res_us = 0;          /* 上次夜视状态帧时刻（2Hz 节流） */
/* 白天曝光余量阶梯（第十七节）：50fps 曝光上限只有 20ms，房间偏暗时 exp 顶满、
   亮度靠增益硬提（噪点大）——降一档帧率换进光。0=50fps 1=20fps 2=10fps。 */
static int  s_day_level = 0;
static int  s_day_pend = 0;          /* 待确认档位（未过保持时间） */
static int  s_day_gain_ema = -1;     /* 增益 EMA（-1=未建立）：AE 打摆的平滑口径 */
static int64_t s_day_state_us = 0;   /* 待确认档位的起始时刻 */
static int64_t s_day_switch_us = 0;  /* 上次切档时刻（冷却防振荡） */
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

/* ---- F2 时域平滑：最近 K 帧 top-1 结果环形缓冲 ---- */
static void smooth_reset(void)
{
    s_smooth_cnt = 0;
    s_smooth_pos = 0;
}

static void smooth_push(uint16_t id, float sim)
{
    s_smooth[s_smooth_pos].id = id;
    s_smooth[s_smooth_pos].sim = sim;
    s_smooth_pos = (s_smooth_pos + 1) % FACE_SMOOTH_K;
    if (s_smooth_cnt < FACE_SMOOTH_K) s_smooth_cnt++;
}

/* 窗口统计：
 *   best_id/best_cnt/best_sim = 出现帧数最多的 id（并列取 sim 更高者）及其帧数、窗口内最大 sim
 *   win_max = 全窗（含未命中帧，sim=0）最大 sim */
static void smooth_tally(uint16_t *best_id, int *best_cnt, float *best_sim, float *win_max)
{
    uint16_t ids[FACE_SMOOTH_K];
    int cnt[FACE_SMOOTH_K] = { 0 };
    float mx[FACE_SMOOTH_K] = { 0 };
    int n = 0;
    *win_max = 0.0f;
    for (int i = 0; i < s_smooth_cnt; i++) {
        const smooth_rec_t &r = s_smooth[i];
        if (r.sim > *win_max) *win_max = r.sim;
        int f = -1;
        for (int j = 0; j < n; j++) {
            if (ids[j] == r.id) { f = j; break; }
        }
        if (f < 0) { ids[n] = r.id; f = n++; }
        cnt[f]++;
        if (r.sim > mx[f]) mx[f] = r.sim;
    }
    *best_id = 0; *best_cnt = 0; *best_sim = 0.0f;
    for (int j = 0; j < n; j++) {
        if (ids[j] == 0) continue;             /* id 0 = 空库/无返回，不算候选 */
        if (cnt[j] > *best_cnt || (cnt[j] == *best_cnt && mx[j] > *best_sim)) {
            *best_id = ids[j]; *best_cnt = cnt[j]; *best_sim = mx[j];
        }
    }
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

/* F9（2026-10-11）：名表主存储 /storage/names.json（cJSON 数组 [{id,name}...]），
 * NVS(faceai/names) 旧格式字符串仅作只读兜底；首次开机 names.json 缺失时从 NVS
 * 自动迁移。文件无 NVS 单值长度限制，人数上限因此可从 10 提到 64+。 */
#define FACE_NAMES_PATH "/storage/names.json"
#define FACE_NAMES_TMP  "/storage/names.tmp"

/* F10（2026-10-11）：特征库备份/恢复的文件与信封常量。
   ⚠️ 文件名一律 8.3 安全（F9 教训：FATFS 未开 LFN 时 4 字符扩展名会静默失败）。
   信封布局（与 PC 侧 .bin 逐字节一致）：
     magic "JCDF"(4) | ver u32 | face_len u32 | names_len u32 | names | face  */
#define DBX_MAGIC      0x4644434AU                  /* "JCDF"（小端）*/
#define DBX_VER        1U
#define DBX_HDR_LEN    16U                          /* magic|ver|face_len|names_len */
#define DBX_MAX_BYTES  (2U * 1024 * 1024)           /* 信封总长上限（拒绝超限）*/
#define DBX_FACE_TMP   "/storage/face_new.db"
#define DBX_FACE_BAK   "/storage/face_bak.db"
#define DBX_NAMES_BAK  "/storage/names.bak"
#define DBX_RECV_PATH  "/storage/dbx_recv.bin"      /* 导入接收文件（存在 = 会话进行中）*/

/* 读整个文件到 malloc 缓冲（末尾补 '\0'，*len_out 为真实字节数）；失败返回 NULL */
static uint8_t *read_file_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nullptr;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > DBX_MAX_BYTES) {
        fclose(f);
        return nullptr;
    }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    if (len_out) *len_out = rd;
    return buf;
}

/* 解析名表 JSON 数组（[{id,name}...]）到 out；返回条数，-1 = JSON 非法。
   不触碰全局名表（F10 导入先校验后提交）。 */
static int names_parse_buf(const char *buf, face_name_t *out, int out_max)
{
    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }
    int cnt = 0;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, root) {
        if (cnt >= out_max) break;
        const cJSON *jid = cJSON_GetObjectItem(it, "id");
        const cJSON *jnm = cJSON_GetObjectItem(it, "name");
        if (!cJSON_IsNumber(jid) || !cJSON_IsString(jnm)) continue;
        int id = jid->valueint;
        if (id <= 0 || id > 0xFFFF) continue;
        char nm[sizeof(out[0].name)];
        name_sanitize(jnm->valuestring, nm, sizeof(nm));
        if (!nm[0]) continue;
        out[cnt].id = (uint16_t)id;
        strlcpy(out[cnt].name, nm, sizeof(out[0].name));
        cnt++;
    }
    cJSON_Delete(root);
    return cnt;
}

/* 名表（内存态）→ JSON 字符串；调用方用 cJSON_free() 释放。names.json 与
   F10 备份信封里的 names 段都由它生成，保证两处格式一致。 */
static char *names_to_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (int i = 0; i < s_name_cnt; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddNumberToObject(o, "id", s_names[i].id);
        cJSON_AddStringToObject(o, "name", s_names[i].name);
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return js;
}

/* 从 names.json 读取；返回 true = 文件存在且解析成功（可能 0 条） */
static bool names_load_json(void)
{
    size_t len = 0;
    uint8_t *buf = read_file_all(FACE_NAMES_PATH, &len);
    if (!buf) return false;
    int cnt = names_parse_buf((const char *)buf, s_names, MAX_FEATS);
    free(buf);
    if (cnt < 0) return false;
    s_name_cnt = cnt;
    return true;
}

/* 从 NVS 旧格式 "1:user1;2:user2" 读取；返回读到的条数（写入 s_names[0..]） */
static int names_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(FACE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int cnt = 0;
    size_t len = 0;
    if (nvs_get_str(h, FACE_NVS_KEY, nullptr, &len) == ESP_OK && len > 1 && len < NVS_NAMES_MAX) {
        char *buf = (char *)malloc(len);
        if (buf) {
            if (nvs_get_str(h, FACE_NVS_KEY, buf, &len) == ESP_OK) {
                char *save = nullptr;
                for (char *tok = strtok_r(buf, ";", &save);
                     tok && cnt < MAX_FEATS;
                     tok = strtok_r(nullptr, ";", &save)) {
                    char *sep = strchr(tok, ':');
                    if (!sep) continue;
                    *sep = 0;
                    int id = atoi(tok);
                    if (id <= 0 || id > 0xFFFF) continue;
                    name_sanitize(sep + 1, s_names[cnt].name, sizeof(s_names[0].name));
                    if (!s_names[cnt].name[0]) continue;
                    s_names[cnt].id = (uint16_t)id;
                    cnt++;
                }
            }
            free(buf);
        }
    }
    nvs_close(h);
    return cnt;
}

/* 写 /storage/names.json（tmp + rename 原子替换；FATFS 的 rename 不覆盖已存在目标，
   故目标存在时先 remove 再 rename） */
static bool names_save_json(void)
{
    char *js = names_to_json();
    if (!js) return false;

    bool ok = false;
    /* 主路径：tmp + rename 原子替换（FATFS 的 rename 不覆盖已存在目标，故先 remove） */
    FILE *f = fopen(FACE_NAMES_TMP, "w");
    if (f) {
        size_t len = strlen(js);
        ok = (fwrite(js, 1, len, f) == len) && (fflush(f) == 0);
        fclose(f);
        if (ok && rename(FACE_NAMES_TMP, FACE_NAMES_PATH) != 0) {
            remove(FACE_NAMES_PATH);
            if (rename(FACE_NAMES_TMP, FACE_NAMES_PATH) != 0) ok = false;
        }
        if (!ok) remove(FACE_NAMES_TMP);
    }
    /* 兜底：直接写目标文件（非原子，但好过完全没有；例如 rename 不可用时） */
    if (!ok) {
        FILE *d = fopen(FACE_NAMES_PATH, "w");
        if (d) {
            size_t len = strlen(js);
            ok = (fwrite(js, 1, len, d) == len) && (fflush(d) == 0);
            fclose(d);
        }
    }
    cJSON_free(js);
    return ok;
}

/* 兜底：尽力同步 NVS 旧格式字符串（超长则告警跳过，不阻断主路径） */
static void names_save_nvs(void)
{
    char *buf = (char *)malloc(NVS_NAMES_MAX);
    if (!buf) return;
    size_t off = 0;
    buf[0] = 0;
    bool truncated = false;
    for (int i = 0; i < s_name_cnt; i++) {
        char item[48];
        int n = snprintf(item, sizeof(item), "%u:%s", (unsigned)s_names[i].id, s_names[i].name);
        if (n <= 0) continue;
        size_t need = (size_t)n + (off ? 1 : 0);
        if (off + need + 1 >= NVS_NAMES_MAX) {
            truncated = true;
            break;
        }
        if (off) buf[off++] = ';';
        memcpy(buf + off, item, (size_t)n);
        off += (size_t)n;
        buf[off] = 0;
    }
    if (truncated) {
        ESP_LOGW(TAG, "NVS name fallback overflow (>%d B): names.json is source of truth",
                 NVS_NAMES_MAX);
        free(buf);
        return;
    }

    nvs_handle_t h;
    if (nvs_open(FACE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, names fallback not persisted");
        free(buf);
        return;
    }
    esp_err_t err = off ? nvs_set_str(h, FACE_NVS_KEY, buf) : nvs_erase_key(h, FACE_NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs names fallback write failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    free(buf);
}

/* 名表保存：主路径 names.json，另尽力同步 NVS 兜底 */
static void names_save(void)
{
    if (!names_save_json()) {
        ESP_LOGW(TAG, "names.json write failed (fallback NVS only)");
    }
    names_save_nvs();
}

/* 名表加载（⚠️ 必须在 storage 挂载之后调用）：names.json 优先；缺失则从 NVS 迁移 */
static void names_load(void)
{
    s_name_cnt = 0;
    if (names_load_json()) {
        ESP_LOGI(TAG, "names loaded from %s: %d", FACE_NAMES_PATH, s_name_cnt);
        return;
    }
    int nvs_cnt = names_load_nvs();
    if (nvs_cnt > 0) {
        s_name_cnt = nvs_cnt;
        ESP_LOGW(TAG, "names.json missing -> migrated %d name(s) from NVS", nvs_cnt);
        app_usb_link_log("faceai: names.json missing, migrated %d name(s) from NVS", nvs_cnt);
        names_save();                           /* 落盘 names.json */
    } else {
        ESP_LOGI(TAG, "names empty (no names.json, no NVS)");
    }
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
                /* F7 多人脸：按面积前 N 的脸在第 5/6 位追加 name/sim
                 * （旧解析只读前 4 个坐标，向后兼容；未提特征的脸不加元素）*/
                for (int m = 0; m < s_multi_n; m++) {
                    if (s_multi[m].res == &r) {
                        cJSON_AddItemToArray(b, cJSON_CreateString(
                                                    s_multi[m].name ? s_multi[m].name : ""));
                        cJSON_AddItemToArray(b, cJSON_CreateNumber((double)s_multi[m].sim));
                        break;
                    }
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
    if (res && s_dbg_stable >= 0) {
        /* F2/F5 诊断字段：stable=本帧是否通过 K 帧确认；sim_raw=本帧无门槛 top-1
         * 相似度（阈值标定工具 thr_calib.py 的原始数据；低于 LO 也如实带出）。
         * 仅流式结果帧携带（res 非空），命令 ack 不带。 */
        cJSON_AddNumberToObject(root, "stable", s_dbg_stable);
        cJSON_AddNumberToObject(root, "sim_raw", s_dbg_sim_raw);
        /* 缓存一致性诊断：frm_crc=生产者指纹（stride*height，含 MCU 填充列），
         * ai_crc=AI 输入指纹（row_bytes*height，重打包紧凑缓冲）——两者统计范围
         * 不同、数值恒不相等；正确用法是「同图各帧应各自恒定」（frm_crc 恒定=
         * 解码确定；ai_crc 恒定=模型输入确定），漂移即非确定性（HANDOVER 廿一节） */
        cJSON_AddNumberToObject(root, "frm_crc", s_dbg_frm_crc);
        cJSON_AddNumberToObject(root, "ai_crc", s_dbg_ai_crc);
        cJSON_AddNumberToObject(root, "dec_core", s_dbg_dec_core);
        cJSON_AddNumberToObject(root, "ai_core", s_dbg_ai_core);
        /* F4 轻跟踪：reuse=1 表示本帧跳过了提特征（沿用上帧结论，sim 是复读值，
         * 做标定/统计时应当剔除）；feat_hz=每秒提特征次数（验收 5~10 次/秒）。*/
        cJSON_AddNumberToObject(root, "reuse", s_dbg_reuse);
        cJSON_AddNumberToObject(root, "feat_hz",
                                (float)((int)(s_feat_hz * 10.0f) / 10.0));
#if CONFIG_APP_FACEAI_DIAG_DEEP
        cJSON_AddNumberToObject(root, "ai_crc2", s_dbg_ai_crc2);
        cJSON_AddNumberToObject(root, "sim_q2", s_dbg_sim_q2);
        cJSON_AddNumberToObject(root, "sim_f2", s_dbg_sim_f2);
#endif
#if CONFIG_APP_FACEAI_DIAG_KP
        if (!s_dbg_kp.empty()) {
            cJSON *kp = cJSON_AddArrayToObject(root, "kp");
            for (int v : s_dbg_kp) cJSON_AddItemToArray(kp, cJSON_CreateNumber(v));
        }
#endif
    }
    cJSON_AddNumberToObject(root, "fps", s_fps);
    cJSON_AddNumberToObject(root, "infer_ms", (int)infer_ms);
    cJSON_AddNumberToObject(root, "lib_n", s_name_cnt);
    cJSON_AddNumberToObject(root, "dropped", (int)s_drops);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddStringToObject(root, "src", app_imgpipe_src_str(app_imgpipe_get_src()));
    /* 2026-10-16：当前生效的发送链路（"usb-cdc" / "tcp"），上位机用它显示链路徽标 */
    cJSON_AddStringToObject(root, "link", app_usb_link_active_name());
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
            cJSON_AddNumberToObject(root, "dn_mmax", s_cam_dn_mmax);
            cJSON_AddNumberToObject(root, "dn_mv", s_cam_dn_mv);
            if (s_cam_dn_skip > 0) {
                cJSON_AddNumberToObject(root, "dn_skip", s_cam_dn_skip);
            }
            }
            /* 自动帧率状态 + 实时运动信号：恒带（不依赖 cam 命令快照），UI 靠它们逐帧刷新。
             * mv 来源随模式切换：自动模式下用独立运动估计的占比；
             * 关闭时回退降噪的 dn_motion（降噪开着的话它同样反映画面动静）*/
            cJSON_AddNumberToObject(root, "auto", s_auto_fps ? 1 : 0);
            cJSON_AddNumberToObject(root, "af_ms", s_auto_af_ms);   /* 最近一次自动切帧耗时 */
            cJSON_AddNumberToObject(root, "mv", s_auto_fps ? s_cam_mv_ratio : s_cam_dn_mv);
            cJSON_AddNumberToObject(root, "mv_peak", s_cam_mv_peak);
            /* AE/AWB 自动模式状态 + 实时亮度：恒带，UI 靠它们同步按钮/滑块 */
            cJSON_AddNumberToObject(root, "ae", app_camauto_get_ae() ? 1 : 0);
            cJSON_AddNumberToObject(root, "awb", app_camauto_get_awb() ? 1 : 0);
            cJSON_AddNumberToObject(root, "luma", s_cam_luma);
            cJSON_AddNumberToObject(root, "ae_tgt", app_camauto_get_target());
            /* 夜视增强当前值（gamma 千分比 0=禁用/1000=线性；bf level 0=禁用） */
            int enh_gamma = 0, enh_bf = 0;
            app_camauto_get_enh(&enh_gamma, &enh_bf);
            cJSON_AddNumberToObject(root, "gamma", enh_gamma);
            cJSON_AddNumberToObject(root, "bf", enh_bf);
            /* CCM 颜色校正混合比例（千分比，0=模块禁用） */
            cJSON_AddNumberToObject(root, "ccm", s_cam_ccm);
            /* WiFi STA IP（未关联时不带该字段） */
            {
                static char wip[16];
                if (app_wifi_get_ip(wip, sizeof(wip))) {
                    cJSON_AddStringToObject(root, "wip", wip);
                }
            }
            /* WiFi 关联探针：已关联时带 RSSI（负值），未关联不带。
             * ⚠️ 节流 2s：未关联时每次 RPC 都会在 esp_hosted 侧刷错误日志 */
            {
                static int64_t last_wifi_probe_us = 0;
                static int wrssi_cache = 0;
                static bool wrssi_valid = false;
                int64_t now_us = esp_timer_get_time();
                if (now_us - last_wifi_probe_us >= 2 * 1000000) {
                    wifi_ap_record_t ap;
                    last_wifi_probe_us = now_us;
                    wrssi_valid = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
                    if (wrssi_valid) {
                        wrssi_cache = ap.rssi;
                    }
                }
                if (wrssi_valid) {
                    cJSON_AddNumberToObject(root, "wrssi", wrssi_cache);
                }
            }
            /* 夜视阶梯（暗度降帧率 + 自动降噪）：night=总开关，ntier=当前档 0/1/2 */
            cJSON_AddNumberToObject(root, "night", s_night_fps ? 1 : 0);
            cJSON_AddNumberToObject(root, "ntier", s_night_tier);
            /* 光照双模式：night=1 && ntier>=1 && face_night=1 时 AI 不跑人脸
               （此时结果帧由夜视模式以 2Hz 低频发出，event="night"） */
            cJSON_AddNumberToObject(root, "face_night", s_face_night_off ? 1 : 0);
    }
    /* F10：库备份/恢复的附加字段（一次性挂入，随即清零） */
    if (s_extra_obj) {
        cJSON_AddItemToObject(root, "dbx", s_extra_obj);
        s_extra_obj = nullptr;
    }
    if (msg) cJSON_AddStringToObject(root, "msg", msg);
    if (s_lib_dirty) {
        cJSON *lib = cJSON_CreateArray();
        cJSON *lib_feats = cJSON_CreateArray();   /* F8：与 lib 平行的每人特征条数 */
        if (lib && lib_feats) {
            for (int i = 0; i < s_name_cnt; i++) {         /* 只列唯一名字 */
                bool dup = false;
                for (int j = 0; j < i; j++) {
                    if (strcmp(s_names[j].name, s_names[i].name) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    int cnt = 0;
                    for (int j = 0; j < s_name_cnt; j++) {
                        if (strcmp(s_names[j].name, s_names[i].name) == 0) cnt++;
                    }
                    cJSON_AddItemToArray(lib, cJSON_CreateString(s_names[i].name));
                    cJSON_AddItemToArray(lib_feats, cJSON_CreateNumber(cnt));
                }
            }
            cJSON_AddItemToObject(root, "lib", lib);
            cJSON_AddItemToObject(root, "lib_feats", lib_feats);
        } else {
            if (lib) cJSON_Delete(lib);
            if (lib_feats) cJSON_Delete(lib_feats);
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

/* ---------------- F4 轻跟踪辅助 ---------------- */

static void track_box_of(const dl::detect::result_t *big, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!big || big->box.size() < 4) return;    /* 框不完整 → 视为无效（不参与跟踪） */
    out[0] = (int)big->box[0];
    out[1] = (int)big->box[1];
    out[2] = (int)big->box[2];
    out[3] = (int)big->box[3];
}

static float box_iou(const int a[4], const int b[4])
{
    const int x0 = a[0] > b[0] ? a[0] : b[0];
    const int y0 = a[1] > b[1] ? a[1] : b[1];
    const int x1 = a[2] < b[2] ? a[2] : b[2];
    const int y1 = a[3] < b[3] ? a[3] : b[3];
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    const float inter = (float)(x1 - x0) * (float)(y1 - y0);
    const float aa = (float)(a[2] - a[0]) * (float)(a[3] - a[1]);
    const float ab = (float)(b[2] - b[0]) * (float)(b[3] - b[1]);
    const float uni = aa + ab - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

/* 同一张脸且没怎么动：IoU 达标 + 中心位移 < 脸宽 * MOVE% */
static bool track_same_face(const int prev[4], const int cur[4])
{
    if (box_iou(prev, cur) < FACE_TRACK_IOU) return false;
    const float w = (float)(cur[2] - cur[0]);
    if (w <= 0.0f) return false;
    const float dcx = ((float)(cur[0] + cur[2]) - (float)(prev[0] + prev[2])) * 0.5f;
    const float dcy = ((float)(cur[1] + cur[3]) - (float)(prev[1] + prev[3])) * 0.5f;
    const float lim = w * FACE_TRACK_MOVE_PCT;
    return (dcx * dcx + dcy * dcy) <= lim * lim;
}

static const char *trk_ev_str(int e)
{
    switch (e) {
    case TRK_MATCH:    return "match";
    case TRK_REJECT:   return "reject";
    case TRK_TRACKING: return "tracking";
    default:           return "none";
    }
}

static void track_cache(const dl::detect::result_t *big, int64_t ts_us,
                        const char *event, const char *name, float sim,
                        int stable, float raw)
{
    track_box_of(big, s_trk_box);
    s_trk_us = ts_us;
    s_trk_ev = (strcmp(event, "match") == 0)      ? TRK_MATCH
               : (strcmp(event, "tracking") == 0) ? TRK_TRACKING
                                                  : TRK_REJECT;
    snprintf(s_trk_name, sizeof(s_trk_name), "%s", name ? name : "");
    s_trk_sim = sim;
    s_trk_stable = stable;
    s_trk_raw = raw;
    s_trk_ok = true;
}

/* 本帧能否跳过提特征、直接沿用上一帧结论？命中则回填结论并返回 true */
static bool track_reuse(const dl::detect::result_t *big, int64_t ts_us,
                        const char **event, const char **name, float *sim,
                        int *stable, float *raw)
{
    if (s_trk_hold_ms <= 0 || !s_trk_ok) return false;
    if (ts_us - s_trk_us >= (int64_t)s_trk_hold_ms * 1000) return false;  /* 到点必须重算 */
    int cur[4];
    track_box_of(big, cur);
    if (!track_same_face(s_trk_box, cur)) return false;
    *event = trk_ev_str(s_trk_ev);
    *name = s_trk_name;
    *sim = s_trk_sim;
    *stable = s_trk_stable;
    *raw = s_trk_raw;
    s_dbg_reuse = 1;
    s_trk_reused++;
    return true;
}

/* ---------------- esp-dl 特征提取统一入口（工作区清零实验） ----------------
 * 按 s_feat_ws_mode 在每次推理前处理模型工作区后调 s_feat->run()。
 * ⚠️ arena 里也含图输入张量——清零/毒化必须发生在调用点（即 run 之前），
 * run 内部的预处理随后重写输入，顺序安全。权重不在 arena，不受影响。 */
static dl::TensorBase *feat_run(dl::image::img_t &img, const std::vector<int> &keypoint)
{
    if (!s_feat) {
        return nullptr;
    }
    dl::Model *m = s_feat->get_raw_model();
    if (m && s_feat_ws_mode == 1) {
        m->zero_workspace();
    } else if (m && s_feat_ws_mode == 2) {
        m->poison_workspace();
    }
    return s_feat->run(img, keypoint);
}

/* ---------------- esp-dl 逐层二分（工作区实验续） ----------------
 * trace 每项 = 该算子输出张量（按 m_outputs_index 取、逻辑字节）的 CRC32，
 * 末项 = 整 arena CRC。第 i 项分歧 = 算子 i 在相同输入下产出不同输出
 * （此前各级输出已证一致）→ 算子 i 即元凶；仅末项分歧 = 只有 padding/无效
 * 区字节不同（无害噪声）。两个判据：A/B（同帧双跑）与 A/参照（跨帧）。 */
static std::vector<uint32_t> s_feat_bisect_ref;
static bool s_feat_bisect_ref_valid = false;

static int trace_first_diff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
{
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)i;
        }
    }
    return -1;                                   /* 完全一致 */
}

static void feat_bisect_analyze(const std::vector<uint32_t> &ta, const std::vector<uint32_t> &tb)
{
    dl::Model *m = s_feat ? s_feat->get_raw_model() : nullptr;
    int n_mod = (int)ta.size() - 1;              /* 末项 = 整 arena */
    int d_ab = trace_first_diff(ta, tb);
    int d_ref = -1;
    bool has_ref = s_feat_bisect_ref_valid;
    if (has_ref) {
        d_ref = trace_first_diff(s_feat_bisect_ref, ta);
    } else {
        s_feat_bisect_ref = ta;
        s_feat_bisect_ref_valid = true;
    }
    auto describe = [&](int e) {
        if (e < 0) {
            return std::string("identical");
        }
        if (e == n_mod) {
            return std::string("arena-only(padding, harmless)");
        }
        const char *nm = m ? m->module_name((size_t)e) : nullptr;
        return std::string("module#") + std::to_string(e) + "(" +
               (nm ? nm : "?") + ")";
    };
    app_usb_link_log("feat-bisect: modules=%d A/B diff=%s | vs-ref diff=%s",
                     n_mod, describe(d_ab).c_str(),
                     has_ref ? describe(d_ref).c_str() : "first-ref-captured");
}

/* 检测模型工作区清零（ws=1 时）：关键点漂移与特征同源（检测 arena 同样跨 run
 * 复用残留内存）——只清特征不清检测，关键点仍逐帧变、对齐裁剪跟着变，特征
 * 依旧不确定（2026-10-06 实测：只清特征时 spread 从 0.89 降到 0.37 但不成簇归零）。
 * MSRMNP 是 MSR+MNP 双模型（get_raw_model(0)/(1)）；ESPDet+MNP 同理两块。 */
static void detect_ws_clear(void)
{
    if (!s_detect) {
        return;
    }
    for (int idx = 0; idx < 2; idx++) {
        dl::Model *m = s_detect->get_raw_model(idx);
        if (m) {
            m->zero_workspace();
        }
    }
#if CONFIG_APP_FACEAI_DETECT_IS_ESPDET
    if (s_mnp) {
        dl::Model *m = s_mnp->get_raw_model();
        if (m) {
            m->zero_workspace();
        }
    }
#endif
}

/* ---------------- F6 事件去重（规划 P1-F6） ----------------
 * match / 稳定 reject 的事件边沿：T ms 内同一人（同名）的 match、任意稳定
 * reject 只报一次；窗口期内 event 回落为 "none"。match/sim/stable 字段不受
 * 影响——UI 的名字显示靠 match 字段（不闪断），host 侧开门/日志联动靠边沿。
 * ⚠️ 必须在 track_cache 之后施加：缓存里存的是"原始结论"，复用帧回放后
 * 重新过一遍本门，保证 F4 不会绕过去重刷屏。 */
static void event_dedup_gate(const char **event, const char *match_name)
{
    if (s_dedup_ms <= 0 || !*event) return;
    const int64_t now = esp_timer_get_time();
    if (strcmp(*event, "match") == 0) {
        if (s_evt_match_name[0] && match_name &&
            strcmp(s_evt_match_name, match_name) == 0 &&
            now - s_evt_match_us < (int64_t)s_dedup_ms * 1000) {
            *event = "none";
            return;
        }
        strlcpy(s_evt_match_name, match_name ? match_name : "", sizeof(s_evt_match_name));
        s_evt_match_us = now;
    } else if (strcmp(*event, "reject") == 0 && s_dbg_stable == 1) {
        /* 陌生人没有 id/名字可作去重键：按时间统一去重（T 内换一个陌生人也不
         * 重报，门禁语义可接受；人脸离开只清平滑窗，reject 计时独立） */
        if (now - s_evt_rej_us < (int64_t)s_dedup_ms * 1000) {
            *event = "none";
            return;
        }
        s_evt_rej_us = now;
    }
}

/* ---------------- F7 多人脸（规划 P1-F7） ----------------
 * 按面积取前 N（s_multi_max）张脸分别提特征比对：
 *   主脸（第 1 大）：走完整 F2/F4/F6 路径，顶层 match/sim 语义不变；
 *   副脸（第 2..N）：每帧裸比对（无平滑窗口，esp-dl 野值靠"raw_sim>=thr_lo
 *   才报名字"兜底），每张每帧 +93ms 提特征。
 * 结果帧 boxes[i] 追加第 5/6 个元素 name/sim；旧解析只读前 4 个坐标，兼容。
 * F4 复用帧整帧不提特征（主脸+副脸都省）：副脸结论从快照回放，标签不闪断。 */
static void multi_set_primary(const dl::detect::result_t *big, const char *name, float sim)
{
    s_multi[0].res = big;
    s_multi[0].name = (name && name[0]) ? name : "";
    s_multi[0].sim = sim;
    s_multi_n = 1;
}

/* 副脸：从 det 里挑除已选之外面积最大的前 N-1 张，逐张提特征比对。
 * 关键点不全的候选直接跳过（MFN 对齐要求 landmarks.size()==10，缺了会 assert）。
 * 结束后快照副脸结论供 F4 复用帧回放。 */
static void multi_fill_secondary(const std::list<dl::detect::result_t> &det,
                                 dl::image::img_t &img)
{
    s_multi_cache_n = 0;
    if (s_multi_max <= 1) return;
    while (s_multi_n < s_multi_max && s_multi_n < 3) {
        const dl::detect::result_t *best = nullptr;
        for (const auto &r : det) {
            if (r.keypoint.size() < 10) continue;
            bool taken = false;
            for (int i = 0; i < s_multi_n; i++) {
                if (s_multi[i].res == &r) { taken = true; break; }
            }
            if (!taken && (!best || r.box_area() > best->box_area())) best = &r;
        }
        if (!best) break;
        dl::TensorBase *f = feat_run(img, best->keypoint);
        if (!f) break;                       /* 提特征失败：本帧副脸到此为止 */
        std::vector<dl::recognition::result_t> q = s_db->query_feat(f, 0.0f, 1);
        const float qsim = q.empty() ? 0.0f : q.front().similarity;
        const uint16_t qid = q.empty() ? 0 : q.front().id;
        const char *nm = (qid && qsim >= s_thr_lo) ? name_find_name(qid) : nullptr;
        s_multi[s_multi_n].res = best;
        s_multi[s_multi_n].name = nm ? nm : "";
        s_multi[s_multi_n].sim = qsim;
        s_multi_n++;
    }
    for (int i = 1; i < s_multi_n && i - 1 < 2; i++) {
        s_multi_cache[i - 1] = s_multi[i];
    }
    s_multi_cache_n = (s_multi_n > 1) ? (s_multi_n - 1) : 0;
}

/* F4 复用帧：主脸用回放的顶层结论，副脸用上次快照（都不提特征） */
static void multi_replay_reuse(const dl::detect::result_t *big, const char *name, float sim)
{
    multi_set_primary(big, name, sim);
    for (int i = 0; i < s_multi_cache_n && s_multi_n < 3; i++) {
        s_multi[s_multi_n++] = s_multi_cache[i];
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
    /* 诊断（缓存一致性，HANDOVER 廿一节）：AI 实际喂给模型的像素指纹。
     * 与结果帧的 frm_crc（生产者指纹）对照：两者不一致 = 跨核读到陈旧像素 */
    s_dbg_ai_crc = esp_rom_crc32_le(0, data, (size_t)row_bytes * fr.height);
    s_dbg_ai_core = (int)xPortGetCoreID();

    dl::image::img_t img = {};
    img.data = (void *)data;
    img.width = (uint16_t)fr.width;
    img.height = (uint16_t)fr.height;
    /* 板载摄像头(ISP)出的是 RGB565LE；PC 送来的 JPEG 硬解成 RGB888 */
    img.pix_type = (fr.fmt == IMG_FMT_RGB565) ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                                              : dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    int64_t t0 = esp_timer_get_time();
    if (s_feat_ws_mode == 1) {
        detect_ws_clear();               /* 检测 arena 同样清零：关键点才不漂（见 detect_ws_clear 注释） */
    }
    /* ⚠️ ESPDet 只出人脸框、**不产出 5 个关键点**（其后处理里没有任何 keypoint 代码），
     * 而 MFN 特征提取靠关键点做 112x112 对齐（landmarks.size()==10 断言）——
     * 直接换检测器会 panic（2026-10-05 实测踩过）。所以这里做成两级：
     *   ESPDet 出候选框（召回好）→ MNP 精修并补关键点（和 MSRMNP 的第二级同一个模型）。
     * MNP 模型与 ESPDet 一起打包在 human_face_detect.espdl 里，两者都已 flash。*/
#if CONFIG_APP_FACEAI_DETECT_IS_ESPDET
    std::list<dl::detect::result_t> &cand = s_detect->run(img);
    const std::list<dl::detect::result_t> &det = s_mnp->run(img, cand);
#else
    const std::list<dl::detect::result_t> &det = s_detect->run(img);
#endif
    uint32_t infer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    /* 缓存一致性深探：模型执行后重算输入指纹——若与 ai_crc 不同，说明有代码
     * 在推理期间写入了输入缓冲（别名/踩内存）；相同则非确定性在模型内部 */
#if CONFIG_APP_FACEAI_DIAG_DEEP
    s_dbg_ai_crc2 = esp_rom_crc32_le(0, data, (size_t)row_bytes * fr.height);
#endif

    /* 最大的人脸用于识别 / 注册 */
    const dl::detect::result_t *big = nullptr;
    for (const auto &r : det) {
        if (!big || r.box_area() > big->box_area()) big = &r;
    }

    const char *event = "none";
    const char *match_name = "";
    float sim = 0.0f;
    const char *msg = nullptr;
    char msgbuf[96] = { 0 };
    s_dbg_stable = -1;                  /* 每帧刷新：只有本帧设置了才随结果帧带出 */
    s_dbg_sim_raw = 0.0f;
    s_dbg_reuse = 0;                    /* F4：本帧是否跳过了提特征 */
    s_multi_n = 0;                      /* F7：本帧的 boxes 标签（有脸的分支里填） */
    s_dbg_frm_crc = fr.crc32;           /* 缓存一致性诊断：生产者侧指纹/核 */
    s_dbg_dec_core = fr.dec_core;

    if (!big) {
        smooth_reset();                 /* 人脸离开：清空平滑窗，下次出现重新确认 */
        s_trk_ok = false;               /* F4：跟踪状态同样失效（下次出现必须重提特征） */
    }

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
            s_reg_shots = 0;
        } else if (s_reg_got > 0 &&
                   fr.ts_us - s_reg_last_us < FACE_ENROLL_INTERVAL_US) {
            /* F1 多帧采集：距上一帧没拉开 400ms，跳过本帧继续等——
               间隔采集带自然姿态变化，比连续同帧质量高 */
        } else {
            /* F1 质量门控：检测分 / 脸宽 / 关键点，低质量帧坚决不入库 */
            const float det_score = big->score;
            const int face_w = big->box.size() >= 4 ? (big->box[2] - big->box[0]) : 0;
            const bool kp_ok = big->keypoint.size() >= 10;
            if (det_score < FACE_ENROLL_MIN_SCORE || face_w < FACE_ENROLL_MIN_PX || !kp_ok) {
                snprintf(msgbuf, sizeof(msgbuf),
                         "quality low (score %.2f, face %dpx) - get closer / more light",
                         det_score, face_w);
                msg = msgbuf;                    /* 保持 pending，等质量达标的一帧 */
            } else {
                dl::TensorBase *f = feat_run(img, big->keypoint);
                if (f && s_db->enroll_feat(f) == ESP_OK) {
                    /* 刚入库的特征与自身相似度=1.0，用它取回新分配的 id */
                    std::vector<dl::recognition::result_t> q = s_db->query_feat(f, 0.999f, 1);
                    uint16_t id = q.empty() ? 0 : q.front().id;
                    if (id && name_add(id, s_reg_name)) {
                        s_reg_got++;
                        s_reg_last_us = fr.ts_us;
                        if (s_reg_got >= s_reg_shots) {
                            names_save();
                            match_name = s_reg_name;    /* 结果里带上刚注册的名字，host 可核对 */
                            event = "registered";
                            s_lib_dirty = true;
                            s_reg_pending = false;
                            snprintf(msgbuf, sizeof(msgbuf), "enrolled %d/%d (feats %d)",
                                     s_reg_got, s_reg_shots, s_name_cnt);
                            msg = msgbuf;
                            ESP_LOGI(TAG, "registered '%s' x%d as id %u (feats %d, persons %d)",
                                     s_reg_name, s_reg_got, (unsigned)id, s_name_cnt,
                                     name_count_persons());
                        } else {
                            event = "enrolling";
                            snprintf(msgbuf, sizeof(msgbuf),
                                     "collected %d/%d - stay in view, vary angle slightly",
                                     s_reg_got, s_reg_shots);
                            msg = msgbuf;
                        }
                    } else {
                        event = "error";
                        msg = "lib full";
                        s_reg_pending = false;
                        s_reg_shots = 0;
                    }
                } else {
                    event = "error";
                    msg = "enroll failed";
                    s_reg_pending = false;
                    s_reg_shots = 0;
                }
            }
        }
    } else if (big && s_mode_match && s_db_ok &&
               track_reuse(big, fr.ts_us, &event, &match_name, &sim,
                           &s_dbg_stable, &s_dbg_sim_raw)) {
        /* F4 轻跟踪：同一张脸、位移很小、距上次提特征 < HOLD → 跳过 93ms 的
         * s_feat->run()，沿用上一帧结论（规划 P0-F4）。
         * ⚠️ 这里**不往平滑窗推记录**：跳过帧不是一次新的测量，推进去会让 K 帧
         * 票数被同一个值灌满 → F2 的"多帧确认"就失去意义了。 */
        multi_replay_reuse(big, match_name, sim);   /* F7：副脸标签从快照回放 */
    } else if (big && s_mode_match && s_db_ok) {
        const bool bisect_round = (s_feat_bisect_left > 0);
        std::vector<uint32_t> trace_a;
        if (bisect_round) {
            s_feat->set_feat_trace(true);    /* run A：traced，本帧正常结论用它的输出 */
        }
        dl::TensorBase *f = feat_run(img, big->keypoint);
        s_feat_cnt++;                   /* F4 验收指标：提特征频率统计 */
        if (bisect_round) {
            trace_a = s_feat->get_feat_trace();
            s_feat->set_feat_trace(false);   /* multi 副脸的 feat_run 不进 trace */
        }
        if (!f) {
            event = "error";
            msg = "feat failed";
        } else {
            /* 无门槛取 top-1：灰区判定与阈值标定（sim_raw）都需要真实最优相似度 */
            std::vector<dl::recognition::result_t> q = s_db->query_feat(f, 0.0f, 1);
            uint16_t raw_id = q.empty() ? 0 : q.front().id;
            float raw_sim = q.empty() ? 0.0f : q.front().similarity;
            s_dbg_sim_raw = raw_sim;
#if CONFIG_APP_FACEAI_DIAG_KP
            if (big) s_dbg_kp = big->keypoint;   /* 诊断：landmark 是否抖（无副作用） */
#endif
#if CONFIG_APP_FACEAI_DIAG_DEEP
            /* ⚠️ 深探（默认关，仅查非确定性时开）：同帧重复查询 + 重复提特征，
             * 把非确定性归因到查询侧/特征侧/检测侧。注意重复 run 本身会让模型
             * 状态更脏，所以开着测出来的 sim 分布不能用于标定。 */
            std::vector<dl::recognition::result_t> q2 = s_db->query_feat(f, 0.0f, 1);
            s_dbg_sim_q2 = q2.empty() ? 0.0f : q2.front().similarity;
            dl::TensorBase *f2 = feat_run(img, big->keypoint);
            if (f2) {
                std::vector<dl::recognition::result_t> q3 = s_db->query_feat(f2, 0.0f, 1);
                s_dbg_sim_f2 = q3.empty() ? 0.0f : q3.front().similarity;
            } else {
                s_dbg_sim_f2 = -2.0f;
            }
#endif

            if (s_name_cnt == 0) {
                smooth_reset();                 /* 空库：无需平滑，直接稳定 reject */
                s_dbg_stable = 1;
                event = "reject";
            } else {
                smooth_push(raw_id, raw_sim);
                uint16_t best_id;
                int best_cnt;
                float best_sim, win_max;
                smooth_tally(&best_id, &best_cnt, &best_sim, &win_max);
                /* F3 确认帧数：>=HI 需 M 帧；LO..HI 灰区需 M+1 帧（更严） */
                const int need = (best_sim >= s_thr_hi) ? FACE_SMOOTH_M
                                                        : FACE_SMOOTH_M + 1;
                /* F2 快速通道：本帧裸相似度足够高就不等票数（检出间歇时窗口
                 * 凑不满票是命中率真瓶颈，第廿二节第 8 节）。只信"本帧自己的
                 * 强测量"——多人库时名字取本帧 top-1 的 id，不是窗口多数派。 */
                const bool normal_hit = (best_cnt >= need && best_sim >= s_thr_lo);
                const bool fast_hit = (s_fast_thr > 0.0f && raw_sim >= s_fast_thr);
                if (normal_hit || fast_hit) {
                    /* F2 稳定命中：整窗统计通过才报名字，防单帧误报闪名 */
                    s_dbg_stable = 1;
                    if (fast_hit && !normal_hit) {
                        sim = raw_sim;
                        const char *nm = name_find_name(raw_id);
                        match_name = nm ? nm : "";
                        event = nm ? "match" : "reject";
                    } else {
                        sim = best_sim;
                        const char *nm = name_find_name(best_id);
                        if (nm) {
                            match_name = nm;
                            event = "match";
                        } else {
                            event = "reject";
                        }
                    }
                } else if (s_smooth_cnt >= FACE_SMOOTH_K && win_max < s_thr_lo) {
                    s_dbg_stable = 1;           /* 整窗都没碰到灰区下限：稳定陌生人 */
                    event = "reject";
                } else {
                    s_dbg_stable = 0;           /* 确认中：不报名字 */
                    event = "tracking";
                    sim = win_max;
                }
            }
            /* F4：缓存本帧结论，供后续"同脸小位移"帧复用（error 不缓存 → 下帧重试）*/
            if (strcmp(event, "error") != 0) {
                track_cache(big, fr.ts_us, event, match_name, sim,
                            s_dbg_stable, s_dbg_sim_raw);
                /* F7：主脸条目 + 副脸提特征/快照（error 帧不填，下帧重试） */
                multi_set_primary(big, match_name, sim);
                multi_fill_secondary(det, img);
            }
            /* 逐层二分（feat_bisect 武装时）：run B = 同帧第二次 traced run，
             * 与 run A 同栈同状态；分析 A/B 与 A/参照（第一轮 A）的逐级 CRC。
             * ⚠️ run B 会覆写 arena 里的输出张量——本帧结论已从 f 提取完毕，
             * 此处是 f 最后一次可用之后。 */
            if (bisect_round && f) {
                s_feat->set_feat_trace(true);
                feat_run(img, big->keypoint);            /* run B */
                std::vector<uint32_t> trace_b = s_feat->get_feat_trace();
                s_feat->set_feat_trace(false);
                feat_bisect_analyze(trace_a, trace_b);
                if (s_feat_bisect_left > 0) {
                    s_feat_bisect_left--;
                }
            }
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
    /* F4 验收：提特征频率（1s 窗口）——目标是降到 5~10 次/秒（静止时 ≈
     * 1000/HOLD_MS，动时更频繁）。结果帧字段 feat_hz。 */
    if (now - s_feat_win_us >= 1000000) {
        s_feat_hz = (float)s_feat_cnt * 1000000.0f / (float)(now - s_feat_win_us);
        s_feat_win_us = now;
        s_feat_cnt = 0;
    }

    if (now - s_status_ms > 500000) {
        s_status_ms = now;
        app_status_set(APP_MOD_FACE, true, "%d face %s", (int)det.size(),
                       match_name[0] ? match_name : event);
    }

    /* F7：副脸提特征也算进本帧 AI 耗时（重算，含检测+主/副脸特征） */
    infer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    /* F6 事件去重：match/稳定 reject 在 T ms 内只报一次事件（在 track_cache
     * 之后施加——F4 复用帧回放的结论也会重新过门，不会绕过去重刷屏） */
    event_dedup_gate(&event, match_name);
    send_result(event, &det, sim, match_name, infer_ms, msg);
}

/* ---------------- 命令处理（AI 任务上下文） ---------------- */

/* ---------------- 相机/降噪/运动统计的每帧缓存 ----------------
 * ⚠️ 这些值必须在每一帧刷新：曾只在 cam 命令时读一次，结果 UI 看到的
 * "运动占比 mv"永远是点击瞬间的冻结快照（用户实测恒为 0% 的根因）。
 * 一次 get_stats 只是结构体拷贝，每帧调用成本可忽略。 */
static void cam_state_tick(void)
{
    s_cam_fps = (int)app_camera_get_fps();
    s_cam_dn = app_imgpipe_get_dn();
    app_imgpipe_stats_t pst = {};
    app_imgpipe_get_stats(&pst);
    s_cam_dn_ms = (int)pst.dn_ms;
    s_cam_dn_skip = (int)pst.dn_skip;
    s_cam_dn_blk = (int)pst.dn_blocks;
    s_cam_dn_sad = (int)pst.dn_sad;
    s_cam_dn_mmax = (int)pst.dn_mmax;
    s_cam_dn_mv = (int)pst.dn_motion;
    s_cam_mv_peak = (int)pst.mv_peak;
    s_cam_mv_ratio = (int)pst.mv_ratio;
    s_cam_mv_ms = (int)pst.mv_ms;
    s_cam_mv_us = pst.mv_stats_us;
    s_cam_mv_ready = (int)pst.mv_ready;
    s_cam_luma = (int)pst.ae_luma;
    app_camauto_wb_get(&s_cam_wb_kr, &s_cam_wb_kb);       /* AWB 自动调增益时每帧同步 */
    /* AE 开启时引擎视角是 exp/gain 的权威（它通过信箱改参数，寄存器读回会滞后一帧）；
       关闭时以手动路径刷新的缓存为准 */
    int ae_exp = 0, ae_gain = 0;
    if (app_camauto_get_ae() && app_camauto_get_exp_gain(&ae_exp, &ae_gain)) {
        s_cam_exp = ae_exp;
        s_cam_gain = ae_gain;
    }
}

/* ---------------- 自动帧率状态机 ----------------
 * 静止 → 15fps（降噪可用，画面干净）；画面持续"在动" → 50fps（无拖影）。
 * 运动信号来自独立运动估计（mv_peak/mv_ratio），与降噪是否开启无关。
 * 按时间做迟滞（不是按帧数）：运动持续 ~100ms 才升档，静止持续 ~1.5s 才回落，
 * 避免阈值边缘来回抖动。 */
#define AUTOFPS_ACTIVE_FPS   CONFIG_APP_CAM_AUTO_ACTIVE_FPS   /* 运动/白天的目标帧率 */
#define AUTOFPS_MOTION_MS    100                              /* 在动持续多久判定"在动" */
#define AUTOFPS_IDLE_MS      1500                             /* 静止持续多久回落 */
#define AUTOFPS_COOL_MS      300                              /* 切换后冷却，期间不判定 */
#define AUTOFPS_PROBE_PERIOD_MS 2000                          /* 50fps 时运动估计探测周期 */
#define AUTOFPS_PROBE_MS     300                              /* 每次探测窗口时长 */
#define AUTOFPS_MIN_ACTIVE_MS 1500                            /* 升档后最短驻留，防抖动 */

/* ---------------- 夜视阶梯（暗度驱动的降帧率 + 自动时域降噪） ----------------
 * 与上方的运动自适应正交、方向相反：运动只负责升到 50fps，暗度只负责降到
 * 5/2fps。两者共用 auto_fps_apply() 的切换编排（暂停运动估计 / 复位时域历史 /
 * 拉满新曝光上限 / 通知 AE），所以不要各写一套。
 * 输入是 AE 采样出的帧内平均亮度（cam_state_tick 每帧刷新的 s_cam_luma），
 * 先做 1/8 EMA 去抖，再要求新档位"持续"若干秒才真的切（变暗/变亮各 3s）。 */
#define NIGHT_LUMA_HYST   10      /* 迟滞带：luma 要高于"暗阈值 + 本值"才判为回正常 */
#define NIGHT_DARK_MS     3000    /* 变暗持续多久才降档 */
#define NIGHT_BRIGHT_MS   3000    /* 变亮持续多久才回升 */

/* 白天目标帧率：曝光余量阶梯档（50 -> 20 -> 10，见 day_expo_tick）。 */
static int day_fps(void)
{
    if (s_day_level >= 2) {
        return CONFIG_APP_CAM_DAY_FPS_LOW;
    }
    if (s_day_level >= 1) {
        return CONFIG_APP_CAM_DAY_FPS_MID;
    }
    return AUTOFPS_ACTIVE_FPS;
}

/* 静止场景的目标帧率：由当前夜视档决定。
 * ⚠️ 白天（tier0）恒定帧率——降帧率/运动自适应是低光照的逻辑（静态可见度、运动
 * 流畅），白天不需要也不应该被接管（2026-10-02 用户定）。但曝光余量阶梯例外：
 * 纯粹因为"50fps 曝光不够用"而降到 20/10fps（常规相机的自动快门，见第十七节）。
 * 夜视档 tier1/2 → 5fps。
 * AUTO 运动状态机回落、以及夜视阶梯自己切档，都必须走这里取目标。 */
static void night_apply(void);   /* 定义在夜间阶梯一节（auto_fps_set 会回调它） */

static int night_idle_fps(void)
{
    if (s_night_tier >= 2) {
        return CONFIG_APP_CAM_NIGHT_FPS_MIN;
    }
    if (s_night_tier >= 1) {
        return CONFIG_APP_CAM_NIGHT_FPS_LOW;
    }
    return day_fps();                    /* 白天：曝光余量阶梯（默认 50） */
}

/* 光照双模式判定：夜视阶梯在档（tier>=1）+ 摄像头源 + 功能开着 = 夜视模式。
 * 依赖的前提：luma/AE 采样、运动估计、时域降噪都在 camera 任务的
 * publish_raw() 里做，与 AI 取不取帧无关 —— 所以门控后 AI 停止 get_frame
 * 也不会让夜视阶梯/AUTO/AE 失去输入（cam_state_tick 照常轮询统计即可）。 */
static bool night_face_gated(void)
{
    return s_face_night_off && s_night_fps &&
           app_imgpipe_get_src() == APP_IMG_SRC_CAM && s_night_tier >= 1;
}

static bool    s_auto_active = false;   /* 当前是否处于高帧率（运动）状态 */
static bool    s_auto_last_moving = false;
static bool    s_auto_mv_on = false;    /* 运动估计当前是否在跑（切帧率时暂停）*/
static int64_t s_auto_state_us = 0;     /* moving 标志上次变化 / 上次切换的时刻 */
static int64_t s_auto_resume_us = 0;    /* 切换后恢复运动估计的时刻 */
static int     s_auto_ratio = CONFIG_APP_CAM_MOTION_RATIO;  /* 触发百分比（可标定）*/
static int64_t s_auto_last_probe_us = 0;   /* 上次探测窗口结束/开窗的时刻（控制 2s 节奏）*/
static int64_t s_auto_probe_start_us = 0;  /* 当前探测窗口开始时刻 */
static int64_t s_auto_probe_mv_us = 0;     /* 开窗时的运动统计时戳（判"有没有新样本"）*/
static int64_t s_auto_active_us = 0;       /* 进入高帧率的时刻（最短驻留保护）*/
static bool    s_auto_boost_pending = false;  /* 降档后待执行的 exp/gain 拉满（延迟 600ms）*/
static int     s_auto_boost_exp = 0;
static int     s_auto_boost_gain = 0;
static int64_t s_auto_boost_at_us = 0;        /* 延迟拉满的到期时刻 */

/* 切到目标帧率，并做一套联动：exp 拉到新上限（否则 50fps 会很暗）、
 * 复位降噪与运动参考帧（否则亮度跳变会被误判成运动导致来回切换）*/
/* want_boost=夜视语义：降档后 600ms 把 exp/gain 拉满（暗处进光优先）。
 * 白天曝光阶梯降档时传 false——曝光交给 AE 连续调节，强制拉满增益只会过曝。 */
static void auto_fps_apply_ex(int fps, bool want_boost)
{
    int64_t t0 = esp_timer_get_time();

    /* 目标帧率与当前一致：什么都不做。白天 tier0 的 apply(50) 多是这种空操作——
       若放行，upscale 判定会把等值当"降档"挂 boost（exp/gain 拉满），白天直接
       过曝闪一下。tier1->2（同为 5fps）的冗余切换也在此跳过（自动 DN 由
       night_apply 继续处理，不受影响）。 */
    if (fps == s_cam_fps) {
        s_auto_af_ms = 0;
        return;
    }

    /* ⚠️ 写操作已改走 app_camera 的写操作信箱（camera 任务在 QBUF 后串行执行），
       与本任务不再有跨任务 ioctl 并发——之前"faceai ioctl + camera 任务运动估计
       并发触发整板重启 / 争设备锁卡死 UI"的两类风险都绕开了。运动估计暂停几百 ms
       的旧逻辑保留（切换瞬间亮度/时序都在变，暂停判定本来就更稳）。 */
    app_imgpipe_set_motion_detect(false);
    s_auto_mv_on = false;
    s_auto_resume_us = esp_timer_get_time();
    /* 切换后重新计探测节奏与驻留：低帧率下降档后第一帧要 ~1s 才来，
       探测窗口必须等它有新样本，不能按时间硬判（见 auto_fps_tick 里的注释） */
    s_auto_last_probe_us = s_auto_resume_us;
    s_auto_active_us = s_auto_resume_us;

    bool upscale = (fps > s_cam_fps);        /* s_cam_fps 是切换前的当前帧率 */

    /* 唯一的一次写：改 VTS（信箱异步执行，≤1 帧后落地）。sc2336_set_fps 内置曝光
     * clamp——升档（VTS 变小）自动把 exp 压到新上限（=天然拉满，994@50fps）；
     * 降档时 exp 保持合法（画面暂时偏暗，600ms 后由 tick 补拉满）。 */
    app_camera_post_fps((int32_t)fps);
    app_imgpipe_reset_prev();

    if (!upscale && want_boost) {
        /* 降档（夜视）：把 exp/gain 拉到新上限（994 → 3327@15fps）。
         * 不在切换瞬间做（加重锁竞争），延迟 600ms 由 auto_boost_maybe() 执行；
         * 画面表现为"半秒内渐亮"而不是"冻结数秒"。
         * ⚠️ 不能挂给运动估计的恢复时机：夜视阶梯降档时 AUTO 可能关着，
         * 那条路径根本不会跑到，exp 就会停在旧上限（画面偏暗）。 */
        s_auto_boost_pending = true;
        s_auto_boost_exp = 1000 * 50 / fps - 6;   /* 与实测 exp_max 完全一致（VTS-6）*/
        s_auto_boost_gain = s_cam_gain_max > 0 ? s_cam_gain_max : 192;
        s_auto_boost_at_us = esp_timer_get_time() + 600 * 1000;
    } else {
        /* 升档（如夜->日），或白天阶梯降档（无 boost）：清掉遗留的待执行 boost，
           曝光由 AE 接管 */
        s_auto_boost_pending = false;
    }

    s_auto_af_ms = (int)((esp_timer_get_time() - t0) / 1000);
    /* 同步 exp 缓存（结果帧显示用）：升档时 exp 被驱动 clamp 到新上限；
       降档时 exp 暂保持旧值，600ms 后 boost 才拉满（tick 里再更新缓存）*/
    s_cam_exp_max = 1000 * 50 / fps - 6;
    if (upscale) {
        s_cam_exp = s_cam_exp_max;
    }
    ESP_LOGW(TAG, "auto-fps -> %d fps (%s, %d ms)", fps, upscale ? "up" : "down", s_auto_af_ms);
    app_usb_link_log("cam: auto-fps -> %d fps (%s, %d ms)", fps, upscale ? "up" : "down", s_auto_af_ms);
    /* 通知 AE 引擎：切换联动优先，冷却 500ms 后 AE 再接管 */
    app_camauto_note_fps_switch(fps, s_cam_exp, s_cam_gain);
}

static void auto_fps_apply(int fps)
{
    auto_fps_apply_ex(fps, true);        /* 默认夜视语义（降档补 boost） */
}

static void auto_fps_set(bool on)
{
    if (s_auto_fps == on) {
        return;
    }
    s_auto_fps = on;
    app_imgpipe_set_motion_detect(on);
    if (on) {
        s_auto_active = false;
        s_auto_last_moving = false;
        s_auto_state_us = esp_timer_get_time();
        auto_fps_apply(night_idle_fps());    /* 进入自动模式：先到静止帧率（白天=50，夜视档=5） */
        app_usb_link_log("cam: auto-fps ON (day fixed %d fps; night: ladder idle <-> motion %d fps)",
                         AUTOFPS_ACTIVE_FPS, AUTOFPS_ACTIVE_FPS);
    } else {
        /* 运动估计停掉（省 ~2ms/帧 与 600KB 缓冲）。
           注意也要清 s_auto_active：否则它会一直是上次的 true，
           而且夜视阶梯"运动期间不动帧率"的互斥判断会读到脏状态。 */
        app_imgpipe_set_motion_detect(false);
        s_auto_mv_on = false;
        s_auto_active = false;
        /* 帧率交还给夜视阶梯：AUTO 只管"运动升档"，静止帧率一向是阶梯的职责，
           否则关掉 AUTO 会把人留在 50fps（暗场下既费电又噪）。
           手动 fps 命令会把 s_night_fps 先置 false，所以不会误覆盖手动值。 */
        if (s_night_fps) {
            night_apply();
        } else {
            app_usb_link_log("cam: auto-fps OFF (keep %d fps)", s_cam_fps);
        }
    }
}

/* 降档后把 exp/gain 拉到新上限（延迟 600ms，避开切换瞬间的设备锁竞争）。
 * ⚠️ 与 AUTO 开关解耦：夜视阶梯降档时 AUTO 可能是关的，所以由主循环每帧调用，
 * 不能只挂在 auto_fps_tick 里（那条路径 !s_auto_fps 时直接早退）。 */
static void auto_boost_maybe(void)
{
    if (!s_auto_boost_pending || esp_timer_get_time() < s_auto_boost_at_us) {
        return;
    }
    s_auto_boost_pending = false;
    app_camera_post_exp_gain(s_auto_boost_exp, s_auto_boost_gain);
    app_imgpipe_reset_prev();                    /* 亮度跳变后重建参考帧 */
    s_cam_exp = s_auto_boost_exp;                /* 同步显示缓存 */
    s_cam_exp_max = s_auto_boost_exp;
    app_camauto_adopt(s_auto_boost_exp, s_auto_boost_gain);   /* 引擎同步视角 */
    ESP_LOGW(TAG, "boost: fps change -> exp=%d gain=%d", s_auto_boost_exp, s_auto_boost_gain);
    app_usb_link_log("cam: boost exp=%d gain=%d", s_auto_boost_exp, s_auto_boost_gain);
}

/* ---------------- 白天曝光余量阶梯（50 -> 20 -> 10fps，第十七节） ----------------
 * 常规相机的自动快门，与夜视阶梯（暗度->5fps）、运动自适应（仅夜视档）正交：
 * 50fps 曝光上限只有 20ms，房间偏暗时 exp 顶满、亮度靠增益硬提（噪点大；
 * 50Hz 抗频闪下 50fps 的 exp 实际只有 10ms/20ms 两档可选）。降一档帧率换
 * 2.5 倍进光（20fps 帧周期 50ms = 5x10ms，与抗频闪对齐，曝光上限 50ms）。
 *   降档：exp 顶满上限 且 增益 >= DAY_GAIN_GATE，持续 DAY_DARK_MS；
 *   升档：exp 余量恢复（<= 800 行 = 16ms，50fps 下占 80%）且增益减半，持续 DAY_BRIGHT_MS。
 * 只在 tier0 + AE 开启时工作；夜视档 / 手动模式（阶梯关）/ AE 关 一概不碰。
 * 切换走 auto_fps_apply_ex(…, false)：无 boost，曝光由 AE 连续调节。 */
#define DAY_DARK_MS     5000    /* exp 顶满+增益超标持续多久降档 */
#define DAY_BRIGHT_MS   8000    /* 余量恢复持续多久升档（更保守，防振荡） */
#define DAY_COOL_MS     10000   /* 切换后冷却（等 AE 收敛再判定） */

static void day_expo_tick(void)
{
    if (!CONFIG_APP_CAM_DAY_STEP) {
        return;
    }
    /* 只管阶梯的白天侧；夜视档（tier>=1）是 5fps 那套，手动模式（阶梯关）不碰 */
    if (!s_night_fps || s_night_tier != 0 ||
        app_imgpipe_get_src() != APP_IMG_SRC_CAM) {
        return;
    }
    if (!app_camauto_get_ae() || s_cam_exp <= 0 || s_cam_exp_max <= 0) {
        return;                      /* 曝光由 AE 管才有"exp 顶满"的语义 */
    }

    int64_t now = esp_timer_get_time();
    /* 增益用 EMA 平滑（1/8）：AE 在量化边界附近瞬时增益来回打摆（0<->70），
       直接拿瞬时值做条件会让 5s 保持时间永远凑不齐（实测踩到）；
       EMA 稳住"这个房间整体需要多少增益"的口径。 */
    if (s_day_gain_ema < 0) {
        s_day_gain_ema = s_cam_gain;
    } else {
        s_day_gain_ema += (s_cam_gain - s_day_gain_ema) / 8;
    }
    /* 降档 = exp 持续顶满 且 平均增益过半门限（画面确实在靠增益硬提）；
       只看 exp 会在"刚好 20ms、增益 0、画面干净"的边界房间白降档。 */
    bool starved = (s_cam_exp >= s_cam_exp_max - 2) &&
                   (s_day_gain_ema >= CONFIG_APP_CAM_DAY_GAIN_GATE / 2);
    /* 升档判据按"要回到的那一档"分级：场景曝光需求必须 ≤ 该档上限的 80%。
       10->20 用 20fps 上限（2494）的 80%（1995 行 = 40ms）——按固定 800 行判会把
       "需要 30ms"的场景永久卡在 10fps（它本可被 20fps 的 50ms 服务）；
       20->50 用 50fps 上限（994）的 80%（795 行 = 16ms）。 */
    int up_exp = (s_day_level >= 2)
                     ? (1000 * 50 / CONFIG_APP_CAM_DAY_FPS_MID - 6) * 8 / 10
                     : (1000 * 50 / AUTOFPS_ACTIVE_FPS - 6) * 8 / 10;
    bool recovery = (s_cam_exp <= up_exp) &&
                    (s_cam_gain < CONFIG_APP_CAM_DAY_GAIN_GATE / 2);

    int want = s_day_level;
    if ((now - s_day_switch_us) / 1000 >= DAY_COOL_MS) {
        if (starved && s_day_level < 2) {
            want = s_day_level + 1;
        } else if (recovery && s_day_level > 0) {
            want = s_day_level - 1;
        }
    }

    /* 保持时间 + 迟滞（同夜视阶梯的模式）：条件一变就重新计时 */
    if (want != s_day_pend) {
        s_day_pend = want;
        s_day_state_us = now;
    }
    if (s_day_pend == s_day_level) {
        return;
    }
    int hold = (s_day_pend > s_day_level) ? DAY_DARK_MS : DAY_BRIGHT_MS;
    if ((now - s_day_state_us) / 1000 < hold) {
        return;
    }

    s_day_level = s_day_pend;
    s_day_switch_us = now;
    ESP_LOGW(TAG, "day: exposure step -> level %d (%d fps), exp %d/%d gain %d",
             s_day_level, day_fps(), s_cam_exp, s_cam_exp_max, s_cam_gain);
    app_usb_link_log("cam: day fps -> %d (exposure headroom)", day_fps());
    auto_fps_apply_ex(day_fps(), false);     /* 无 boost：曝光交给 AE 连续调节 */
}

static void auto_fps_tick(void)
{
    if (!s_auto_fps) {
        return;
    }

    /* 白天（阶梯开着且 tier0）不跑运动自适应：帧率恒定 50fps——"降帧率换可见度 /
       运动升档保流畅"是低光照的逻辑，白天没有需求（2026-10-02 用户定）。
       运动估计也一并关掉：50fps 下持续估计会把 camera 任务拖到 ~26fps（实测）。
       切进夜视档时 night_apply -> auto_fps_apply 会重置恢复计时，估计自动回来。
       阶梯关着（用户手动接管）时不门控，保持原有行为。 */
    if (s_night_fps && s_night_tier == 0) {
        if (s_auto_active) {
            s_auto_active = false;          /* 防御：夜视运动态落回白天时清状态 */
            auto_fps_apply(AUTOFPS_ACTIVE_FPS);
        }
        if (s_auto_mv_on) {
            s_auto_mv_on = false;
            app_imgpipe_set_motion_detect(false);
        }
        return;
    }

    int64_t now = esp_timer_get_time();

    if (s_auto_active) {
        /* 50fps 高负载：运动估计改为周期性探测窗口（默认每 2s 开一个 ≤300ms 的窗），
           把帧预算还给取流/后处理（实测持续跑会把 camera 任务拖到 ~26fps 并最终失稳）。

           ⚠️ 判定必须基于"窗口内新采到的样本"（s_cam_mv_us 变了），不能只看时间到没到：
           夜视阶梯把静止档压到 2fps 后帧周期 ≈ 950ms，而窗口只有 300ms —— 按时间判定时
           窗口里根本没有新帧，只能读切换瞬间被复位成 0 的陈旧运动值，于是立刻误判"不动了"
           掉回 2fps，下一拍又检测到运动升回 50fps，来回振荡（这就是"切换卡顿"的真身，
           2026-10-02 板端日志实测：up/down 每 ~1s 反复一次）。 */
        if (!s_auto_mv_on) {
            /* ⚠️ esp_timer_get_time() 是微秒：常量名带 _MS，比较时必须 ×1000。
               原来漏了换算，窗口实际每 2ms 就重开一次，运动估计被反复复位。 */
            if (now - s_auto_last_probe_us >= (int64_t)AUTOFPS_PROBE_PERIOD_MS * 1000) {
                s_auto_mv_on = true;
                s_auto_probe_start_us = now;
                s_auto_probe_mv_us = s_cam_mv_us;   /* 记下开窗时的样本时戳 */
                app_imgpipe_set_motion_detect(true);
            }
            return;
        }
        bool fresh = (s_cam_mv_us != s_auto_probe_mv_us);   /* 窗口内拿到了新样本 */
        /* 可用样本 = 新样本 + 来自真实比较。重开运动估计后的第一帧只是基准帧
           （mv_has_prev=false），mv 恒为 0，拿它判定会把"在动"读成"静止"。
           ⚠️ 必须用它当"关窗条件"：否则每个窗口都在第一个新样本（恰好是基准帧）
           上就关掉，永远等不到可用样本、也就永远不下结论（实测 AUTO 再也回落不下来）。*/
        bool usable = fresh && s_cam_mv_ready;
        bool window_done = (now - s_auto_probe_start_us) / 1000 >= AUTOFPS_PROBE_MS;
        if (!usable && !window_done) {
            return;                                             /* 等一个可用样本 */
        }
        s_auto_mv_on = false;
        s_auto_last_probe_us = now;
        app_imgpipe_set_motion_detect(false);
        if (!usable) {
            return;      /* 窗口内没拿到可用样本（低帧率下 300ms 太短）：保持 50fps */
        }
        /* 最短驻留：刚升上来的一两个帧里亮度/时序都在变，别立刻又掉回去 */
        if ((now - s_auto_active_us) / 1000 < AUTOFPS_MIN_ACTIVE_MS) {
            return;
        }
        if (s_cam_mv_ratio < s_auto_ratio &&
            s_cam_mv_peak < CONFIG_APP_CAM_MOTION_THR) {
            s_auto_active = false;             /* 静止确认，回落到"夜视档的静止帧率" */
            s_auto_state_us = now;
            auto_fps_apply(night_idle_fps());
        }
        return;                                /* active 期间不做常规判定 */
    }

    /* 切帧率时暂停了运动估计，等 500ms（VTS/曝光切换完成）后再恢复；
     * 降档延迟的 exp/gain 拉满也在这里补做（先做 ioctl——此时估计未恢复、无并发）*/
    if (!s_auto_mv_on) {
        if ((now - s_auto_resume_us) / 1000 >= 500) {
            /* 降档延迟的 exp/gain 拉满已移到 auto_boost_maybe()（主循环每帧调用），
               这里只负责恢复运动估计 */
            s_auto_mv_on = true;
            app_imgpipe_set_motion_detect(true);
        } else {
            return;
        }
    }

    if ((now - s_auto_state_us) / 1000 < AUTOFPS_COOL_MS) {
        return;                              /* 切换后冷却，不判定 */
    }

    /* 必须是有真实比较结果的样本：基准帧 mv 恒 0，用它判定等于"没动"，
       会白白拖慢升档（也不算错，只是保守） */
    bool moving = s_cam_mv_ready &&
                  ((s_cam_mv_ratio >= s_auto_ratio) ||
                   (s_cam_mv_peak >= CONFIG_APP_CAM_MOTION_THR));

    if (moving != s_auto_last_moving) {
        s_auto_last_moving = moving;
        s_auto_state_us = now;               /* 状态标志变化，重新计时 */
    }
    int held_ms = (int)((now - s_auto_state_us) / 1000);
    /* 保持时间的意义是"滤掉瞬时噪声"。低帧率下一帧本身就比保持时间长
       （夜视档 2fps → 帧周期 ~1s >> 100ms），再等一个保持时间等于白丢一整帧，
       所以帧周期已经超过保持时间时直接判定。 */
    int hold_needed = AUTOFPS_MOTION_MS;
    if (s_cam_fps > 0 && 1000 / s_cam_fps > hold_needed) {
        hold_needed = 0;
    }
    if (moving && held_ms >= hold_needed) {
        s_auto_active = true;                  /* 升档：apply 内部会暂停运动估计 */
        s_auto_state_us = now;
        s_auto_active_us = now;
        auto_fps_apply(AUTOFPS_ACTIVE_FPS);
    }
}

/* 把当前夜视档真正下发：帧率（复用 auto_fps_apply 的切换编排）+ 自动降噪。
 * 切档后、以及阶梯被重新打开时都要调——只判档不下发的话，用户在夜视档手动改过
 * 帧率后会一直停在手动值（实测 T4 踩到）。 */
static void night_apply(void)
{
    int fps = night_idle_fps();
    ESP_LOGW(TAG, "night: tier %d -> %d fps", s_night_tier, fps);
    app_usb_link_log("cam: night tier %d -> fps %d", s_night_tier, fps);
    auto_fps_apply(fps);

    /* #2 时域降噪：夜视档自动抬档（长曝光下噪声主导画面），手动调过就让位 */
    if (!s_dn_manual) {
        int dn = (s_night_tier >= 2) ? CONFIG_APP_CAM_NIGHT_DN_MIN
               : (s_night_tier == 1) ? CONFIG_APP_CAM_NIGHT_DN_LOW
               : s_dn_user;
        if (dn != app_imgpipe_get_dn()) {
            app_imgpipe_set_dn(dn);
            app_imgpipe_reset_prev();    /* 档位变化：丢弃旧时域历史，防残影 */
            ESP_LOGW(TAG, "night: auto dn -> %d", dn);
            app_usb_link_log("cam: night auto dn %d", dn);
        }
    }
}

/* ---------------- 夜视阶梯：暗度驱动降帧率 + 自动时域降噪 ----------------
 * 每帧调用（主循环），与 AUTO 开关无关（默认常开，这是"自动夜视"的核心）。
 * 判定顺序上运动优先：运动高帧率期间完全不碰帧率，避免与 AUTO 抢。
 * 手动 fps 命令会让位（s_night_fps=false），{"cmd":"cam","night":true} 可恢复。 */
static void night_tick(void)
{
    if (!s_night_fps) {
        return;
    }
    /* 只对板载摄像头闭环（PC 送帧没有可调的曝光）；运动高帧率期间不动 */
    if (app_imgpipe_get_src() != APP_IMG_SRC_CAM || (s_auto_fps && s_auto_active)) {
        return;
    }
    if (s_cam_luma <= 0) {
        return;                          /* 没有新鲜采样（流停了），不盲调 */
    }

    int64_t now = esp_timer_get_time();
    if (s_night_luma_ema < 0) {
        s_night_luma_ema = s_cam_luma;   /* 首帧直接建立 */
    } else {
        s_night_luma_ema += (s_cam_luma - s_night_luma_ema) / 8;
    }

    /* 即时档位：升档（更暗）用阈值本身，降档（变亮）要越过阈值 + 迟滞带，
       带内维持现状——否则在阈值边缘会来回切帧率 */
    int luma = s_night_luma_ema;
    int want;
    if (luma < CONFIG_APP_CAM_NIGHT_LUMA_DARKER) {
        want = 2;
    } else if (luma < CONFIG_APP_CAM_NIGHT_LUMA_DARK) {
        want = 1;
    } else if (luma > CONFIG_APP_CAM_NIGHT_LUMA_DARK + NIGHT_LUMA_HYST) {
        want = 0;
    } else {
        want = s_night_tier;
    }

    /* 保持时间：目标档位一变就重新计时；取反方向（回升）给同样的 3s，
       防止 AE/ENH 的亮度收敛过程把档位抖出去 */
    if (want != s_night_pend) {
        s_night_pend = want;
        s_night_state_us = now;
    }
    if (s_night_pend == s_night_tier) {
        return;                          /* 已在该档 */
    }
    int hold_ms = (s_night_pend < s_night_tier) ? NIGHT_BRIGHT_MS : NIGHT_DARK_MS;
    if ((now - s_night_state_us) / 1000 < hold_ms) {
        return;                          /* 保持时间未到 */
    }

    s_night_tier = s_night_pend;
    ESP_LOGW(TAG, "night: luma %d (ema %d) -> tier %d", s_cam_luma, luma, s_night_tier);
    night_apply();
}

/* ---------------- F10：特征库备份 / 恢复（2026-10-11） ----------------
 * 命令面：db_export / db_import{size,crc} / db_import_end / db_import_abort；
 * 数据面：JCLINK 0x06 分片（payload = offset(u32 LE) + data）。
 * 设计约束（重要）：0x06 回调**不保存任何跨核共享状态**——"接收文件是否存在"
 * 就是会话标记，数据一律经 VFS 落盘；长度/CRC/信封/名表全部在收尾时由 faceai
 * 任务读回校验。这样既不需要大块 PSRAM 缓冲，也绕开廿一节的跨核 L1 缓存坑。 */
static bool     s_dbx_importing = false;   /* 只被 faceai 任务读写 */
static uint32_t s_dbx_total = 0;
static uint32_t s_dbx_exp_crc = 0;

/* 复制文件（src 不存在视为成功：无可备份）；返回 false = 真实 I/O 失败 */
static bool copy_file(const char *src, const char *dst)
{
    FILE *a = fopen(src, "rb");
    if (!a) return true;
    FILE *b = fopen(dst, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    bool ok = true;
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) ok = false;
    while (ok) {
        size_t n = fread(buf, 1, 4096, a);
        if (n == 0) break;
        if (fwrite(buf, 1, n, b) != n) {
            ok = false;
            break;
        }
    }
    if (ok && ferror(a)) ok = false;
    if (ok && fflush(b) != 0) ok = false;
    if (buf) free(buf);
    fclose(a);
    fclose(b);
    return ok;
}

/* 导入前自动备份现有库（F10 需求：防误操作，可人工回滚） */
static void db_backup_current(void)
{
    bool ok = copy_file(FACE_DB_PATH, DBX_FACE_BAK);
    if (!copy_file(FACE_NAMES_PATH, DBX_NAMES_BAK)) ok = false;
    ESP_LOGW(TAG, "dbx: current lib -> %s / %s (%s)", DBX_FACE_BAK, DBX_NAMES_BAK,
             ok ? "ok" : "partial");
    app_usb_link_log("faceai: dbx backup -> %s / %s (%s)", DBX_FACE_BAK, DBX_NAMES_BAK,
                     ok ? "ok" : "partial");
}

/* 0x06 回调（rx 任务上下文）：只做"按偏移顺序追加"，异常留给收尾整批拒绝 */
static void db_blob_cb(const uint8_t *payload, uint32_t len)
{
    if (len < 5) return;                      /* 4B 偏移 + 至少 1B 数据 */
    uint32_t off;
    memcpy(&off, payload, 4);
    FILE *f = fopen(DBX_RECV_PATH, "r+b");    /* 不存在 -> 非导入会话，静默忽略 */
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long got = ftell(f);
    if (got < 0 || (uint32_t)got != off) {
        ESP_LOGW(TAG, "dbx: chunk off=%u but received=%ld (dropped)", (unsigned)off, got);
        fclose(f);
        return;
    }
    size_t n = fwrite(payload + 4, 1, len - 4, f);
    fclose(f);
    if (n != len - 4) {
        ESP_LOGW(TAG, "dbx: short write %u/%u", (unsigned)n, (unsigned)(len - 4));
    }
}

/* 导出：内存名表 + face.db → 信封 → 0x06 分片（ack 带 size/crc/parts 先回） */
static void db_export_run(void)
{
    uint8_t *names_buf = nullptr, *face_buf = nullptr, *env = nullptr, *chunk = nullptr;
    size_t names_len = 0, face_len = 0;
    uint32_t total = 0, crc = 0, parts = 0;
    cJSON *d = nullptr;
    char msg[128];

    if (!s_db_ok) {
        send_result("error", nullptr, 0, nullptr, 0, "feature db unavailable");
        return;
    }
    char *names_js = names_to_json();
    if (names_js) {
        names_buf = (uint8_t *)names_js;
        names_len = (size_t)strlen(names_js);
    }
    face_buf = read_file_all(FACE_DB_PATH, &face_len);
    if (!face_buf || face_len == 0) {
        ESP_LOGE(TAG, "dbx: face.db unreadable");
        send_result("error", nullptr, 0, nullptr, 0, "face.db unreadable");
        goto out;
    }
    if ((unsigned long)(DBX_HDR_LEN + names_len + face_len) > DBX_MAX_BYTES) {
        send_result("error", nullptr, 0, nullptr, 0, "lib too large to export");
        goto out;
    }

    total = (uint32_t)(DBX_HDR_LEN + names_len + face_len);
    env = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    chunk = (uint8_t *)heap_caps_malloc(JCL_BLOB_CHUNK_DEF + 4, MALLOC_CAP_8BIT);
    if (!env || !chunk) {
        send_result("error", nullptr, 0, nullptr, 0, "no mem for export");
        goto out;
    }
    {
        uint32_t magic = DBX_MAGIC, ver = DBX_VER;
        uint32_t face32 = (uint32_t)face_len, names32 = (uint32_t)names_len;
        memcpy(env, &magic, 4);
        memcpy(env + 4, &ver, 4);
        memcpy(env + 8, &face32, 4);
        memcpy(env + 12, &names32, 4);
    }
    if (names_len) memcpy(env + DBX_HDR_LEN, names_buf, names_len);
    memcpy(env + DBX_HDR_LEN + names_len, face_buf, face_len);
    crc = esp_rom_crc32_le(0, env, total);
    parts = (total + JCL_BLOB_CHUNK_DEF - 1) / JCL_BLOB_CHUNK_DEF;

    d = cJSON_CreateObject();
    if (d) {
        cJSON_AddNumberToObject(d, "size", total);
        cJSON_AddNumberToObject(d, "crc", (double)crc);   /* u32 用 double 精确承载 */
        cJSON_AddNumberToObject(d, "parts", parts);
        cJSON_AddNumberToObject(d, "chunk", JCL_BLOB_CHUNK_DEF);
        s_extra_obj = d;
    }
    snprintf(msg, sizeof(msg), "db_export %u bytes / %u part(s) crc=%08x",
             (unsigned)total, (unsigned)parts, (unsigned)crc);
    send_result("none", nullptr, 0, nullptr, 0, msg);
    app_usb_link_log("faceai: db_export %u bytes (%u parts) crc=%08x", (unsigned)total,
                     (unsigned)parts, (unsigned)crc);

    for (uint32_t i = 0; i < parts; i++) {
        uint32_t off = i * JCL_BLOB_CHUNK_DEF;
        uint32_t n = (total - off < JCL_BLOB_CHUNK_DEF) ? (total - off) : JCL_BLOB_CHUNK_DEF;
        memcpy(chunk, &off, 4);
        memcpy(chunk + 4, env + off, n);
        if (app_usb_link_send(JCL_TYPE_BLOB, chunk, n + 4) != ESP_OK) {
            ESP_LOGW(TAG, "dbx: export part %u/%u send failed", (unsigned)(i + 1), (unsigned)parts);
        }
        vTaskDelay(1);                        /* 让出 CPU：别长时间占住 AI 任务 */
    }

out:
    if (env) heap_caps_free(env);
    if (chunk) heap_caps_free(chunk);
    if (face_buf) free(face_buf);
    if (names_buf) cJSON_free(names_buf);
}

/* 导入准备：校验参数 → 备份现有库 → 建接收文件（=会话标记） */
static void db_import_begin(const cJSON *root)
{
    const cJSON *js = cJSON_GetObjectItem(root, "size");
    const cJSON *jc = cJSON_GetObjectItem(root, "crc");
    uint32_t total = 0, crc = 0;

    if (!cJSON_IsNumber(js)) {
        send_result("error", nullptr, 0, nullptr, 0, "db_import needs size");
        return;
    }
    total = (uint32_t)js->valuedouble;
    crc = cJSON_IsNumber(jc) ? (uint32_t)jc->valuedouble : 0;
    if (!s_db_ok) {
        send_result("error", nullptr, 0, nullptr, 0, "feature db unavailable");
        return;
    }
    if (total <= DBX_HDR_LEN || total > DBX_MAX_BYTES) {
        send_result("error", nullptr, 0, nullptr, 0, "db_import bad size");
        return;
    }

    db_backup_current();                      /* 覆盖式恢复：先留回滚副本 */

    remove(DBX_RECV_PATH);                    /* 清掉上一次未完成的会话 */
    FILE *f = fopen(DBX_RECV_PATH, "wb");
    if (!f) {
        send_result("error", nullptr, 0, nullptr, 0, "cannot create temp file");
        return;
    }
    fclose(f);

    s_dbx_importing = true;
    s_dbx_total = total;
    s_dbx_exp_crc = crc;
    char msg[128];
    snprintf(msg, sizeof(msg), "db_import ready: expect %u bytes (crc=%08x), .bak saved",
             (unsigned)total, (unsigned)crc);
    send_result("none", nullptr, 0, nullptr, 0, msg);
    app_usb_link_log("faceai: db_import ready, expect %u bytes crc=%08x", (unsigned)total,
                     (unsigned)crc);
}

/* 导入收尾：整包读回校验 → 替换 face.db → 写名表 → 重建特征库 → 复位识别缓存 */
static void db_import_end(void)
{
    size_t total = 0;
    uint8_t *buf = nullptr;
    face_name_t *tmp_names = nullptr;
    const char *err = nullptr;
    int n_names = 0;
    uint32_t magic = 0, ver = 0, face_len = 0, names_len = 0;
    char msg[128];

    if (!s_dbx_importing) {
        send_result("error", nullptr, 0, nullptr, 0, "no db_import in progress");
        return;
    }
    s_dbx_importing = false;

    buf = read_file_all(DBX_RECV_PATH, &total);
    if (!buf) {
        err = "no data received";
    } else if (total != (size_t)s_dbx_total) {
        err = "size mismatch";
    } else if (esp_rom_crc32_le(0, buf, (uint32_t)total) != s_dbx_exp_crc) {
        err = "crc mismatch";
    } else {
        memcpy(&magic, buf, 4);
        memcpy(&ver, buf + 4, 4);
        memcpy(&face_len, buf + 8, 4);
        memcpy(&names_len, buf + 12, 4);
        if (magic != DBX_MAGIC || ver != DBX_VER) {
            err = "bad envelope magic/ver";
        } else if (face_len == 0 || DBX_HDR_LEN + face_len + names_len != total) {
            err = "bad envelope lengths";
        } else {
            tmp_names = (face_name_t *)malloc(sizeof(face_name_t) * MAX_FEATS);
            char *nb = tmp_names ? (char *)malloc(names_len + 1) : nullptr;
            if (!tmp_names || !nb) {
                err = "no mem for names";
            } else {
                memcpy(nb, buf + DBX_HDR_LEN, names_len);
                nb[names_len] = 0;
                n_names = names_parse_buf(nb, tmp_names, MAX_FEATS);
                if (n_names < 0) err = "bad names json";
            }
            if (nb) free(nb);
        }
    }

    if (err) {
        ESP_LOGE(TAG, "dbx: import rejected: %s", err);
        app_usb_link_log("faceai: db_import FAILED (%s) - library unchanged", err);
        if (tmp_names) free(tmp_names);
        if (buf) free(buf);
        remove(DBX_RECV_PATH);
        send_result("error", nullptr, 0, nullptr, 0, err);
        return;
    }

    /* face.db 落盘：先写 face_new.db，再删旧 + rename 替换（原子性靠这一步） */
    bool ok = true;
    FILE *f = fopen(DBX_FACE_TMP, "wb");
    if (!f) {
        ok = false;
    } else {
        size_t n = fwrite(buf + DBX_HDR_LEN + names_len, 1, face_len, f);
        ok = (n == face_len) && (fflush(f) == 0);
        fclose(f);
    }
    if (ok) {
        remove(FACE_DB_PATH);
        if (rename(DBX_FACE_TMP, FACE_DB_PATH) != 0) ok = false;
    }
    if (!ok) remove(DBX_FACE_TMP);
    free(buf);
    if (!ok) {
        free(tmp_names);
        remove(DBX_RECV_PATH);
        ESP_LOGE(TAG, "dbx: face.db replace failed");
        send_result("error", nullptr, 0, nullptr, 0, "face.db replace failed");
        return;
    }

    /* 提交名表（内存 + names.json + NVS 兜底） */
    for (int i = 0; i < n_names && i < MAX_FEATS; i++) s_names[i] = tmp_names[i];
    s_name_cnt = n_names;
    free(tmp_names);
    names_save();
    remove(DBX_RECV_PATH);

    /* 重建特征库（覆盖式恢复：旧 id 映射全部作废） */
    if (s_db) {
        delete s_db;
        s_db = nullptr;
    }
    s_db = new dl::recognition::DataBase(FACE_DB_PATH, s_feat ? s_feat->get_feat_len() : 512);
    s_db_ok = (s_db != nullptr);

    /* 复位识别侧缓存（同 clear/delete 的清单一套） */
    smooth_reset();
    s_trk_ok = false;
    s_evt_match_name[0] = 0;
    s_evt_match_us = 0;
    s_evt_rej_us = 0;
    s_multi_cache_n = 0;
    s_reg_pending = false;
    s_reg_shots = 0;
    s_lib_dirty = true;

    snprintf(msg, sizeof(msg), "db_import ok: %d feat(s), %d person(s)",
             s_name_cnt, name_count_persons());
    ESP_LOGW(TAG, "dbx: %s", msg);
    app_usb_link_log("faceai: %s", msg);
    send_result("none", nullptr, 0, nullptr, 0, msg);
}

static void db_import_abort(void)
{
    s_dbx_importing = false;
    remove(DBX_RECV_PATH);
    send_result("none", nullptr, 0, nullptr, 0, "db_import aborted, library unchanged");
}

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
        if (s_reg_pending) {
            s_reg_pending = false;              /* stop 同时取消未完成的注册采集 */
            s_reg_shots = 0;
            send_result("none", nullptr, 0, nullptr, 0, "stopped (enrollment cancelled)");
        } else {
            send_result("none", nullptr, 0, nullptr, 0, "stopped");
        }
    } else if (strcmp(cmd, "mode") == 0) {
        const cJSON *jd = cJSON_GetObjectItem(root, "detect");
        const cJSON *jm = cJSON_GetObjectItem(root, "match");
        const cJSON *jt = cJSON_GetObjectItem(root, "thr");
        const cJSON *jth = cJSON_GetObjectItem(root, "thr_hi");
        const cJSON *jtl = cJSON_GetObjectItem(root, "thr_lo");
        if (cJSON_IsBool(jd)) s_mode_detect = cJSON_IsTrue(jd);
        if (cJSON_IsBool(jm)) s_mode_match = cJSON_IsTrue(jm);
        /* F3 双阈值。旧命令 "thr":x 保持兼容：设 LO=x，HI 同步抬到 x+0.10；
         * 独立调节用 "thr_hi"/"thr_lo" */
        if (cJSON_IsNumber(jt)) {
            float t = (float)jt->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;      /* 允许 70 或 0.7 */
            if (t > 0.0f && t < 1.0f) {
                s_thr_lo = t;
                s_thr_hi = t + 0.10f;
                if (s_thr_hi > 0.99f) s_thr_hi = 0.99f;
            }
        }
        if (cJSON_IsNumber(jth)) {
            float t = (float)jth->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;
            if (t > 0.0f && t < 1.0f) s_thr_hi = t;
        }
        if (cJSON_IsNumber(jtl)) {
            float t = (float)jtl->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;
            if (t > 0.0f && t < 1.0f) s_thr_lo = t;
        }
        if (s_thr_hi < s_thr_lo) s_thr_hi = s_thr_lo;      /* 恒保持 hi >= lo */
        /* F4 保持窗（ms，运行时可调）：{"cmd":"mode","trk_hold_ms":400}；0=关闭节流。
         * 现场必须能扫：AI 一帧 ~244ms，默认 150ms 根本触发不了（见 s_trk_hold_ms 注释）*/
        const cJSON *jtk = cJSON_GetObjectItem(root, "trk_hold_ms");
        if (cJSON_IsNumber(jtk)) {
            int v = (int)jtk->valuedouble;
            if (v < 0) v = 0;
            if (v > 2000) v = 2000;
            s_trk_hold_ms = v;
            s_trk_ok = false;               /* 改了参数，旧结论作废（别跨参数复用） */
        }
        /* 检测分数阈值：{"cmd":"mode","det_thr":0.35}（也接受 35）。
         * 排查"检不到脸"时用它现场扫，不必重烧。⚠️ 单阶段检测器只有 idx 0。*/
        const cJSON *jdt = cJSON_GetObjectItem(root, "det_thr");
        if (cJSON_IsNumber(jdt)) {
            float t = (float)jdt->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;
            if (t > 0.0f && t < 1.0f) {
                s_det_thr = t;
                if (s_detect) {
                    s_detect->set_score_thr(t, 0);
#if !CONFIG_APP_FACEAI_DETECT_IS_ESPDET
                    s_detect->set_score_thr(t, 1);
#endif
                }
                if (s_mnp) s_mnp->set_score_thr(t);      /* 二级（关键点阶段）同步 */
                ESP_LOGI(TAG, "detect score thr -> %.2f", (double)t);
            }
        }
        /* F6 事件去重窗口（ms）：{"cmd":"mode","dedup_ms":3000}；0 = 关闭去重。
         * 同一人的 match / 稳定 reject 在窗口内只报一次事件（规划 P1-F6）。 */
        const cJSON *jdd = cJSON_GetObjectItem(root, "dedup_ms");
        if (cJSON_IsNumber(jdd)) {
            int v = (int)jdd->valuedouble;
            if (v < 0) v = 0;
            if (v > 600000) v = 600000;
            s_dedup_ms = v;
        }
        /* F7 多人脸张数：{"cmd":"mode","multi":2}（1..3，1 = 只识别最大脸旧行为） */
        const cJSON *jmu = cJSON_GetObjectItem(root, "multi");
        if (cJSON_IsNumber(jmu)) {
            int v = (int)jmu->valuedouble;
            if (v < 1) v = 1;
            if (v > 3) v = 3;
            s_multi_max = v;
        }
        /* F2 快速通道：{"cmd":"mode","fast_thr":0.80}（接受 0~1 浮点或 0~100，
         * 0 = 关闭）。本帧 raw_sim 达到该值时不等窗口票数直接确认 match。 */
        const cJSON *jft = cJSON_GetObjectItem(root, "fast_thr");
        if (cJSON_IsNumber(jft)) {
            float t = (float)jft->valuedouble;
            if (t > 1.0f && t <= 100.0f) t /= 100.0f;
            if (t >= 0.0f && t < 1.0f) s_fast_thr = t;
        }
        /* esp-dl 工作区实验：{"cmd":"mode","feat_ws":0|1|2}
         * （0=关 1=每次提特征前清零工作区 2=随机毒化，见 s_feat_ws_mode 注释） */
        const cJSON *jws = cJSON_GetObjectItem(root, "feat_ws");
        if (cJSON_IsNumber(jws)) {
            int v = (int)jws->valuedouble;
            if (v < 0) v = 0;
            if (v > 2) v = 2;
            s_feat_ws_mode = v;
            s_trk_ok = false;               /* 提取语义变了，旧结论作废 */
            ESP_LOGW(TAG, "feat workspace mode -> %d (%s)", v,
                     v == 0 ? "off" : v == 1 ? "zero" : "poison");
        }
        /* esp-dl 逐层二分：{"cmd":"mode","feat_bisect":K}（K 轮，0=关）。
         * 武装后强制 feat_ws=1（清零制度），每个带脸帧跑两次 traced run
         * （每算子后 arena CRC），0x04 日志报 A/B 同帧分歧级与跨帧首分歧级。 */
        const cJSON *jbi = cJSON_GetObjectItem(root, "feat_bisect");
        if (cJSON_IsNumber(jbi)) {
            int v = (int)jbi->valuedouble;
            if (v < 0) v = 0;
            if (v > 50) v = 50;
            s_feat_bisect_left = v;
            s_feat_ws_mode = 1;             /* 二分设计在清零制度下 */
            s_feat_bisect_ref_valid = false;
            s_feat_bisect_ref.clear();
            s_trk_ok = false;
            ESP_LOGW(TAG, "feat bisect armed: %d rounds (ws=1)", v);
        }
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "detect=%d match=%d thr=%.2f/%.2f (lo/hi) det_thr=%.2f"
                 " trk=%dms/iou%d/sh%d%% dedup=%dms multi=%d fast=%.2f ws=%d bisect=%d %s",
                 (int)s_mode_detect, (int)s_mode_match, s_thr_lo, s_thr_hi,
                 (double)s_det_thr, s_trk_hold_ms,
                 CONFIG_APP_FACEAI_TRACK_IOU, CONFIG_APP_FACEAI_TRACK_MOVE_PCT,
                 s_dedup_ms, s_multi_max, (double)s_fast_thr, s_feat_ws_mode,
                 s_feat_bisect_left, detect_model_name());
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
                /* F1 多帧注册：{"cmd":"register","name":X,"shots":3}，缺省 1 帧
                 * （与旧版行为一致）；多帧按质量门控 + 400ms 间隔自动采集 */
                const cJSON *jshots = cJSON_GetObjectItem(root, "shots");
                int shots = cJSON_IsNumber(jshots) ? jshots->valueint : 1;
                if (shots < 1) shots = 1;
                if (shots > FACE_ENROLL_MAX_SHOTS) shots = FACE_ENROLL_MAX_SHOTS;
                strlcpy(s_reg_name, name, sizeof(s_reg_name));
                s_reg_arm_us = esp_timer_get_time();
                s_reg_pending = true;
                s_reg_shots = shots;
                s_reg_got = 0;
                s_reg_last_us = 0;
                smooth_reset();
                s_trk_ok = false;              /* F4：注册会话开始，旧结论作废 */
                s_evt_match_name[0] = 0;       /* F6：库操作后事件去重状态一并复位 */
                s_evt_match_us = 0;
                s_evt_rej_us = 0;
                s_multi_cache_n = 0;           /* F7：副脸快照同样作废 */
                s_running = true;
                ESP_LOGI(TAG, "register armed for '%s' x%d", s_reg_name, shots);
                /* 立即 ack：host 先等这条 ack，再对准镜头，注册就不会错配 */
                char ack[160];
                snprintf(ack, sizeof(ack), "register armed for %s: collect %d frame(s)%s",
                         s_reg_name, shots, shots > 1 ? " (auto, stay in view)" : "");
                if (night_face_gated()) {
                    strlcat(ack, " (night mode: face off, runs when light returns)", sizeof(ack));
                }
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
                smooth_reset();                 /* 已删 id 不能残留在确认窗里 */
                s_trk_ok = false;               /* F4：同上，被删的人不能继续被复用 */
                s_evt_match_name[0] = 0;        /* F6/F7：库变了，事件去重与快照作废 */
                s_evt_match_us = 0;
                s_evt_rej_us = 0;
                s_multi_cache_n = 0;
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
    } else if (strcmp(cmd, "link") == 0) {
        /* {"cmd":"link"} 查询 / {"cmd":"link","value":"usb"|"tcp"|"auto"} 强制切换链路
           —— 2026-10-16「自由切换」：auto = 历史语义（TCP 客户端连上抢占、断开回落 CDC）；
           usb / tcp = 强制指定发送后端（强制 usb 时新 TCP 客户端会被拒绝并说明）。 */
        const cJSON *jv = cJSON_GetObjectItem(root, "value");
        app_usb_link_stats_t st;
        app_usb_link_get_stats(&st);
        char msg[160];
        if (cJSON_IsString(jv)) {
            if (!app_usb_link_force_backend(jv->valuestring)) {
                send_result("error", nullptr, 0, nullptr, 0, "link value must be usb|tcp|auto");
            } else {
                snprintf(msg, sizeof(msg), "link=%s active=%s tx_frames=%u tx_kb=%u tx_drop=%u",
                         app_usb_link_forced_name(), app_usb_link_active_name(),
                         (unsigned)st.tx_frames, (unsigned)(st.tx_bytes / 1024),
                         (unsigned)st.tx_drop);
                send_result("none", nullptr, 0, nullptr, 0, msg);
            }
        } else {
            snprintf(msg, sizeof(msg),
                     "link=%s active=%s tx_frames=%u tx_kb=%u tx_drop=%u last_ms=%u",
                     app_usb_link_forced_name(), app_usb_link_active_name(),
                     (unsigned)st.tx_frames, (unsigned)(st.tx_bytes / 1024),
                     (unsigned)st.tx_drop, (unsigned)st.tx_last_ms);
            send_result("none", nullptr, 0, nullptr, 0, msg);
        }
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
    } else if (strcmp(cmd, "wifi") == 0) {
        /* {"cmd":"wifi","ssid":"X","pass":"Y"} —— 运行时配置 STA 凭据并连接。
         * 写 NVS（FLASH 存储）断电保存；断线自动重连。ack 只表示已受理，
         * 关联结果看后续结果帧的 got IP / 状态磁贴。 */
        const cJSON *jss = cJSON_GetObjectItem(root, "ssid");
        const cJSON *jpw = cJSON_GetObjectItem(root, "pass");
        if (!cJSON_IsString(jss) || !jss->valuestring[0]) {
            send_result("error", nullptr, 0, nullptr, 0, "wifi needs ssid");
        } else {
            app_wifi_set_credentials(jss->valuestring, cJSON_IsString(jpw) ? jpw->valuestring : "");
            char m[96];
            snprintf(m, sizeof(m), "wifi connecting to %s (saved)", jss->valuestring);
            send_result("none", nullptr, 0, nullptr, 0, m);
        }
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
        /* AE 自动开关（app 级闭环，app_camauto）：{"cmd":"cam","ae":true|false}。
           手动 exp/gain/fps 命令会自动退出 AE（见下 note_manual）。 */
        j = cJSON_GetObjectItem(root, "ae");
        if (cJSON_IsBool(j)) {
            app_camauto_set_ae(cJSON_IsTrue(j));
        }
        /* AWB 自动开关（灰世界 -> 软件白平衡） */
        j = cJSON_GetObjectItem(root, "awb");
        if (cJSON_IsBool(j)) {
            app_camauto_set_awb(cJSON_IsTrue(j));
        }
        /* AE 目标亮度：{"cmd":"cam","ae_target":100}（0..255） */
        j = cJSON_GetObjectItem(root, "ae_target");
        if (cJSON_IsNumber(j)) {
            app_camauto_set_target(j->valueint);
        }
        /* AE 曝光上限（毫秒）：{"cmd":"cam","ae_exp_max_ms":40}（0..1000，0=解除）。
         * 暗光下 AE 不再把积分拉满帧周期（模糊毁掉检测与特征），超过上限转提增益。
         * 夜视档（tier>=1）自动解除。手动 exp/gain/fps 照旧让 AE 让位。 */
        j = cJSON_GetObjectItem(root, "ae_exp_max_ms");
        if (cJSON_IsNumber(j)) {
            int v = j->valueint;
            if (v < 0) v = 0;
            if (v > 1000) v = 1000;
            s_ae_exp_cap_ms = v;
            app_usb_link_log("cam: ae exp cap -> %s", v > 0 ? "on" : "off");
        }
        /* 夜视增强总开关：{"cmd":"cam","enh":true|false}
           （gamma 提亮 + BF 降噪自适应；关=两个 ISP 模块都禁用） */
        j = cJSON_GetObjectItem(root, "enh");
        if (cJSON_IsBool(j)) {
            app_camauto_enh_set(cJSON_IsTrue(j));
        }
        /* 手动 gamma：{"cmd":"cam","gamma":0.65}（<3 按浮点参数，否则按千分比）
           或 {"cmd":"cam","gamma_en":false} 禁用模块。手动后自适应让位，
           {"cmd":"cam","enh":true} 重新恢复自动。 */
        j = cJSON_GetObjectItem(root, "gamma");
        if (cJSON_IsNumber(j)) {
            double v = j->valuedouble;
            int permille = (v > 0.0 && v <= 3.0) ? (int)(v * 1000.0) : (int)v;
            app_camauto_enh_manual_gamma(permille);
        }
        j = cJSON_GetObjectItem(root, "gamma_en");
        if (cJSON_IsBool(j)) {
            app_camauto_enh_manual_gamma(cJSON_IsTrue(j) ? 1000 : 0);
        }
        /* 手动 BF level：{"cmd":"cam","bf_level":5}（0=禁用，2..20） */
        j = cJSON_GetObjectItem(root, "bf_level");
        if (cJSON_IsNumber(j)) {
            app_camauto_enh_manual_bf(j->valueint);
        }
        /* CCM 颜色校正混合比例：{"cmd":"cam","ccm_mix":N}
         * 千分比：0=禁用 CCM 模块、1000=完整厂商标定矩阵（默认）、>1000 过饱和方向。
         * 静态颜色校准，与 AE/AWB/夜视增强无联动；开机由 app_camera_init 按
         * APP_CAM_CCM_MIX 默认下发。 */
        j = cJSON_GetObjectItem(root, "ccm_mix");
        if (cJSON_IsNumber(j)) {
            int mix = j->valueint;
            if (mix < 0) mix = 0;
            if (mix > 1500) mix = 1500;
            s_cam_ccm = mix;
            if (app_camera_post_ccm(mix)) {
                app_usb_link_log("cam: ccm mix -> %d", mix);
            } else {
                app_usb_link_log("cam: ccm unavailable (no ISP channel)");
            }
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
        /* 镜像/翻转（0x00980914/15 经 esp_video 的 sensor 映射表直写 sc2336）。
           ⚠️ 必须在下面的 set_ctrl 之前解析——原代码在 set_ctrl 之后才加进 mask，
           等于从未下发（静默 no-op 的老 bug）。 */
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
        if (mask) {
            app_camera_set_ctrl(&c, mask);
            /* 手动改曝光/增益：AE 自动模式让位（手动优先，与手动 fps 退出 auto_fps 同规则） */
            if (mask & (APP_CAM_SET_EXP | APP_CAM_SET_GAIN)) {
                app_camauto_note_manual();
            }
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
            /* ⚠️ 先关夜视阶梯再退 AUTO：auto_fps_set(false) 会在阶梯开着时
               把帧率交还给阶梯，顺序反了会把用户刚设的手动帧率顶掉 */
            if (s_night_fps) {
                /* 手动帧率同时让夜视阶梯让位（同"手动优先"规则）；
                   想恢复自动：{"cmd":"cam","night":true} */
                s_night_fps = false;
                app_usb_link_log("cam: manual fps, night ladder OFF");
            }
            if (s_auto_fps) {
                auto_fps_set(false);    /* 手动改帧率 = 退出自动模式 */
                app_usb_link_log("cam: manual fps, auto-fps OFF");
            }
            app_camauto_note_manual();  /* 手动帧率同样让 AE 退出（Night 预设是手动档） */
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
            /* 手动指定降噪档 = 自动 DN 让位（否则夜视阶梯会把用户的选择顶掉）；
               恢复自动：{"cmd":"cam","night":true} */
            s_dn_manual = true;
            s_dn_user = j->valueint;
        }

        /* 发布端按需发布开关（2026-10-06 待办 5；A/B 对比用）：
         * {"cmd":"cam","pub_demand":false} 恢复"每帧都发布"的旧行为。
         * 效果看 imgpipe/prof 每秒日志的 pub[...] n（实际发布数）与 skip 数。 */
        j = cJSON_GetObjectItem(root, "pub_demand");
        if (cJSON_IsBool(j)) {
            app_imgpipe_set_pub_demand(cJSON_IsTrue(j));
        }

        /* 夜视阶梯总开关：{"cmd":"cam","night":true|false}
         * 开 = 暗度驱动的降帧率 + 自动时域降噪（默认由 Kconfig 决定）；
         * night:true 同时复位手动让位标志（手动 DN 重新交给自动管）。 */
        j = cJSON_GetObjectItem(root, "night");
        if (cJSON_IsBool(j) || cJSON_IsNumber(j)) {
            bool on = cJSON_IsBool(j) ? cJSON_IsTrue(j) : (j->valueint != 0);
            bool was = s_night_fps;
            s_night_fps = on;
            if (on) {
                s_dn_manual = false;
                s_night_pend = s_night_tier;     /* 重新按亮度判定并计时 */
                s_night_state_us = esp_timer_get_time();
                s_night_luma_ema = -1;
                /* 已经在夜视档就直接把帧率/降噪拉回来（用户可能刚手动改过 fps）；
                   tier0（亮场景）不主动接管，尊重用户手动设的帧率 */
                if (!was && s_night_tier > 0) {
                    night_apply();
                }
            }
            app_usb_link_log("cam: night ladder %s (tier %d)", on ? "ON" : "OFF", s_night_tier);
            ESP_LOGW(TAG, "night ladder -> %s", on ? "ON" : "OFF");
        }
        /* 夜间人脸开关（光照双模式）：{"cmd":"cam","face_night":true|false}
         * true（默认）= 夜视档（ntier>=1）不跑人脸、AI 让出帧池给 0x05 预览；
         * false = 夜视档也照常识别（调试/特殊场景用）。
         * 生效/退出由主循环的边沿检测上报（日志 + event="night"/正常帧恢复）。 */
        j = cJSON_GetObjectItem(root, "face_night");
        if (cJSON_IsBool(j)) {
            s_face_night_off = cJSON_IsTrue(j);
            app_usb_link_log("faceai: face_night %s",
                             s_face_night_off ? "ON (no face in low light)" : "OFF (face always on)");
        }
        /* 曝光/增益被改过就丢弃时域历史：亮度突变混进历史会留下整屏残影 */
        if (mask & (APP_CAM_SET_EXP | APP_CAM_SET_GAIN)) {
            app_imgpipe_reset_prev();
        }

        /* 运动估计阈值标定：{"cmd":"cam","mv_thr":N}（8x8 块内最大 G 通道差 1..63，
         * 静止噪声实测块最大差 ~13~20，挥手/移动会到 30+；调小灵敏、调大迟钝）*/
        j = cJSON_GetObjectItem(root, "mv_thr");
        if (cJSON_IsNumber(j)) {
            app_imgpipe_set_motion_thr(j->valueint);
        }

        /* 自动帧率：{"cmd":"cam","auto_fps":true|false} —— 静止 15fps / 运动 50fps。
         * 开启后独立运动估计一直跑（与降噪无关），运动持续 ~100ms 升档、静止 ~1.5s 回落；
         * 手动 fps 命令会自动退出自动模式。结果帧带 "auto" 与实时 "mv"/"mv_peak"。 */
        j = cJSON_GetObjectItem(root, "auto_fps");
        if (cJSON_IsBool(j)) {
            auto_fps_set(cJSON_IsTrue(j));
        } else if (cJSON_IsNumber(j)) {
            auto_fps_set(j->valueint != 0);
        }
        j = cJSON_GetObjectItem(root, "mv_ratio");
        if (cJSON_IsNumber(j)) {
            int v = j->valueint;
            if (v < 1) v = 1;
            if (v > 100) v = 100;
            s_auto_ratio = v;
        }

        const cJSON *jkr = cJSON_GetObjectItem(root, "wb_kr");
        const cJSON *jkb = cJSON_GetObjectItem(root, "wb_kb");
        if (cJSON_IsNumber(jkr) || cJSON_IsNumber(jkb)) {
            int kr = 0, kb = 0;
            app_camauto_wb_get(&kr, &kb);
            if (cJSON_IsNumber(jkr)) {
                kr = jkr->valueint;
            }
            if (cJSON_IsNumber(jkb)) {
                kb = jkb->valueint;
            }
            app_camauto_wb_apply(kr, kb, true);
            app_camauto_note_manual_wb();   /* 手动 WB 让 AWB 退出 */
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
        app_camauto_wb_get(&s_cam_wb_kr, &s_cam_wb_kb);   /* WB 权威在 camauto（ISP/软件统一） */
        s_cam_fps = (int)app_camera_get_fps();
        s_cam_dn = app_imgpipe_get_dn();
        /* 引擎视角与真实寄存器对齐（之后 AE 的信箱写入会继续更新它） */
        app_camauto_adopt((int)now.exp_val, (int)now.gain_val);
        {
            app_imgpipe_stats_t pst = {};
            app_imgpipe_get_stats(&pst);
            s_cam_dn_ms = (int)pst.dn_ms;
            s_cam_dn_skip = (int)pst.dn_skip;
            s_cam_dn_blk = (int)pst.dn_blocks;
            s_cam_dn_sad = (int)pst.dn_sad;
            s_cam_dn_mmax = (int)pst.dn_mmax;
            s_cam_dn_mv = (int)pst.dn_motion;
        }
        char msg[224];
        /* exp 的单位是"行"（VTS≈1000 @640x480/50fps）：994 行 ≈ 19.9ms（=帧周期 20ms 的上限）。
           夜视想更长曝光只能降帧率（换 lower-fps sensor 格式或改 VTS）。 */
        int enh_gamma = 0, enh_bf = 0;
        app_camauto_get_enh(&enh_gamma, &enh_bf);
        snprintf(msg, sizeof(msg),
                 "exp=%d/%d | gain=%d/%d | mirror=%d | sat=%d | ae=%d awb=%d luma=%d tgt=%d gamma=%d bf=%d ccm=%d face_night=%d tier=%d ae_exp_max=%dms",
                 (int)now.exp_val, (int)now.exp_max, (int)now.gain_val, (int)now.gain_max,
                 (int)app_imgpipe_get_hflip(), (int)now.sat_val,
                 (int)app_camauto_get_ae(), (int)app_camauto_get_awb(),
                 s_cam_luma, app_camauto_get_target(), enh_gamma, enh_bf,
                 s_cam_ccm, (int)s_face_night_off, s_night_tier,
                 (s_night_tier >= 1) ? 0 : s_ae_exp_cap_ms);
        send_result("none", nullptr, 0, nullptr, 0, msg);
    } else if (strcmp(cmd, "list") == 0) {
        s_lib_dirty = true;
        send_result("list", nullptr, 0, nullptr, 0, nullptr);
    } else if (strcmp(cmd, "clear") == 0) {
        if (s_db_ok) s_db->clear_all_feats();
        s_name_cnt = 0;
        names_save();
        s_lib_dirty = true;
        smooth_reset();
        s_trk_ok = false;                       /* F4：清库后旧结论作废 */
        s_evt_match_name[0] = 0;                /* F6/F7：事件去重与副脸快照作废 */
        s_evt_match_us = 0;
        s_evt_rej_us = 0;
        s_multi_cache_n = 0;
        s_reg_pending = false;                  /* 清库同时取消未完成的注册采集 */
        s_reg_shots = 0;
        send_result("deleted", nullptr, 0, nullptr, 0, "cleared");
    } else if (strcmp(cmd, "db_export") == 0) {
        db_export_run();                       /* F10：导出名表 + face.db */
    } else if (strcmp(cmd, "db_import") == 0) {
        db_import_begin(root);                 /* F10：准备接收（自动 .bak） */
    } else if (strcmp(cmd, "db_import_end") == 0) {
        db_import_end();                       /* F10：校验并覆盖恢复 */
    } else if (strcmp(cmd, "db_import_abort") == 0) {
        db_import_abort();                     /* F10：放弃本次导入 */
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

    /* F9：名表在 storage 挂载之后加载（names.json 优先，NVS 兜底 + 首次开机自动迁移） */
    names_load();

    /* 2. 模型加载（几秒；此间 imgpipe 内置 ack 仍在回 PC） */
    int64_t t0 = esp_timer_get_time();
    /* F11：检测模型可选（Kconfig APP_FACEAI_DETECT_MODEL）。
     * ⚠️ 单阶段 ESPDet 只有 idx 0，传 idx 1 会命中 assert（见 human_face_detect.cpp）。 */
#if CONFIG_APP_FACEAI_DETECT_IS_ESPDET
    s_detect = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
    s_detect->set_score_thr(s_det_thr, 0);
    /* 第二级：MNP 出框精修 + 关键点（ESPDet 本身不给 landmark，见 detect_and_report）*/
    s_mnp = new human_face_detect::MNP("human_face_detect_mnp_s8_v1.espdl",
                                       s_det_thr, human_face_detect::MNP::default_nms_thr);
    if (!s_mnp || !s_mnp->get_raw_model()) {
        ESP_LOGE(TAG, "MNP (keypoint stage) unavailable - is "
                      "CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1=y ?");
    }
#else
    s_detect = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
    s_detect->set_score_thr(s_det_thr, 0);
    s_detect->set_score_thr(s_det_thr, 1);
#endif
    float det_thr = s_det_thr;
    s_feat = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    if (err == ESP_OK) {
        s_db = new dl::recognition::DataBase(FACE_DB_PATH, s_feat->get_feat_len());
        s_db_ok = true;
        ESP_LOGI(TAG, "feature db %s: %d feat(s) in lib, name table %d", FACE_DB_PATH,
                 s_db->get_num_feats(), s_name_cnt);
    }
    ESP_LOGI(TAG, "models loaded in %d ms (detect=%s thr=%.2f match_thr=%.2f/%.2f lo/hi feat_len=%d)",
             (int)((esp_timer_get_time() - t0) / 1000), detect_model_name(), (double)det_thr,
             (double)s_thr_lo, (double)s_thr_hi, s_feat ? s_feat->get_feat_len() : 0);

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

        /* 光照双模式边沿：进入/退出夜视档时打日志并复位 AI 帧率口径
           （夜视档不产生推理帧，s_fps 不再有意义，画面帧率由 cam_fps 表达） */
        bool gated = night_face_gated();
        int64_t now = esp_timer_get_time();
        if (gated != s_face_gated) {
            s_face_gated = gated;
            s_fps = 0;
            s_fps_cnt = 0;
            s_fps_win = now;
            s_night_res_us = now;        /* 边沿后从下一个周期开始发状态帧 */
            if (gated) {
                ESP_LOGW(TAG, "face: NIGHT mode (tier %d), recognition off", s_night_tier);
                app_usb_link_log("face: night mode (tier %d) - face off, preview only",
                                 s_night_tier);
                app_status_set(APP_MOD_FACE, true, "night tier%d no face", s_night_tier);
            } else {
                ESP_LOGW(TAG, "face: DAY mode, recognition on");
                app_usb_link_log("face: day mode - face recognition on");
            }
        }

        img_frame_t fr;
        bool got = false;
        if (gated) {
            /* 夜视模式：不参与抢帧（帧池整个让给 0x05 预览）。luma/mv 等统计
               都在 camera 任务的 publish_raw 里更新，靠下面的 cam_state_tick
               轮询照常拿到，夜视阶梯/AUTO/AE 状态机不受影响。 */
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            got = (app_imgpipe_get_frame(&fr, 200) == ESP_OK);
        }
        /* 统计刷新每轮都跑（含夜视档不取帧的轮次）：判定类 tick 的输入
           必须保持新鲜，否则夜视档下亮度回升也检测不到、永远出不了夜视档 */
        cam_state_tick();
        /* exp_max 开机是 0（只有 cam 命令/切档才更新）：先从驱动播种一次真实上限，
           否则白天曝光阶梯在第一个客户端连上之前永远不会工作 */
        if (s_cam_exp_max <= 0) {
            app_cam_ctrl_t t = {};
            if (app_camera_get_ctrl(&t) == ESP_OK && t.exp_max > 0) {
                s_cam_exp_max = (int)t.exp_max;
                s_cam_gain_max = (int)t.gain_max;
            }
        }
        /* 判定类 tick 每轮都跑（含等新帧超时的轮次）：低帧率（夜视档 1~2fps）下
           统计值一帧才更新一次，若只在"拿不到帧就 continue"之后跑，决策会被
           白白多拖一个帧周期（实测升档因此多花约 1 秒） */
        auto_fps_tick();                     /* 运动自适应帧率（AUTO 开启时判定运动） */
        night_tick();                        /* 夜视阶梯：暗度降帧率 + 自动时域降噪 */
        day_expo_tick();                     /* 白天曝光余量阶梯（50->20->10，仅 tier0+AE） */
        auto_boost_maybe();                  /* 降档后延迟 600ms 的 exp/gain 拉满 */
        /* AE/AWB/夜视增强自动引擎：写操作走 camera 任务信箱，这里只做低频判定。
         * 曝光上限（2026-10-06）：只在人脸路径生效——夜视档（tier>=1）传 0 解除
         * （夜视要亮度，且那时人脸识别已门控关闭）。行数换算 1ms = 50 行。 */
        app_camauto_tick(s_cam_fps, s_cam_gain, s_cam_exp_max, s_cam_gain_max,
                         (s_night_tier >= 1) ? 0 : s_ae_exp_cap_ms * 50);
        if (!got) {
            /* 夜视模式：2Hz 低频状态帧，让 UI/PC 知道当前模式与参数
               （faces 恒 0、无检测框；识别结果停发，画面帧率看 cam_fps） */
            if (gated && s_running &&
                esp_timer_get_time() - s_night_res_us >= 500 * 1000) {
                s_night_res_us = esp_timer_get_time();
                char m[48];
                snprintf(m, sizeof(m), "face off, preview only (tier %d)", s_night_tier);
                send_result("night", nullptr, 0, nullptr, 0, m);
            }
            continue;                       /* 本轮没有新帧 */
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
    /* F9：名表加载已移到 faceai_task 的 storage 挂载之后（names_load）——
       否则 /storage/names.json 挂载前读不到，会误判为"空名表"并从 NVS 覆盖。 */

    /* AE/AWB 自动引擎（imgpipe 已初始化，可以开统计采样） */
    app_camauto_init();

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    ESP_RETURN_ON_FALSE(s_cmd_q, ESP_ERR_NO_MEM, TAG, "cmd queue create");

    app_usb_link_set_cmd_cb(cmd_cb);
    app_usb_link_set_blob_cb(db_blob_cb);    /* F10：0x06 块传输（库备份/恢复）*/

    /* 钉 CPU0（与 imgpipe 同核，camera 钉 CPU1）：跨核缓存一致性（HANDOVER 廿一节） */
    if (xTaskCreatePinnedToCore(faceai_task, "faceai", TASK_STACK, nullptr, TASK_PRIO,
                                nullptr, 0) != pdPASS) {
        return ESP_FAIL;
    }
    app_status_set(APP_MOD_FACE, true, "starting");
    ESP_LOGI(TAG, "faceai starting: detect=%s thr=%.2f match_thr=%.2f/%.2f lo/hi, db=%s",
             detect_model_name(), (double)s_det_thr, (double)s_thr_lo, (double)s_thr_hi,
             FACE_DB_PATH);
    return ESP_OK;
}

extern "C" bool app_faceai_ready(void)
{
    return s_ready;
}
