#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "board_pins.h"
#include "app_status.h"
#include "app_i2c.h"
#include "app_display.h"
#include "app_wifi.h"
#include "app_ble.h"
#include "app_sdcard.h"
#include "app_eth.h"
#include "app_audio.h"
#include "app_rs485.h"
#include "app_battery.h"
#include "app_camera.h"
#include "app_usb_link.h"
#include "app_netlink.h"
#include "app_imgpipe.h"
#include "app_stream.h"
#include "app_faceai.h"
#include "sdkconfig.h"

static const char *TAG = "main";

static void nvs_init_safe(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/*
 * esp_netif + 默认事件循环必须由 app 自己初始化。以前这里是被 esp_hosted(经
 * esp_wifi_remote) 的构造流程顺带初始化掉的，一旦停用 Wi-Fi（见 app_wifi/Kconfig）
 * 就会露出问题：app_eth_init() 里 esp_netif_new() 返回 NULL -> esp_netif_attach()
 * 空指针 Load access fault（实测）。此函数幂等，重复调用安全。
 */
static void netif_init_safe(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

void app_main(void)
{
    printf("\n==============================================\n");
    printf("  JC-ESP32P4-M3-DEV framework app\n");
    printf("  ESP32-P4 @360MHz + ESP32-C6 (Wi-Fi 6/BLE5)\n");
    printf("  build: %s %s\n", __DATE__, __TIME__);
    printf("==============================================\n");
    esp_chip_info_t chip_info = { 0 };
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip: %s rev%d, flash 16MB, PSRAM 32MB hex@200M",
             CONFIG_IDF_TARGET, chip_info.revision);

    nvs_init_safe();
    netif_init_safe();

    /* 1. shared I2C bus first (display/audio/camera SCCB depend on it) */
    app_i2c_init();
    app_i2c_scan();     /* 板级诊断：列出总线上有应答的器件 */

    /* 2. USB CDC PC link early: PC becomes camera+HMI during no-screen period */
#if CONFIG_APP_ENABLE_USB_LINK
    app_usb_link_init();
#else
    app_status_set(APP_MOD_USB, true, "disabled");
#endif

    /* 3. display + touch + LVGL status page (skip during no-screen period) */
#if CONFIG_APP_ENABLE_DISPLAY
    app_display_init();
#else
    ESP_LOGI(TAG, "display disabled (PC-as-HMI mode)");
#endif

    /* 4. peripherals; each module updates its own status tile */
    app_sdcard_init();

#if CONFIG_APP_ENABLE_ETH
    app_eth_init();
#if CONFIG_APP_ENABLE_NET_LINK
    app_netlink_init(CONFIG_APP_NETLINK_PORT);   /* JCLINK over TCP，TCP 通时优先走网线 */
#endif
#else
    app_status_set(APP_MOD_ETH, true, "disabled");
#endif

#if CONFIG_APP_ENABLE_WIFI
    app_wifi_init();
#else
    app_status_set(APP_MOD_WIFI, true, "disabled");
    ESP_LOGW(TAG, "wifi disabled (esp-hosted host/CP fw mismatch: see HANDOVER)");
#endif

    /* BLE 与 WiFi 共用 C6/esp-hosted 链路（HCI over SDIO），必须在 wifi 之后 */
#if CONFIG_APP_ENABLE_BLE
    app_ble_init();
#else
    app_status_set(APP_MOD_BLE, true, "disabled");
#endif

#if CONFIG_APP_ENABLE_AUDIO
    app_audio_init();
#else
    app_status_set(APP_MOD_AUDIO, true, "disabled");
#endif

    app_rs485_init();
    app_battery_init();
#if CONFIG_APP_ENABLE_DISPLAY
    app_display_set_battery_provider(app_battery_get_percent);
#endif

#if CONFIG_APP_ENABLE_CAMERA
    app_camera_init();
#else
    app_status_set(APP_MOD_CAM, true, "disabled");
#endif

    /* 5. image pipe: JPEG hw decode -> RGB888 frames (PC link source; app_faceai follows) */
#if CONFIG_APP_ENABLE_IMG_PIPE
    app_imgpipe_init();
#else
    app_status_set(APP_MOD_PIPE, true, "disabled");
#endif

    /* 5b. image stream: pipeline frame -> hw JPEG -> JCLINK 0x05 (PC/LVGL UI preview) */
#if CONFIG_APP_ENABLE_STREAM
    app_stream_init();
#else
    app_status_set(APP_MOD_STREAM, true, "disabled");
#endif

    /* 6. edge AI on top of the pipe (models load inside its own task) */
#if CONFIG_APP_ENABLE_FACEAI && CONFIG_APP_ENABLE_IMG_PIPE
    app_faceai_init();
#else
    app_status_set(APP_MOD_FACE, true, "disabled");
#endif

    ESP_LOGI(TAG, "all modules launched");
}
