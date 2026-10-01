set(srcs
    "main.c"
    "app_status.c"
    "app_i2c.c"
    "app_display.c"
    "app_audio.c"
    "app_sdcard.c"
    "app_eth.c"
    "app_rs485.c"
    "app_battery.c"
    "app_camera.c"
    "app_usb_link.c"
    "app_netlink.c"
    "app_imgpipe.c"
    "app_stream.c"
    "app_faceai.cpp"
)

set(requires
    nvs_flash
    driver
    esp_lcd
    esp_event
    esp_netif
    esp_eth
    lwip
    fatfs
    sdmmc
    esp_adc
    esp_timer
    heap
    esp_lvgl_port
    lvgl
    esp_codec_dev
    esp_video
    esp_cam_sensor
    jc_lcd_jd9365
    esp_tinyusb
    esp_driver_jpeg
    json
    wear_levelling
    esp-dl
    human_face_detect
    human_face_recognition
)

# Wi-Fi (C6 + esp_hosted over SDIO) is built only when enabled: the host driver
# in IDF 5.5 (esp_hosted 3.x) does not talk to the C6 slave fw 2.3.2 currently
# on the module, and esp_hosted's transport constructor asserts when the app
# does not complete its init. See docs/HANDOVER.md -> "C6 固件版本不匹配".
# NOTE: main/idf_component.yml has the matching espressif/esp_wifi_remote
# dependency commented out; restore both when the C6 fw is updated.
if(CONFIG_APP_ENABLE_WIFI)
    list(APPEND srcs "app_wifi.c")
    list(APPEND requires esp_wifi_remote esp_wifi)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES ${requires}
)
