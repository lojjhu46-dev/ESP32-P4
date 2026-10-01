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
void app_imgpipe_get_wb_gain(int *kr, int *kb);

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

/* 丢弃历史帧（回到"首帧"语义）：换图像源、改曝光/增益/帧率后调用，避免亮度突变留下残影 */
void app_imgpipe_reset_prev(void);

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
    uint32_t jpeg_bytes;   /* 最近一帧压缩字节数 */
    uint32_t dec_ms;       /* 最近一帧解码耗时 */
    uint32_t dn_ms;        /* 最近一帧时域降噪耗时（0 = 未跑） */
    uint32_t dn_skip;      /* 因高帧率守卫被跳过的降噪帧数 */
    uint32_t dn_blocks;    /* 最近一帧实际参与混合的块数（调参用） */
    uint32_t dn_sad;       /* 最近一帧全帧平均 SAD（G 通道采样均值，调参用） */
    float    fps;          /* 最近 1s 解码帧率 */
    int      width;        /* 最近一帧尺寸 */
    int      height;
} app_imgpipe_stats_t;

void app_imgpipe_get_stats(app_imgpipe_stats_t *out);

#ifdef __cplusplus
}
#endif
