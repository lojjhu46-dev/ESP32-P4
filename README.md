# ESP32-P4 边缘人脸识别设备（jc_p4_app）

Guition **JC-ESP32P4-M3-DEV**（ESP32-P4 @360MHz + ESP32-C6 Wi-Fi6/BLE，32MB PSRAM / 16MB flash）
上的**边缘人脸识别终端**：板载 SC2336 摄像头采集 → 图像管道（AE/AWB/CCM/夜视阶梯/时域降噪）
→ esp-dl 人脸检测 + 特征 + 1:N 识别 → JCLINK 协议回传（结果 JSON + 硬件 JPEG 预览），
PC 端 LVGL SDL 模拟器 / 取景中枢显示与操作。

> **接手/继续开发请先读** [`jc_p4_app/docs/HANDOVER.md`](jc_p4_app/docs/HANDOVER.md)：
> 全部技术结论、踩坑、验收数据、目录约定都在那里；本 README 只做导览与**依赖获取说明**。
>
> 🔴 **本工作区是 H.264 重构的起点**：方向、框架规划、阶段计划（M0~M4）、预算表、风险登记册、
> 重构护栏、开工 checklist 全在 [`docs/H264重构交接与阶段计划.md`](docs/H264重构交接与阶段计划.md)。**先读它再动手。**

---

## ⚠️ 未入库的依赖与文件（**克隆后必须自备，否则无法 configure / build**）

这些条目体积大或属第三方（其中 2 个超过 GitHub 100MB 硬限制），已从 git 历史剔除，
**本地磁盘上仍保留**（不影响本机开发）。按下表补齐：

| 缺失项 | 为什么不在库里 | 如何获取 |
|---|---|---|
| `esp-dl/` | 第三方组件仓库，本地打过 patch（约 500MB） | `git clone --depth 1 -b v3.3.11 https://github.com/espressif/esp-dl.git`，再**按 `jc_p4_app/docs/HANDOVER.md` 第廿二节 15 节的 patch 登记表重打 4 个文件**（`zero_workspace/poison_workspace/run_trace/module_name` + Feat trace 透传） |
| `esp-adf-libs/` | 第三方仓库，只需其中 `esp_new_jpeg` | `git clone --depth 1 --filter=blob:none --sparse https://github.com/espressif/esp-adf-libs.git` 然后 `git sparse-checkout set esp_new_jpeg` |
| `lvgl/` | 第三方组件源码（约 200MB） | `git clone -b v9.6.0 --depth 1 https://github.com/lvgl/lvgl.git lvgl`（⚠️ 版本以 `jc_p4_app/dependencies.lock` 中的 `lvgl/lvgl: version` 为准 = **9.6.0**；⚠️ 目录名必须叫 `lvgl`——`override_path` 要求目录名等于组件名。本地**无 patch**，可放心重新克隆） |
| `jc_p4_app/managed_components/` | 组件管理器官家目录，可再生成 | `idf.py reconfigure` 会按 `jc_p4_app/main/idf_component.yml` 自动拉取（registry 可达；离线方案见 HANDOVER 第三节） |
| `C3982-JC-ESP32P4-M3-DEV/` | 板卡厂商资料包（规格书/原理图/Demo/烧录工具，502MB，含 144MB rar） | 向板卡厂商索取；**本仓库不含任何厂商资料**。硬件结论已整理进 [`docs/JC-ESP32P4-M3-DEV-开发要点.md`](docs/JC-ESP32P4-M3-DEV-开发要点.md) |
| `lvgl_project/.../lvgl`、`build/`、`bin/` | 模拟器的第三方库与构建产物 | 该工程自带 `.gitmodules`：`git submodule update --init`；再 `cmake -B build -S . && cmake --build build -j8`（同源 UI 源码 `main/` **已在库内**） |
| `jc_p4_app/build/` | 编译产物 | `idf.py build` 生成 |
| `_dev/`、`jc_p4_app/_dev/`、`_old_managed/` | 开发期零散产物 / 旧组件副本 | 不需要（见 HANDOVER 第 6 节目录约定） |

> ✅ **已在库内、无需另取**：`esp_video/`、`esp_cam_sensor/`（本地化并打过 patch 的组件，
> 工程 configure 必需）、`jc_p4_app/components/jc_lcd_jd9365/`、`pc_host/` 全部脚本。
> `esp-dl/` 与 `esp-adf-libs/` 在父仓库中仅作为 gitlink（子模块指针）存在。

---

## 仓库内容

| 目录 | 内容 |
|---|---|
| `jc_p4_app/` | **固件工程**（ESP-IDF v5.5.5）：`main/` 应用源码、`docs/` 文档、`tools/` 脚本、`components/` |
| `pc_host/` | PC 侧 Python 工具：`protocol.py`（JCLINK 库）、`board_hub.py`（取景中枢）、`jc_test.py`（CLI）、`thr_calib.py`（阈值标定）、各回归脚本 |
| `qt_host/` | **C++/Qt6 全功能上位机**（TCP + USB 双通路）：实时视频 + 检测框、名单/阈值/相机/F10 备份面板、截图；构建与用法见其 `README.md`（Qt 6.12 mingw_64，`build.ps1` 一键构建） |
| `docs/` | 硬件文档：`JC-ESP32P4-M3-DEV-开发要点.md`（引脚与硬件结论）、`项目框架构建指南.md` |
| `esp_video/`、`esp_cam_sensor/` | 本地化 + patched 的 IDF 组件（`override_path` 依赖，**不可移动**） |
| `lvgl_project/` | WSL LVGL 模拟器工程（只保留自研 `main/`、FreeRTOS 与配置） |

---

## 快速上手

```powershell
# 0) 环境：ESP-IDF v5.5.5（EIM 安装）
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
# 1) 按上表补齐依赖（esp-dl / esp-adf-libs / lvgl / managed_components ...）
# 2) 编译 + 烧录（COM4 = CH340；中文 Windows 需先 chcp 65001 + PYTHONIOENCODING=utf-8）
cd jc_p4_app
idf.py reconfigure      # 改过 Kconfig 必须跑
idf.py build
idf.py -p COM4 flash
```

```bash
# 3) PC 侧：取景中枢（独占板端连接 + 本机 13333 代理 + 常驻取景窗口）
python pc_host/board_hub.py --exit-after 7200 --zoom 2
#    中枢在跑时，其它脚本一律连本机端口（板子一次只服务一个 TCP 客户端）
python pc_host/jc_test.py --tcp 127.0.0.1:13333 list
python pc_host/_face_r1_test.py --tcp 127.0.0.1:13333        # 识别回归 25 项
python pc_host/_f10_test.py   --tcp 127.0.0.1:13333          # 库备份/恢复验收 12 项
```

```bash
# 4) LVGL 模拟器（WSL；与板端同源 UI：main/src/ui_shared.c）
cd lvgl_project/lv_port_pc_vscode_official
cmake -B build -S . && cmake --build build -j8
./bin/main --host 192.168.33.2 --port 3333
```

---

## 能力速览（全部真机验证）

- **成像**：白天 50fps + AE/AWB/厂商标定 CCM + 曝光余量阶梯（50→20→10fps）+ AE 曝光上限 40ms；
  夜视暗度驱动降帧（5fps）+ 时域降噪 + 低光增强（gamma/BF），低光照自动停人脸识别
- **识别**：多帧注册（质量门控）、K 帧确认、双阈值 0.61/0.61 + 单帧高分快速通道 0.80、
  事件去重、多人脸、轻跟踪节流；**F9 名表 64 人**（`/storage/names.json` + NVS 兜底）；
  **F10 特征库备份/恢复**（`jc_test.py db_backup/db_restore`，JCLINK 0x06）
- **连接**：网口 `192.168.33.2:3333`（JCLINK over TCP，单客户端）、WiFi 运行时配网、BLE 广播 "JC-P4"
- **协议**：JCLINK v1，帧类型 0x01~0x06，见 [`jc_p4_app/docs/usb_link_protocol.md`](jc_p4_app/docs/usb_link_protocol.md)
- ⚠️ **已知限制**：esp-dl 特征提取在 P4 上存在执行级非确定性（跨开机不可复现）——
  绝对阈值不可移植，需每 boot 相对标定 + 多帧投票绕行。详见
  [`jc_p4_app/docs/esp-dl_issue_draft.md`](jc_p4_app/docs/esp-dl_issue_draft.md) 与 HANDOVER 廿二节 14/15 节

---

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/H264重构交接与阶段计划.md` | 🔴 **本阶段开工文档**：H.264 方向决策 / 已核实事实（文件:行号）/ 分层与协议 0x07 设计 / M0~M4 与验收阈值 / PSRAM 预算 / 风险登记 / 重构护栏 / checklist |
| `jc_p4_app/docs/HANDOVER.md` | ★ 主文档：技术结论 / 踩坑 / 验收记录 / **目录约定（第 6 节）** |
| `jc_p4_app/docs/face_recog_day_plan.md` | 人脸识别功能规划（P0~P3 + 三轮实施）与进度 |
| `jc_p4_app/docs/usb_link_protocol.md` | JCLINK 协议规范（两端共用） |
| `jc_p4_app/docs/pc_link_references.md` | esp-dl / tinyusb / jpeg / lv_port 技术核实记录 |
| `jc_p4_app/docs/esp-dl_issue_draft.md` | esp-dl 非确定性 issue 草稿（⏳ 待提交 Espressif） |
| `jc_p4_app/README.md` | 固件工程导览（模块表 / 运行时命令 / 硬性纪律） |
| `qt_host/README.md` | Qt 上位机：依赖/构建/连接方式/快捷键/无界面自检/**USB 实测数据** |
| `docs/JC-ESP32P4-M3-DEV-开发要点.md` | 板卡硬件要点与引脚表（不依赖厂商资料包） |
| `docs/项目框架构建指南.md` | 从零搭同类工程的步骤 |
