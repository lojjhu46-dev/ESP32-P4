# jc_p4_app - JC-ESP32P4-M3-DEV framework starter

Modular starter project for the Guition JC-ESP32P4-M3-DEV board
(ESP32-P4 @360MHz + ESP32-C6 Wi-Fi6, 32MB PSRAM, 16MB flash).

## Modules (main/)

| file | function |
|---|---|
| board_pins.h | single source of truth for all pin assignments |
| app_i2c | shared I2C1 bus (SDA=7 SCL=8): touch/codec/backlight/camera SCCB |
| app_display | MIPI-DSI JD9365 800x1280 + backlight(I2C 0x45/0x96) + GT911 + LVGL9 status page |
| app_wifi | Wi-Fi via C6 (esp_wifi_remote/esp_hosted SDIO), scan + optional connect |
| app_sdcard | TF card SDMMC 4-bit, power via internal LDO VO4 |
| app_eth | RMII ethernet, IP101GRI PHY (MDC=31 MDIO=52 RST=51 CLK-in=50) |
| app_audio | I2S0 + ES8311 speaker + ES7210 mic, startup beep |
| app_rs485 | UART1 half-duplex echo + heartbeat (TX=26 RX=27) |
| app_battery | GPIO53 ADC gauge task |
| app_camera | esp_video V4L2 capture FPS stats (OV5647 RAW8 800x800 demo format) |
| app_status | thread-safe status store rendered by the LVGL page |

## Build

```powershell
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
cd D:\esp32project\ESP32-P4\jc_p4_app
idf.py set-target esp32p4   # first time only
idf.py build
idf.py -p COMx flash monitor
```

## Configure

`idf.py menuconfig` -> "JC P4 App Configuration":

- APP_WIFI_SSID / APP_WIFI_PASSWORD (empty = scan only)
- APP_ENABLE_AUDIO / APP_ENABLE_ETH / APP_ENABLE_CAMERA toggles

## Notes

- MIPI DSI PHY must be powered first: LDO VO3 @2.5V (handled in app_display).
- Backlight is NOT a PWM gpio: write I2C addr 0x45 reg 0x96 (0..255).
- Wi-Fi requires the C6 slave firmware (factory flashed, Burn files/JC1060P470_C6.bin).
- Do not reuse GPIO35 (BOOT/RMII_TXD1) or GPIO26/27 (RS485).