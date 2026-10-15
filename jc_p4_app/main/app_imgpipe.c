/*
 * app_imgpipe.c —— 图像管道（任务四）
 *
 * USB(JCLINK 0x01 JPEG) -> esp_driver_jpeg 硬解 -> RGB888 帧池 -> app_faceai / ack JSON
 */
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_rom_crc.h"
#include "driver/jpeg_decode.h"

#include "app_imgpipe.h"
#include "sdkconfig.h"
#include "app_usb_link.h"
#include "app_camera.h"
#include "app_status.h"

#define TAG "imgpipe"

#define PIPE_RX_BUF        (256 * 1024)   /* JPEG 接收缓冲（PSRAM） */
/* 帧池槽数（Kconfig，默认 4）。⚠️ 不要改回 2：`pipe_take_slot()` 要求槽既不是
 * latest 又不能 busy，而 AI 拿到一帧要占槽 ~190ms —— 2 槽时相机在这期间无法发布，
 * 池发布率被锁死在 ≈AI 消费率（实测 5.3fps，相机有 20fps）；0x05 预览流再进来和
 * AI 平分这 5.3 帧 → 两边都掉到 ~2.6fps（2026-10-06 实测，见 HANDOVER 第廿一节）。*/
#ifndef CONFIG_APP_CAM_PIPE_SLOTS
#define CONFIG_APP_CAM_PIPE_SLOTS 4
#endif
#define PIPE_SLOTS         CONFIG_APP_CAM_PIPE_SLOTS
/* 发布端按需发布（Kconfig APP_CAM_PUB_ON_DEMAND / APP_CAM_PUB_MIN_MS）：
 * bool=n 时 sdkconfig.h 不生成宏，兜底为 0（关闭）——改 Kconfig 后必须 reconfigure */
#ifndef CONFIG_APP_CAM_PUB_ON_DEMAND
#define CONFIG_APP_CAM_PUB_ON_DEMAND 0
#endif
#ifndef CONFIG_APP_CAM_PUB_MIN_MS
#define CONFIG_APP_CAM_PUB_MIN_MS 100
#endif
#define PIPE_TASK_STACK    4096
#define PIPE_TASK_PRIO     5
#define PIPE_ACQ_TIMEOUT   200            /* 源取帧超时 ms */
#define PIPE_MAX_DEC_W     1920
#define PIPE_MAX_DEC_H     1088

static void pipe_report(void);   /* 前向声明（pipe_publish 里用） */

typedef struct {
    img_frame_t   frame;      /* 发布给消费者的描述 */
    size_t        cap;        /* frame.data 容量 */
    int           pad_w;      /* 解码对齐后的宽（决定 stride） */
    int           pad_h;
    volatile bool busy;       /* 消费者持有 */
    volatile bool consumed;   /* 已被取走过（每帧只交付一次） */
} pipe_slot_t;

static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t evt;      /* 新帧事件 */
    pipe_slot_t       slot[PIPE_SLOTS];
    volatile int      latest;   /* 最新帧槽号，-1 = 无 */
    jpeg_decoder_handle_t dec;
    uint8_t          *rx_buf;
    frame_source_t   *src;
    app_img_src_t     src_mode;   /* PC(拉 JPEG) / CAM(摄像头推 raw) */
    bool              hflip;      /* 摄像头帧左右镜像 */
    int               kr, kb;     /* 软件白平衡：R/B 增益千分比（1000 = 不校正） */
    /* 时域降噪（仅摄像头 raw 路径）*/
    int               dn;         /* 档位 0=关 1=低 2=中 3=高 */
    uint8_t          *prev;       /* 上一帧（未镜像、未 WB 的混合结果），PSRAM */
    size_t            prev_cap;
    int               prev_stride, prev_h;
    bool              has_prev;   /* prev 是否可用（首帧/失效后为 false） */
                                  /* 降噪耗时统计见 s_pipe.st.dn_ms */
    /* 运动估计（自动帧率用，与降噪解耦）*/
    bool              mv_on;      /* 开：每帧估计运动（自动模式）*/
    int               mv_thr;     /* 块运动阈值（G 通道差）*/
    uint8_t          *mv_prev;    /* 上一帧原始内容（每帧无条件刷新）*/
    size_t            mv_prev_cap;
    int               mv_prev_stride, mv_prev_h;
    bool              mv_has_prev;
    bool              ae_stats_on;  /* AE/AWB 统计采样开关（app_camauto 用）*/
    /* 发布端按需发布：没有消费者等帧且未到强制间隔时，相机帧整条链路跳过 */
    bool              pub_demand;
    int64_t           last_pub_us;  /* 上次真实发布的时刻（强制下限计时用）*/
    volatile int      waiters;      /* 正在 get_frame 里等帧的消费者数（启发式，无需加锁）*/
    app_imgpipe_sink_t sink;
    bool              auto_ack;
    bool              ready;
    app_imgpipe_stats_t st;
    int64_t           fps_win;
    uint32_t          fps_cnt;
} s_pipe;

/* ---- 分段性能计数（诊断，每秒清零）----------------------------------------
 * 目的：找出"相机→帧池"每帧约 59ms 固定开销花在哪（2026-10-06 实测：cam50 时
 * AI 结果帧率 17/s，而检测只需 18ms → 每帧 ~59ms 开销压在管线里）。
 * 发布侧跑在 camera 任务（CPU1），消费侧失效跑在消费者核（CPU0）。          */
static struct {
    uint32_t copy_us;    /* 槽内 memcpy（= frame.dec_ms，这里累计便于算均值）*/
    uint32_t ae_us;      /* AE/AWB 统计全帧扫描 */
    uint32_t dn_us;      /* 时域降噪 */
    uint32_t post_us;    /* 白平衡 + 行内镜像（一次遍历 307200 像素）*/
    uint32_t crc_us;     /* 逐帧 CRC32 全帧扫描（诊断字段用，非必需）*/
    uint32_t msync_us;   /* 发布侧 C2M 回写（614KB）*/
    uint32_t inv_us;     /* 消费侧 M2C 失效累计 us */
    uint32_t inv_max;    /* 单次 M2C 失效最大 us */
    uint32_t inv_n;      /* 失效次数 */
    uint64_t inv_bytes;  /* 失效字节总量（算等效带宽）*/
    uint64_t lat_us;     /* "发布 → 被消费者取走"排队延迟累计 us */
    uint32_t lat_max_ms; /* 最大排队延迟 ms */
    uint32_t lat_n;
    uint32_t skip_n;     /* 按需发布跳过的帧数（本统计窗口） */
} s_prof;

/* ---------------- USB (JCLINK) 图像源 ---------------- */

static esp_err_t usb_src_start(frame_source_t *self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t usb_src_acquire(frame_source_t *self, src_frame_t *out, uint32_t timeout_ms)
{
    (void)self;
    if (!out) return ESP_ERR_INVALID_ARG;
    int64_t deadline = (timeout_ms == APP_IMG_WAIT_FOREVER)
                       ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        uint8_t type = 0;
        int n = app_usb_link_recv(&type, s_pipe.rx_buf, PIPE_RX_BUF);
        if (n > 0) {
            out->data = s_pipe.rx_buf;
            out->len = (uint32_t)n;
            out->fmt = IMG_FMT_JPEG;
            out->seq = s_pipe.st.rx_frames + 1;
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);              /* 1 tick（HZ=100 -> 10ms），不能用 pdMS_TO_TICKS(2)=0 */
    }
}

/* recv() 已把 payload 拷进 rx_buf，源侧无需引用计数 */
static void usb_src_release(frame_source_t *self, const src_frame_t *f)
{
    (void)self;
    (void)f;
}

static frame_source_t s_usb_src = {
    .name = "usb-jclink",
    .start = usb_src_start,
    .acquire = usb_src_acquire,
    .release = usb_src_release,
    .priv = NULL,
};

frame_source_t *app_imgpipe_usb_source(void)
{
    return &s_usb_src;
}

/* ---------------- 帧池 ---------------- */

static int pipe_take_slot(void)
{
    int slot = -1;
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    for (int i = 0; i < PIPE_SLOTS; i++) {
        if (i != s_pipe.latest && !s_pipe.slot[i].busy) {
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_pipe.lock);
    return slot;
}

/* MCU 对齐尺寸：JPEG 解码输出按采样方式对齐（4:2:0 -> 16x16, 4:2:2 -> 16x8, 4:4:4/灰度 -> 8x8） */
static void pipe_mcu_size(jpeg_down_sampling_type_t m, int *mcux, int *mcuy)
{
    switch (m) {
    case JPEG_DOWN_SAMPLING_YUV420:
        *mcux = 16; *mcuy = 16;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        *mcux = 16; *mcuy = 8;
        break;
    default:            /* YUV444 / GRAY */
        *mcux = 8;  *mcuy = 8;
        break;
    }
}

/* 硬解 JPEG 到指定槽（只写无人持有的槽） */
static esp_err_t pipe_decode_into(int idx, const uint8_t *jpeg, uint32_t len)
{
    jpeg_decode_picture_info_t info = { 0 };
    esp_err_t err = jpeg_decoder_get_info(jpeg, len, &info);
    ESP_RETURN_ON_ERROR(err, TAG, "jpeg header parse");
    if (info.width == 0 || info.height == 0 ||
        info.width > PIPE_MAX_DEC_W || info.height > PIPE_MAX_DEC_H) {
        ESP_LOGE(TAG, "unsupported picture %"PRIu32"x%"PRIu32, info.width, info.height);
        return ESP_ERR_INVALID_SIZE;
    }

    int mcux = 8, mcuy = 8;
    pipe_mcu_size(info.sample_method, &mcux, &mcuy);
    int pad_w = (int)((info.width + mcux - 1) / mcux * mcux);
    int pad_h = (int)((info.height + mcuy - 1) / mcuy * mcuy);
    size_t need = (size_t)pad_w * pad_h * 3;

    pipe_slot_t *sl = &s_pipe.slot[idx];
    if (sl->frame.data && (sl->cap < need || sl->pad_w != pad_w || sl->pad_h != pad_h)) {
        heap_caps_free(sl->frame.data);
        sl->frame.data = NULL;
        sl->cap = 0;
    }
    if (!sl->frame.data) {
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        void *p = jpeg_alloc_decoder_mem(need, &mem_cfg, &got);
        ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "dec buf %u B", (unsigned)need);
        sl->frame.data = p;
        sl->cap = got;
        sl->pad_w = pad_w;
        sl->pad_h = pad_h;
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = APP_IMG_JPEG_RGB_ORDER,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    int64_t t0 = esp_timer_get_time();
    uint32_t out_size = 0;
    err = jpeg_decoder_process(s_pipe.dec, &cfg, jpeg, len, sl->frame.data, sl->cap, &out_size);
    ESP_RETURN_ON_ERROR(err, TAG, "jpeg decode %"PRIu32" B", len);

    sl->frame.width = (int)info.width;
    sl->frame.height = (int)info.height;
    sl->frame.stride = pad_w * 3;          /* 解码输出行距 = MCU 对齐宽 * 3 */
    sl->frame.fmt = IMG_FMT_RGB888;
    sl->frame.dec_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    sl->frame.seq = s_pipe.st.rx_frames;
    /* 诊断：生产者侧像素指纹 + 所在核。CRC 读取发生在驱动 invalidate 同核，
     * 反映 DMA 真实输出；消费者读到的内容是否一致由 ai_crc 对照判读 */
    sl->frame.crc32 = esp_rom_crc32_le(0, sl->frame.data,
                                       (size_t)sl->frame.stride * sl->frame.height);
    sl->frame.dec_core = (int)xPortGetCoreID();
    return ESP_OK;
}

/*
 * 摄像头等 raw 帧：拷进槽（palette 与 JPEG 路径共用 pad_w/pad_h 作为
 * "缓冲失效判断键"：raw 路径存 stride/height，JPEG 路径存 MCU 对齐宽高）
 */
/* ---------------- 时域降噪（Temporal Denoise，仅摄像头 raw 路径） ----------------
 *
 * 弱光噪声以散粒噪声为主（实测：只降增益几乎不改善信噪比），而相邻帧的随机噪声
 * 互不相关、场景内容高度相关 —— 对"没动的地方"做多帧加权平均，噪声按 √N 下降。
 *
 * 做法（性能红线：camera 任务 prio 6，逐像素乘除会触发 task_wdt，历史踩过）：
 *   - 8x8 分块（2026-10-01 由 16x16 改小：块太大时局部小运动会被块内均值稀释掉）
 *   - 每块取 4x4=16 个采样点，用**块内最大差**（G 通道 6bit，0..63）判运动 ——
 *     用最大差而不是均值：块里只要有一小片在动就必须判为运动，否则会留半透明拖影
 *   - 静止块强混合、运动块减弱或**完全跳过**；跳过的块把当前帧刷进 prev，
 *     否则旧影像会"冻"在历史里、等运动停下再混回来形成闪回
 *   - 像素混合用整数移位：out = cur + ((prev - cur) * wq) >> 8
 *     （ESP32 硬件乘法 1 周期；全程无除法/浮点），混合结果同时写回 prev（IIR，
 *     等效多帧平均，省掉一次 600KB memcpy）
 *   - dn=0 时完全不进这里（零开销）；高帧率由守卫跳过（CONFIG_APP_CAM_DN_MAX_FPS）
 *
 * ⚠️ 必须在镜像之前调用：prev 始终保存"未镜像"内容，否则 cur/prev 左右错位会出鬼影。
 */

#define DN_BLOCK     8       /* 块边长（越小小运动越不容易被漏判） */
#define DN_SAD_STEP  2       /* 采样步长（8x8 块取 4x4=16 点） */

/* 档位参数：wq = 静止块混合权重（0..256，越大越依赖历史）；mmax_lo/hi = 运动门限
 * （块内**最大** G 通道差：<=lo 全混、>=hi 不混并跳过，中间线性衰减） */
static const struct {
    int wq;
    int mmax_lo;
    int mmax_hi;
} s_dn_tab[4] = {
    {   0,   0,   0 },       /* 0 = 关（不会走到这里） */
    { 128,  14,  28 },       /* 1 = 低：静止块混 50% */
    { 192,  16,  32 },       /* 2 = 中：混 75% */
    { 236,  18,  38 },       /* 3 = 高：混 92%（留 8% 新帧，避免画面"冻住"） */
};

/* 按当前尺寸准备 prev 缓冲（沿用槽的 cap/stride/h 失效键思路） */
static bool pipe_dn_ensure(int stride, int h)
{
    size_t need = (size_t)stride * h;
    if (s_pipe.prev && s_pipe.prev_cap >= need &&
        s_pipe.prev_stride == stride && s_pipe.prev_h == h) {
        return true;
    }
    if (s_pipe.prev) {
        heap_caps_free(s_pipe.prev);
        s_pipe.prev = NULL;
        s_pipe.prev_cap = 0;
        s_pipe.has_prev = false;
    }
    uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "denoise prev buf %u B alloc failed", (unsigned)need);
        return false;
    }
    s_pipe.prev = p;
    s_pipe.prev_cap = need;
    s_pipe.prev_stride = stride;
    s_pipe.prev_h = h;
    return true;
}

static void pipe_dn_apply(uint8_t *data, int w, int h, int stride)
{
    if (!pipe_dn_ensure(stride, h)) {
        return;
    }
    if (!s_pipe.has_prev) {
        memcpy(s_pipe.prev, data, (size_t)stride * h);
        s_pipe.has_prev = true;
        return;                                   /* 首帧：只建立历史，不混合 */
    }

    const int wq0 = s_dn_tab[s_pipe.dn].wq;
    const int mmax_lo = s_dn_tab[s_pipe.dn].mmax_lo;
    const int mmax_hi = s_dn_tab[s_pipe.dn].mmax_hi;
    uint32_t nblk = 0, nmv = 0, nsum_sad = 0, nsum_max = 0, ntot = 0;

    for (int y0 = 0; y0 < h; y0 += DN_BLOCK) {
        int bh = (h - y0 < DN_BLOCK) ? (h - y0) : DN_BLOCK;
        for (int x0 = 0; x0 < w; x0 += DN_BLOCK) {
            int bw = (w - x0 < DN_BLOCK) ? (w - x0) : DN_BLOCK;

            /* 块内运动判定：采样点的 G 通道差，同时取均值（旧口径）和最大值（新判据）*/
            int sum = 0, mmax = 0, nsmp = 0;
            for (int by = 0; by < bh; by += DN_SAD_STEP) {
                const uint16_t *pc = (const uint16_t *)(data + (size_t)(y0 + by) * stride) + x0;
                const uint16_t *pp = (const uint16_t *)(s_pipe.prev + (size_t)(y0 + by) * stride) + x0;
                for (int bx = 0; bx < bw; bx += DN_SAD_STEP) {
                    int d = (int)((pc[bx] >> 5) & 0x3F) - (int)((pp[bx] >> 5) & 0x3F);
                    if (d < 0) d = -d;
                    sum += d;
                    if (d > mmax) mmax = d;
                    nsmp++;
                }
            }
            if (nsmp == 0) {
                continue;                         /* 尺寸退化导致的空块（理论上不会）*/
            }
            ntot++;
            nsum_sad += (uint32_t)(sum / nsmp);
            nsum_max += (uint32_t)mmax;

            int wq = wq0;
            if (mmax >= mmax_hi) {
                wq = 0;                           /* 运动块：完全不混，整块保留当前帧 */
            } else if (mmax > mmax_lo) {
                wq = wq0 * (mmax_hi - mmax) / (mmax_hi - mmax_lo);   /* 块级除法，每块一次 */
            }

            if (wq <= 0) {
                /* 运动块：不混合，但必须把当前帧刷进 prev —— 否则残影会"冻"在历史里，
                   等物体停下（差值落回阈值以下）再被混回画面，就成了"画面滞留" */
                nmv++;
                for (int y = 0; y < bh; y++) {
                    uint16_t *pc = (uint16_t *)(data + (size_t)(y0 + y) * stride) + x0;
                    uint16_t *pp = (uint16_t *)(s_pipe.prev + (size_t)(y0 + y) * stride) + x0;
                    memcpy(pp, pc, (size_t)bw * 2);
                }
                continue;
            }
            if (wq < wq0) {
                nmv++;                            /* 权重被减弱的块也算"运动"（UI 显示用）*/
            }
            nblk++;

            /* 像素混合：**按 R/G/B 分量分别**做整数插值
               （⚠️ 不能对 RGB565 整包插值：位打包会跨分量借位，实测噪声反涨 2.6 倍）*/
            for (int y = 0; y < bh; y++) {
                uint16_t *pc = (uint16_t *)(data + (size_t)(y0 + y) * stride) + x0;
                uint16_t *pp = (uint16_t *)(s_pipe.prev + (size_t)(y0 + y) * stride) + x0;
                for (int x = 0; x < bw; x++) {
                    int c = pc[x];
                    int p = pp[x];
                    int r = (c >> 11) & 0x1F;
                    int g = (c >> 5) & 0x3F;
                    int b = c & 0x1F;
                    r += ((((p >> 11) & 0x1F) - r) * wq) >> 8;
                    g += ((((p >> 5) & 0x3F) - g) * wq) >> 8;
                    b += (((p & 0x1F) - b) * wq) >> 8;
                    int o = (r << 11) | (g << 5) | b;
                    pc[x] = (uint16_t)o;
                    pp[x] = (uint16_t)o;          /* prev 存混合结果（IIR） */
                }
            }
        }
    }

    s_pipe.st.dn_blocks = nblk;                   /* 实际混合块数 */
    s_pipe.st.dn_sad = ntot ? (nsum_sad / ntot) : 0;      /* 差均值口径（旧，对比历史用）*/
    s_pipe.st.dn_mmax = ntot ? (nsum_max / ntot) : 0;     /* 最大差口径（当前判据）*/
    s_pipe.st.dn_motion = ntot ? (nmv * 100 / ntot) : 0;  /* 运动块占比 %（UI 显示）*/
}

/* ---------------- 运动估计（自动帧率用，与降噪解耦） ----------------
 *
 * 与降噪的区别：
 *   - 降噪是 IIR：混合结果写回 prev，要靠多帧历史才能降噪，且高帧率会被守卫整体跳过
 *   - 运动估计只比"相邻两帧"：每帧**无条件**把当前帧刷进参考缓冲，因此不依赖降噪开关，
 *     在 50fps（降噪被跳过）时照样能工作
 *
 * 输出两个指标（写进 s_pipe.st，供自动帧率状态机判定）：
 *   mv_peak  = 全帧"块内最大差"的**最大值**（0..63）——
 *              用峰值而不是平均值：少数运动块的大差值不会被几千个静止块稀释，
 *              这正是之前 mv 判据"运动时也恒为 0"的灵敏度根源
 *   mv_ratio = 块内最大差 >= mv_thr 的块占比（%）
 *
 * 成本：4800 块 x 16 采样 + 每帧 600KB 分块拷贝，实测约 2~5ms；仅自动模式启用时才跑。
 */
static void pipe_motion_estimate(uint8_t *data, int w, int h, int stride)
{
    size_t need = (size_t)stride * h;
    if (!s_pipe.mv_prev || s_pipe.mv_prev_cap < need ||
        s_pipe.mv_prev_stride != stride || s_pipe.mv_prev_h != h) {
        if (s_pipe.mv_prev) {
            heap_caps_free(s_pipe.mv_prev);
            s_pipe.mv_prev = NULL;
            s_pipe.mv_prev_cap = 0;
        }
        uint8_t *p = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "motion prev buf %u B alloc failed", (unsigned)need);
            s_pipe.mv_on = false;                 /* 直接关掉，不反复报错刷屏 */
            s_pipe.st.mv_peak = 0;
            s_pipe.st.mv_ratio = 0;
            s_pipe.st.mv_ready = 0;
            return;
        }
        s_pipe.mv_prev = p;
        s_pipe.mv_prev_cap = need;
        s_pipe.mv_prev_stride = stride;
        s_pipe.mv_prev_h = h;
        s_pipe.mv_has_prev = false;
    }
    if (!s_pipe.mv_has_prev) {
        memcpy(s_pipe.mv_prev, data, need);
        s_pipe.mv_has_prev = true;
        s_pipe.st.mv_peak = 0;
        s_pipe.st.mv_ratio = 0;
        /* ⚠️ 基准帧的 mv 恒为 0，不是"画面静止"的结论。判定方必须靠 mv_ready
           区分"真实比较结果"与"刚重建基准"，否则每次重开运动估计都会立刻读到
           一个全 0 样本、把 50fps 误判成静止（振荡的最后一环，2026-10-02） */
        s_pipe.st.mv_ready = 0;
        return;                                   /* 首帧：只建立参考帧 */
    }

    const int thr = s_pipe.mv_thr;
    uint32_t peak = 0, nmv = 0, ntot = 0;

    for (int y0 = 0; y0 < h; y0 += DN_BLOCK) {
        int bh = (h - y0 < DN_BLOCK) ? (h - y0) : DN_BLOCK;
        for (int x0 = 0; x0 < w; x0 += DN_BLOCK) {
            int bw = (w - x0 < DN_BLOCK) ? (w - x0) : DN_BLOCK;

            int bmax = 0;
            for (int by = 0; by < bh; by += DN_SAD_STEP) {
                const uint16_t *pc = (const uint16_t *)(data + (size_t)(y0 + by) * stride) + x0;
                const uint16_t *pp = (const uint16_t *)(s_pipe.mv_prev + (size_t)(y0 + by) * stride) + x0;
                for (int bx = 0; bx < bw; bx += DN_SAD_STEP) {
                    int d = (int)((pc[bx] >> 5) & 0x3F) - (int)((pp[bx] >> 5) & 0x3F);
                    if (d < 0) d = -d;
                    if (d > bmax) bmax = d;
                }
            }
            ntot++;
            if ((uint32_t)bmax > peak) peak = (uint32_t)bmax;
            if (bmax >= thr) nmv++;

            /* 每帧无条件刷新参考帧（只比相邻帧，不需要多帧历史）*/
            for (int y = 0; y < bh; y++) {
                uint16_t *pc = (uint16_t *)(data + (size_t)(y0 + y) * stride) + x0;
                uint16_t *pp = (uint16_t *)(s_pipe.mv_prev + (size_t)(y0 + y) * stride) + x0;
                memcpy(pp, pc, (size_t)bw * 2);
            }
        }
    }

    s_pipe.st.mv_peak = peak;
    s_pipe.st.mv_ratio = ntot ? (nmv * 100 / ntot) : 0;
    s_pipe.st.mv_ready = 1;      /* 本样本来自真实的相邻帧比较，可以据此下结论 */
}

/* ---------------- AE/AWB 统计采样（app_camauto 用） ----------------
 *
 * 在 WB/镜像/降噪之前对原始帧做稀疏采样（每 8 像素 x 每 8 行，640x480 -> 4800 点），
 * 量出"传感器直出"的亮度与 R/G/B 均值：AWB 需要校正前的通道比例，
 * AE 需要不受 WB 增益影响的真实亮度。采样走位段拆取，无乘除，成本可忽略。
 */
static void pipe_ae_stats(const uint8_t *data, int w, int h, int stride, img_fmt_t fmt)
{
    uint32_t rs = 0, gs = 0, bs = 0, n = 0;
    if (fmt == IMG_FMT_RGB565) {
        for (int y = 0; y < h; y += 8) {
            const uint16_t *row = (const uint16_t *)(data + (size_t)y * stride);
            for (int x = 0; x < w; x += 8) {
                uint16_t v = row[x];
                rs += (v >> 11) & 0x1F;
                gs += (v >> 5) & 0x3F;
                bs += v & 0x1F;
                n++;
            }
        }
        if (n == 0) {
            return;
        }
        uint32_t r5 = rs / n, g6 = gs / n, b5 = bs / n;
        rs = (r5 << 3) | (r5 >> 2);         /* 5/6/5bit -> 8bit */
        gs = (g6 << 2) | (g6 >> 4);
        bs = (b5 << 3) | (b5 >> 2);
    } else if (fmt == IMG_FMT_RGB888) {
        for (int y = 0; y < h; y += 8) {
            const uint8_t *row = data + (size_t)y * stride;
            for (int x = 0; x < w; x += 8) {
                rs += row[x * 3];
                gs += row[x * 3 + 1];
                bs += row[x * 3 + 2];
                n++;
            }
        }
        if (n == 0) {
            return;
        }
        rs /= n; gs /= n; bs /= n;
    } else {
        return;                              /* JPEG 压缩帧没有像素可采 */
    }
    s_pipe.st.ae_r = rs;
    s_pipe.st.ae_g = gs;
    s_pipe.st.ae_b = bs;
    s_pipe.st.ae_luma = (77u * rs + 151u * gs + 28u * bs) >> 8;   /* Rec.601 */
    s_pipe.st.ae_stats_us = esp_timer_get_time();
}

/*
 * 帧后处理 = 软件白平衡(R/B 增益) + 行内镜像，**合并成一次内存遍历**。
 * 白平衡用查表（5bit/8bit LUT）而不是每像素乘除 —— 逐像素乘除实测会把
 * camera 任务(prio 6)占死到触发 task_wdt、把 faceai 饿死（0x02 全停）。
 */
static void pipe_postprocess(uint8_t *data, int w, int h, int stride, img_fmt_t fmt,
                             int kr, int kb, bool hflip)
{
    bool wb = (kr != 1000 || kb != 1000);
    if (!wb && !hflip) {
        return;
    }

    static uint8_t lut_r5[32], lut_b5[32], lut_r8[256], lut_b8[256];
    static int last_kr = -9999, last_kb = -9999;
    if (wb && (kr != last_kr || kb != last_kb)) {
        for (int i = 0; i < 32; i++) {
            int v = i * kr / 1000;
            lut_r5[i] = (uint8_t)(v > 31 ? 31 : v);
            v = i * kb / 1000;
            lut_b5[i] = (uint8_t)(v > 31 ? 31 : v);
        }
        for (int i = 0; i < 256; i++) {
            int v = i * kr / 1000;
            lut_r8[i] = (uint8_t)(v > 255 ? 255 : v);
            v = i * kb / 1000;
            lut_b8[i] = (uint8_t)(v > 255 ? 255 : v);
        }
        last_kr = kr;
        last_kb = kb;
    }

    for (int y = 0; y < h; y++) {
        uint8_t *row = data + (size_t)y * stride;
        if (fmt == IMG_FMT_RGB565) {
            uint16_t *p = (uint16_t *)row;
            if (hflip) {
                for (int i = 0, j = w - 1; i < j; i++, j--) {
                    uint16_t t = p[i];
                    p[i] = p[j];
                    p[j] = t;
                }
            }
            if (wb) {
                for (int x = 0; x < w; x++) {
                    uint16_t v = p[x];
                    p[x] = (uint16_t)(((uint16_t)lut_r5[(v >> 11) & 0x1F] << 11) |
                                      (v & 0x07E0) | lut_b5[v & 0x1F]);
                }
            }
        } else {                    /* RGB888 */
            if (hflip) {
                for (int i = 0, j = w - 1; i < j; i++, j--) {
                    for (int k = 0; k < 3; k++) {
                        uint8_t t = row[i * 3 + k];
                        row[i * 3 + k] = row[j * 3 + k];
                        row[j * 3 + k] = t;
                    }
                }
            }
            if (wb) {
                for (int x = 0; x < w; x++) {
                    row[x * 3] = lut_r8[row[x * 3]];
                    row[x * 3 + 2] = lut_b8[row[x * 3 + 2]];
                }
            }
        }
    }
}

static esp_err_t pipe_copy_into(int idx, const uint8_t *data, int w, int h, int stride, img_fmt_t fmt)
{
    int bpp = (fmt == IMG_FMT_RGB565) ? 2 : 3;
    if (stride <= 0) stride = w * bpp;
    if (w <= 0 || h <= 0 || stride < w * bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t need = (size_t)stride * h;

    pipe_slot_t *sl = &s_pipe.slot[idx];
    if (sl->frame.data && (sl->cap < need || sl->pad_w != stride || sl->pad_h != h)) {
        heap_caps_free(sl->frame.data);
        sl->frame.data = NULL;
        sl->cap = 0;
    }
    if (!sl->frame.data) {
        jpeg_decode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t got = 0;
        void *p = jpeg_alloc_decoder_mem(need, &mem_cfg, &got);
        ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "raw buf %u B", (unsigned)need);
        sl->frame.data = p;
        sl->cap = got;
        sl->pad_w = stride;
        sl->pad_h = h;
    }

    int64_t t0 = esp_timer_get_time();
    memcpy(sl->frame.data, data, need);
    sl->frame.width = w;
    sl->frame.height = h;
    sl->frame.stride = stride;
    sl->frame.fmt = fmt;
    sl->frame.dec_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);   /* raw 帧=拷贝耗时 */
    sl->frame.seq = s_pipe.st.rx_frames + 1;
    /* ⚠️ 这里不做 C2M 回写：publish_raw 在拷贝之后还会在槽内就地写入
     * （DN、白平衡+镜像），回写必须等所有 CPU 写入完成后统一做——
     * 见 publish_raw 末尾与 HANDOVER 廿一节的顺序坑 */
    return ESP_OK;
}

/* 发布某一槽为最新帧并通知消费者/sink */
static void pipe_publish(int slot)
{
    img_frame_t fr = s_pipe.slot[slot].frame;
    s_pipe.st.dec_frames++;
    s_pipe.st.dec_ms = fr.dec_ms;
    s_pipe.st.width = fr.width;
    s_pipe.st.height = fr.height;

    s_pipe.slot[slot].frame.ts_us = esp_timer_get_time();
    s_pipe.last_pub_us = s_pipe.slot[slot].frame.ts_us;   /* 按需发布的强制下限计时 */
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    s_pipe.latest = slot;
    s_pipe.slot[slot].consumed = false;
    xSemaphoreGive(s_pipe.lock);
    xSemaphoreGive(s_pipe.evt);

    pipe_report();

    if (s_pipe.sink) {
        s_pipe.sink(&fr);                   /* faceai 自己回 0x02 */
    }
}

/* ---------------- 0x02 ack（sink 未注册时的最小结果帧） ---------------- */

static void pipe_send_ack(const char *event, const char *msg, int w, int h, uint32_t dec_ms)
{
    char js[288];
    int n = snprintf(js, sizeof(js),
                     "{\"faces\":0,\"boxes\":[],\"event\":\"%s\","
                     "\"fps\":%.1f,\"infer_ms\":%u,\"dropped\":%"PRIu32","
                     "\"pipe\":{\"w\":%d,\"h\":%d,\"jpeg\":%"PRIu32",\"dec_ms\":%u}%s%s%s}",
                     event, (double)s_pipe.st.fps, (unsigned)dec_ms, s_pipe.st.dropped,
                     w, h, s_pipe.st.jpeg_bytes, (unsigned)dec_ms,
                     msg ? ",\"msg\":\"" : "", msg ? msg : "", msg ? "\"" : "");
    if (n > 0 && n < (int)sizeof(js)) {
        app_usb_link_send(JCL_TYPE_RESULT, js, (uint32_t)n);
    }
}

/* ---------------- 解码任务 ---------------- */

static void pipe_report(void)
{
    int64_t now = esp_timer_get_time();
    s_pipe.fps_cnt++;
    int64_t win = now - s_pipe.fps_win;
    if (win < 1000000) {
        return;
    }
    uint32_t n = s_pipe.fps_cnt;
    s_pipe.st.fps = (float)n * 1000000.0f / (float)win;
    s_pipe.fps_win = now;
    s_pipe.fps_cnt = 0;
    app_status_set(APP_MOD_PIPE, true, "%dx%d %.1ffps %ums",
                   s_pipe.st.width, s_pipe.st.height, (double)s_pipe.st.fps, (unsigned)s_pipe.st.dec_ms);
    if (s_pipe.st.dec_frames && n) {
        app_usb_link_log("imgpipe: %dx%d %.1ffps dec=%ums jpeg=%"PRIu32"B rx=%"PRIu32" drop=%"PRIu32,
                         s_pipe.st.width, s_pipe.st.height, (double)s_pipe.st.fps,
                         (unsigned)s_pipe.st.dec_ms, s_pipe.st.jpeg_bytes,
                         s_pipe.st.rx_frames, s_pipe.st.dropped);
        /* 分段耗时（每帧均值 us）：发布侧 copy/ae/dn/post/crc/msync + 消费侧失效；
         * skip = 按需发布在源头跳过的帧数（这些帧没进 copy/post，n 是实际发布数）*/
        uint32_t inv_n = s_prof.inv_n ? s_prof.inv_n : 1;
        uint32_t lat_n = s_prof.lat_n ? s_prof.lat_n : 1;
        app_usb_link_log("imgpipe/prof n=%u pub[copy %u ae %u dn %u post %u crc %u msync %u skip %u] "
                         "get[inv %uus max %u n=%u %.0fMB/s lat %ums max %u]",
                         (unsigned)n,
                         (unsigned)(s_prof.copy_us / n), (unsigned)(s_prof.ae_us / n),
                         (unsigned)(s_prof.dn_us / n), (unsigned)(s_prof.post_us / n),
                         (unsigned)(s_prof.crc_us / n), (unsigned)(s_prof.msync_us / n),
                         (unsigned)s_prof.skip_n,
                         (unsigned)(s_prof.inv_us / inv_n), (unsigned)s_prof.inv_max,
                         (unsigned)s_prof.inv_n,
                         (double)s_prof.inv_bytes / 1048576.0,
                         (unsigned)(s_prof.lat_us / lat_n / 1000), (unsigned)s_prof.lat_max_ms);
        memset(&s_prof, 0, sizeof(s_prof));
    }
}

static void imgpipe_task(void *arg)
{
    (void)arg;
    src_frame_t sf;

    while (1) {
        frame_source_t *src = s_pipe.src;
        if (!src) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool have = false;
        if (s_pipe.src_mode == APP_IMG_SRC_PC) {
            have = (src->acquire(src, &sf, PIPE_ACQ_TIMEOUT) == ESP_OK);
        } else {
            /* CAM 模式：摄像头帧由 camera 任务直接 publish_raw 推进来，这里只
               瞄一眼 PC 有没有在发帧（发了就自动切回 PC 源，PC 侧不用先发命令） */
            if (src->acquire(src, &sf, 0) == ESP_OK) {
                app_imgpipe_select_src(APP_IMG_SRC_PC);
                have = true;
            } else {
                vTaskDelay(1);
            }
        }
        if (!have) {
            continue;
        }

        s_pipe.st.rx_frames++;
        s_pipe.st.jpeg_bytes = sf.len;

        int slot = pipe_take_slot();
        if (slot < 0) {
            s_pipe.st.dropped++;
            ESP_LOGW(TAG, "no free frame slot, drop %"PRIu32" B", sf.len);
            src->release(src, &sf);
            continue;
        }

        esp_err_t err = pipe_decode_into(slot, sf.data, sf.len);
        src->release(src, &sf);
        if (err != ESP_OK) {
            s_pipe.st.dropped++;
            ESP_LOGE(TAG, "decode failed (%"PRIu32" B): %s", sf.len, esp_err_to_name(err));
            pipe_send_ack("error", "jpeg decode failed", 0, 0, 0);
            continue;
        }

        bool want_ack = (s_pipe.sink == NULL) && s_pipe.auto_ack;
        img_frame_t fr = s_pipe.slot[slot].frame;
        pipe_publish(slot);
        if (want_ack) {
            pipe_send_ack("none", NULL, fr.width, fr.height, fr.dec_ms);
        }
    }
}

/* ---------------- 源模式 / raw 帧推送 ---------------- */

const char *app_imgpipe_src_str(app_img_src_t src)
{
    return src == APP_IMG_SRC_CAM ? "cam" : "pc";
}

app_img_src_t app_imgpipe_get_src(void)
{
    return s_pipe.src_mode;
}

void app_imgpipe_set_hflip(bool on)
{
    if (s_pipe.hflip == on) {
        return;
    }
    s_pipe.hflip = on;
    ESP_LOGW(TAG, "camera hflip -> %s", on ? "ON" : "OFF");
    app_usb_link_log("imgpipe: hflip %s", on ? "on" : "off");
}

bool app_imgpipe_get_hflip(void)
{
    return s_pipe.hflip;
}

static void wb_gain_set(int kr, int kb, bool log)
{
    if (kr < 100) kr = 100;
    if (kr > 4000) kr = 4000;
    if (kb < 100) kb = 100;
    if (kb > 4000) kb = 4000;
    s_pipe.kr = kr;
    s_pipe.kb = kb;
    if (log) {
        ESP_LOGW(TAG, "software WB: R*%.3f B*%.3f", kr / 1000.0, kb / 1000.0);
        app_usb_link_log("imgpipe: wb kr=%d kb=%d", kr, kb);
    }
}

void app_imgpipe_set_wb_gain(int kr, int kb)
{
    wb_gain_set(kr, kb, true);
}

void app_imgpipe_set_wb_gain_quiet(int kr, int kb)
{
    wb_gain_set(kr, kb, false);
}

void app_imgpipe_get_wb_gain(int *kr, int *kb)
{
    if (kr) *kr = s_pipe.kr;
    if (kb) *kb = s_pipe.kb;
}

void app_imgpipe_set_dn(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    if (s_pipe.dn == level) {
        return;
    }
    s_pipe.dn = level;
    s_pipe.has_prev = false;       /* 换档丢弃历史，避免策略切换瞬间的残影 */
    ESP_LOGW(TAG, "temporal denoise -> %d (%s)", level,
             level == 0 ? "off" : level == 1 ? "low" : level == 2 ? "mid" : "high");
    app_usb_link_log("imgpipe: denoise -> %d", level);
}

int app_imgpipe_get_dn(void)
{
    return s_pipe.dn;
}

void app_imgpipe_set_ae_stats(bool on)
{
    if (s_pipe.ae_stats_on == on) {
        return;
    }
    s_pipe.ae_stats_on = on;
    if (!on) {
        s_pipe.st.ae_stats_us = 0;       /* 让 AE 立刻看到"没有新鲜数据" */
    }
    ESP_LOGI(TAG, "ae stats sampling %s", on ? "on" : "off");
}

void app_imgpipe_reset_prev(void)
{
    s_pipe.has_prev = false;
    s_pipe.mv_has_prev = false;    /* 运动估计的参考帧也一起丢弃 */
    s_pipe.st.mv_ready = 0;
}

void app_imgpipe_set_motion_detect(bool on)
{
    if (s_pipe.mv_on == on) {
        return;
    }
    s_pipe.mv_on = on;
    s_pipe.mv_has_prev = false;
    s_pipe.st.mv_ready = 0;
    ESP_LOGW(TAG, "motion detect -> %s (thr=%d)", on ? "ON" : "OFF", s_pipe.mv_thr);
    app_usb_link_log("imgpipe: motion detect %s", on ? "on" : "off");
}

void app_imgpipe_set_motion_thr(int thr)
{
    if (thr < 1) thr = 1;
    if (thr > 63) thr = 63;
    s_pipe.mv_thr = thr;
    s_pipe.mv_has_prev = false;
    s_pipe.st.mv_ready = 0;
    ESP_LOGW(TAG, "motion thr -> %d", thr);
}

void app_imgpipe_set_pub_demand(bool on)
{
    if (s_pipe.pub_demand == on) {
        return;
    }
    s_pipe.pub_demand = on;
    ESP_LOGW(TAG, "publish-on-demand -> %s (min %d ms)", on ? "ON" : "OFF",
             CONFIG_APP_CAM_PUB_MIN_MS);
    app_usb_link_log("imgpipe: pub_demand %s", on ? "on" : "off");
}

bool app_imgpipe_get_pub_demand(void)
{
    return s_pipe.pub_demand;
}

void app_imgpipe_select_src(app_img_src_t src)
{
    if (s_pipe.src_mode == src) {
        return;
    }
    s_pipe.src_mode = src;
    ESP_LOGW(TAG, "active image source -> %s", app_imgpipe_src_str(src));
    app_usb_link_log("imgpipe: image source -> %s", app_imgpipe_src_str(src));
}

esp_err_t app_imgpipe_publish_raw(const uint8_t *data, int w, int h, int stride, img_fmt_t fmt)
{
    ESP_RETURN_ON_FALSE(data && s_pipe.lock, ESP_ERR_INVALID_STATE, TAG, "not inited");
    if (s_pipe.src_mode != APP_IMG_SRC_CAM) {
        return ESP_ERR_INVALID_STATE;        /* PC 模式：摄像头帧丢弃，不做拷贝 */
    }

    /* ---- 发布端按需发布（2026-10-06 待办 5）----
     * 相机 50fps 每帧 ~17ms 的 copy+post（614KB 拷贝 8.6ms + 白平衡/镜像 8.3ms），
     * 而消费者实际只取 ~7(AI)+10(预览) 帧/s —— 没人要的帧整条链路白跑，
     * CPU1 约 87% 的占空比花在无效帧上（日光分段计时，HANDOVER 第廿一节）。
     * 跳过条件：没有消费者正在 get_frame 里等帧 && 距上次真实发布 < 强制间隔。
     * ⚠️ 强制下限必须有：AE 亮度采样 / 运动估计 / 时域降噪全挂在发布路径上，
     * 全跳过会让夜视阶梯与 AE 失去输入（luma 冻结 = 判定状态机瞎掉）。
     * 10 帧/s 足够喂饱全部判定节流（AE 250ms / AUTO 探测窗 300ms）。 */
    if (s_pipe.pub_demand) {
        bool waiter = s_pipe.waiters > 0;
        bool forced = (esp_timer_get_time() - s_pipe.last_pub_us) >=
                      (int64_t)CONFIG_APP_CAM_PUB_MIN_MS * 1000;
        if (!waiter && !forced) {
            s_pipe.st.pub_skip++;
            s_prof.skip_n++;
            return ESP_OK;
        }
    }

    int slot = pipe_take_slot();
    if (slot < 0) {
        s_pipe.st.dropped++;
        return ESP_ERR_NO_MEM;               /* 消费者还没放槽，丢帧 */
    }
    int64_t t_sect = esp_timer_get_time();
    esp_err_t err = pipe_copy_into(slot, data, w, h, stride, fmt);
    if (err != ESP_OK) {
        s_pipe.st.dropped++;
        return err;
    }
    s_prof.copy_us += (uint32_t)(esp_timer_get_time() - t_sect);
    uint8_t *slot_data = s_pipe.slot[slot].frame.data;

    /* AE/AWB 统计：必须在降噪/镜像/白平衡之前采（要的是传感器直出画像）*/
    if (s_pipe.ae_stats_on) {
        t_sect = esp_timer_get_time();
        pipe_ae_stats(slot_data, w, h, stride, fmt);
        s_prof.ae_us += (uint32_t)(esp_timer_get_time() - t_sect);
    }

    /* 运动估计（自动帧率用）：跑在降噪之前、用原始帧；与降噪解耦，高帧率下也照跑 */
    if (s_pipe.mv_on) {
        int64_t t_mv = esp_timer_get_time();
        pipe_motion_estimate(slot_data, w, h, stride);
        s_pipe.st.mv_ms = (uint32_t)((esp_timer_get_time() - t_mv) / 1000);
        /* 新鲜度时戳：判定方靠"这一帧有没有新样本"来决定能不能下结论 */
        s_pipe.st.mv_stats_us = esp_timer_get_time();
    }

    /* 时域降噪（必须在镜像之前，见 pipe_dn_apply 注释）；高帧率下跳过以保证链路稳定 */
    if (s_pipe.dn > 0 && fmt == IMG_FMT_RGB565) {
        if (CONFIG_APP_CAM_DN_MAX_FPS > 0 &&
            app_camera_get_fps() > (uint32_t)CONFIG_APP_CAM_DN_MAX_FPS) {
            s_pipe.st.dn_skip++;
            s_pipe.has_prev = false;      /* 回到低帧率时不要复用陈旧历史（会残影） */
        } else {
            int64_t t_dn = esp_timer_get_time();
            pipe_dn_apply(slot_data, w, h, stride);
            uint32_t d = (uint32_t)((esp_timer_get_time() - t_dn) / 1000);
            s_pipe.st.dn_ms = d;
            s_prof.dn_us += d * 1000;
        }
    }

    /* 只处理槽里的副本（白平衡 + 镜像一次遍历），不动摄像头 mmap 缓冲 */
    t_sect = esp_timer_get_time();
    pipe_postprocess(slot_data, w, h, stride, fmt,
                     s_pipe.kr, s_pipe.kb, s_pipe.hflip);
    s_prof.post_us += (uint32_t)(esp_timer_get_time() - t_sect);
    int bpp = (fmt == IMG_FMT_RGB565) ? 2 : 3;
    s_pipe.st.rx_frames++;
    s_pipe.st.jpeg_bytes = (uint32_t)(stride > 0 ? stride : w * bpp) * (uint32_t)h;
    /* ⚠️ 缓存一致性回写（2026-10-04，HANDOVER 廿一节）：本函数跑在 camera 任务
     * （钉 CPU1），拷贝/DN/镜像的 CPU 写脏行留在 CPU1 的 L1；faceai（CPU0）与
     * 0x05 stream 编码 DMA 读的都是 L2/RAM——P4 双核 L1 互不一致且无跨核 IPI，
     * 必须在发布前统一回写。⚠️ 位置铁律：必须在该帧所有 CPU 写入（含 postprocess
     * 的镜像）之后——回写放在拷贝阶段会让镜像内容滞留本核 L1，消费者读到
     * "部分行镜像、部分行未镜像"的撕裂画面（实测：上下区域左右排序相反） */
    {
        size_t need = (size_t)stride * h;
        /* ⚠️ 全帧 CRC32 实测 **18.5ms/帧**（614KB）——纯诊断（结果帧 frm_crc/ai_crc
         * 对照，见 HANDOVER 廿一节），默认关（APP_CAM_FRAME_CRC）。常驻它是单核
         * ~30% 的 CPU 白烧，直接压低整条管线帧率（2026-10-06 分段计时查出来的）。*/
#if CONFIG_APP_CAM_FRAME_CRC
        int64_t tc = esp_timer_get_time();
        s_pipe.slot[slot].frame.crc32 = esp_rom_crc32_le(0, slot_data, need);
        s_prof.crc_us += (uint32_t)(esp_timer_get_time() - tc);
#endif
        s_pipe.slot[slot].frame.dec_core = (int)xPortGetCoreID();
        int64_t tm = esp_timer_get_time();
        esp_cache_msync(slot_data, need,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        s_prof.msync_us += (uint32_t)(esp_timer_get_time() - tm);
    }
    pipe_publish(slot);
    return ESP_OK;
}

/* ---------------- 公共 API ---------------- */

esp_err_t app_imgpipe_get_frame(img_frame_t *out, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(out && s_pipe.lock, ESP_ERR_INVALID_STATE, TAG, "not inited");
    int64_t deadline = (timeout_ms == APP_IMG_WAIT_FOREVER)
                       ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
        int idx = s_pipe.latest;
        if (idx >= 0 && !s_pipe.slot[idx].busy && !s_pipe.slot[idx].consumed) {
            s_pipe.slot[idx].busy = true;
            s_pipe.slot[idx].consumed = true;
            *out = s_pipe.slot[idx].frame;
            xSemaphoreGive(s_pipe.lock);
            /* ⚠️ 跨核缓存一致性（2026-10-04，HANDOVER 廿一节）：生产者可能运行在
             * 另一个核（CAM 路径 memcpy 在 CPU1 / JPEG 解码 invalidate 在解码核），
             * P4 双核 L1 互不一致、无跨核 IPI——消费者核的 L1 可能残留本缓冲
             * 上一次内容的 cache 行。在读者核上做 M2C 失效（缓冲无脏行，安全；
             * cap 由 jpeg_alloc_decoder_mem 64B 对齐，满足 M2C 对齐要求）。 */
            int64_t t_inv = esp_timer_get_time();
            esp_cache_msync(out->data, s_pipe.slot[idx].cap,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            uint32_t inv = (uint32_t)(esp_timer_get_time() - t_inv);
            s_prof.inv_us += inv;
            s_prof.inv_n++;
            s_prof.inv_bytes += s_pipe.slot[idx].cap;
            if (inv > s_prof.inv_max) s_prof.inv_max = inv;
            /* 发布 → 被取走 的排队延迟：帧池拥塞/消费者跟不上的直接指标 */
            uint32_t lat_ms = (uint32_t)((esp_timer_get_time() - out->ts_us) / 1000);
            s_prof.lat_us += (uint64_t)lat_ms * 1000;
            s_prof.lat_n++;
            if (lat_ms > s_prof.lat_max_ms) s_prof.lat_max_ms = lat_ms;
            return ESP_OK;
        }
        xSemaphoreGive(s_pipe.lock);
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_NOT_FOUND;
        }
        /* 告诉发布端"有消费者在等帧"：按需发布据此决定要不要为这一帧跑
         * copy/post 链。等帧是 1 tick 一轮的短窗口，进出都计数（启发式即可，
         * 偶发的错过/多算只影响一帧，下一轮相机帧就纠正）。 */
        s_pipe.waiters++;
        xSemaphoreTake(s_pipe.evt, 1);   /* 仅作唤醒(1 tick)，超时后重查；0 tick 会空转 */
        s_pipe.waiters--;
    }
}

void app_imgpipe_release(const img_frame_t *frame)
{
    if (!frame || !s_pipe.lock) return;
    xSemaphoreTake(s_pipe.lock, portMAX_DELAY);
    for (int i = 0; i < PIPE_SLOTS; i++) {
        if (s_pipe.slot[i].frame.data == frame->data) {
            s_pipe.slot[i].busy = false;
            break;
        }
    }
    xSemaphoreGive(s_pipe.lock);
}

void app_imgpipe_set_sink(app_imgpipe_sink_t cb)
{
    s_pipe.sink = cb;
}

void app_imgpipe_set_auto_ack(bool enable)
{
    s_pipe.auto_ack = enable;
    ESP_LOGI(TAG, "auto ack %s", enable ? "on" : "off (consumer replies)");
}

frame_source_t *app_imgpipe_source(void)
{
    return s_pipe.src;
}

void app_imgpipe_set_source(frame_source_t *src)
{
    s_pipe.src = src ? src : &s_usb_src;
    ESP_LOGI(TAG, "source -> %s", s_pipe.src->name);
}

/* ---------------- 工具：缓冲 / 缩放 ---------------- */

esp_err_t app_imgpipe_alloc_rgb888(img_frame_t *frame, int w, int h)
{
    ESP_RETURN_ON_FALSE(frame && w > 0 && h > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");
    memset(frame, 0, sizeof(*frame));
    size_t size = (size_t)w * h * 3;
    uint8_t *p = heap_caps_aligned_calloc(64, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "rgb888 %dx%d", w, h);
    frame->data = p;
    frame->width = w;
    frame->height = h;
    frame->stride = w * 3;
    frame->fmt = IMG_FMT_RGB888;
    return ESP_OK;
}

void app_imgpipe_free_frame(img_frame_t *frame)
{
    if (!frame) return;
    if (frame->data) heap_caps_free(frame->data);
    memset(frame, 0, sizeof(*frame));
}

esp_err_t app_imgpipe_resize_rgb888(const img_frame_t *src, img_frame_t *dst)
{
    ESP_RETURN_ON_FALSE(src && dst && src->data && dst->data, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(src->fmt == IMG_FMT_RGB888 && dst->fmt == IMG_FMT_RGB888,
                        ESP_ERR_NOT_SUPPORTED, TAG, "only RGB888");
    const int sw = src->width, sh = src->height, dw = dst->width, dh = dst->height;
    ESP_RETURN_ON_FALSE(sw >= 2 && sh >= 2 && dw > 0 && dh > 0 && dst->stride >= dw * 3,
                        ESP_ERR_INVALID_ARG, TAG, "%dx%d -> %dx%d", sw, sh, dw, dh);

    /* 16.16 定点步进，dst 边缘映射到 src 边缘 */
    const uint32_t xstep = (dw > 1) ? (uint32_t)(((uint64_t)(sw - 1) << 16) / (dw - 1)) : 0;
    const uint32_t ystep = (dh > 1) ? (uint32_t)(((uint64_t)(sh - 1) << 16) / (dh - 1)) : 0;

    uint32_t yfp = 0;
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)(yfp >> 16);
        uint32_t fy = (yfp >> 8) & 0xFF;
        if (sy0 > sh - 2) sy0 = sh - 2;
        const uint8_t *row0 = src->data + (size_t)sy0 * src->stride;
        const uint8_t *row1 = row0 + src->stride;
        uint8_t *drow = dst->data + (size_t)y * dst->stride;

        uint32_t xfp = 0;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)(xfp >> 16);
            uint32_t fx = (xfp >> 8) & 0xFF;
            if (sx0 > sw - 2) sx0 = sw - 2;
            const uint8_t *p00 = row0 + (size_t)sx0 * 3;
            const uint8_t *p01 = row1 + (size_t)sx0 * 3;

            uint32_t w00 = (256 - fx) * (256 - fy);
            uint32_t w10 = fx * (256 - fy);
            uint32_t w01 = (256 - fx) * fy;
            uint32_t w11 = fx * fy;
            for (int c = 0; c < 3; c++) {
                uint32_t v = p00[c] * w00 + p00[3 + c] * w10 + p01[c] * w01 + p01[3 + c] * w11;
                drow[x * 3 + c] = (uint8_t)(v >> 16);
            }
            xfp += xstep;
        }
        yfp += ystep;
    }
    return ESP_OK;
}

void app_imgpipe_get_stats(app_imgpipe_stats_t *out)
{
    if (out) *out = s_pipe.st;
}

esp_err_t app_imgpipe_init(void)
{
    if (s_pipe.ready) return ESP_OK;

    s_pipe.lock = xSemaphoreCreateMutex();
    s_pipe.evt = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_pipe.lock && s_pipe.evt, ESP_ERR_NO_MEM, TAG, "sync primitives");

    s_pipe.latest = -1;
    s_pipe.auto_ack = true;
    s_pipe.pub_demand = CONFIG_APP_CAM_PUB_ON_DEMAND;
    s_pipe.last_pub_us = esp_timer_get_time();
    s_pipe.waiters = 0;
    /* 镜像：传感器 HMIRROR 已生效时软件镜像关闭（两处都开=翻回去）；传感器不支持
       （set/get 失败）则退回软件行内镜像 */
    s_pipe.hflip = CONFIG_APP_CAM_HFLIP && !app_camera_sensor_hflip();
    if (s_pipe.hflip) {
        ESP_LOGW(TAG, "sensor hflip unavailable, software hflip ON");
    }
    s_pipe.kr = CONFIG_APP_CAM_WB_KR;      /* 软件白平衡（默认按本机模组实测偏绿量校正） */
    s_pipe.kb = CONFIG_APP_CAM_WB_KB;
    s_pipe.dn = CONFIG_APP_CAM_DN;         /* 时域降噪档（0=关，运行时可用 cam 命令改） */
    s_pipe.mv_thr = CONFIG_APP_CAM_MOTION_THR;   /* 运动估计阈值（自动帧率模式下用） */
    s_pipe.fps_win = esp_timer_get_time();
    s_pipe.rx_buf = heap_caps_malloc(PIPE_RX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_pipe.rx_buf, ESP_ERR_NO_MEM, TAG, "rx buf %d B", PIPE_RX_BUF);

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&eng_cfg, &s_pipe.dec), TAG, "jpeg decoder engine");

    s_pipe.src = app_imgpipe_usb_source();
    if (s_pipe.src->start) {
        ESP_RETURN_ON_ERROR(s_pipe.src->start(s_pipe.src), TAG, "source start");
    }

    /* 摄像头已经出图就默认用它（板载源优先），否则等 PC 送帧 */
    s_pipe.src_mode = app_camera_ready() ? APP_IMG_SRC_CAM : APP_IMG_SRC_PC;

    /* 钉 CPU0（与 faceai 同核）：解码后驱动 invalidate 生效于本核，消费者同核
     * 读取天然一致；跨核场景由 get_frame 的读者侧 M2C 失效兜底（2026-10-04） */
    BaseType_t ok = xTaskCreatePinnedToCore(imgpipe_task, "imgpipe", PIPE_TASK_STACK,
                                            NULL, PIPE_TASK_PRIO, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task create");

    s_pipe.ready = true;
    app_status_set(APP_MOD_PIPE, true, s_pipe.src_mode == APP_IMG_SRC_CAM ? "camera src"
                                                                          : "waiting jpeg");
    ESP_LOGI(TAG, "up: src=%s(%s), hw jpeg decode -> RGB888, slots=%d",
             app_imgpipe_src_str(s_pipe.src_mode),
             s_pipe.src_mode == APP_IMG_SRC_CAM ? "onboard camera" : s_pipe.src->name, PIPE_SLOTS);
    app_usb_link_log("imgpipe ready: image source=%s", app_imgpipe_src_str(s_pipe.src_mode));
    return ESP_OK;
}
