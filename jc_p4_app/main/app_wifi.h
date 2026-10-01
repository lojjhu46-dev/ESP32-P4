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

#ifdef __cplusplus
}
#endif
