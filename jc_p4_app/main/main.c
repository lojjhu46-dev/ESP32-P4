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
#include "app_h264enc.h"
#include "app_camauto.h"
#include "app_ai_detect.h"
#include "app_yuv2rgb.h"
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
    /* 预热 newlib stderr 锁：assert 走 stderr，若等到运行期内部 RAM 碎片化后
       才第一次初始化该锁，xQueueCreateMutex 失败会先 abort，把真正的 assert
       信息吞掉（stream off 实测）。这里趁堆干净先建好。 */
    fprintf(stderr, "stderr lock ready\n");
    fflush(stderr);
    esp_chip_info_t chip_info = { 0 };
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip: %s rev%d, flash 16MB, PSRAM 32MB hex@200M",
             CONFIG_IDF_TARGET, chip_info.revision);

    nvs_init_safe();
#if CONFIG_APP_ENABLE_ETH || CONFIG_APP_ENABLE_WIFI
    netif_init_safe();
#endif

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
#if CONFIG_APP_ENABLE_SDCARD
    app_sdcard_init();
#else
    app_status_set(APP_MOD_SD, true, "disabled");
#endif

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

    /* 5. image pipe: JPEG 解码引擎要在相机/H.264 启动前初始化（硬件共存时序） */
#if CONFIG_APP_ENABLE_IMG_PIPE
    app_imgpipe_init();
#if CONFIG_APP_CAM_OUT_FMT_YUV420
    /* 相机已在 ISP 做 HFLIP + 固定 WB，imgpipe 不能再做一次；
       这些调用会走 USB 日志，必须在 H.264 开始推流前完成。 */
    app_imgpipe_set_hflip(false);
    app_imgpipe_set_wb_gain_quiet(1000, 1000);
    app_imgpipe_select_src(APP_IMG_SRC_CAM);
#endif
#else
    app_status_set(APP_MOD_PIPE, true, "disabled");
#endif

#if CONFIG_APP_ENABLE_CAMERA
    /* 帧回调必须在 app_camera_init() 之前注册：相机任务一起来就会出帧 */
#if CONFIG_APP_CAM_OUT_FMT_YUV420
    app_h264enc_register();
    app_camauto_register();      /* Y 平面 AE：帧回调 + tick，须在相机启动前 */
#if CONFIG_APP_ENABLE_IMG_PIPE
    app_yuv2rgb_register();      /* YUV420 → RGB565 → imgpipe（faceai 送帧） */
#endif
#if CONFIG_APP_AI_DETECT
    app_ai_detect_register();    /* 抽帧检测：同样须在相机启动前注册 */
#endif
#if !CONFIG_APP_ENABLE_FACEAI
    /* FACEAI 关闭时它不注册命令回调，这里接管最小 stream 命令 */
    app_usb_link_set_cmd_cb(app_h264enc_cmd_cb);
#endif
#endif
    app_camera_init();
#if CONFIG_APP_ENABLE_FACEAI
    /* 相机/H.264 设备创建完成后再挂 /storage（提前挂载实测会让 H.264 设备创建失败） */
    app_faceai_storage_init();
#endif
#if CONFIG_APP_ENABLE_IMG_PIPE && CONFIG_APP_CAM_OUT_FMT_YUV420
    app_yuv2rgb_start();         /* 相机就绪后把 imgpipe 源切回 cam 并开始送帧 */
#endif
#else
    app_status_set(APP_MOD_CAM, true, "disabled");
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
