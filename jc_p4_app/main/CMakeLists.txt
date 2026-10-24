set(srcs
    "main.c"
    "app_status.c"
    "app_i2c.c"
    "app_display.c"
    "app_audio.c"
    "app_sdcard.c"
    "app_rs485.c"
    "app_battery.c"
    "app_camera.c"
    "app_usb_link.c"
    "app_imgpipe.c"
    "app_yuv2rgb.c"
    "app_stream.c"
    "app_camauto.c"
    "app_h264enc.c"
)

if(CONFIG_APP_ENABLE_ETH)
    list(APPEND srcs "app_eth.c")
endif()
if(CONFIG_APP_ENABLE_NET_LINK)
    list(APPEND srcs "app_netlink.c")
endif()

# app_faceai.cpp 只能在 FACEAI 开启时编译：它直接引用 APP_FACEAI_* 系列
# Kconfig 符号，而这些符号随 APP_ENABLE_FACEAI/IMG_PIPE 关闭会从 sdkconfig 消失。
if(CONFIG_APP_ENABLE_FACEAI)
    list(APPEND srcs "app_faceai.cpp")
endif()

if(CONFIG_APP_AI_DETECT)
    list(APPEND srcs "app_ai_detect.cpp")
endif()

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
    # ⚠️ REQUIRES 必须无条件列出：IDF 在独立 CMake 脚本里"早期展开"组件
    # CMakeLists 收集 REQUIRES/PRIV_REQUIRES（真实注册阶段传入的会被丢弃），
    # 而早期展开脚本里没有 CONFIG_* 变量 -> if(CONFIG_xxx) 内的 list(APPEND
    # requires ...) 从不生效（2026-10-04 BLE 实测：bt 头目录/链接全部缺失）。
    # 上面这行列表里的所有组件都必须在 BT/WIFI 关闭时也存在于构建图
    # （bt/esp_wifi 组件注册为空库；esp_wifi_remote/esp_hosted 经
    # idf_component.yml 始终在图内），否则 configure 报 component not found。
    esp_wifi_remote
    esp_wifi
    esp_hosted
    bt
    esp_mm        # esp_cache_msync（跨核缓存一致性，2026-10-04）
    esp_driver_ppa  # YUV420->RGB565（人脸检测送检帧）
    esp_rom       # esp_rom_crc32_le（帧指纹诊断）
)

set(includes ".")

# Wi-Fi (C6 + esp_hosted over SDIO slot1, 复位 GPIO54)。
# ⚠️ 版本按厂商 wifi_scan 模板钉死：esp_wifi_remote 0.14.2 + esp_hosted 2.0.13
# （与出厂 C6 从机固件 2.3.2 配套，3.0.x 宿主会 "major version mismatch"）。
# ⚠️ IDF 5.5.5 组合下，esp_wifi_remote 对 esp_wifi 的 injected 头注入
# （injected/esp_wifi.h 垫片，P4 无原生 wifi 时替换原生头）没有传播到 main 的
# 编译路径（实测 esp_wifi.h not found）——显式补上，垫片目录排在原生 include
# 之前优先命中。
if(CONFIG_APP_ENABLE_WIFI)
    list(APPEND srcs "app_wifi.c")
    # （requires 追加无效，esp_wifi_remote/esp_wifi 已无条件列入基础 requires；
    #   此处只处理 srcs 与 injected 头目录——INCLUDE_DIRS 走真实注册，追加有效）
    idf_component_get_property(wifi_remote_dir espressif__esp_wifi_remote COMPONENT_DIR)
    idf_component_get_property(esp_wifi_dir esp_wifi COMPONENT_DIR)
    list(APPEND includes "${wifi_remote_dir}/idf_v5.5/include/injected" "${esp_wifi_dir}/include")
endif()

# BLE (2026-10-04, 任务17)：NimBLE host 在 P4，BT controller 在 C6 从机，
# HCI 走 esp-hosted SDIO (VHCI)。bt/esp_hosted 已无条件列入基础 requires
# （见上文注释：CONFIG 包裹的 requires 追加会被早期展开丢弃）。
if(CONFIG_APP_ENABLE_BLE)
    list(APPEND srcs "app_ble.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    REQUIRES ${requires}
)
