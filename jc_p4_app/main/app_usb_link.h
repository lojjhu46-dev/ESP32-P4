#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JCLINK v1 frame types (see docs/usb_link_protocol.md) */
#define JCL_TYPE_JPEG    0x01   /* PC -> board, JPEG frame */
#define JCL_TYPE_RESULT  0x02   /* board -> PC, JSON result */
#define JCL_TYPE_CMD     0x03   /* PC -> board, JSON command */
#define JCL_TYPE_LOG     0x04   /* board -> PC, log line */
#define JCL_TYPE_IMAGE   0x05   /* board -> PC, image frame (see app_stream.h) */
#define JCL_TYPE_H264    0x07   /* board -> PC, H.264 access unit (see app_h264enc.c)
                                 * payload = 20B 头 + Annex-B NAL，小端：
                                 * codec u8 | flags u8 | stream_id u16 | pts_us u32 |
                                 * frame_seq u32 | width u16 | height u16 | nal_len u32 */
#define JCL_TYPE_BLOB    0x06   /* both ways, bulk blob chunk (F10 db backup):
                                 * payload = offset(u32 LE) + data，板端按偏移重组 */
#define JCL_BLOB_CHUNK_DEF  (16 * 1024)  /* 0x06 默认分片（不含 4B 偏移头） */

#define JCL_MAX_PAYLOAD  (1024 * 1024)   /* 0x05 raw 模式要发 640x480x2=614KB，留一倍余量 */

/*
 * USB CDC link (esp_tinyusb, USB0 OTG HS).
 * Frame protocol: J C | type | seq | len(u32le) | payload | crc16
 * init() installs the tinyusb driver and spawns the RX parser task.
 */
esp_err_t app_usb_link_init(void);

/* Send a frame to the PC (thread-safe). */
esp_err_t app_usb_link_send(uint8_t type, const void *payload, uint32_t len);

/* 两段式发送（2026-10-16，USB 视频流）：hdr 与 payload 分两次写入**同一帧**（同一 seq、
   同一 CRC、同一 tx_lock）。用于 0x05 大帧——避免为了"8 字节头 + 614KB 数据"再分配
   一整块 PSRAM 并 memcpy 一次（raw 10fps 时那是 6MB/s 的无谓拷贝）。 */
esp_err_t app_usb_link_send2(uint8_t type,
                             const void *hdr, uint32_t hdr_len,
                             const void *payload, uint32_t payload_len);

/* ---- 链路统计（供 link 命令/状态显示；只读快照）---- */
typedef struct {
    uint32_t tx_frames;   /* 成功发出的帧数 */
    uint64_t tx_bytes;    /* 成功发出的总字节（含帧头与 CRC） */
    uint32_t tx_drop;     /* 发送失败或超预算丢弃的帧数 */
    uint32_t tx_last_ms;  /* 最近一帧发送耗时 ms */
} app_usb_link_stats_t;

void app_usb_link_get_stats(app_usb_link_stats_t *out);

/* ---- 发送后端强制选择（2026-10-16 USB 视频流"自由切换"）----
 * name = "usb" | "tcp" | "auto"（nullptr 等同 "auto"）；
 *   auto（默认）：TCP 客户端连上则抢占为 tcp、断开回落内置 USB CDC —— 与历史行为一致；
 *   usb  ：强制走 USB CDC（此时新来的 TCP 客户端会被 app_netlink 拒绝并说明原因）；
 *   tcp  ：强制走 TCP（从 USB 侧发起的命令也回 TCP，便于"切到网口"）。
 * 返回 false = 名字非法。 */
bool app_usb_link_force_backend(const char *name);

/* 当前**实际生效**的后端名（"usb-cdc" / "tcp"），用于结果帧 link 字段与 link 查询 */
const char *app_usb_link_active_name(void);

/* 强制态名（"auto" / "usb" / "tcp"） */
const char *app_usb_link_forced_name(void);

/* 是否处于"强制 usb"（app_netlink 用它决定要不要拒绝新 TCP 客户端） */
bool app_usb_link_usb_forced(void);

/* Send a log line (type 0x04, printf style, capped at 512 bytes). */
void app_usb_link_log(const char *fmt, ...);

/*
 * Poll for a complete received frame (JPEG or CMD).
 * Copies payload into caller buffer.
 * Returns: payload length (>0), 0 = no frame, <0 = frame dropped (crc error).
 */
int app_usb_link_recv(uint8_t *type_out, void *payload, uint32_t max_len);

/* Register callback for JSON command frames (called from rx parser context). */
void app_usb_link_set_cmd_cb(void (*cb)(const char *json, uint32_t len));

/* Register callback for blob chunk frames (0x06, called from rx parser context).
 * ⚠️ 回调纪律：只做"校验 + 顺序写入/小缓冲拷贝"，禁止阻塞、禁止调用发送接口、
 * 禁止访问特征库；payload 在回调返回后立即被释放。 */
void app_usb_link_set_blob_cb(void (*cb)(const uint8_t *data, uint32_t len));

/* ---------------- 传输层后端（app_netlink 用；不注册时就是 USB CDC） ----------------
 * JCLINK 的帧格式/CRC/解析与传输层无关：把收到的字节喂给 app_usb_link_feed()，
 * 把 app_usb_link_send() 的输出交给注册进来的 write/flush 即可。
 * 同一时刻只应有一个后端在工作（两端都发会有帧交错风险）。
 */
typedef struct {
    const char *name;
    esp_err_t (*write)(const uint8_t *data, uint32_t len);   /* 阻塞式，返回后缓冲可复用 */
    esp_err_t (*flush)(void);
} jcl_transport_t;

/* NULL -> 回落到内置 USB CDC */
void app_usb_link_set_transport(const jcl_transport_t *tp);
/* 外部后端把收到的字节注入帧解析器 */
void app_usb_link_feed(const uint8_t *data, uint32_t len);
/* 后端断开时丢弃半截帧（状态机复位），保证下一个后端从干净状态开始 */
void app_usb_link_rx_reset(void);
/* USB CDC 是否已初始化 */
bool app_usb_link_ready(void);

#ifdef __cplusplus
}
#endif