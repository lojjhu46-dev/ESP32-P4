/*
 * net_link —— LVGL 模拟器侧的 JCLINK 客户端（TCP 直连 P4 板子）
 *
 * 与板端 main/app_usb_link.c、PC 端 pc_host/protocol.py 完全同一套帧格式：
 *     'J''C' | type(1) | seq(1) | len(u32 LE) | payload | crc16(2 LE)
 *     crc16 = CRC-16/CCITT-FALSE(init 0xFFFF) over type+seq+len+payload
 * type: 0x01 JPEG(PC->板) / 0x02 结果JSON(板->PC) / 0x03 命令JSON(PC->板)
 *       0x04 日志(板->PC) / 0x05 图像帧(板->PC，见 app_stream.h)
 *
 * 设计：非阻塞 socket，全部在 LVGL 主循环里 net_link_tick() 轮询，
 * 不引入线程/LV_USE_OS 依赖（lv_conf.h 里 LV_USE_OS == LV_OS_NONE）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NET_LINK_MAX_PAYLOAD (1024 * 1024)   /* 与板端 JCL_MAX_PAYLOAD 一致（0x05 raw 帧 614KB） */
#define NET_LINK_RX_BUF      (1024 * 1024)   /* 0x05 raw 帧 614KB，留够缓冲 */

enum {
    NL_TYPE_JPEG   = 0x01,
    NL_TYPE_RESULT = 0x02,
    NL_TYPE_CMD    = 0x03,
    NL_TYPE_LOG    = 0x04,
    NL_TYPE_IMAGE  = 0x05,
};

/* 0x05 图像帧头：fmt u8 | flags u8 | w u16 | h u16 | seq u16 */
enum { NL_IMG_FMT_JPEG = 0, NL_IMG_FMT_RGB565 = 1 };

typedef struct {
    int      fmt;
    int      width;
    int      height;
    uint16_t seq;
    const uint8_t *data;   /* 指向内部缓冲，仅在回调内有效 */
    uint32_t len;
} nl_image_t;

typedef void (*nl_result_cb_t)(const char *json, void *arg);
typedef void (*nl_image_cb_t)(const nl_image_t *img, void *arg);
typedef void (*nl_log_cb_t)(const char *line, void *arg);
typedef void (*nl_state_cb_t)(bool connected, const char *detail, void *arg);

void net_link_set_callbacks(nl_result_cb_t on_result, nl_image_cb_t on_image,
                            nl_log_cb_t on_log, nl_state_cb_t on_state, void *arg);

/* 连接（可反复调用；tick 里也会自动重连）。host 形如 "192.168.33.2"，port 3333 */
void net_link_start(const char *host, int port);
void net_link_stop(void);

void net_link_tick(void);          /* LVGL 主循环里每 ~10ms 调一次 */
bool net_link_connected(void);
const char *net_link_detail(void);

/* 发命令（自动加帧头+CRC）。返回 0 成功，<0 失败/未连接 */
int net_link_send_cmd(const char *json);
