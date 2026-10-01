# jc_p4_app 开发接手文档（会话交接）

> 交接日期：2026-09-30 | **任务一~五 + 七 已完成，真机端到端跑通** ✅：
> 板载 SC2336（640x480@50fps）→ 图像管道 → esp-dl 检测/识别 → 0x02 JSON 回包；
> 板端新增 **0x05 图像回传**（硬件 JPEG，~10KB/帧）→ **LVGL SDL 模拟器**显示画面 + 检测框 +
> 名单 + 按钮操作（替代板子触摸屏），UI 与板端同源（`ui_shared.c` 只用 LVGL）。
> ⚠️ **图像采集只在板端做（用户 2026-09-30 定）**：PC 端不再采集、不再发 JPEG；`src=pc` 仅作备用分支。
> 任务六（host_app.py UDP 桥）**不再必需**：模拟器在 WSL 里直接 TCP 连板子。
> 已知问题：摄像头画面偏绿偏暗（ISP AE/AWB 未收敛，见「九、已知问题」）。
> 写文件策略：write_to_file 失败时（报 "missing required parameter filePath"），立即改用
> PowerShell 数组 + base64 + WSL 管线：`$b=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((@('line1','line2') -join [char]10))); wsl -e bash -c "echo $b | base64 -d > /mnt/d/.../file.c"`
> （长内容分段追加 `>>`；命令过长会被拆解报错，此时拆小块重试）

## 一、总体计划（7 步，1~5 已完成 ✅）

板载 SC2336 摄像头（640x480@50fps，默认源）或 PC 送帧 →(JCLINK，USB CDC 或 网线 TCP)→ P4 图像管道
→ esp-dl 人脸检测/特征/1:N 比对 → 结果 JSON 回传 PC → PC HMI 画框 + LVGL SDL 模拟器展示（与板端同源 UI）。
（原计划里「PC 摄像头采集→板端 AI」已废弃：采集只在板端做，PC 端只显示。）

| # | 任务 | 状态 |
|---|---|---|
| 1 | GitHub 案例核实 → `jc_p4_app/docs/pc_link_references.md` | ✅ |
| 2 | USB CDC 协议 → `jc_p4_app/docs/usb_link_protocol.md` | ✅ |
| 3 | app_usb_link CDC 收发框架 + 编译通过 | ✅ |
| 4 | app_imgpipe（JPEG 硬解 + 缩放 + 图像源抽象） | ✅ |
| 5 | app_faceai（esp-dl 检测/特征/1:N 比对 + storage 特征库 + JSON 回传） | ✅ |
| 6 | ~~pc_host/host_app.py（UDP 桥）~~ | ⏭️ 不需要（模拟器直连 TCP；`pc_host/` 只保留测试脚本） |
| 7 | **LVGL SDL 模拟器 + ui_shared 同源 UI + 端到端联调**（画面显示 + 触摸屏替代 UI） | ✅ |
| 8 | 待优化：ISP AE/AWB（画面偏绿偏暗）；PPA 缩图后再回传（进一步省带宽/CPU） | pending |

## 二、已核实的技术结论（新会话直接用，勿重复调研）

1. **esp-dl**：官方支持 P4（IDF≥5.3）；模型 `models/human_face_detect` + `models/human_face_recognition`；例程 `examples/human_face_detect|human_face_recognition` 是 app_faceai 的直接模板。⚠️ v3 API 与 v2 差异大，以组件源码为准。加依赖：`espressif/esp-dl`（建议同时在 examples 里看它怎么引模型组件，模型可能要单独加 `espressif/esp-dl` 下的子组件或整仓）。
2. **esp_tinyusb ^2.0.0**：P4 官方支持。本地模板例程：`D:\ESP-IDF\v5.5.5\esp-idf\examples\peripherals\usb\device\tusb_serial_device\`。关键配置 `CONFIG_TINYUSB_CDC_ENABLED=y`（已写入 sdkconfig + defaults，漏了会报 "TinyUSB CDC driver must be enabled in menuconfig"）。
3. **JPEG 硬解**：用 IDF 5.5.5 **内置** `esp_driver_jpeg`（`driver/jpeg_decode.h`，P4 硬件 2D-DMA 解码），
   无需 `espressif/esp_new_jpeg`。参考例程 `esp-idf/examples/peripherals/jpeg/jpeg_decode`。
   PPA 同样内置（`esp_driver_ppa`）。
4. **PC LVGL 模拟器**：`D:\esp32project\ESP32-P4\lvgl_project\lv_port_pc_vscode_official` 已编译成功，WSL 目录下 `./bin/main` 启动。结构：`config/ FreeRTOS/ lvgl/ main/`。任务七把 ui_shared/net_link 加进去。
5. **agent-browser CLI**：已装 v0.38.1（`& "$env:APPDATA\npm\agent-browser.cmd" <cmd>`），复用 C 盘已有 Playwright Chromium（ms-playwright/chromium-1228）。⚠️ C 盘空间紧张，勿再往 C 盘装大件；Python 包装包用 `pip install --target D:\...`。
6. 参考报告：`jc_p4_app/docs/pc_link_references.md`（esp-dl/tinyusb/jpeg/lv_port 全部核实过）。

## 三、工程现状（D:\esp32project\ESP32-P4\jc_p4_app，IDF v5.5.5）

**编译状态：✅ build 通过 + ✅ 已烧录 COM4 真机跑通**（esp_tinyusb CDC + esp_driver_jpeg 硬解 +
esp-dl 人脸检测/识别），bin **5.26MB / 8MB 分区（剩 37%）**。烧录：`idf.py -p COM4 flash`，
看日志见下面「八、真机实测记录」。

⚠️ **离线依赖**：本机到组件注册表 `components.espressif.com` 的 TCP 完全不通（DNS 能解析出
CloudFront IP 但连接超时；`components.espressif.cn` NXDOMAIN）。因此 esp-dl 全家桶**不走注册表**，
改用本地检出 + `override_path`（见 `main/idf_component.yml` 注释）。两个外部仓库在项目**上一级目录**：
| 路径 | 来源 | 内容 |
|---|---|---|
| `D:\esp32project\ESP32-P4\esp-dl` | `git clone --depth 1 -b v3.3.11 https://github.com/espressif/esp-dl.git` | esp-dl 组件本体 + `models/human_face_*` + `tools/dl_fft` |
| `D:\esp32project\ESP32-P4\esp-adf-libs` | `git clone --depth 1 --filter=blob:none --sparse` + `git sparse-checkout set esp_new_jpeg` | `espressif/esp_new_jpeg`（头文件 + 各芯片预编译 .a，含 esp32p4） |

**没有这两个目录工程就无法 configure**（删除/移动会报 component not found）。GitHub 大文件传输会
中途断（`RPC failed; curl 18`），重试即可成功；换机器/重装时按上表重新 clone。
（`espressif/eigen` 只被 `models/feat_database` 用，我们没引那个组件，不需要。）

### main/ 模块清单
| 文件 | 状态 | 说明 |
|---|---|---|
| app_usb_link.c/h | ✅ 新增 | JCLINK v1：tinyusb CDC + 128KB PSRAM 环形缓冲 + 状态机帧解析 + CRC16。API：`app_usb_link_init/send/log/recv/set_cmd_cb` |
| app_imgpipe.c/h | ✅ 新增 | 图像管道：USB 源 → `esp_driver_jpeg` 硬解 → RGB888 帧池（双缓冲最新帧语义）→ sink/ack。含 `frame_source_t` 抽象、双线性缩放、统计、`set_auto_ack()` |
| app_faceai.cpp/h | ✅ 新增 | 边缘 AI：HumanFaceDetect(MSRMNP) + HumanFaceFeat(MFN) + `dl::recognition::DataBase`(/storage/face.db)，名字表存 NVS；命令队列 + 0x02 JSON 回传 |
| app_status.c/h | ✅ 改 | 新增 `APP_MOD_USB`/`APP_MOD_PIPE`/`APP_MOD_FACE`（共 13 磁贴，显示实况） |
| main.c | ✅ 改 | 初始化顺序：nvs→i2c→**usb_link**→display(守卫)→sdcard→eth→wifi→audio→rs485→battery→camera→**imgpipe**→**faceai**；各守卫已加 |
| Kconfig.projbuild | ✅ 改 | 新增 `APP_ENABLE_DISPLAY/USB_LINK/IMG_PIPE/FACEAI`、`APP_FACEAI_DETECT_THR/MATCH_THR/MAX_BOXES`（默认均 y / 50 / 70 / 5） |
| idf_component.yml | ✅ 改 | 加 `espressif/esp_tinyusb ^2`（任务四）+ 5 个 esp-dl 系依赖（全部 `override_path` 指向本地检出，**不走注册表**） |
| CMakeLists.txt | ✅ 改 | SRCS 加 app_usb_link.c、app_imgpipe.c、app_faceai.cpp；REQUIRES 加 esp_tinyusb、esp_driver_jpeg、json、wear_levelling、esp-dl、human_face_detect、human_face_recognition |
| sdkconfig(+defaults) | ✅ 改 | APP_ENABLE_*=y、TINYUSB_CDC_*、FACEAI 阈值；**AI 性能预设**：`COMPILER_OPTIMIZATION_PERF` + `CACHE_L2_CACHE_256KB` + `CACHE_L2_CACHE_LINE_128B` |

### 其他既有模块（均已编译验证过）
app_i2c（I2C1 SDA7/SCL8）、app_display（DSI+JD9365+GT911+LVGL9.3 状态页）、app_wifi（esp_wifi_remote 经 C6）、app_audio（ES8311/ES7210）、app_sdcard、app_eth（RMII IP101）、app_rs485、app_battery（GPIO53 ADC）、app_camera（SC2336 自动探测，模组未到货）

### JCLINK v1 协议（docs/usb_link_protocol.md，两端共用）
帧：`'J''C' | type(1) | seq(1) | len(u32LE) | payload | crc16(2, CRC-16/CCITT-FALSE over type+seq+len+payload)`
type：0x01 JPEG(PC→板) / 0x02 结果JSON(板→PC) / 0x03 命令JSON(PC→板) / 0x04 日志(板→PC)
流控：窗口=1，PC 发一帧等 0x02 回复再发下一帧；命令：start/stop/mode/register/delete/list/clear；结果字段：faces/boxes/match/sim/fps/infer_ms/dropped/lib/event(+lib_n/msg)

**命令应答语义（任务五实现，任务六照此写 host）**：
- `start/stop/mode/list/delete/clear` → **立即**回一条 0x02（event=none/list/deleted，lib 随 start/list/delete 带上）
- `register` → **不回帧**，只"武装"注册；之后每帧照常回结果，含人脸那帧回 `event=registered`（带 lib）
- 停止(stop)后仍会消费帧并回 `{"faces":0,"event":"stopped"}`，保证窗口=1 不卡死

## 四、任务四（app_imgpipe）✅ 已完成

**结论先说：不需要 `espressif/esp_new_jpeg` 外部组件。** IDF v5.5.5 自带 `esp_driver_jpeg`
（`driver/jpeg_decode.h`，P4 硬件 2D-DMA JPEG 解码，依赖 DMA2D），PPA 也自带
（`esp_driver_ppa` / `driver/ppa.h`）。所以 `idf_component.yml` 未改动，只在 CMakeLists
REQUIRES 加了 `esp_driver_jpeg`（组件自动参与构建，无需 sdkconfig 开关）。

### 实现（main/app_imgpipe.c/h）
- `app_imgpipe_init()`：`jpeg_new_decoder_engine()`（timeout 200ms）+ 256KB PSRAM 接收缓冲 +
  解码任务 `imgpipe_task`（优先级 5）。
- 主循环：源 `acquire()` → `jpeg_decoder_get_info()` 取尺寸/采样方式 → 按 MCU 对齐宽高
  （4:2:0→16x16、4:2:2→16x8、4:4:4/灰→8x8）分配输出缓冲（`jpeg_alloc_decoder_mem`，
  cache 对齐、只会扩容）→ `jpeg_decoder_process()` 硬解 RGB888。
- **帧池**：2 个槽、最新帧语义（无队列积压）。解码任务只往"非 latest 且未被持有"的槽里写，
  消费者 `app_imgpipe_get_frame(&fr, timeout)` / `app_imgpipe_release(&fr)`（按 data 指针匹配）。
  消费者持有期间该槽不会被覆盖；无空槽则丢帧并计入 dropped。
- **frame_source_t 抽象**：`start/acquire/release` 三个函数指针，源统一交付**压缩帧**；
  内置 `app_imgpipe_usb_source()`（JCLINK 0x01，用 `app_usb_link_recv()`）。
  SC2336 到货后写 V4L2 源（esp_video 可硬件 JPEG 编码）→ `app_imgpipe_set_source()` 切换，
  上层零改动。`release()` 对 USB 源是空操作（recv 已拷贝）。
- **缩放**：`app_imgpipe_resize_rgb888()` 16.16 定点双线性（dst 边缘 → src 边缘映射）。
  320x240 与模型输入同尺寸时可直接跳过缩放。大倍率降采样（1080p→320）建议让源侧
  直接输出小尺寸（esp_video 支持），别指望软件双线性抗混叠。
- **消费接口**：`app_imgpipe_set_sink(cb)`（faceai 注册后管道不再自动回 0x02；cb 在
  imgpipe 任务上下文执行，别在里面做重活）；未注册 sink 时管道自动回
  `{"faces":0,"boxes":[],"event":"none","pipe":{...},"fps":..,"dropped":..}`，
  解码失败回 `event":"error"` + msg —— 保证任务六的 PC 窗口=1 流控不卡死。
- 统计 `app_imgpipe_get_stats()`；每 1s 更新 `APP_MOD_PIPE` 磁贴（"320x240 24.9fps 7ms"）
  并经 `app_usb_link_log()` 往 PC 打一行实时日志（0x04）。
- 工具：`app_imgpipe_alloc_rgb888()/free_frame()`（PSRAM 64B 对齐，可直接喂 PPA/模型）。

### 接线
`main.c` 末尾 `#if CONFIG_APP_ENABLE_IMG_PIPE → app_imgpipe_init()`（在 `app_usb_link_init()` 之后，
依赖其 jpeg_sem）；`app_status` 新增 `APP_MOD_PIPE` 磁贴。任务五的 faceai 初始化放在其后。

### 未做的验证
本机无串口（`GetPortNames()` 为空，板子未连），故**只做了编译验证**。板子到手后的第一个
测试：PC 端发一帧 320x240 JPEG，应看到 0x04 日志 "imgpipe: 320x240 ... dec=Nms" 和 0x02 ack；
若颜色 R/B 反了，把 `APP_IMG_JPEG_RGB_ORDER`（app_imgpipe.h）改成 `JPEG_DEC_RGB_ELEMENT_ORDER_BGR`。

### 编译命令（同前）
`. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1; cd D:\esp32project\ESP32-P4\jc_p4_app; idf.py build`
（managed_components 变更触发 safe-delete 防护时：`wsl -e bash -c "rm -rf /mnt/d/esp32project/ESP32-P4/jc_p4_app/managed_components"` 后重建）

## 五、任务五（app_faceai）✅ 已完成

### 结构（main/app_faceai.cpp，C++；对外只暴露 C 接口 app_faceai.h）
- 模型：`HumanFaceDetect`（MSRMNP_S8_V1，P4 上 msr 61KB + mnp 127KB）+
  `HumanFaceFeat`（MFN_S8_V1，1.24MB）——均在 **AI 任务里 eager 加载**（`lazy_load=false`），
  约几秒；这段时间 `app_imgpipe` 的内置 ack 继续回 PC，加载完才 `app_imgpipe_set_auto_ack(false)`
  接管（**必须成对理解**：接管后每帧的 0x02 由 faceai 回，漏回就卡窗口=1）。
- 特征库：`dl::recognition::DataBase(DB_PATH, feat_len)`，文件 `/storage/face.db`，
  挂载 `storage`(FAT, 8MB) 分区（`esp_vfs_fat_spiflash_mount_rw_wl`，format_if_mount_failed=true，
  首次开机自动格式化）。esp-dl 的库**按自增 id 存特征、没有 label**，所以：
  - 名字 <-> id 映射由我们自己维护，存 NVS `faceai/names`（格式 `"1:user1;2:user2"`，≤16 人）
  - 注册时取回新 id 的办法：enroll 后用**同一特征**再 `query_feat(feat, 0.999, 1)`，
    自己与自己相似度=1.0，返回的就是新 id
  - `register` 同名先 `delete_feat(old_id)` 再入库 = 覆盖旧特征
- 取帧：AI 任务循环 `app_imgpipe_get_frame(&fr, 200)`（每帧只交付一次，用毕 `release`）；
  `fr.stride != width*3`（JPEG 按 MCU 对齐）时先重打包到 PSRAM 缓冲，再喂 `dl::image::img_t`。
- 识别策略：所有框都上报，**只对最大人脸**做特征/比对（与官方例程一致）；
  `query_feat(feat, thr=CONFIG_APP_FACEAI_MATCH_THR/100, top_k=1)` 命中→event=match，
  有脸但没命中→event=reject，没人脸→event=none；`mode` 命令可关 detect/match、改 thr。
- 命令：`app_usb_link_set_cmd_cb()` 注册的回调**跑在 usb_rx 任务里**，只把 JSON 拷进
  256B 的 FreeRTOS 队列（深度 4），真正解析/执行在 AI 任务（cJSON）。
- 结果帧用 cJSON 生成（自动转义），字段：faces/boxes/match/sim/fps/infer_ms/lib_n/dropped/event(+lib/msg)。
- `APP_MOD_FACE` 磁贴每 0.5s 刷新（"1 face user1"）；启动/就绪经 0x04 日志上报 PC。

### 未做的验证（板子未连，只有编译验证）
1. 首次启动：storage 分区自动格式化 + 模型加载耗时（预计 2~5s），看串口日志
2. 识别精度/帧率（MSRMNP+MFN @360MHz，-O2 + L2 256K/128B 预估 8~15fps）
3. R/B 通道：若人脸检测明显失败或结果异常，先查 `APP_IMG_JPEG_RGB_ORDER`
   （JPEG 解码）与 esp-dl 期望的 RGB888 顺序是否一致
4. 分区余量：bin 5.9MB / 8MB；若以后模型换成 MBF(3.35MB) 或再加模型，需换更大分区表

## 六、任务六/七（PC 端）✅ 任务七已完成

- **已完成**（本会话）：`pc_host/protocol.py`（JCLINK 帧/CRC + Link 类，支持 **TCP 或串口** 两种传输）、
  `pc_host/jc_test.py`（CLI：start/stop/list/clear/mode/frame/register/delete/demo，含用 esp-dl 官方人脸图跑注册+1:N 的 demo）
  —— 只用标准库 + pyserial，无第三方依赖，已实测跑通
- **host_app.py 不再需要**（原计划作 UDP 桥）：模拟器在 WSL 里直接 TCP 连板子；
  `pc_host/protocol.py` 补了 `recv_image()/recv_any()`，`jc_test.py` 补了 `stream/img/preview` 子命令。
- **板端 0x05 图像回传（本会话新增）**：`main/app_stream.c` 独立任务限速取帧（默认关，10fps / JPEG q75）
  → **P4 硬件 JPEG 编码**（`jpeg_encode.h`）→ JCLINK `0x05`（8 字节头 + 数据）→ 模拟器/上位机显示。
  ⚠️ 编码器输出缓冲必须用 `jpeg_alloc_encoder_mem()` 分配，否则报 `bit stream is not aligned`。
  命令 `{"cmd":"stream","value":"on","fps":10,"quality":75}`，ack 的 msg 带 `sent/enc/KB/drop`。
- 环境：**pyserial 3.5 已装进宿主机 Python 3.14** ✓；OpenCV 未装（仅画框用，可换成 PIL/tkinter 免依赖）
- **连接方式**：网线 TCP 已验证可用（板子 server 3333；PC 静态 192.168.33.53/23，板子 4s 无租约回落 192.168.33.2/23）；
  走 USB 则需把 P4 的 **High speed USB(OTG) 口**插到 PC（出现 VID 303A/PID 4001 的新 COM 口，非 CH340），
  CH340 那个 COM4 只用于烧录/看日志
- 板端行为对齐点（任务五已定，host 必须按此实现）：
  - **命令回复带 `"ack":"<命令名>"`**（流式帧结果不带），host 靠它区分命令应答与推流结果，
    见 `pc_host/protocol.py` 的 `cmd_and_wait()`；结果帧带 `"src":"cam"|"pc"`
  - 默认 `src=cam`：板载摄像头**持续推流**（0.026s 一帧，无需请求），host 只收不发即可看到结果
  - `{"cmd":"src","value":"cam"|"pc"}` 切图像源；PC 一发 JPEG 会自动切回 pc 源
  - 每帧必回一条 0x02，**register 命令本身不回帧**（下一帧含人脸时回 event=registered）
  - `stop` 后仍会回 `{"faces":0,"event":"stopped"}`；`start` 回复里带 `lib` 数组即链路就绪
  - 命令与帧共用同一个窗口=1：发命令后可以发帧，reply 顺序 = 先命令回复后帧回复
  - 目前 host 可能遇到的"无回复"情形：板端模型加载中（前几秒，此时由 imgpipe 兜底 ack）、或
    fr 被 faceai 取走后 0x02 由 faceai 回（两者不会同时回，见 app_imgpipe_set_auto_ack）
- **模拟器实现（本会话完成）**：`D:\esp32project\ESP32-P4\lvgl_project\lv_port_pc_vscode_official`
  - `main/inc/net_link.h` + `main/src/net_link.c`：**非阻塞 TCP JCLINK 客户端**（与板端/`protocol.py`
    同一套帧 + CRC16），全在 LVGL 主循环 `net_link_tick()` 里轮询，不需要线程（`LV_USE_OS == LV_OS_NONE`）；
    断开自动重连；0x02/0x04/0x05 回调给 UI。
  - `main/inc/ui_shared.h` + `main/src/ui_shared.c`：**同源 UI（只依赖 LVGL）**——状态栏（链路/src）、
    480x360 预览 + 检测框 + 名字标签、结果行（faces/match/sim/fps/infer/lib）、名单、8 个按钮
    （Start/Stop/Register/Delete/List/Clear/Camera/Preview ON）、日志区（板端 0x04 直接上屏）。
    Register/Delete 弹 msgbox + textarea + keyboard 输入名字 → 发命令。连上后自动发
    `start` + `src=cam` + `stream on fps=10`（等于"开机默认界面"）。
    JPEG 用 **lvgl 内置 tjpgd** 直接解到自管 RGB888 缓冲显示，**不走 LVGL 的 FS/MEMFS 解码路径**
    （`LV_CACHE_DEF_SIZE=0`，每帧重解，PC 上 640x480 无压力）。
  - `main/src/main.c`：窗口 **480x800**（竖屏，与将来板端 800x1280 同构型）、`--host/--port`、
    主循环加 `net_link_tick()`；调试截图 `SIM_SHOT=<前缀> SIM_SHOT_MS=5000`（SDL_RenderReadPixels 存 BMP）。
  - 编译运行（WSL）：
    ```bash
    cd /mnt/d/esp32project/ESP32-P4/lvgl_project/lv_port_pc_vscode_official
    cmake -B build -S . && cmake --build build -j 8      # 产物 bin/main
    ./bin/main --host 192.168.33.2 --port 3333
    ```
    截图可视化：`jc_p4_app/tools/bmp2png.ps1 -In x.bmp -Out x.png`；JPEG 体检：`tools/img_check.ps1 -Path x.jpg`
- UI 同源：ui_shared 只依赖 LVGL；板端将来屏幕到货后同一文件接 esp_lvgl_port
- **联调实测**：模拟器显示板端实时画面 + `lib: bill, musk` + `faces 0 - 29.4fps 25ms lib 3` + 板端日志；
  UI 自动开的 stream 生效（画面在流动）⇒「UI 按钮 → 命令 → 板端 → 0x02/0x05 → UI」闭环通。

## 八、真机实测记录（2026-09-30，COM4 烧录）

### 硬件实况（和之前的假设不同，务必记住）
| 项 | 实测 |
|---|---|
| 芯片 | **ESP32-P4 rev v1.3（< 3.0！）**，MAC 30:ed:a0:ea:b0:40，360MHz |
| PSRAM | 32MB HEX @200MHz ✓（`Found 32MB PSRAM device / Speed: 200MHz`） |
| Flash | 16MB（Boya 颗粒，IDF 提示用 generic 驱动） |
| 串口 | **COM4 = CH340（UART0, GPIO37/38）**；P4 原生 USB(OTG) 口未接 PC（只枚举 CH340） |

**板上有 3 个 Type-C（按原理图分块）**：
| 原理图块 | P4 引脚 | 用途 |
|---|---|---|
| **High speed USB**（信号 ESP_USB_P/N） | **USB0 OTG-HS** | ✅ **PC-as-HMI 的 JCLINK CDC 走这个口**（`TINYUSB_DEFAULT_CONFIG()` 在 P4 上默认 `TINYUSB_PORT_HIGH_SPEED_0`，启动日志 "TinyUSB Driver installed on port 1" 即此口） |
| Full Speed USB（信号 USB1P1_M/P） | USB1 OTG-FS | IDF 5.5.5 尚未支持 P4 OTG1.1（esp_tinyusb 会拒绝 `TINYUSB_PORT_0`），**当前固件不可用** |
| USB-TTL / 串口烧录（CH340C + Q3/Q4 自动复位） | UART0 TXD/RXD | 烧录 + 看日志（COM4，Q3/Q4 接 DTR/RTS，所以 `serial_capture.ps1` 拉 RTS 能复位） |

插上 High speed USB 口后，PC 应多出一个 **VID 303A / PID 4001** 的 "USB 串行设备 (COMx)"（不是 CH340）：
用这个口跑 host_app.py。若插的是 Full Speed USB 口则不会有任何新设备（该口当前固件未驱动）。

### 烧录/看日志
```powershell
# 烧录
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
cd D:\esp32project\ESP32-P4\jc_p4_app; idf.py -p COM4 flash
# 复位 + 抓 25s 串口日志（idf.py monitor 是交互式的，脚本更适合自动化）
powershell -NoProfile -ExecutionPolicy Bypass -File D:\esp32project\ESP32-P4\jc_p4_app\tools\serial_capture.ps1 -PortName COM4 -Seconds 25 > D:\esp32project\ESP32-P4\_boot_log.txt 2>&1
```

### 必须的工程配置（都是实测踩出来的）
1. **rev<3.0 与 ≥3.0 互斥**：IDF 默认 `ESP32P4_REV_MIN_301`，bootloader 直接拒绝烧 v1.3 芯片
   （"requires chip revision in range [v3.1 - v3.99]"）。已在 sdkconfig(+defaults) 设
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`。
   换 rev≥3.0 的板子要反过来改，否则同样烧不进去。
2. **Wi-Fi 必须摘掉**：C6 里跑的 esp-hosted slave 固件是 **2.3.2**，宿主驱动是 **3.0.7**
   → `E eh_init_evt: major version mismatch ??? OTA coprocessor from host`，wifi 初始化不返回。
   而且**只在代码里不调 app_wifi_init() 是不够的**：esp_hosted 的 SDIO transport 由构造函数启动，
   宿主没走完它的初始化流程时会 `assert(xQueueSemaphoreTake(NULL))` 反复重启。
   现在把整条链路从构建里摘掉（`main/idf_component.yml` 里 esp_wifi_remote 注释掉、
   `main/CMakeLists.txt` 里 app_wifi.c 与 esp_wifi_remote/esp_wifi 由 `CONFIG_APP_ENABLE_WIFI` 控制）。
   修好 C6 固件（刷与宿主匹配的 esp-hosted 3.x slave）后三处一起恢复。
3. **显示挂死**：DSI 读面板参数时 `panel_io_dbi_rx_param()` → `mipi_dsi_host_ll_gen_is_read_cmd_busy()`
   无限忙等（无超时），main 永久卡死 → TWDT。屏幕未接/未上电时必现。
   现用 `CONFIG_APP_ENABLE_DISPLAY=n`（PC-as-HMI 模式）。要查显示：先确认屏线/LDO VO3 2.5V。
4. **esp_netif 初始化不能靠 esp_hosted 顺带**：停用 Wi-Fi 后 `app_eth_init()` 在
   `esp_netif_attach()` 空指针崩（Load access fault）——因为 `esp_netif_init()` 之前是 esp_hosted
   的构造函数调的，而 app_eth.c 自己从来没调。已在 main.c 加 `netif_init_safe()`（幂等），
   并在 app_eth.c 里加了 NULL 检查。
5. **HZ=100 下 `pdMS_TO_TICKS(2)==0`**：app_usb_link 的 rx 任务和 imgpipe 的源轮询用 0 个 tick
   的 delay 空转，饿死 IDLE0 → TWDT。已改成 1 tick。**以后所有 <10ms 的 delay 都要用 1 tick**。

### 启动实测结果（当前固件，稳定不重启）
```
sys      PSRAM 32MB hex 200M ✓ / CPU 360MHz ✓ / rev v1.3 通过校验
usb_link JCLINK v1 ready (TinyUSB on port 1) ✓
sdcard   无卡：sdmmc_card_init failed 0x107（正常，未插卡）
eth      RMII IP101 started ✓（link 状态看路由器）
wifi     disabled（见上）
audio    ES8311(0x18)/ES7210(0x40) 无 I2C 应答 -> codec 打开失败（待查供电/使能脚）
rs485    UART1 26/27 @115200 ✓（空闲时会周期性 "rx 1 bytes: 00" —— RX 悬空噪声，无害）
battery  ADC 就绪（calibrated=1）✓
camera   ✅ 已出图：SC2336(WTDK-CAM-2336) 检测到 PID=0xcb3a，640x480 RGB565 **50fps**
         （5s 采 250 帧）；注意 camera_task 目前只跑 5s 自检
imgpipe  up: src=usb-jclink, hw jpeg decode -> RGB888, slots=2 ✓
faceai   storage 分区挂载 ✓ -> /storage/face.db 0 feat ✓ -> 模型加载 156ms ->
         auto ack off -> 接管 0x02 ✓ -> "all modules launched" ✓
```
- **"usb_link: send type=4 ... failed: ERROR"** 是因为 P4 原生 USB 口没接 PC（CDC 无主机），
  接上 PC 后应消失；**用网线走 TCP 时不会有这个报错**。
- 特征库文件已在首次启动时建好（storage 分区挂载即完成格式化）。

### 以太网传输层（JCLINK over TCP）✅ 已实现并跑通
板子插网线直连 PC 即可（PC 是静态 192.168.33.53/23，没有 DHCP，所以板子 DHCP 4s 超时后
回落到 `CONFIG_APP_ETH_STATIC_IP`=192.168.33.2/23）。实现：
- `app_usb_link.c` 拆出传输层后端 `jcl_transport_t`（write/flush）+ `app_usb_link_feed/rx_reset/set_transport`：
  帧格式/CRC/解析仍是同一套，imgpipe/faceai 一行没改。
- `app_netlink.c/h`：板子做 TCP server（`CONFIG_APP_NETLINK_PORT` 默认 **3333**，一次一个客户端，
  TCP_NODELAY），连接上来就注册为发送后端，断开自动回落 USB CDC 并复位半截帧状态。
- `app_eth.c`：`eth_ip_task` 先试 DHCP 4s，拿不到租约就配静态 IP（`esp_netif_dhcpc_stop` +
  `esp_netif_set_ip_info`，网关填 0）；PC 端 `protocol.py` 的 `Link(tcp="192.168.33.2:3333")` 直接连。

**实测（本会话，300x300 测试图，端到端全通过）**：
```
ping 192.168.33.2                      -> 0~2ms
python pc_host/jc_test.py --tcp 192.168.33.2:3333 list
   板端日志: netlink: client 192.168.33.53 connected, JCLINK over TCP (port 3333)
   回包:    {"faces":0,...,"event":"list","lib":[]}
register bill ×2(bill1/bill2) / register musk(musk1) -> registered + match=<名字>
musk2 -> {"event":"match","match":"musk","sim":0.773}    ← 1:N 识别命中（阈值 0.70）
每帧: imgpipe 硬解 1ms + faceai infer 20ms，端到端 167~357ms(含 300x300 JPEG 传输)
```

### 摄像头排查（2026-09-30）✅ 已出图 640x480@50fps
**模组**：WTDK-CAM-2336（= SC2336，MIPI 2-lane，I2C 7bit 地址 0x30，PID=0xcb3a）。
**三个原因叠加，按发现顺序**：
1. **真 bug（已修）**：`app_camera.c` 里 `sccb_config` 没给 `.freq`，而
   `esp_video_init.c:161` 是 `sccb_config.scl_speed_hz = init_sccb_config->freq` →
   传 0 进 `i2c_master_bus_add_device` → `invalid scl frequency` → SCCB 根本没建起来，
   **传感器从未被探测过**。已设 `.freq = 100*1000`（与厂商参考例程
   `1-Demo/IDF-DEMO/withDisplay/test` 的 100kHz 一致，它们同样是 I2C1 + SCL8/SDA7 + reset/pwdn=-1）。
   （厂商例程的 freq 来自 Kconfig `EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ=100000`，我们抄代码时漏了。）
1b. **排线没插到位/方向不对**（硬件）：SCCB 修好后驱动去探 0x30 仍无应答，I2C 扫描
   （`app_i2c_scan()`，已做成开机固定项）显示整条 I2C1 只有 0x18(ES8311)。
   重插排线后 0x30 立刻出现。**排查手段就是看开机那行 `i2c: scan: ...`。**
2. **ISP FIFO 溢出（配置）**：SC2336 用 1080p30 RAW10（≈78MB/s）时刷屏
   `E ISP: fifo overflow`，DQBUF 永远阻塞（所以没有 "captured" 日志）。
   根因：**esp_video 把 ISP 时钟硬编码成 80MHz**（`esp_video_isp_device.c:40`
   `ISP_CLK_FREQ_HZ (80*1000*1000)`），厂商例程也只敢用 800x800@50fps。
   已改 `CONFIG_CAMERA_SC2336_MIPI_RAW10_640X480_50FPS=y`（≈19MB/s）→ 稳定 **50fps**。
   ISP 输出是 **RGB565**（fourcc "RGBP"/0x50424752），esp-dl 可直接吃 `RGB565LE`。
   ⚠️ 以后想上 1080p，要么改 esp_video 源码里的 ISP 时钟（managed_components 会被覆盖，
   得放 components/ 里），要么给 ISP 单独配更高时钟源。
2. **修完后的实测**：SCCB 建立成功、驱动去探 0x30，但
   `sc2336: Get sensor ID failed` → `failed to detect MIPI-CSI camera sensor with address=30`。
3. **I2C1 总线扫描（已加进启动日志，`app_i2c_scan()`）**：0x08~0x77 全扫一遍，
   **只有 0x18 应答**（ES8311）。摄像头 0x30/0x36、ES7210 0x40、GT911 0x5D 全无应答。
   打开内部上拉（45k）后结果不变 → 总线本身没问题，是这些器件不在/没上电。
4. **结论 & 待用户确认的硬件项**：模组是否插在 CSI FPC 座（方向/卡扣）、模组 3.3V 是否到位、
   原理图里 CSI 那页的 PWDN/RESET/供电接法（厂商例程把它们都设成 -1，说明一直供电）。
5. ⚠️ **注意厂商套件的摄像头是 OV5647**（他们的 sdkconfig：`CONFIG_CAMERA_OV5647=y` +
   AUTO_DETECT_MIPI），本工程只开了 SC2336。若手上模组是 OV5647，需要同时打开
   `CONFIG_CAMERA_OV5647*` 才会被识别（esp_cam_sensor 有该驱动）。I2C 扫描与型号无关，
   所以"扫不到任何地址"已能排除型号问题。
6. ~~后续（模组能应答之后）：camera_task 改成持续采集~~ → **✅ 已完成，见下一节**
   （实现方式是 `app_imgpipe_publish_raw()` 推帧，比原计划的 `frame_source_t` 分支更简单）。

### 摄像头接入 AI 链路 ✅（2026-09-30）
板载 SC2336 现在是**图像管道的默认源**：camera 任务持续 DQBUF → 把 RGB565 帧
`app_imgpipe_publish_raw()` 推进帧池 → faceai 照常 `get_frame()` 检测/识别 → 0x02 JSON。
实测：**采集 50fps 稳定，AI 吞吐 ~38fps（infer 17~25ms），TCP 上结果帧 37~38/s**。

- `app_imgpipe` 新增：`app_img_publish_raw(data,w,h,stride,fmt)`、源模式
  `APP_IMG_SRC_CAM / APP_IMG_SRC_PC`、`app_imgpipe_select_src()/get_src()/src_str()`；
  `pipe_copy_into()` 把 raw 帧拷进槽（与 JPEG 路径共用槽与"最新帧"语义），
  `pipe_publish()` 抽出来给两条路径共用。
- `app_camera`：不再是"采 5 秒自检"，改成**持续采集任务**（3 个 V4L2 mmap 缓冲循环
  DQBUF/QBUF，优先级 6），初始化后 `app_camera_ready()` 为真；`imgpipe_init()` 据此
  把默认源选成 camera（摄像头没起来就还是 PC 源）。
- `app_faceai`：按 `fr.fmt` 映射 esp-dl 像素格式（RGB565LE / RGB888），重打包按 bpp；
  新增 `{"cmd":"src","value":"cam"|"pc"}` 运行时切源，结果帧里带 `"src"`。
- PC 一旦发 JPEG，管道自动从 CAM 切回 PC 源（`imgpipe_task` 里零超时瞄一眼 USB 源）。

**顺带修掉一个性能大坑**：不插 USB 主机（PC 只走网线）时，每帧 `tinyusb_cdcacm_write_flush`
会白等 100ms 并刷一条 "Flush failed"，把 CPU/串口拖死 —— 表现为摄像头**每两秒有整整一秒
只取到 1 帧**。现在加 `s_lk.cdc_up`（tinyusb ATTACHED/DETACHED 事件回调）门控：没主机就
直接失败不 flush，发送失败日志也限流到 5s 一条。之后摄像头 50fps 稳如直线。

**联调姿势**（PC 走网线，见 pc_host/）：
```bash
python pc_host/jc_test.py --tcp 192.168.33.2:3333 watch 10      # 只收看板端推来的结果帧
python pc_host/jc_test.py --tcp 192.168.33.2:3333 register alice # 对着摄像头注册（下一帧含人脸）
python pc_host/jc_test.py --tcp 192.168.33.2:3333 src pc         # 切回 PC 送帧模式
```

### 本会话顺带修掉的协议/逻辑问题
1. **register 竞态**：命令(0x03)与帧(0x01)到达顺序不确定，导致"注册命令配上了之前那帧"
   （实测把 musk1 注册成了 bill）。两手修：①`register` 立即回 ack，host 等 ack 再发帧；
   ②`img_frame_t` 加 `ts_us` 发布时刻，只接受"武装注册之后发布"的帧。
2. **一人多张特征**：名字表改成"id→name 多行"（上限 32 条特征 / 10 人），register 不再删旧特征，
   `lib` 只列唯一名字，`delete` 删该名字的全部特征，NVS 串上限提到 2048B。

## 九、已知问题与下一步（2026-09-30）

1. ✅ **画面偏暗：根因已定位并修复 —— 增益恒为最小值 0**（2026-09-30 实测）
   - 板端加了 `{"cmd":"cam"}` 查询/设置（走 V4L2 **EXT** controls，esp_video 不支持旧版
     `VIDIOC_QUERYCTRL/G_CTRL/S_CTRL`）：`exp=519(8..994) | gain=0(0..192) | ...`。
     实测增益扫描（曝光固定 519，`tools/img_check.ps1` 量 avg_luma）：
     `gain=0 -> 30`、`48 -> 67`、`64 -> 86`、`80 -> 120`、`96 -> 145`、`192 -> 238`（过曝）。
   - **原因**：SC2336 驱动默认 `gain_def = 0`（sensor 寄存器默认值），esp_video 的 AE/AGC
     在本配置下**从不写增益**（曝光也停在驱动默认 519，与 ISP controller 开关无关 —— 已 A/B 验证）。
   - **修复**：`app_camera_init()` 里 streamon 后写默认增益，`CONFIG_APP_CAM_DEF_GAIN`（默认 72，
     室内 avg≈120，范围 0..192）✓；ISP pipeline controller 暂时关掉（它不调增益也不调曝光，
     留着只是多一个任务；想再试 AE/AWB 时改回 y 并对比 `{"cmd":"cam"}` 读数）。
   - **夜视测试用法**（PC 或模拟器 UI 都可以）：
     ```
     jc_test.py cmd '{"cmd":"cam"}'                  # 看当前 exp/gain/范围
     jc_test.py cmd '{"cmd":"cam","gain":128}'       # 加增益（夜视主力）
     jc_test.py cmd '{"cmd":"cam","exp":994}'        # 曝光拉到最大
     jc_test.py cmd '{"cmd":"cam","enum":true}'      # 枚举驱动支持的全部 controls（0x04 日志）
     jc_test.py img out.jpg                          # 抓一张看效果
     ```
   - **仍待解决**：画面**偏绿**（AWB 未生效）。ISP 侧的 WB 走自定义 id
     `V4L2_CID_USER_ESP_ISP_WB`（`esp_video_isp_pipeline.c` 里用 RED/BLUE_BALANCE 设过），
     dump 里没列出可见的 `V4L2_CID_RED/BLUE_BALANCE`，下一步可试 `V4L2_CID_USER_ESP_ISP_WB`
     的 ext control 结构，或调 SC2336 的 IPA JSON（`awb` 段）。
   - 想更长曝光（真夜视）得**降帧率**：50fps 下曝光上限≈20ms（VTS 1000 行），
     可改 `CONFIG_CAMERA_SC2336_MIPI_RAW10_*` 到更低帧率格式，或写 sensor VTS 寄存器降帧。

2. **UI 里画面是 640x480 全尺寸回传**（JPEG ~10KB/帧 @10fps）。想更省可以把 PPA 打开先缩到
   320x240 再编码（PPA 已内置 `esp_driver_ppa`），带宽/编码耗时都能再降。
3. **板端显示仍未点亮**（JD9365 DSI 读参数忙等，`CONFIG_APP_ENABLE_DISPLAY=n`）——
   UI 已按 480x800 竖屏做（将来换 800x1280 只需改 `hal_init()` 与 `IMG_W/IMG_H` 宏），
   `ui_shared.c` 只依赖 LVGL，接 `esp_lvgl_port` 即可复用。
4. 人脸检测在暗画面下 `faces=0` 是正常的（画面本身很暗）——修好曝光后再验证识别。

5. ✅ **画面镜像（左右反）已解决 —— 应用层行内翻转**（2026-09-30）
   - 驱动层三条路都不通：①`esp_video_set_ext_ctrls_to_sensor()` 是**死代码**（全仓库无调用点），
     sensor class 的 `ESP_CAM_SENSOR_HMIRROR(0x0002000A)`/`VFLIP(0x0002000B)` 传不进去；
     ②ISP 设备枚举出的 `0x00980914/0x00980915` 只有 `VIDIOC_QUERY_EXT_CTRL` 支持，
     GET/SET 都没实现（实测设置前后画面 same-diff 14.4 完全没变）。
   - 因此改在**图像管道**里对摄像头帧做行内镜像：`app_imgpipe_set_hflip()`，
     默认 `CONFIG_APP_CAM_HFLIP=y`（模组安装方向导致镜像，默认翻回来），
     运行时 `{"cmd":"cam","mirror":true|false}` 实时切换（ack 的 msg 里带当前 `mirror=`）。
   - 关键好处：翻转发生在 `publish_raw()` 进帧池时，**AI 检测与 0x05 回传看到同一份画面**，
     检测框坐标不会左右错位（若在显示端翻转就会错位）。
   - 验证工具：`jc_test.py fliptest <prefix>`（同会话连抓 3 组帧，帧间隔 200ms，内容几乎不变）+
     `tools/compare_img.ps1 -PathA a.jpg -PathB b.jpg`（判 same/mirror 关系）。
     实测：a0 vs b0(mirror=off) → same=138.7 / mirror=48.1 ⇒ MIRROR RELATION ✓。

6. ✅/⚠️ **低光（夜视）实测结论**（2026-09-30）：SC2336 的低光能力是有的，但**默认设置没发挥出来**。
   - 实测（同一暗环境，用 0x05 `fmt=raw` 原始像素统计 + 转 PNG 看）：
     `gain=72 exp=519`（默认）→ 原始像素均值 0x0C4F，**画面几乎全黑**；
     `gain=192 exp=994`（两者拉满）→ 均值 0x149A(+67%)，**能看清桌面/物体轮廓**。
     对比图：`docs/img/night_default_vs_max.png`（左默认/右拉满）。
   - 一条命令切夜视 / 恢复白天：
     ```
     jc_test.py cmd '{"cmd":"cam","gain":192,"exp":994}'   # 夜视
     jc_test.py cmd '{"cmd":"cam","gain":72,"exp":519}'    # 白天（默认）
     ```
   - **曝光上限被帧率锁死**：`exp` 单位是行，640x480@50fps 的 VTS≈1000 → 最多 994 行 ≈ 19.9ms
     （帧周期 20ms）。想更长曝光必须降帧率。
   - **试过换格式但不可行**：1280x720@30fps 需要 ~34MB/s（ISP 时钟写死 80MHz）→ 实测只跑到
     17.7fps + `task_wdt: CPU 0: camera` 报警 + 命令响应超时，而且曝光上限并没随帧周期变长 ⇒ 已回退
     640x480@50fps（`sdkconfig` 里该组件有**大小写两套** CONFIG 名：`_1280X720_` 与 `_1280x720_`，
     改一个不生效，要一起改）。
   - 真夜视的两条路（都还没做）：①给 `esp_cam_sensor` 加 `override_path` 改 sc2336 驱动的 VTS，
     把帧率降到 10~15fps（曝光 ×3~5）；②等 IR 补光（模组若带 IR LED/IR-CUT 需再接 GPIO）。
   - 顺带产物：0x05 支持 `{"cmd":"stream","value":"on","fmt":"raw"}` 发**原始 RGB565**（614KB/帧，
     排查"JPEG 编码是否把画面搞花"用），PC 端 `jc_test.py img` 自动存 `.rgb565`，
     `tools/rgb565_to_png.ps1` 转 PNG（可指定字节序）。

7. ✅ **画面偏绿（白平衡）已修复 —— 软件白平衡**（2026-09-30）
   - 驱动侧**三条路全不可用**：①sensor class 的 `ESP_CAM_SENSOR_AWB/WB` 传不进去；
     ②ISP 设备的 `V4L2_CID_USER_ESP_ISP_WB`(0x009819E5) 与标准 `V4L2_CID_RED/BLUE_BALANCE`
     用 EXT controls 设置**全部 FAIL**；③把 `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`
     开回来（AWB controller）也不修偏绿（G/(R+B)/2 只从 1.53 → 1.46）。
   - ⚠️ **别调用 `app_camera_set_wb()`**：它的 EXT_CTRLS 在 ISP controller 运行时**会阻塞**，
     实测把整个 faceai 任务卡死（0x02 全停、imgpipe drop=0）。已从 cam 命令里移除。
   - 改为**软件白平衡**（在图像管道里对摄像头帧做 R/B 增益，G 不变）：
     `app_imgpipe_set_wb_gain(kr, kb)`（千分比，1000 = 关），默认 `CONFIG_APP_CAM_WB_KR=1320 /
     KB=1645`（本机模组实测偏绿量），运行时 `{"cmd":"cam","wb_kr":1330,"wb_kb":1645}`，
     模拟器 UI 有 **WB ON/OFF** 按钮。实测（0x05 raw 帧统计）：
     `off → R50.5 G67.8 B40.8, G/(R+B)/2 = 1.48`；`on → R62.0 G67.8 B63.1, 1.08` ✓（顺带亮度 +22%）
     对比图 `docs/img/wb_off_vs_on.png`。
   - ⚠️ **性能教训**：第一版是逐像素乘除法 → camera 任务(prio 6)把 CPU 占死 →
     `task_wdt: CPU 0: camera` + faceai 被饿死（0x02 全停）。改成 **5bit/8bit LUT 查表**并与
     行内镜像**合并成一次内存遍历**后恢复正常。写这类逐像素处理一定要先想 LUT/合并。
8. ✅ **0x05 回传链路两个坑（都已修）**
   - **JPEG 编码器字节序**：`jpeg_encode_cfg.pixel_reverse` 必须为 **true**，否则回传画面是
     彩色噪声（实测 false → 蓝紫噪点、true → 正常画面）。已设为默认；
     命令 `{"cmd":"stream","value":"on","fps":10,"reverse":true|false,"fmt":"jpeg"|"raw"}`。
   - **raw 模式的整帧缓冲要与 JPEG 输出缓冲分开**：JPEG 输出必须用 `jpeg_alloc_encoder_mem()`
     分配（对齐），复用 raw 的 `heap_caps_malloc` 缓冲会在切回 JPEG 时报
     `bit stream is not aligned`。已拆成 `out_buf` / `raw_buf`。
   - PC/UI 侧的 `MAX_PAYLOAD`/`RX_BUF` 也要 ≥ 1MB（raw 帧 614KB），否则整帧被判非法丢弃。
9. ✅ **模拟器 UI 新增摄像头控制**（替代触摸屏）：`gain` / `exp` 两个滑块（松手才发命令，
   板端每条 0x02 都回带 gain/exp/mirror/wb_kr/wb_kb 供滑块同步）+ **Night / Day / Mirror / WB**
   四个快捷按钮；连上后自动 `start` + `src=cam` + `stream on(fmt=jpeg)`。

## 十、本地化组件与驱动 patch（2026-09-30，帧率/夜视能力）

**动机**：夜视要更长曝光，但单帧曝光 ≤ 帧周期（640x480@50fps → 20ms），所以必须能**降帧率**。
原库两处拦路：① `esp_video` 的 `esp_video_set_ext_ctrls_to_sensor()` 是**死代码**（全仓无调用者），
sensor 侧参数根本传不进去（所以 `V4L2_CID_EXPOSURE_ABSOLUTE`/`HFLIP`/`GAIN` 等一直改不动）；
② `sc2336` 驱动没有运行时改 VTS/帧率的能力。

**做法**：两个组件本地化（override_path），另外把 lvgl 也移出 managed_components：

| 组件 | 位置 | patch 内容 |
|---|---|---|
| `espressif/esp_video` | `D:\esp32project\ESP32-P4\esp_video` | ① `isp_video_set_ext_ctrl()`/`get_ext_ctrl()` 的 `default:` 分支转发给 sensor（用 `esp_video_get_csi_video_device_sensor()` + `esp_video_..._to/from_sensor`，并在文件顶部加 extern 声明）；② `src/esp_video_sensor.c` 的 `s_control_map_table` 加一条 `ESP_CAM_SENSOR_FPS ↔ V4L2_CID_USER_BASE + 0x20` |
| `espressif/esp_cam_sensor` | `D:\esp32project\ESP32-P4\esp_cam_sensor` | `sensors/sc2336/sc2336.c`：`struct sc2336_cam` 加 `base_vts/base_fps/cur_vts`；新增 `sc2336_set_fps()`（写 0x320E/0x320F 改 VTS，`VTS = base_vts*base_fps/fps`）；set/query/get para 都加 `ESP_CAM_SENSOR_FPS` 分支（`get` 用于读回当前帧率，必须加，否则 `cam_fps` 恒为 0），另加 `EXPOSURE_US` 读回；曝光上限改用 `cur_vts` |
| `lvgl/lvgl` | `D:\esp32project\ESP32-P4\lvgl` | 无 patch，只是移出来（原因见下） |

**为什么 lvgl 也要移**：`main/idf_component.yml` 一变，组件管理器就可能把
`managed_components/lvgl__lvgl` 判为"需要重装"→ 触发**批量删除保护**
（`[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED] {"count":6563,...}`）→ configure 直接失败；
而组件注册表在本机网络不可达，删了就没法恢复。把 override 的组件与 lvgl 都放到
`managed_components` 之外，它就不再有删除意图。

**⚠️ 关键坑**：
1. **override_path 的目录名必须等于组件名**（`esp_video` / `esp_cam_sensor` / `lvgl`）；
   叫 `esp_video_local` 会报 "Component name ... doesn't match the directory name" 并 configure 失败。
2. `main/idf_component.yml` 里要**显式声明** `espressif/esp_video` 和 `espressif/esp_cam_sensor`
   （后者原本只是 esp_video 的间接依赖，不显式声明无法 override）。
3. `esp_cam_sensor_format_t.isp_info` 是**只读成员**，改不了 ⇒ 当前 VTS 由驱动自己用 `cur_vts`
   维护（曝光上限 = `cur_vts - 6` 跟着它走），**不要**去改 `dev->cur_format`。
4. **上游升级这两个组件后必须重打 patch**（脚本：`jc_p4_app/tools/patch_sc2336.py`，幂等；
   esp_video 的两处改动见上表）。
5. 被移出的旧副本留在 `D:\esp32project\ESP32-P4\_old_managed\`（可回退）。

**能力效果**：板端 `{"cmd":"cam","fps":10,"auto_exp":true}` → 帧率 50→10fps ⇒ VTS 1000→5000
⇒ 曝光上限 994→4994 行（≈19.9ms → ≈99.9ms，约 5 倍进光量）；`auto_exp` 顺手把 exp/gain
拉到新上限。UI 第 4 行是 50/25/15/10fps 四个档位按钮（`ui_shared.c`）。

**真机实测（2026-09-30，已验证 ✅）**：

```
{"cmd":"cam"}                            -> cam_fps=50, exp_max=994
{"cmd":"cam","fps":10,"auto_exp":true}   -> cam_fps=10, exp=4994/4994   (夜视档)
{"cmd":"cam","fps":50,"auto_exp":true}   -> cam_fps=50, exp_max=994     (切回)
```

画面亮度（`tools/rgb_stats.py` 量 0x05 raw 帧）：`avg16` **17745 → 32504（+83%）**、
`max16` 48154 → 65535（饱和）；从"几乎全黑"到能看清墙面/门框/窗户轮廓。
10fps 长曝光的固有代价是噪点与轻微拖影，偏青用 UI 的 WB 滑块调。

**UI 截图**：`docs/img/ui_sim_fps_20260930.png`（模拟器 `SIM_SHOT=<前缀> SIM_SHOT_MS=<ms>`
自动存 BMP → `tools/bmp2png.ps1 -In x.bmp -Out x.png`）。图里可见第 4 行 50/25/15/10fps
档位按钮，结果行末尾显示板端帧率 `cam10fps`（`set_result()` 读 `cam_fps` 字段）。

**UI 档位按钮（2026-10-01 补）**：
- 第 3 行 5 个：`Night Hi`（10fps+exp max+gain max，最亮）/ `Night Lo`（10fps+exp max+
  **gain 62%**）/ `Day`（gain72/exp519）/ `Mirror ON` / `WB ON`
- 第 4 行 4 个：`50fps` / `25fps` / `15fps` / `10fps`
- `auto_exp` 里显式给的 `exp`/`gain` 会覆盖自动填满的值：
  `{"cmd":"cam","fps":10,"auto_exp":true,"gain":119}` → 曝光拉满、增益只到 119。
- 截图：`docs/img/ui_sim_nightlo_20261001.png`（Night Lo 态：gain 119 / exp 4994 / cam10fps）

⚠️ **低噪档实测结论**（同场景短间隔对照；`tools/rgb_stats.py` 新增 `noise(grad)` 指标 =
水平相邻像素平均梯度）：gain 192 → 119（-38%）时 `avg16` 35034→34538（-1.4%）、
`noise` 7.9→7.7（-2.5%），**信噪比基本不变（4435 → 4485）**。原因是场景光子充足时
噪声以散粒噪声为主，降增益只是等比缩小信号。⇒ 低噪档只在**很暗**的场景才有意义；
若要在当前照度下明显降噪，需要**时域/空域降噪**（多帧平均 / 中值滤波，代价是拖影或 CPU）。

**⚠️ 副作用**：降帧率会同步降低 AI 处理帧率（10fps 时人脸检测每秒 10 次）——
夜视/静态场景够用，正常光照建议切回 50fps。

### 时域降噪（Temporal Denoise，2026-10-01）

弱光噪声以散粒噪声为主（**只降增益几乎不改善信噪比**，实测 SNR 4435→4485），所以改在
图像管道内做**运动自适应时域降噪**：对"没动的地方"做多帧加权平均（噪声按 √N 下降），
运动区域自动减弱/跳过以避免拖影。

**实现**（`main/app_imgpipe.c` → `pipe_dn_apply()`，在 `app_imgpipe_publish_raw()` 里、
**镜像之前**调用 —— prev 必须保存未镜像内容，否则 cur/prev 左右错位出鬼影）：
- 16x16 分块，每块用下采样 4x4 的 G 通道 SAD 判断运动强度
- 静止块强混合、运动块按 SAD 线性衰减权重、超阈值不混（无拖影）
- 像素混合 `out = cur + ((prev-cur)*wq)>>8`，**按 R/G/B 分量分别混合**
  （⚠️ 对 RGB565 整包做线性插值会**跨分量借位**：实测噪声反而涨 2.6 倍、出彩色伪影）
- 混合结果写回 prev（IIR，省掉一次 600KB memcpy）；`dn=0` 完全不进这段，零开销
- 失效重置：首帧 / 尺寸变化 / 换档 / 换图像源 / 改曝光·增益·帧率（`app_imgpipe_reset_prev()`）
- **帧率守卫**：`app_camera_get_fps() > CONFIG_APP_CAM_DN_MAX_FPS`（默认 25）时跳过 ——
  50fps 下 34ms/帧的成本会把 camera 任务拖垮（实测 `dn_skip` 正常累加、帧率不受影响）

**档位**：`s_dn_tab[]` = {关, 低 50%, 中 75%, 高 92%} × SAD 门限（低 6..18 / 中 8..24 / 高 10..30）。
板端 `{"cmd":"cam","dn":0..3}`；UI 第 4 行循环按钮 `DN Off → Low → Mid → High`。

**实测（真机 2026-10-01）**：
- 静止场景（`dn_sad: 4`、`dn_blk: 1200/1200`）：`noise2(lap)` **83.79 → 48.51（-42%）**，
  彩色麻点基本消失、边缘细节保留；raw→PNG 由 505KB 缩到 421KB（更干净也更易压缩）
- `dn_ms: 34`（10fps 帧周期 100ms，占 34%）—— **这就是必须要有帧率守卫的原因**
- 50fps：守卫跳过；10fps 连续 20s：`dropped: 0`、无 task_wdt、0x02 流不中断
- 截图：`docs/img/ui_sim_denoise_20261001.png`（DN High，结果行 `cam10fps dn3`）

**调试字段**（结果帧，仅在 dn>0 时带）：`dn` 档位、`dn_ms` 耗时、`dn_blk` 混合块数、
`dn_sad` 全帧平均 SAD、`dn_skip` 被守卫跳过帧数。调门限靠 `dn_sad` 判断场景是
"静止（≈4）"还是"在动（≈17）"。
新增工具：`tools/raw2png.py`（RGB565→PNG，纯标准库）与 `rgb_stats.py` 的
`noise2(lap)` 二阶差分指标（对噪点敏感、对线性边缘不敏感，比一阶梯度更能区分二者）。

**烧录环境坑（2026-09-30 踩到）**：
- `idf.py -p COM4 flash` 在中文 Windows 上可能因 esptool 进度字符（`ϵ`）触发
  `UnicodeEncodeError: 'gbk' codec can't encode ...` 而**静默失败**（表现为"没有 Hash of data verified"）。
  先 `chcp 65001` + `Set-Item Env:PYTHONIOENCODING utf-8` 再烧。
- 烧录前先确认串口在位：`[System.IO.Ports.SerialPort]::GetPortNames()`；
  CH340 的 COM4 若在设备管理器里是 `Unknown`，说明 USB 数据线断了（板子可能仍在跑，以太网照常）。

## 七、环境备忘

- IDF 激活：`. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`（idf.py 为别名）
- Python：宿主机 3.14（C:\Users\cxk\AppData\Local\Programs\Python\Python314），pip 装包优先 venv 或 --target 到 D 盘；IDF venv 勿污染
- WSL 可用（bash/python3/base64 齐全）；C 盘空间紧张，避免大文件落 C 盘
- 板卡硬件要点、SC2336 适配结论、全部引脚表：见 `D:\esp32project\ESP32-P4\JC-ESP32P4-M3-DEV-开发要点.md` 与 `项目框架构建指南.md`
