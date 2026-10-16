# jc_p4_app —— ESP32-P4 边缘人脸识别设备

Guition JC-ESP32P4-M3-DEV（ESP32-P4 @360MHz + ESP32-C6 Wi-Fi6/BLE，32MB PSRAM，16MB flash）
上的**人脸识别终端**：板载 SC2336 摄像头采集 → 图像管道（AE/AWB/CCM/夜视阶梯/时域降噪）
→ esp-dl 人脸检测 + 特征 + 1:N 识别 → 结果 JSON（JCLINK 0x02）+ 硬件 JPEG 预览（0x05），
PC 端 LVGL SDL 模拟器与板端同源 UI 显示与操作。

**新会话/新 agent 接手：先读 [`docs/HANDOVER.md`](docs/HANDOVER.md)**（全部踩坑与结论都在那里，
本 README 只做导览）。

> ⚠️ **本仓库不含第三方组件与厂商资料**（体积原因）：`esp-dl/`、`esp-adf-libs/`、`lvgl/`、
> `managed_components/`、`C3982-JC-ESP32P4-M3-DEV/`、各级 `build/`。克隆后请按
> **根 [`README.md`](../README.md) 的「未入库的依赖与文件」** 一节补齐，否则无法 configure。

## 当前能力（2026-10-07，全部真机验证）

- **白天**：50fps 恒定 + AE/AWB/厂商标定 CCM；曝光余量阶梯（50→20→10fps）防偏暗；
  **AE 曝光上限 40ms**（暗光超过上限转提增益保锐度——模糊帧毁掉检测与特征）
- **夜视**：暗度驱动自动降帧（5fps）+ 时域降噪 + 低光增强（gamma/BF），
  低光照自动停人脸识别（夜视预览独享帧池），光照恢复自动恢复识别
- **识别**：多帧注册向导（质量门控 + 400ms 间隔自动采 3~5 帧）、连续 K 帧确认
  （防单帧误报）、双阈值 0.61/0.61（已持久化）+ **单帧高分快速通道**（0.80 直达 match）、
  **F6 事件去重**（同人 match 5s 一次）、**F7 多人脸**（boxes 带各自 name/sim）、
  **F4 轻跟踪节流**（特征提取 ≤1/HOLD）、每人多特征、FAT 特征库 + NVS 名表
- **吞吐**：帧池 4 槽 + 发布端按需发布（无人消费的帧跳过 copy/后处理）+ stream 源守卫
- **连接**：网口 192.168.33.2:3333（TCP JCLINK）、WiFi（`{"cmd":"wifi"}` 运行时配网）、
  BLE 广播 "JC-P4"（NimBLE host on P4 + C6 controller）
- 预览/显示：0x05 硬件 JPEG 回传 → 模拟器（480x800 竖屏 UI：注册向导/稳定态/脸宽反馈/
  每框标签/名单特征数）；**F8b：注册弹窗可用性修复**（键盘可点、空输入 OK 置灰、
  `Shots: 1/3/5` 英文标题、关窗不再残留全屏遮罩——详见 HANDOVER 廿二节 17 节）；
  板端 800x1280 屏驱动就绪（`APP_ENABLE_DISPLAY`，无屏期关闭）
- ⚠️ 已知限制：esp-dl 特征提取在 P4 上存在执行级非确定性（详见
  `docs/esp-dl_issue_draft.md` 与 HANDOVER 廿二节 14/15 节）——绝对阈值跨开机不可移植，
  每 boot 相对标定 + 多帧投票绕行

## 仓库布局

```
jc_p4_app/            本工程（IDF v5.5.5）
  main/               应用代码（模块见下表）
  docs/HANDOVER.md    ★ 接手文档：全部结论/踩坑/验收记录（最优先阅读）
  docs/face_recog_day_plan.md   人脸识别功能规划（P0~P3 + 三轮实施）
  docs/usb_link_protocol.md     JCLINK 协议规范（两端共用）
  docs/pc_link_references.md    esp-dl/tinyusb 等技术核实记录
  docs/esp-dl_issue_draft.md    esp-dl 特征非确定性 issue 草稿（⏳ 待提交）
  docs/img/           UI/验收截图
  tools/              串口抓取/图像转换/传感器 patch 脚本
  sdkconfig.defaults  关键配置（BT/WiFi/栈/主频等，改动需 idf.py reconfigure）
pc_host/              PC 侧 Python 工具（仅标准库 + pyserial；py3.14 另有 cv2/Pillow）
  protocol.py         JCLINK 帧/CRC/TCP 传输库（其他脚本的依赖；ack 按命令名精确匹配）
  board_hub.py        ★ 取景中枢：独占板端连接 + 本机 13333 代理 + Tkinter 常驻取景窗口
  jc_test.py          CLI：register/list/mode/cam/stream/img/watch/demo
  thr_calib.py        阈值标定（注册者/陌生人分布 → HI/LO 建议；两段式 --phase a/b）
  _face_r1_test.py    识别回归验收（25 项，PC 送官方人脸图全自动；中枢在跑加 --tcp 127.0.0.1:13333）
  _f7_test.py         F7 多人脸验收（双人拼图 PC 送图）
  night_accept_test.py / ae_accept_test.py   夜视阶梯 / AE 回归
  _cycle_probe.py     状态机调试探针（单连接、时间表发命令）
../lvgl_project/lv_port_pc_vscode_official   LVGL SDL 模拟器（WSL 编译运行；
                      库内只保留自研 main/ 与配置，其 lvgl/build/bin 按根 README 自备）
【已在库内】../esp_video ../esp_cam_sensor   本地化并打过 patch 的组件（configure 必需，不可移动）
【需自备】  ../esp-dl ../esp-adf-libs ../lvgl  第三方组件（根 README「未入库的依赖」有获取命令；
                      删除/移动会导致 configure 失败；esp-dl/esp_video/esp_cam_sensor 已打 patch，
                      升级组件必须重打，见 HANDOVER 廿二节 15 节登记表）
```

## main/ 模块一览

| 模块 | 职责 |
|---|---|
| app_camera / app_camauto | SC2336 采集（钉 CPU1）+ 信箱式 sensor 写；AE/AWB/夜视增强自适应引擎（AE 曝光上限 40ms） |
| app_imgpipe | 帧池（4 槽）+ 降噪/镜像/WB 后处理 + 跨核缓存一致性回写/失效 + 按需发布（无人消费跳过整链） |
| app_faceai | 检测/特征/1:N（esp-dl）；多帧注册、平滑确认、双阈值+快速通道、事件去重、多人脸、轻跟踪节流、夜视门控（钉 CPU0） |
| app_usb_link / app_netlink | JCLINK 传输层（USB CDC + TCP server :3333，单客户端） |
| app_stream | 0x05 硬件 JPEG/raw 回传（限速任务，钉 CPU0；**源守卫：pc 模式不取帧**） |
| app_wifi / app_ble | C6 WiFi（esp-hosted SDIO）+ NimBLE BLE 外设（HCI over SDIO VHCI） |
| app_display / app_status | 板端 LVGL 页（无屏期关闭）+ 15 模块状态磁贴 |
| app_i2c/app_eth/app_audio/app_rs485/app_battery/app_sdcard | 板级外设（均已验证） |

## 编译 / 烧录 / 连接

```powershell
# Git Bash 里必须经 PowerShell 且先删 MSYSTEM（否则 IDF python 报 MSys 不支持）
powershell -NoProfile -ExecutionPolicy Bypass -Command `
  "Remove-Item Env:MSYSTEM; . C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1; `
   cd D:\esp32project\ESP32-P4\jc_p4_app; idf.py build"
# 烧录前 chcp 65001 + PYTHONIOENCODING=utf-8（中文 Windows GBK 会让烧录静默失败）
idf.py -p COM4 flash     # COM4 = CH340（烧录+日志）；抓日志 tools/serial_capture.ps1（会复位）
```

```bash
# 模拟器（WSL）；UI 代码 = ../lvgl_project/.../main/src/ui_shared.c（与板端同源）
cd /mnt/d/esp32project/ESP32-P4/lvgl_project/lv_port_pc_vscode_official
cmake -B build -S . && cmake --build build -j 8
./bin/main --host 192.168.33.2 --port 3333
# 改 UI 后的自动验证（不需要板子）：自截图 + XTest 合成点击
#   DISPLAY=:0 SIM_SHOT=<前缀> SIM_SHOT_MS=900 setsid nohup ./bin/main ... &
#   （每 0.9s 存一张 480x800 BMP，tools/bmp2png.ps1 转 PNG；点击脚本见 HANDOVER 廿二节 17 节）
```

```bash
# PC 侧快速操作（板子网口 192.168.33.2，无 DHCP 回落静态 IP）
python pc_host/jc_test.py --tcp 192.168.33.2:3333 watch 10          # 看结果流
python pc_host/jc_test.py --tcp 192.168.33.2:3333 register alice    # 注册（对镜头）
python pc_host/jc_test.py --tcp 192.168.33.2:3333 cmd '{"cmd":"register","name":"bob","shots":3}'
python pc_host/thr_calib.py --tcp 192.168.33.2:3333                 # 阈值标定（需真人）
```

## 常用运行时命令（完整语义见 HANDOVER 第三节/第廿一、廿二节）

| 命令 | 作用 |
|---|---|
| `{"cmd":"start"/"stop"}` | 识别启停（stop 取消未完成的注册采集） |
| `{"cmd":"register","name":X,"shots":3}` | 多帧注册（缺省 shots=1 兼容旧路径） |
| `{"cmd":"list"/"delete","name":X/"clear"}` | 名单管理（回包带 lib_feats 每人特征数） |
| `{"cmd":"mode","thr_lo":0.61,"thr_hi":0.61}` | 双阈值（编译默认 0.61/0.61；旧 `"thr":x` 兼容） |
| `{"cmd":"mode","fast_thr":0.80}` | F2 快速通道（单帧高分直达 match，0=关） |
| `{"cmd":"mode","dedup_ms":5000,"multi":2}` | 事件去重窗口 / 多人脸张数 |
| `{"cmd":"cam","fps":N,"dn":0-3,"night":true,"ae":true,...}` | 摄像头/夜视/AE 总控 |
| `{"cmd":"cam","ae_exp_max_ms":40}` | AE 曝光上限（0=解除；夜视档自动解除） |
| `{"cmd":"cam","pub_demand":false}` | 按需发布开关（默认开） |
| `{"cmd":"stream","value":"on","fmt":"jpeg/raw"}` | 0x05 预览开关 |
| `{"cmd":"wifi","ssid":X,"pass":Y}` | WiFi 运行时配网（NVS 保存） |

## 硬性纪律（踩过的坑，违反会重蹈）

1. **改 Kconfig.projbuild / sdkconfig.defaults 后必须 `idf.py reconfigure`**——增量 build
   不会重新生成 sdkconfig.h。
2. **组件条件依赖（`if(CONFIG_xxx)` 内的 REQUIRES 追加）不生效**——IDF 早期展开丢弃；
   依赖一律放 main/CMakeLists.txt 的**无条件基础列表**（esp_wifi_remote/esp_wifi/
   esp_hosted/bt/esp_mm/esp_rom 已在）。
3. **跨核缓存一致性**：P4 双核 L1 互不一致、`esp_cache_msync` 无跨核 IPI。凡
   "A 核写、B 核（或 DMA）读" 的缓冲：写侧 `DIR_C2M` 回写（在**该帧所有 CPU 写入之后**、
   发布之前），读侧 `DIR_M2C` 失效（已落在 app_imgpipe_get_frame）。**所有消费帧的任务
   必须绑核**（失效与读取同核），现 imgpipe/faceai/stream 均钉 CPU0、camera 钉 CPU1。
4. **faceai 上下文不直接做 V4L2 写 ioctl**——sensor 写一律走 camera 任务信箱
   （`app_camera_post_*`），否则与 camera 任务大数据处理并发会无日志整板重启。
5. **Git Bash 编译先删 MSYSTEM；烧录先 chcp 65001 + PYTHONIOENCODING=utf-8**。
6. **板子一次只服务一个 TCP 客户端**：中枢/模拟器/脚本三选一；中枢在跑时脚本一律连
   `127.0.0.1:13333`（protocol.py 的 ack 已按命令名精确匹配，不会被中枢补发的 ack 干扰）。
7. **esp-dl/esp_video/esp_cam_sensor 本地检出带 patch**——升级组件必须按 HANDOVER
   登记表重打；esp-dl 补丁同时是特征非确定性实验的仪表（`feat_ws`/`feat_bisect`，默认关）。
8. **诊断探针只要会额外调用模型就不能常驻**（会改变被测对象）——深探默认关
   （`APP_FACEAI_DIAG_DEEP=n`），`DIAG_KP` 无副作用可开。
9. **含反斜杠的批量代码改动别用 Bash heredoc**（折叠 `\\`）——用 Write 落 .py 脚本再执行；
   **pkill 与启动命令必须分两次调用**（同命令行含目标字面量会自杀）。
10. **LVGL 弹窗分层**（改 `ui_shared.c` 弹窗前必读）：`lv_msgbox_create(NULL)` 会自建**全屏
   可点击背板**（`lv_layer_top()` 上、mbox 的父对象）——建在屏幕层的控件（键盘）**完全点不动**；
   关窗只能 `lv_msgbox_close()`（`lv_obj_delete(mbox)` 会留下遮罩，主 UI 从此被盖住且点不动）；
   弹窗内控件要建在 `lv_layer_top()`，关窗后指针全部置 NULL。
11. **UI 文案用 ASCII**：LVGL 内置字体无中文字形，中文会渲染成空方框；**临时 `.ps1` 也别写
   中文注释**（PowerShell 5.1 按 ANSI 读脚本，UTF-8 中文会串码导致 param 块解析失败）。
12. **PC 送图（0x01）的 JPEG 宽高必须是 4 的倍数**：板端硬件解码对非 4 倍数尺寸直接回
   `event:"error"` + `msg:"jpeg decode failed"`（pipe.w/h=0，`dec_ms`=0）。2026-10-15 实测：
   `295x413`（奇数）❌、`294x414`（偶数但非 4 倍数）❌、`296x412`/`300x300`/`304x416` ✅
   （官方测试图 `faces/*.jpg` 都是 300x300）。用证件照/手机照注册前先 `resize`/白底 `pad`
   到 4 的倍数（推荐直接 304x416 或 300x300）。

## 当前待办（详见 HANDOVER 头部速览）

1. 镜头对准拍摄位 + 补光——检出连续性（21%）与成簇性是识别率唯一剩余瓶颈
2. ⏳ 提交 esp-dl issue（英文草稿 `docs/esp-dl_issue_draft.md` + 补丁 + trace 已备；
   Espressif 修复前，绝对阈值跨开机不可用，每 boot 相对标定绕行）
3. ae_accept_test.py 按 AE 曝光上限重校；白天自然光白参考重标（wb_kr/kb + ccm_mix）；
   BLE 手机端 nRF Connect 交互验收
4. P3 可选进阶：F11 换检测/特征模型、F12 活体防照片、F13 事件外发（见
   `docs/face_recog_day_plan.md`）—— **第 3 轮 F8/F9/F10 已全部完成 ✅**
   （F9 名表 64 人 `names.json`；F10 库备份/恢复 JCLINK 0x06 + `jc_test.py db_backup/db_restore`）
