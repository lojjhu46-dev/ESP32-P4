#pragma once
/*
 * app_netlink —— JCLINK over TCP（以太网，板子做 server）
 *
 * 帧格式/CRC/解析与 USB CDC 完全一致（见 docs/library/usb_link_protocol.md），
 * 只是把字节流从 tinyusb CDC 换成了 lwIP socket：
 *   PC(TCP client) --帧字节流--> netlink rx task --> app_usb_link_feed() --> 帧解析
 *   板端 --app_usb_link_send()--> jcl_transport_t(tcp write) --> PC
 *
 * 有 TCP 客户端连接时，所有 0x02/0x04 回包走 TCP；断开后自动回落到 USB CDC。
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起监听 + 收连接任务（eth 拿到 IP 前后都能调，bind 到 INADDR_ANY） */
esp_err_t app_netlink_init(uint16_t port);

bool app_netlink_client_connected(void);

#ifdef __cplusplus
}
#endif
