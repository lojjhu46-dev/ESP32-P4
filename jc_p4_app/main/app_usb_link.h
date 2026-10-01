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

#define JCL_MAX_PAYLOAD  (1024 * 1024)   /* 0x05 raw 模式要发 640x480x2=614KB，留一倍余量 */

/*
 * USB CDC link (esp_tinyusb, USB0 OTG HS).
 * Frame protocol: J C | type | seq | len(u32le) | payload | crc16
 * init() installs the tinyusb driver and spawns the RX parser task.
 */
esp_err_t app_usb_link_init(void);

/* Send a frame to the PC (thread-safe). */
esp_err_t app_usb_link_send(uint8_t type, const void *payload, uint32_t len);

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