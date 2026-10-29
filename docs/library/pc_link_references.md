# PC 上位机 × ESP32-P4 边缘 AI：GitHub 开源参考案例核实报告
>
> 核实方式：浏览器实抓 GitHub 仓库页 + 组件注册表 + 本地 IDF v5.5.5 例程验证（2026-09-28）
> 结论：**所有计划依赖的开源组件均已核实可用**，其中两项官方例程就在本地 IDF 中。

## 1. espressif/esp-dl —— 板端边缘 AI（核心）✅
| 项目 | 核实结果 |
|---|---|
| 仓库 | https://github.com/espressif/esp-dl |
| P4 支持 | ✅ 明确支持（IDF ≥ v5.3，本工程 v5.5.5 满足）；最新 v3.3.11+ 已有 P4 per-channel 量化优化 |
| 人脸模型 | ✅ models/human_face_detect（检测）+ models/human_face_recognition（特征提取+比对） |
| 例程 | ✅ examples/human_face_detect、examples/human_face_recognition（**本计划 app_faceai 的直接模板**） |
| 其他模型 | pedestrian_detect、person_reid、hand_gesture_recognition、speaker_verification、pp_ocr_v6 等 |
| 组件依赖 | espressif/esp-dl（组件注册表），模型同样以组件形式分发 |
> 注意：v3 系 API 与 v2 差异大，实现时以组件源码 + human_face_recognition 例程为准。

## 2. USB CDC 设备栈 —— esp_tinyusb ✅（本地已验证）
| 项目 | 核实结果 |
|---|---|
| 组件 | espressif/esp_tinyusb ^2.0.0（IDF 组件注册表） |
| P4 支持 | ✅ 本地 examples/peripherals/usb/device/tusb_serial_device/README.md 明确列出 **Supported Targets: ESP32-P4** |
| 本地例程 | D:\ESP-IDF\v5.5.5\esp-idf\examples\peripherals\usb\device\ 下：**tusb_serial_device（CDC）**、tusb_composite_msc_serialdevice、tusb_console、tusb_hid、tusb_ncm 等 |
| CDC 能力 | menuconfig 可配 CDC 通道数；P4 USB0 = OTG HS 480Mbps |
| 参考用法 | 例程 tusb_serial_device/main/usb_descriptors.c + tusb_serial_device.c 即 app_usb_link 的模板 |
## 3. JPEG 硬解码 —— esp_new_jpeg ✅
- ESP32-P4 内置硬件 JPEG 编解码器，驱动组件 espressif/esp_new_jpeg（IDF 组件注册表）
- 解码输出 RGB888/RGB565，直接对接 AI 预处理（缩放到模型输入尺寸）

## 4. lvgl/lv_port_pc_vscode —— PC 端 LVGL 模拟器 ✅（本地已有）
- 官方仓库：https://github.com/lvgl/lv_port_pc_vscode（CMake + SDL2 + LVGL 9）
- **本机已克隆并编译**：D:\esp32project\ESP32-P4\lvgl_project\lv_port_pc_vscode_official（WSL 下 ./bin/main 可直接启动）
- 用途：PC 上位机 UI 宿主；ui_shared 共享 UI 代码同时编译进该工程与板端工程

## 5. espressif/esp-who（备选参考）✅
- 老牌人脸检测仓库（ESP32-S3 为主），其 HTTP 服务器上传图像 + 浏览器显示模式可借鉴
- P4 上已被 esp-dl v3 + esp_video 取代，仅作上位机交互模式参考

## 6. 其他相关（按需查阅）
| 仓库 | 说明 |
|---|---|
| espressif/esp-webrtc-solution | P4 WebRTC 推流（浏览器直看） |
| espressif/esp_cam_sensor | 已在工程中（SC2336 就绪），到货后切换 V4L2 输入源 |
| espressif/esp-iot-solution | USB 外设合集（vendor class、UVC），640×480 高帧率时参考 |

## 结论与选型落地
```
板端 AI     : esp-dl (human_face_detect + human_face_recognition 例程为模板)
USB 通道    : esp_tinyusb ^2.0.0 + 本地 tusb_serial_device 例程模板（P4 已验证）
JPEG 解码   : espressif/esp_new_jpeg（P4 硬件编解码器）
PC 模拟器   : 本地 lv_port_pc_vscode_official（已编译可运行）
```