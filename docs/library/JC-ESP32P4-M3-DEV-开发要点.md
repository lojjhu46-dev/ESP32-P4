# JC-ESP32P4-M3-DEV 开发板要点整合

> 依据资料：`D:\esp32project\ESP32-P4\C3982-JC-ESP32P4-M3-DEV\JC-ESP32P4-M3-DEV` 下的规格书、入门手册、8 张原理图、IDF/Arduino Demo、烧录说明整理。
> 整理日期：2026-09-28

---

## 1. 板卡概述

| 项目 | 参数 |
|---|---|
| 模组 | JC-ESP32P4-M3（SoC: ESP32P4-NRW32，叠封 32MB PSRAM） |
| 主控 | ESP32-P4，双核 RISC-V，最高 360MHz（带 AI 指令扩展、单精度 FPU） |
| 无线 | **P4 本身无 Wi-Fi/BT**，由板载 ESP32-C6 提供 Wi-Fi 6 + BLE 5（经 ESP-Hosted，SDIO） |
| 存储 | 16MB NOR Flash（模组内置）、768KB HP L2MEM、32KB LP SRAM、128KB HP ROM |
| 接口 | MIPI-DSI（2 lane）、MIPI-CSI（2 lane）、USB OTG HS（USB0）+ FS（USB1）、SDIO 3.0、RMII 以太网、RS485 |
| 尺寸 | 92 × 62 mm（模组区域 62 × 56.4 mm） |
| 供电 | 5V（≥500mA），内置 IPS5305 锂电池充放电管理（CN4 电池座） |
| 工作温度 | -20℃ ~ +70℃ |

## 2. 核心外设与芯片（原理图汇总）

### 2.1 显示（MIPI-DSI）
- 屏：微雪 **10.1-DSI-TOUCH-A**（800×1280，竖向），驱动 IC **JD9365**（Demo 内含 `esp_lcd_jd9365_10_1` 驱动源码）
- 数据通道：**2 lane** DSI；像素格式 RGB565；刷新 60Hz
- MIPI DSI PHY 供电：P4 内部 LDO **VO3 通道 2.5V**（`esp_ldo_acquire_channel(chan_id=3, 2500mV)`）
- LCD_RST / LCD_LED：**未接 GPIO**（RST 上拉到 VCC；背光由屏上 I2C 背光 IC 控制）
- 背光调节：I2C 总线 1，器件地址 **0x45**，寄存器 **0x96**，写 0~255 PWM 值
- 触摸：**GT911**（I2C 地址 0x14/0x5D），无 RST/INT 引脚（上拉处理）；部分屏为 GSL3680（test 工程附带 `esp_lcd_touch_gsl3680` 组件）

### 2.2 I2C 总线（一条总线，两个入口）
- 常规入口：**I2C1，SDA=GPIO7，SCL=GPIO8**（Arduino/IDF demo 统一用 bus 1）
- 备用入口：并联至 P4 LP I2C1 引脚（网络名 `RTC_DAT/SDA1`、`RTC_CLK/SCL1`）
- 挂载器件：ES8311（codec）、ES7210（麦阵）、GT911（触摸）、背光 IC(0x45)、CSI 摄像头 SCCB
- 总线上已有 2.2K/5.1K 上拉，内部上拉可不开

### 2.3 音频
| 信号 | GPIO |
|---|---|
| I2S0 MCLK | GPIO13 |
| I2S0 SCLK (BCLK) | GPIO12 |
| I2S0 LRCK (WS) | GPIO10 |
| I2S0 DSDIN（P4→codec 播放） | GPIO9 |
| ES7210 ASDOUT（麦克风→P4） | GPIO48 |
| 功放使能 PA_CTRL | GPIO11 |
- DAC 链路：ES8311 DAC_OUT_P/N → **NS4150** D 类功放 → CN1 扬声器座
- 麦克风：MSM381A（PDM/模拟）→ ES7210（I2C 配置）
- 音量/静音经 ES8311 I2C 控制；`bsp_board_extra.c` 提供参考实现

### 2.4 以太网（RMII + IP101GRI）
| 信号 | GPIO | | 信号 | GPIO |
|---|---|---|---|---|
| RMII_TXD0 | 34 | | RMII_RXD0 | 29 |
| RMII_TXD1 | 35 | | RMII_RXD1 | 30 |
| RMII_TXEN | 49 | | RMII_RXDV | 28 |
| MDC | 31 | | MDIO | 52 |
| PHY_RSTN | 51 | | RMII_CLK(50M) | 50 |
- PHY：IP101GRI，25MHz 晶振，PHY_3V3 供电；strap 引脚 PHY_AD0/AD3
- Demo：`IDF-DEMO/NoDisplay/ethernet/basic`（还支持 EthToAP，见 withDisplay/test）

### 2.5 TF 卡（SDMMC，4-bit）
- 使用 P4 SDMMC **slot 1 默认引脚**：CLK=GPIO43, CMD=GPIO44, D0=39, D1=40, D2=41, D3=42
- 卡电源由 P4 内部 **LDO VO4** 输出经 AO3401 开关控制（`ESP_LDO_VO4`）
- BSP 中 `bsp_sdcard_mount()` 可直接参考

### 2.6 USB
| 电路 | 说明 |
|---|---|
| High Speed USB（USB0 OTG） | Type-C（USB3 座），`ESP_USB_P/N` → P4 USB0 D+/D-，支持 UVC/UAC 等高速外设 |
| Full Speed USB（USB1） | Type-C（USB2 座），`USB1P_P/N` → P4 USB1 D+/D- |
| USB-TTL（CH340C） | Type-C → CH340C → **P4 UART0（TXD=GPIO37，RXD=GPIO38）**，DTR/RTS 经三极管接 **BOOTMODE(GPIO35)** 和 **CHIP_PU**，实现自动下载 |

> 串口烧录/日志口 = CH340C 这一路（USB-TTL 座）；CN2（MX1.25-4P）为备用调试串口+5V 输入口。

### 2.7 RS485
- 收发器 **MAX485**，接 P4 **UART1：TXD=GPIO26，RXD=GPIO27**
- 半双工由硬件（74LVC1G132+8550）自动方向控制，**无需 RTS**
- Demo：`uart_echo_rs485`（115200-8N1，`UART_MODE_RS485_HALF_DUPLEX`）

### 2.8 电池与 ADC
- 充电管理：IPS5305（USB5V_IN → BAT+，CN4 电池接口，带 LED 指示）
- 电池电量检测：BAT+ 经 R62(68K)/R57(100K) 分压 → **GPIO53**（ADC）
- Demo `battery_adc_test` 用 **ADC2_CH4** 读数并换算电量（V_C_MAX=2500mV / V_C_MIN=2250mV）

### 2.9 按键与指示
- BOOT：GPIO35（按下接 GND）
- RST：CHIP_PU（模组复位）
- 电源指示 LED（VDDA）；充电指示（IPS5305 LED1-3）

### 2.10 P4 ↔ C6 互联（重点）
| 连接 | 引脚（P4 侧） | 说明 |
|---|---|---|
| SDIO 数据 | CMD=GPIO19, CLK=GPIO18, D0=14, D1=15, D2=16, D3=17 | ESP-Hosted 传输（4-bit, 40MHz） |
| UART 调试/通信 | GPIO12 → C6 RX，GPIO13 ← C6 TX | C6 日志/透传 |
| C6 复位/使能 | GPIO54（C6_CHIP_PU，高有效） | ESP-Hosted `SLAVE_RESET=54` |
| C6_BOOT | GPIO6（C6_IO2） | C6 下载模式控制 |
| JP1 扩展口 | C6_U0RXD / C6_U0TXD / C6_IO9 / C6_CHIP_PU | 可直接操作 C6 |

- Wi-Fi 软件栈：**esp_wifi_remote + esp_hosted**（IDF 组件），C6 端需烧录 slave 固件（板子出厂已带，`8-Burn operation/Burn files/JC1060P470_C6.bin`）
- C6 固件由厂商预烧录，一般无需重烧；如需重烧走 JP1 的 C6 串口 + C6_IO2/CHIP_PU 控制下载模式

### 2.11 摄像头（MIPI-CSI，2 lane）
- J3 座（15-pin）：CSI_A_DATA0/1 ±、CSI_A_CLK ±、CSI_IO0/IO1、SCCB 共用 I2C1（GPIO7/8）
- 软件：**esp_video（V4L2 接口）** + `esp_ipa`；Demo `uvc_camera_ov5647`（OV5647 sensor）、test 工程实时取流

### 2.12 背光/其他
- LCD 背光恒流驱动 MP3202（EN 由 LCD_PWM 控制）——但 10.1 DSI 屏背光实际走屏上 I2C IC（0x45/0x96）
- JP1 扩展排针（2×13）：GPIO1/2/3/4/5/10/20/32/33/45/46/47、ES_I2C_SDA/SCL、C6_U0RXD/TXD、C6_IO9、C6_CHIP_PU、VCC5V/VCC3V3/GND
- 板上另有 GPIO20(TOUCH_INT)、GPIO21(TOUCH_RST)、GPIO23(LCD_PWM) 预留网络

## 3. 软件与开发环境

### 3.1 ESP-IDF（推荐，资料最全）
- **官方测试版本：ESP-IDF v5.5**（本机已装 v5.5.5 @ `D:\ESP-IDF\v5.5.5\esp-idf`，直接可用）
- 关键 sdkconfig：
  ```
  CONFIG_IDF_TARGET="esp32p4"
  CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
  CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
  CONFIG_PARTITION_TABLE_CUSTOM=y
  CONFIG_SPIRAM=y
  CONFIG_SPIRAM_SPEED_200M=y
  ```
- Wi-Fi 依赖组件：`espressif/esp_wifi_remote ^0.14.2` + `espressif/esp_hosted ^2.x`（SDIO, slave=esp32c6, RESET=GPIO54）
- 显示：`esp_lcd_mipi_dsi` + JD9365 驱动（Arduino demo 里有移植好的 `esp_lcd_jd9365_10_1.c/h`，可直接拷入 IDF 工程）
- 摄像头：`espressif/esp_video ^0.9.1`（V4L2）、`espressif/esp_ipa`
- 音频：`esp_codec_dev` + ES8311/ES7210 组件
- LVGL：Demo 含 v8.4 与 v9.2 两个版本（`lvgl_demo_v8` / `lvgl_demo_v9`；`lvgl_sw_rotation` 演示软件旋转 + avoid tearing）

### 3.2 Arduino（快速验证）
- Arduino IDE ≥ 2.3.4，Arduino-ESP32 core **v3.2**，Boards Manager URL：
  `https://espressif.github.io/arduino-esp32/package_esp32_dev_index.json`
- 板型：**ESP32P4 Dev Module**；配置：CPU 360MHz / Flash 80MHz QIO / 16MB / PSRAM Enabled / Partition "16M Flash (3MB APP/9.9MB FATFS)" / Upload "UART0/Hardware CDC" / 921600
- Demo：`jc_esp32p4_m3_lvgl_v8`（LVGL 8.4 + JD9365 + GT911，`pins_config.h`：TP_I2C_SDA=7, TP_I2C_SCL=8）、`Wifi_scan`、`adc_test`、`mp3_player`、`lvgl_v9_sw_rotation`

### 3.3 烧录
1. PC 装 **CH340 驱动**；供电 ≥500mA
2. `idf.py -p COMx flash monitor`（CH340 口，自动进入下载模式）
3. 或用 `flash_download_tool`：ChipType=**ESP32-P4**，WorkMode=Develop，LoadMode=**UART**；测试固件 `JC-ESP32P4-M3-DEV-TEST-V1.3.bin` @ **0x0000**，SPI QIO / 80MHz / 115200
4. 烧完按 RST 或重新上电
5. C6 固件 `JC1060P470_C6.bin` 出厂已烧（C6 flash 4MB：bootloader@0x2000、分区表@0x8000、app@0x10000、4MB 数据@0x91000）

## 4. 常用引脚速查表（P4 侧）

| 功能 | GPIO | 功能 | GPIO |
|---|---|---|---|
| I2C1 SDA / SCL | 7 / 8 | UART0 TXD / RXD | 37 / 38 |
| I2S0 MCLK/SCLK/LRCK | 13/12/10 | RS485 TXD / RXD (UART1) | 26 / 27 |
| I2S0 DOUT / DIN | 9 / 48 | 电池电压 ADC | 53 (ADC2_CH4) |
| 功放 PA_CTRL | 11 | SDMMC CLK/CMD | 43 / 44 |
| RMII TXD0/TXD1/TXEN | 34/35/49 | SDMMC D0~D3 | 39/40/41/42 |
| RMII RXD0/RXD1/RXDV | 29/30/28 | TF 卡电源 (LDO VO4 开关) | 经内部 LDO |
| MDC / MDIO / PHY_RST | 31/52/51 | C6 SDIO CMD/CLK | 19 / 18 |
| BOOT / RST | 35 / CHIP_PU | C6 SDIO D0~D3 | 14/15/16/17 |
| USB0 HS D+/D- | ESP_USB_P/N (USB0) | C6 复位 / C6_BOOT | 54 / 6 |
| USB1 FS D+/D- | USB1P_P/N (USB1) | C6 UART (P4↔C6) | 12→C6RX / 13←C6TX |
| 触摸 INT / RST 预留 | 20 / 21 | LCD_PWM 预留 | 23 |

> 注意：GPIO35 同时是 BOOT 脚与 RMII_TXD1，上电瞬间电平影响启动模式，以太网使用正常（复位后释放）；GPIO26/27 也是官方背光 PWM 示例常用脚，勿冲突。

## 5. Demo 资源索引（相对 `JC-ESP32P4-M3-DEV\1-Demo`）

| 路径 | 内容 | 适用 |
|---|---|---|
| `IDF-DEMO/withDisplay/lvgl_demo_v8|v9` | LVGL 官方 BSP 移植（注意 v9 README 基于 EV 板 7" 屏） | 移植参考 |
| `IDF-DEMO/withDisplay/test` | **本板综合测试**：LCD+TP(GSL3680/GT911)+SD+网口+ES8311+摄像头+RS485+WiFi | 本板主参考 |
| `IDF-DEMO/withDisplay/lvgl_sw_rotation` | LVGL 软件旋转 + avoid tearing | 竖屏横用 |
| `IDF-DEMO/NoDisplay/wifi_scan` | **Wi-Fi 扫描（esp_wifi_remote + esp_hosted，SDIO）** | 本板联网模板 |
| `IDF-DEMO/NoDisplay/ethernet/basic` | RMII 以太网上网 | 以太网 |
| `IDF-DEMO/NoDisplay/battery_adc_test` | 电池电压/电量 | 供电管理 |
| `IDF-DEMO/NoDisplay/uart_echo_rs485|uart_send_rs485` | RS485 半双工收发 | RS485 |
| `IDF-DEMO/NoDisplay/uvc_camera_ov5647` | OV5647 CSI 取流（esp_video/V4L2）+ UVC | 摄像头 |
| `IDF-DEMO/NoDisplay/mp3_player` | MP3 播放（ES8311+功放） | 音频 |
| `IDF-DEMO/xiaozhi-esp32` | 小智 AI 语音助手完整工程（含 P4 配置） | AI 应用 |
| `ARDUINO-DEMO/jc_esp32p4_m3_lvgl_v8` | 本板 LVGL8 + JD9365 + GT911（**含驱动源码**） | 屏幕驱动源 |
| `ARDUINO-DEMO/{Wifi_scan,adc_test,mp3_player,lvgl_v9_sw_rotation}` | Arduino 各功能验证 | 快速验证 |

## 6. 后续项目建议

1. **新工程模板**：复制 `NoDisplay/wifi_scan` 或 `withDisplay/test` 为模板；它们已含正确的 sdkconfig（16MB/QIO/PSRAM200M/自定义分区）与 esp_hosted SDIO 配置。
2. **屏幕方案**：直接复用 Arduino demo 的 `esp_lcd_jd9365_10_1` 驱动 + `i2c1(GPIO7/8)` 背光(0x45,reg0x96) + GT911 触摸；LVGL v8/v9 flush 回调参考 `jc_esp32p4_m3_lvgl_v8.ino`。
3. **Wi-Fi/BT**：所有无线操作按普通 esp_wifi API 写即可（esp_wifi_remote 透明转发到 C6），但要确认 C6 slave 固件存在（出厂已烧）。
4. **资源分配注意**：GPIO35 双职能（BOOT/RMII）、GPIO26/27 已被 RS485 占用、GPIO20/21 预留触摸、GPIO23 背光预留；扩展口引出的 GPIO1-5/10/20/32/33/45-47 可作通用 IO。
5. **MIPI DSI PHY**：初始化显示前必须 `esp_ldo_acquire_channel(3, 2500mV)` 给 PHY 供电。
6. **数据手册**：`4-Driver_IC_Data_Sheet/esp32-p4_datasheet_en.pdf`、`esp32-c6_datasheet_en.pdf` 可查外设寄存器/引脚复用细节。
