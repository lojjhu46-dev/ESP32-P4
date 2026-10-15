#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wi-Fi via onboard ESP32-C6 (esp_wifi_remote + esp_hosted over SDIO).
 * C6 firmware must be flashed (vendor ships it: Burn files/JC1060P470_C6.bin).
 *
 * Scans APs; connects to CONFIG_APP_WIFI_SSID if non-empty.
 */
esp_err_t app_wifi_init(void);

/*
 * 运行时配置 STA 凭据并立即连接（{"cmd":"wifi"} 用）。
 * 写 NVS（FLASH 存储），断电保存；断线自动重连。
 */
void app_wifi_set_credentials(const char *ssid, const char *pass);

/*
 * STA 当前 IP 字符串（未关联返回 false）。结果帧 "wip" 字段用。
 */
bool app_wifi_get_ip(char *out, size_t len);

#ifdef __cplusplus
}
#endif
