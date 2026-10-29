# ESP32-P4 边缘人脸识别设备（jc_p4_app）

Guition **JC-ESP32P4-M3-DEV**（ESP32-P4 @360MHz + ESP32-C6 Wi-Fi6/BLE，32MB PSRAM / 16MB flash）
上的**边缘人脸识别终端**：板载 SC2336 摄像头采集 → 图像管道（AE/AWB/CCM/夜视阶梯/时域降噪）
→ esp-dl 人脸检测 + 特征 + 1:N 识别 → JCLINK 协议回传（结果 JSON + 硬件 H.264 码流 + 硬件 JPEG 预览），
PC 端 Qt6 上位机显示与操作；**可选云端旁路**：Qt 把识别到的签到事件、抓拍照片与板端人脸库快照
转发到 `cloud_server/`（人脸比对全部留在板端，**云端绝不比对特征**）。

> **接手/继续开发请先读** [`docs/library/HANDOVER.md`](docs/library/HANDOVER.md)：
> 全部技术结论、踩坑、验收数据、目录约定都在那里；本 README 只做导览与**依赖获取说明**。
>
> 🔴 **本工作区是 H.264 重构的起点**：方向、框架规划、阶段计划（M0~M4）、预算表、风险登记册、
> 重构护栏、开工 checklist 全在 [`docs/library/H264重构交接与阶段计划.md`](docs/library/H264重构交接与阶段计划.md)。**先读它再动手。**
>
> 🧭 **遇到现象先查** [`docs/library/嵌入式软件测试与修复实践手册.md`](docs/library/嵌入式软件测试与修复实践手册.md)：
> 44 个已修 bug 的分类入口 + 18 条判据 + 测试矩阵 + 硬件联调纪律。

---

## ⚠️ 未入库的依赖与文件（**克隆后必须自备，否则无法 configure / build**）

这些条目体积大或属第三方（其中 2 个超过 GitHub 100MB 硬限制），已从 git 历史剔除，
**本地磁盘上仍保留**（不影响本机开发）。按下表补齐：

| 缺失项 | 为什么不在库里 | 如何获取 |
|---|---|---|
| `esp-dl/` | 第三方组件仓库，本地打过 patch（约 500MB） | `git clone --depth 1 -b v3.3.11 https://github.com/espressif/esp-dl.git`，然后 **`git -C esp-dl apply -p1 ../patches/esp-dl-arena.patch`**（补丁已留档在 `patches/`，4 文件 / +191 行：`zero_workspace`/`poison_workspace`/`run_trace`/`module_name` + Feat trace 透传；说明与校验方法见 [`patches/README.md`](patches/README.md)） |
| `esp-adf-libs/` | 第三方仓库，只需其中 `esp_new_jpeg` | `git clone --depth 1 --filter=blob:none --sparse https://github.com/espressif/esp-adf-libs.git` 然后 `git sparse-checkout set esp_new_jpeg` |
| `lvgl/` | 第三方组件源码（约 200MB） | `git clone -b v9.6.0 --depth 1 https://github.com/lvgl/lvgl.git lvgl`（⚠️ 版本以 `jc_p4_app/dependencies.lock` 中的 `lvgl/lvgl: version` 为准 = **9.6.0**；⚠️ 目录名必须叫 `lvgl`——`override_path` 要求目录名等于组件名。本地**无 patch**，可放心重新克隆） |
| `jc_p4_app/managed_components/` | 组件管理器官家目录，可再生成 | `idf.py reconfigure` 会按 `jc_p4_app/main/idf_component.yml` 自动拉取（registry 可达；离线方案见 HANDOVER 第三节） |
| `C3982-JC-ESP32P4-M3-DEV/` | 板卡厂商资料包（规格书/原理图/Demo/烧录工具，502MB，含 144MB rar） | 向板卡厂商索取；**本仓库不含任何厂商资料**。硬件结论已整理进 [`docs/library/JC-ESP32P4-M3-DEV-开发要点.md`](docs/library/JC-ESP32P4-M3-DEV-开发要点.md) |
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
| `docs/library/` | **全部文档集中目录**（2026-10-29 迁移，逐文件 SHA-256 核对）：交接、修复记录、开发要点、构建指南 + `img/` 截图 |
| `jc_p4_app/` | **固件工程**（ESP-IDF v5.5.5）：`main/` 应用源码、`tools/` 脚本、`components/`（原 `jc_p4_app/docs/` 已并入 `docs/library/`）|
| `pc_host/` | PC 侧 Python 工具：`protocol.py`（JCLINK 库）、`board_hub.py`（取景中枢）、`jc_test.py`（CLI）、`thr_calib.py`（阈值标定）、各回归脚本 |
| `qt_host/` | **C++/Qt6 全功能上位机**（TCP + USB 双通路）：实时视频 + 检测框、名单/阈值/相机/F10 备份面板、截图、**「云」页**（签到事件与抓拍上报、人脸库上传/下发、心跳探活、断网补发）；构建与用法见其 `README.md`（Qt 6.12 mingw_64，`build.ps1` 一键构建） |
| `cloud_server/` | **可选云签到后端**（纯 Python 3.6 标准库 + SQLite WAL，无 pip/Nginx/Docker）：`app.py` 路由与鉴权、`db.py` DDL、`facebank.py` JCDF 快照解析（只读不比对）、`web/` 后台、`install.sh` 部署、`test_client.py` 验收。部署与运维见 [`cloud_server/SERVICE_GUIDE.md`](cloud_server/SERVICE_GUIDE.md) |
| `docs/` | 文档根：只含 `library/` 一个目录（11 份正文 + `img/` 24 张截图，2026-10-29 集中）|
| `esp_video/`、`esp_cam_sensor/` | 本地化 + patched 的 IDF 组件（`override_path` 依赖，**不可移动**） |
| `lvgl_project/` | WSL LVGL 模拟器工程（只保留自研 `main/`、FreeRTOS 与配置） |

---

## 快速上手

```powershell
# 0) 环境：ESP-IDF v5.5.5（EIM 安装）
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
# 1) 按上表补齐依赖（esp-dl / esp-adf-libs / lvgl / managed_components ...）
# 2) 编译 + 烧录（CH340 串口；中文 Windows 需先 chcp 65001 + PYTHONIOENCODING=utf-8）
#    ⚠️ 端口号会变（历史 COM4，H.264 工作区实测 COM3=烧录/日志、COM5=JCLINK 数据）
#    用 [System.IO.Ports.SerialPort]::GetPortNames() 现查，别写死
cd jc_p4_app
idf.py reconfigure      # 改过 Kconfig 必须跑
idf.py build
idf.py -p COM3 flash
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

```bash
# 5) 可选：云签到服务（本地起测试实例，18080 只监听 loopback，不碰生产云）
cp cloud_server/config.example.json /tmp/facecheck.local.json   # 改 token / cookie_secret / data_dir
python cloud_server/app.py --config /tmp/facecheck.local.json --port 18080
python cloud_server/test_client.py --url http://127.0.0.1:18080 --token <TOKEN>   # 服务端验收 23 项
#    Qt 侧：「云」页填 URL / Token / 设备名 → 勾「启用」→ 签到事件与抓拍开始上报；
#    名单（注册/删除）变化后自动上传人脸库快照，也可手动「上传/从云端同步到板端」。
#    生产部署：tar czf facecheck.tgz cloud_server → scp → bash /opt/cloud_server/install.sh
#    （运维、Token 位置、公网不通排障与测试实例见 cloud_server/SERVICE_GUIDE.md）
```

---

## 能力速览（全部真机验证）

- **成像**：白天 50fps + AE/AWB/厂商标定 CCM + 曝光余量阶梯（50→20→10fps）+ AE 曝光上限 40ms；
  夜视暗度驱动降帧（5fps）+ 时域降噪 + 低光增强（gamma/BF），低光照自动停人脸识别
- **识别**：多帧注册（质量门控）、K 帧确认、双阈值 0.61/0.61 + 单帧高分快速通道 0.80、
  事件去重、多人脸、轻跟踪节流；**F9 名表 64 人**（`/storage/names.json` + NVS 兜底）；
  **F10 特征库备份/恢复**（`jc_test.py db_backup/db_restore`，JCLINK 0x06）
- **传输**：硬件 H.264 编码支路（SC2336 YUV420 → `/dev/video11` M2M → JCLINK **0x07** Annex-B 码流，
  Qt 侧 openh264 解码），640×480@1Mbps 实测 20~24fps（同链路 JPEG 只有 8~11fps）
- **连接**：网口 `192.168.33.2:3333`（JCLINK over TCP，单客户端）、USB CDC（COM5）、WiFi 运行时配网、BLE 广播 "JC-P4"
- **云签到（可选旁路，v1 已公网真人实测验收）**：板端识别出 `match` → Qt 上报**签到事件 + 抓拍 JPEG**
  → 云端按 `person+device` 滚动去重（生产 **1800 s**，测试实例 10 s）→ Web 后台查询 / CSV（UTF-8 BOM）导出 / 照片下载；
  名单变化（注册/删除）后 Qt 自动把板端 `db_export` 的 **JCDF 人脸库快照**上传（每设备保留最近 5 版，可按 `?id=` 取回），
  也可从云端下发覆盖板端库（板端导入前自动备份 `face_bak.db`/`names.bak`）。**云端不保存比对逻辑、不比对特征**。
  详见 [`cloud_server/README.md`](cloud_server/README.md) 与 [阶段计划 §15](docs/library/H264重构交接与阶段计划.md)
- **协议**：JCLINK v1，帧类型 0x01~0x07（0x06=库分片导出/导入，0x07=H.264 码流），见 [`docs/library/usb_link_protocol.md`](docs/library/usb_link_protocol.md)
- ⚠️ **已知限制**：esp-dl 特征提取在 P4 上存在执行级非确定性（跨开机不可复现）——
  绝对阈值不可移植，需每 boot 相对标定 + 多帧投票绕行。详见
  [`docs/library/esp-dl_issue_draft.md`](docs/library/esp-dl_issue_draft.md) 与 HANDOVER 廿二节 14/15 节
- ⚠️ **云端待办**：注册后的自动同步由单条 best-effort `registered` 结果帧触发，USB 拥塞时该帧可能丢失，
  表现为"注册成功但云端库没更新且无告警"——注册后请确认日志里有 `☁ 名单已变化`，否则手动点一次「上传人脸库」
  （决策与三个候选修法见 [阶段计划 §15.10 遗留 5](docs/library/H264重构交接与阶段计划.md)）

---

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/library/README.md` | 文档集中目录的索引与路径约定（改文档前先看） |
| `docs/library/嵌入式软件测试与修复实践手册.md` | 🧭 **排障入口**：44 个已修 bug 的现象/取证/根因/修复/验收 + 18 条判据 + L0~L8 测试矩阵 + 硬件联调纪律 |
| `docs/library/H264重构交接与阶段计划.md` | 🔴 **本阶段开工文档**：H.264 方向决策 / 已核实事实（文件:行号）/ 分层与协议 0x07 设计 / M0~M4 与验收阈值 / PSRAM 预算 / 风险登记 / 重构护栏 / checklist；**§15 = 云签到架构、API 边界与实测验收**（§15.9 四修、§15.10 零命中根因与遗留） |
| `docs/library/HANDOVER.md` | ★ 主文档：技术结论 / 踩坑 / 验收记录 / **目录约定（第 6 节）** |
| `docs/library/face_recog_day_plan.md` | 人脸识别功能规划（P0~P3 + 三轮实施）与进度 |
| `docs/library/usb_link_protocol.md` | JCLINK 协议规范（两端共用） |
| `docs/library/pc_link_references.md` | esp-dl / tinyusb / jpeg / lv_port 技术核实记录 |
| `docs/library/esp-dl_issue_draft.md` | esp-dl 非确定性 issue 草稿（⏳ 待提交 Espressif） |
| `jc_p4_app/README.md` | 固件工程导览（模块表 / 运行时命令 / 硬性纪律） |
| `qt_host/README.md` | Qt 上位机：依赖/构建/连接方式/快捷键/无界面自检/**USB 实测数据** |
| `docs/library/JC-ESP32P4-M3-DEV-开发要点.md` | 板卡硬件要点与引脚表（不依赖厂商资料包） |
| `docs/library/项目框架构建指南.md` | 从零搭同类工程的步骤 |
| `docs/library/2026-10-22_storage挂载失败_删除注册失效_修复记录.md` | `HW_H264` 挤爆内部 RAM → `/storage` 挂不上 → 删除/注册全废的逐字取证 |
| `docs/library/2026-10-29_face.db膨胀_槽号与下标错位_同名重注册零命中_修复记录.md` | face.db 三件套：体积只增不减、`query_feat` 返回下标、同名重注册 100% 零命中的取证与被推翻假设 |
| `cloud_server/README.md` | 云签到服务总览：架构边界（云端不比对特征）、目录、安装、API 摘要、去重与保留语义 |
| `cloud_server/SERVICE_GUIDE.md` | 服务器现场版部署/运维/排障（Token 位置、systemd、公网不通、测试实例 18080） |
| `cloud_server/TEST_PLAN_PC_IMAGE.md` | PC 送图端到端测试方案（免光照验证注册/识别/快照/事件/CSV） |
