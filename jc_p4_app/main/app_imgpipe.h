#pragma once
/*
 * app_imgpipe —— 图像管道（任务四）
 *
 *   PC(USB CDC, JCLINK 0x01 JPEG) --[frame_source]--> JPEG 硬解(esp_driver_jpeg)
 *        --> RGB888 帧池(latest-frame 双缓冲) --> sink(app_faceai) 或内置 0x02 ack
 *
 * 设计要点：
 *   - 图像源与消费者解耦：源实现 frame_source_t（当前 USB/JCLINK；SC2336 到货后新增
 *     V4L2 源实现同一接口，app_faceai/UI 代码不动）。esp_video 侧可用硬件 JPEG 编码
 *     输出，因此源统一交付"压缩帧"。
 *   - 帧池采用最新帧语义（无队列积压）：消费者用 app_imgpipe_get_frame() 取最新一帧，
 *     处理完 app_imgpipe_release()。解码任务只写无人持有的槽，绝不与消费者竞态。
 *   - 未注册 sink（app_faceai 尚未启用）时，管道自动回一条 0x02 JSON，保证 PC 端
 *     窗口=1 流控不会卡死（任务六联调用）。
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JPEG 解码输出 RGB 元素序：若发现 R/B 反了，改成 JPEG_DEC_RGB_ELEMENT_ORDER_BGR */
#define APP_IMG_JPEG_RGB_ORDER   JPEG_DEC_RGB_ELEMENT_ORDER_RGB

/* ---------------- 帧与图像源抽象 ---------------- */

typedef enum {
    IMG_FMT_JPEG = 0,   /* 压缩帧（源输出，管道负责硬解） */
    IMG_FMT_RGB888,     /* 3 B/px，内存序 R,G,B */
    IMG_FMT_RGB565,     /* 2 B/px，小端（预留：LVGL 预览） */
} img_fmt_t;

/* 一帧图像。data 指向 PSRAM，所有权见各 API 注释 */
typedef struct {
    uint8_t *data;
    int      width;
    int      height;
    int      stride;      /* 行字节数，可能 > width*bpp（JPEG 按 MCU 对齐） */
    img_fmt_t fmt;
    uint32_t seq;         /* 源侧帧序号 */
    uint32_t dec_ms;      /* 本帧解码耗时（源为压缩帧时有效） */
    int64_t  ts_us;       /* 发布时刻(esp_timer us)，用于判断命令与帧的先后 */
    uint32_t crc32;       /* 生产者对 stride*height 全缓冲的 CRC32（缓存一致性诊断） */
    int      dec_core;    /* 生产者写入本帧时所在核（跨核缓存诊断） */
} img_frame_t;

/* 源产出的编码帧。data 归源所有，acquire 后到 release 前保持有效 */
typedef struct {
    const uint8_t *data;
    uint32_t       len;
    img_fmt_t      fmt;
    uint32_t       seq;
} src_frame_t;

typedef struct frame_source frame_source_t;
struct frame_source {
    const char *name;
    esp_err_t (*start)(frame_source_t *self);
    /* 取一帧：ESP_OK / ESP_ERR_TIMEOUT（超时无帧） */
    esp_err_t (*acquire)(frame_source_t *self, src_frame_t *out, uint32_t timeout_ms);
    void      (*release)(frame_source_t *self, const src_frame_t *f);
    void       *priv;
};

/* 内置 USB(JCLINK) 源单例 */
frame_source_t *app_imgpipe_usb_source(void);

/* ---------------- 图像源模式：PC 送帧 还是 板载摄像头 ---------------- */

typedef enum {
    APP_IMG_SRC_PC = 0,   /* PC 经 JCLINK 0x01 发 JPEG，管道硬解 */
    APP_IMG_SRC_CAM,      /* 板载 SC2336：camera 任务直接 app_imgpipe_publish_raw() */
} app_img_src_t;

void          app_imgpipe_select_src(app_img_src_t src);
app_img_src_t app_imgpipe_get_src(void);
const char   *app_imgpipe_src_str(app_img_src_t src);

/*
 * 摄像头等"已出图"的源把帧直接推进帧池（拷进槽，一帧只交付一次，sink 照常被回调）。
 * 注意：当前不是 CAM 模式时返回 ESP_ERR_INVALID_STATE（帧被丢弃），调用者照常 QBUF 即可。
 * PC 模式下一旦 PC 发来 JPEG，管道会自动切回 PC 源。
 */
esp_err_t app_imgpipe_publish_raw(const uint8_t *data, int w, int h, int stride, img_fmt_t fmt);

/* ---------------- 管道 ---------------- */

#define APP_IMG_WAIT_FOREVER  0xFFFFFFFFu

esp_err_t app_imgpipe_init(void);

frame_source_t *app_imgpipe_source(void);
/* 运行时切换图像源（SC2336 到货后使用），传 NULL 恢复 USB 源 */
void app_imgpipe_set_source(frame_source_t *src);

/*
 * 取最新解码帧（RGB888）。ESP_OK 时 *out 有效，用毕必须 app_imgpipe_release()。
 * 每一帧只会被取到一次（下帧到来前再取返回 ESP_ERR_NOT_FOUND）；release 仅归还缓冲，
 * 不会让同一帧被重复取到。
 */
esp_err_t app_imgpipe_get_frame(img_frame_t *out, uint32_t timeout_ms);
void      app_imgpipe_release(const img_frame_t *frame);

/*
 * 摄像头画面水平镜像（模组安装方向导致左右反）。驱动层改不了：esp_video 的
 * sensor ext-ctrl 转发是死代码、ISP 的 0x00980914/15 只可枚举不可读写，
 * 所以在管道里对**摄像头源**的帧做行内镜像 —— AI 检测与回传显示看到的是同一份
 * 画面，检测框坐标不会错位。
 */
void app_imgpipe_set_hflip(bool on);
bool app_imgpipe_get_hflip(void);

/*
 * 软件白平衡（修"偏绿"）：驱动侧 AWB 不工作（esp_ipa 不给增益、external WB control
 * 三条路径全 FAIL），所以在管道里对**摄像头帧**直接做 R/B 增益。
 * 千分比：1000 = 不校正。偏绿时把 kr/kb 提到 ~1300/1600（用 tools/rgb_stats.py 看
 * 对着白纸的 G/R、G/B 各是多少，就填多少）。
 * ⚠️ 场景本身是绿/蓝物体时不要硬拉成灰 —— 请对着白纸标定。
 */
void app_imgpipe_set_wb_gain(int kr, int kb);
/* 同 set_wb_gain 但不打日志（自动白平衡引擎高频调用时避免刷屏） */
void app_imgpipe_set_wb_gain_quiet(int kr, int kb);
void app_imgpipe_get_wb_gain(int *kr, int *kb);

/*
 * AE/AWB 统计采样（app_camauto 自动曝光/白平衡引擎用）：
 * 开启后 publish_raw 在镜像/降噪/白平衡**之前**对摄像头帧做稀疏网格采样
 * （每 8 像素 x 每 8 行），把亮度与 R/G/B 均值写进 stats 的 ae_* 字段。
 * 必须在 WB 之前采，AWB 才能量到"校正前"的通道比例；采样点稀疏，成本可忽略。
 */
void app_imgpipe_set_ae_stats(bool on);

/*
 * 时域降噪（弱光降噪；仅摄像头 raw 路径，PC 送帧时序不连续没有意义）：
 *   0 = 关（零额外开销）、1 = 低、2 = 中、3 = 高。
 * 对"没动的地方"做多帧加权平均压制随机噪声（理论上按 √N 下降），运动区域自动减弱或
 * 跳过以避免拖影；降噪在镜像之前做，prev 保存未镜像内容，所以 AI 检测与 0x05 回传
 * 看到的是同一份降噪后的画面。
 * 高帧率（> CONFIG_APP_CAM_DN_MAX_FPS）会自动跳过降噪以保证链路稳定（宁可不要效果）。
 */
void app_imgpipe_set_dn(int level);
int  app_imgpipe_get_dn(void);

/* 丢弃历史帧（回到"首帧"语义）：换图像源、改曝光/增益/帧率后调用，避免亮度突变留下残影。
 * 同时复位降噪与运动估计两套参考缓冲。 */
void app_imgpipe_reset_prev(void);

/*
 * 运动估计（自动帧率模式用；与降噪解耦，任何帧率下都能工作）：
 * 开启后按需分配独立的参考帧缓冲（PSRAM，约 600KB），每帧无条件刷新、只比较相邻两帧。
 * 结果通过 app_imgpipe_get_stats() 的 mv_peak（峰值 0..63，判据用）/ mv_ratio（运动块占比 %）读取。
 */
void app_imgpipe_set_motion_detect(bool on);
/* 块运动阈值（缺省 CONFIG_APP_CAM_MOTION_THR）；调小更灵敏、调大更迟钝 */
void app_imgpipe_set_motion_thr(int thr);

/*
 * 发布端按需发布（2026-10-06 待办 5）：开启后 camera 任务在"没有消费者正在
 * app_imgpipe_get_frame 里等帧、且距上次发布不足 APP_CAM_PUB_MIN_MS"时直接跳过
 * 该帧（不做拷贝/AE/降噪/白平衡，源头丢弃并计入 stats.pub_skip）——省下 CPU1 上
 * 为没人消费的帧白烧的 copy+post（实测每帧 ~17ms、50fps 下约 87% 占空比）。
 * 强制下限保证 AE/运动估计/降噪的输入不冻结。默认 CONFIG_APP_CAM_PUB_ON_DEMAND。
 */
void app_imgpipe_set_pub_demand(bool on);
bool app_imgpipe_get_pub_demand(void);

/* 消费者回调（app_faceai 注册）；注册后管道不再自动回 0x02，由 sink 自己回 */
typedef void (*app_imgpipe_sink_t)(const img_frame_t *frame);
void app_imgpipe_set_sink(app_imgpipe_sink_t cb);

/*
 * 轮询式消费者（app_faceai 用 get_frame 取帧）用这个关掉内置 0x02 ack，
 * 否则 PC 会同时收到两条结果帧。关掉后由消费者负责回，默认 true。
 */
void app_imgpipe_set_auto_ack(bool enable);

/* ---------------- 工具 ---------------- */

/* 分配/释放独立 RGB888 缓冲（PSRAM，64 字节对齐，可直接喂 PPA/模型） */
esp_err_t app_imgpipe_alloc_rgb888(img_frame_t *frame, int w, int h);
void      app_imgpipe_free_frame(img_frame_t *frame);

/* 双线性缩放 RGB888；dst 需已按 width/height 分配（可用 alloc_rgb888） */
esp_err_t app_imgpipe_resize_rgb888(const img_frame_t *src, img_frame_t *dst);

/* ---------------- 统计 ---------------- */

typedef struct {
    uint32_t rx_frames;    /* 收到的源帧数 */
    uint32_t dec_frames;   /* 解码成功帧数 */
    uint32_t dropped;      /* 解码失败 / 无空槽被丢弃 */
    uint32_t pub_skip;     /* 按需发布跳过的相机帧数（源头直接丢，没做拷贝/后处理） */
    uint32_t jpeg_bytes;   /* 最近一帧压缩字节数 */
    uint32_t dec_ms;       /* 最近一帧解码耗时 */
    uint32_t dn_ms;        /* 最近一帧时域降噪耗时（0 = 未跑） */
    uint32_t dn_skip;      /* 因高帧率守卫被跳过的降噪帧数 */
    uint32_t dn_blocks;    /* 最近一帧实际参与混合的块数（调参用） */
    uint32_t dn_sad;       /* 全帧「块内差均值」的平均（旧口径，仅为对比历史标定保留） */
    uint32_t dn_mmax;      /* 全帧「块内最大差」的平均 0..63（当前运动判据，调参用） */
    uint32_t dn_motion;    /* 运动块占比 0..100%（权重被减弱或完全跳过的块） */
    /* 运动估计（自动帧率用，与降噪解耦）*/
    uint32_t mv_peak;      /* 全帧「块内最大差」的峰值 0..63（判据用它，比均值灵敏）*/
    uint32_t mv_ratio;     /* 运动块占比 0..100% */
    uint32_t mv_ms;        /* 最近一帧运动估计耗时 */
    int64_t  mv_stats_us;  /* 最近一次运动统计的时刻（esp_timer us，0=从未）。判"窗口内
                              有没有拿到新样本"必须靠它：低帧率下拿陈旧值判定会把
                              50fps 误掉回静止档，来回振荡（2026-10-02 实测踩过）*/
    uint32_t mv_ready;     /* 本样本是否来自真实比较（0 = 刚重建基准帧，mv 恒为 0，
                              不可当作"静止"结论）*/
    /* AE/AWB 统计（app_imgpipe_set_ae_stats(true) 时采样，WB/镜像之前取值）*/
    uint32_t ae_luma;      /* 平均亮度 0..255（Rec.601 加权） */
    uint32_t ae_r;         /* R 通道均值 0..255 */
    uint32_t ae_g;         /* G 通道均值 0..255 */
    uint32_t ae_b;         /* B 通道均值 0..255 */
    int64_t  ae_stats_us;  /* 最近一次采样的时刻（esp_timer us，0=从未）；陈旧则 AE 跳过 */
    float    fps;          /* 最近 1s 解码帧率 */
    int      width;        /* 最近一帧尺寸 */
    int      height;
} app_imgpipe_stats_t;

void app_imgpipe_get_stats(app_imgpipe_stats_t *out);

#ifdef __cplusplus
}
#endif
