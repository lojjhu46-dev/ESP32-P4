# jc_p4_app 开发接手文档（会话交接）

> 交接日期：2026-09-30（**最后更新 2026-10-11，F10 特征库备份/恢复完成（JCLINK 0x06 +
> db_export/db_import + jc_test.py db_backup/db_restore，见第廿四节）；同日 F9 容量提升：
> 名表迁移 /storage/names.json + 人数上限 10→64，见第廿三节**；上一轮 2026-10-07：第 2 轮收口
> + F8b 注册弹窗 UI 修复：F6/F7/快速通道/AE 曝光上限/F8 + esp-dl 非确定性定位）

---

# 🚀 新 agent 从零接手（先读这一节，5 分钟）

## 0. 一句话现状

固件/PC 工具链都可用；**2026-10-06 晚~10-07 凌晨：第 2 轮全部收口**——F6（事件去重）、
F7（多人脸）、**F2 快速通道**（单帧高分直达 match）、**AE 曝光上限 40ms**（暗光保锐度）、
**stream 源守卫**（修掉 pc 模式被预览偷帧的潜伏竞态）、发布端按需发布、**F8 模拟器 UI**
（shots 弹窗/稳定态/脸宽反馈/每框标签/名单计数）全部实现并真机验证，回归 **25/25**；
**2026-10-07 追加 F8b：注册弹窗 4 个 UI 缺陷修复**（键盘可点/OK 空输入置灰/`Shots:` 英文
标题/关窗不再残留全屏遮罩）+ 合成点击逐项验证 ✅（第 17 节，**仅模拟器侧改动，固件未动**）；
**esp-dl 特征非确定性已定位到执行级硬件竞态**（输入/kp/工作区全 crc 级恒定仍乱跳——
清零/C2M 均无效，上报材料已备：`docs/esp-dl_issue_draft.md`，⏳ 挂起待提交）。
**剩余硬前提：镜头对准拍摄位 + 补光**（检出连续性 21% 是识别率唯一瓶颈）。

## 1. 机器现状（2026-10-07 凌晨收工实况，用 `{"cmd":"mode"}` 可回读核对）

| 项 | 值 / 说明 |
|---|---|
| 固件 | MSRMNP + 帧池 4 槽 + CRC 探针关 + 按需发布 ON + F4(250ms) + **F6 去重 5000ms** + **F7 multi=2** + **F2 快速通道 0.80** + **AE 曝光上限 40ms**（夜视档自动解除）+ **stream 源守卫**（pc 模式不抢帧）+ **F9 名表 /storage/names.json（人数上限 64 / 特征 320）** + **F10 备份/恢复（0x06 块传输）** |
| `mode` ack 应为 | `detect=1 match=1 thr=0.61/0.61 (lo/hi) det_thr=0.50 trk=250ms/iou40/sh10% dedup=5000ms multi=2 fast=0.80 ws=0 bisect=0 msrmnp_s8_v1` |
| 阈值 | ✅ `0.61/0.61` 已持久化（sdkconfig + defaults）且已烧录生效——跨重启不再丢 |
| 特征库 | `['musk']`（1 特征；回归反复覆盖，`--enroll` 随时重注册） |
| 相机 | ⚠️ **镜头对着桌面/线缆**（最后一次抓帧无人脸）——**第一件事对准拍摄位 + 补光** |
| 取景中枢 / 模拟器 | 都已停。中枢：`python pc_host/board_hub.py --exit-after 7200 --zoom 2`（起来后脚本一律连 `127.0.0.1:13333`）；模拟器：WSL `cd lvgl_project/lv_port_pc_vscode_official && ./bin/main --host 192.168.33.2 --port 3333` |
| esp-dl 补丁 | ⚠️ 本地检出已打 patch（`zero_workspace/poison_workspace/run_trace/module_name` + Feat trace，见第廿二节 15 节登记表）——**esp-dl 升级必须重打** |
| 配置一致性 | ✅ sdkconfig 与 defaults 统一（MSRMNP + 0.61/0.61 + 全部新符号） |

## 2. 2026-10-06 晚 ~ 10-07 凌晨改动总表（便于 diff/回溯；详细数据全在第廿二节 8~16 节）

| 文件 | 改动 |
|---|---|
| `main/app_faceai.cpp` | **F6 事件去重**（dedup_ms 5000，运行时可调 0=关）；**F7 多人脸**（multi=2，boxes 第 5/6 位带 name/sim）；**F2 快速通道**（fast_thr 0.80，单帧高分直达 match）；结果帧新增 `lib_feats` 平行数组（每人特征数） |
| `main/app_camauto.c/h` + `app_faceai.cpp` | **AE 曝光上限**（`APP_CAM_AE_EXP_MAX_MS`=40ms，tick 加 exp_cap_lines 参，超上限转提增益；夜视档自动解除；`{"cmd":"cam","ae_exp_max_ms":N}` 运行时调） |
| `main/app_imgpipe.c/h` | 发布端按需发布（无消费者等帧且 <100ms 强制间隔→整链跳过，`pub_skip` 计数；`{"cmd":"cam","pub_demand":false}` 关） |
| `main/app_stream.c` | 🔴 **源守卫修复**：src=pc 时 stream 任务不再取帧——此前预览会偷走 AI 的帧（每帧只交付一次），中枢保活把 stream 钉在开才炸出来（经中枢跑 PC 送图必丢帧） |
| `main/Kconfig.projbuild` | 新增 `APP_FACEAI_EVENT_DEDUP_MS`(5000)/`APP_FACEAI_MULTI_MAX`(2)/`APP_FACEAI_FAST_THR`(80)/`APP_CAM_PUB_ON_DEMAND`(y)/`APP_CAM_PUB_MIN_MS`(100)/`APP_CAM_AE_EXP_MAX_MS`(40) |
| `main/app_faceai.cpp`（**F9，10-11**） | 名表持久化迁移 `/storage/names.json`（cJSON 数组，tmp+rename 原子写 + 直写兜底）+ NVS 只读兜底 + 首次开机自动迁移；上限 `MAX_PERSONS` 10→64、`MAX_FEATS` 32→320；名表加载移到 storage 挂载之后 |
| `main/Kconfig.projbuild`（**F9**） | 新增 `APP_FACEAI_MAX_PERSONS`(64)/`APP_FACEAI_MAX_FEATS`(320) |
| `sdkconfig` / `sdkconfig.defaults`（**F9**） | F9 新符号 + **`CONFIG_FATFS_LFN_HEAP=y`**（names.json 4 字符扩展名必需，见第廿三节） |
| `main/app_usb_link.h/c`（**F10，10-11**） | 新增 **JCLINK 0x06 块数据帧**（双向分片 `offset(u32)+data`）+ `app_usb_link_set_blob_cb()`（接收分支，回调在 rx 任务上下文）；发送复用 `app_usb_link_send` |
| `main/app_faceai.cpp`（**F10**） | `db_export` / `db_import` / `db_import_end` / `db_import_abort` 四个命令；备份信封 `"JCDF"+ver+face_len+names_len+names+face.db`；导入前自动 `.bak`、整包 CRC32+长度校验、覆盖替换后**重建 DataBase** 并复位 F2/F4/F6/F7 缓存；名表读写抽成 `names_to_json/names_parse_buf/read_file_all` 复用 |
| `pc_host/protocol.py` / `jc_test.py` / `_f10_test.py`（**F10**） | `T_BLOCK=0x06` + `send_block/recv_block`；`db_backup` / `db_restore` 子命令；F10 验收脚本（正向 + 负向 CRC 用例） |
| `esp-dl/esp-dl/dl/model/` + `vision/recognition/dl_feat_base.*` | ⚠️ **esp-dl patch**：`zero_workspace/poison_workspace/run_trace/module_name` + Feat trace 透传（非确定性实验仪表，feat_ws/feat_bisect 运行时开关，默认全关）；**升级 esp-dl 必须重打** |
| `sdkconfig` / `sdkconfig.defaults` | 全部新符号；defaults 检测模型 ESPDet→MSRMNP（统一）；阈值 0.61/0.61（两处都改） |
| `pc_host/_face_r1_test.py` | drain 修复（9~11/15→全绿）；P4 自适应阈值；P4.5 快速通道断言；P5 陌生人换 bill2；`--tcp` 参数 |
| `pc_host/protocol.py` | 🔴 `cmd_and_wait` 的 ack **按命令名精确匹配**（中枢补发的 stream ack 会冒充任何命令的应答） |
| `pc_host/_f7_test.py` / `esp-dl_issue_draft.md` | F7 双人验收脚本 / esp-dl issue 英文草稿（⏳ 挂起待提交） |
| LVGL 模拟器 `ui_shared.c` | **F8**：Register 弹窗 shots 选择(1/3/5)+采集进度条（Cancel=stop）；结果行稳定态（OK/…/??）+ 脸宽 `w px`；每框名字/相似度标签（json_boxes 扩展第 5/6 位）；名单特征数（json_lib 解析 lib_feats）。**F8b（10-07）**：弹窗 4 个 UI 缺陷修复——键盘改 `lv_layer_top()`（全屏背板吞点击）、OK 空输入置灰、`Shots:` 英文标题、`lv_msgbox_close()`（残留遮罩），见第 17 节 |
| 文档 | 本节 + 第廿二节 8~17 节 + `docs/face_recog_day_plan.md` 进度 |

## 3. 待办（按优先级；1 是硬前提）

1. **物理：镜头对准拍摄位 + 补光**——识别率的唯一剩余瓶颈是检出连续性（21%）与
   成簇性（F2 票数要 ≥4 帧连续簇）。**脸迎光、眼平高度、看镜头**；曝光上限已修（不再有
   100ms 模糊帧），补光后 50fps+锐利帧检出会上台阶。
2. **⏳ 提交 esp-dl issue**（挂起，用户定以后处理）：英文草稿 `docs/esp-dl_issue_draft.md`
   + esp-dl 补丁 + trace 全部就绪，github.com/espressif/esp-dl/issues/new；提交后链接回填
   第廿二节 15 节。**在 Espressif 修复前，"跨 boot 固定阈值"不可达**——每 boot 相对标定
   （thr_calib.py 两段式）的纪律继续有效。
3. **第 3 轮全部完成 ✅**：F9 容量（2026-10-11，见第廿三节：名表迁移 /storage/names.json +
   上限 10→64）、**F10 备份/恢复（2026-10-11，见第廿四节：JCLINK 0x06 + db_export/db_import、
   PC 端 `jc_test.py db_backup/db_restore`）**；F8 已完成（第廿二节 16 节；F8b 见第 17 节）。
   验收：`_face_r1_test.py` 25/25、`_f10_test.py` 12/12。**剩余唯一硬杠杆仍是第 1 条
   （镜头对准拍摄位 + 补光）**；可选进阶 F11/F12/F13 见 `face_recog_day_plan.md` P3。
4. **F7 已知局限**：副脸无平滑窗，esp-dl 单帧野值偶发闪错名（双人图 1/16，复跑未现）。
5. **ae_accept_test.py 按曝光上限重校**：其中依赖"exp 顶到 fps 最大值"的断言需调整
   （上限 40ms 绑定在 20/10fps 档；夜视档解除不受影响）。
6. 观察项：按需发布白天复核（`imgpipe/prof` 的 n=/skip=，预期省 CPU1 ~40%）。

## 4. 必读的坑（机械性，全踩过）

1. **只允许一个客户端连板子**：中枢/模拟器/脚本三选一。模拟器直连默认
   `192.168.33.2:3333` 且每帧自动重连会静默抢槽；中枢在跑时脚本一律连
   `127.0.0.1:13333`（`_face_r1_test.py` 已加 `--tcp`）。
2. **打开 COM4 会拉 DTR/RTS → 复位板子**。别边抓串口日志边测量；板端日志走
   JCLINK 0x04（`Link.logs`）。软复位/烧录后：以太网几秒自愈；**运行时值回编译默认**
   （现编译默认已是 0.61/0.61+fast0.80，无需重发）。
3. **`{"cmd":"cam","fps":N}` 会让夜视阶梯与 AE 让位** → 恢复必须补
   `{"cmd":"cam","night":true}` + `{"cmd":"cam","ae":true}`；**恢复顺序：先发 fps 再发
   night/ae**（反了会把它们再次关掉——踩了两次）。
4. **测吞吐/帧率必须钉死相机档位**；`detect=0` 时结果帧 `fps`/`feat_hz` 是陈旧值。
5. **netlink 偶发"命令无应答"**（连着不响应/上游 TCP 全断）：复位板子（COM4 打开即复位）
   + 中枢自动重连，一次即好；**"library: []" 可能是通信超时假象**——先 `list` 核对再下结论。
6. **排查画面异常先抓一帧**（`jc_test.py img`）：今晚"0% 检出"两次都是镜头没对准人
   （对显示器/对线缆），不是算法问题。
7. **经中枢跑测试的三条纪律**（protocol.py 已内置修复）：ack 按命令名精确匹配；帧结果
   读取跳过带 ack 的帧；脚本连 `127.0.0.1:13333` 不直连。
8. **批量代码改动含反斜杠时**：别用 Bash heredoc（折叠 `\\`）——用 Write 落 .py 脚本再执行。
9. **WSL 后台 GUI**：`DISPLAY=:0 setsid nohup ./bin/main ... &`（普通后台随 wsl 会话退出
   被杀）；**pkill 与启动命令必须分两次调用**（同命令行含 ./bin/main 字面量会自杀，第 3 次）。
10. **LVGL 弹窗分层（改 ui_shared.c 弹窗前必读）**：`lv_msgbox_create(NULL)` 会自建一块
    **全屏可点击背板**（`lv_layer_top()` 上、mbox 的父对象）→ 建在屏幕层的控件（键盘）
   **完全点不动**；关窗只能用 **`lv_msgbox_close()`**（`lv_obj_delete(mbox)` 会留下遮罩，
   主 UI 从此被盖住且点不动）。弹窗内的子控件要建在 `lv_layer_top()`，关窗后指针全部置 NULL。
11. **UI 文案必须 ASCII**：LVGL 内置字体（montserrat）无中文字形，中文字符串在界面上渲染成
   空方框（结果行/日志里已全部改英文）。另：**临时 `.ps1` 也别写中文注释**——Windows
   PowerShell 5.1 按 ANSI/GBK 读脚本，UTF-8 中文会串码导致 param 块解析失败（踩过）。

## 5. 常用命令速查

```powershell
# 编译/烧录（改 Kconfig 后必须先 reconfigure）
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
cd D:\esp32project\ESP32-P4\jc_p4_app; idf.py reconfigure; idf.py build; idf.py -p COM4 flash

# 取景中枢（窗口常驻、本机代理；脚本一律连 127.0.0.1:13333）
python pc_host/board_hub.py --exit-after 7200 --zoom 2

# 回归（25 项；--tcp 缺省直连板子，中枢在跑时必须指 127.0.0.1:13333）
python pc_host/_face_r1_test.py --tcp 127.0.0.1:13333

# 阈值标定（两段式；--enroll 真机多帧注册；⚠️ --phase a 会覆盖 _thr_calib_A.json）
python pc_host/thr_calib.py --tcp 127.0.0.1:13333 --enroll me --shots 3
python pc_host/thr_calib.py --tcp 127.0.0.1:13333 --phase a --a 30
python pc_host/thr_calib.py --tcp 127.0.0.1:13333 --phase b --b 30

# 运行时调参（都在 mode/cam 命令上，重启回编译默认）
jc_test.py cmd '{"cmd":"mode","fast_thr":0.80,"dedup_ms":5000,"multi":2}'
jc_test.py cmd '{"cmd":"cam","ae_exp_max_ms":40}'      # AE 曝光上限（0=解除）
jc_test.py cmd '{"cmd":"cam","pub_demand":false}'      # 按需发布开关

# F10 特征库备份/恢复（⚠️ 会与流式结果帧共用通道，大库建议先 stream off；hub 在跑时指 127.0.0.1:13333）
python pc_host/jc_test.py --tcp 192.168.33.2:3333 db_backup face_db_backup.bin
python pc_host/jc_test.py --tcp 192.168.33.2:3333 db_restore face_db_backup.bin
python pc_host/_f10_test.py --tcp 192.168.33.2:3333   # F10 验收（含负向 CRC 用例）

# 诊断仪表（默认全关）
jc_test.py cmd '{"cmd":"mode","feat_ws":1}'    # esp-dl 工作区清零（实验）
jc_test.py cmd '{"cmd":"mode","feat_bisect":6}' # 逐算子 CRC 二分（0x04 看 feat-bisect 日志）
```

```bash
# 模拟器 UI 验证（不需要板子、不用手点：自截图 + XTest 合成点击，详见第廿二节 17 节）
cd lvgl_project/lv_port_pc_vscode_official
mkdir -p /mnt/d/esp32project/ESP32-P4/_dev/shots   # 开发产物目录，见第 6 节「目录约定」
DISPLAY=:0 SIM_SHOT=/mnt/d/esp32project/ESP32-P4/_dev/shots/sim SIM_SHOT_MS=900 \
  setsid nohup ./bin/main --host 127.0.0.1 --port 13333 > /tmp/sim.log 2>&1 &   # 每 0.9s 存一张 BMP
# BMP→PNG：powershell -File jc_p4_app/tools/bmp2png.ps1 -In D:/.../sim_051.bmp -Out D:/.../x.png
#   ⚠️ 别读正在写的那个 BMP（会报"内存不足"）——取最新的**完整**文件（480x800 那档 1.5MB）
# 合成点击：WSL 侧 ctypes + libXtst（X 窗口 480x800 1:1 ⇒ X 坐标 == LVGL 坐标）
#   XOpenDisplay(":0") → XQueryTree/XFetchName 找 "LVGL Simulator" → XTranslateCoordinates
#   取 root 位置 → XTestFakeMotionEvent + XTestFakeButtonEvent(1) + XFlush
#   ⚠️ 64 位下必须给每个函数写 argtypes/restype，否则 Window 被截成 32 位（静默点空）
```

## 6. 目录约定（2026-10-15 整理；新增文件请按此归位）

工作区 `D:\esp32project\ESP32-P4\`：

| 路径 | 内容 | 备注 |
|---|---|---|
| `docs/` | **顶层开发文档**：`JC-ESP32P4-M3-DEV-开发要点.md`（硬件/引脚）、`项目框架构建指南.md` | 2026-10-15 从根目录移入 |
| `jc_p4_app/` | 固件工程（IDF v5.5.5）：`main/` 源码、`docs/` 交接与规划文档、`tools/` 脚本、`components/` | 主体 |
| `jc_p4_app/_dev/` | 固件工程的临时产物（`_bf_tmp.ps1`/`_changelog.md`/`_comp.json`/`_gh_issues.json`） | 可整目录删 |
| `pc_host/` | PC 端脚本（协议/取景中枢/回归/标定）。⚠️ **脚本与其运行时依赖（`_noface.jpg`、`_f7_two.jpg`、`_thr_calib_*.json`）必须同目录**，不可挪走 | 见第 4 节纪律 |
| `_dev/` | **开发期零散产物总汇**（可整目录删）：`logs/`（串口/启动日志）、`frames/`（调试图 jpg/png）、`raw/`（RGB565 裸帧）、`shots/`（模拟器 BMP 截图） | 2026-10-15 从根目录归位 |
| `_old_managed/` | 本地化前被移出的旧 managed 组件副本（可回退） | 见第十节 |
| `C3982-JC-ESP32P4-M3-DEV/` | 板卡厂商资料（规格书/原理图/Demo），原始 rar 包也放在其内 | 只读参考 |
| `esp-dl/` `esp_cam_sensor/` `esp_video/` `lvgl/` `esp-adf-libs/` | 本地检出/本地化的组件（`override_path` 依赖） | ⚠️ **不可移动/删除**，见第三节 |
| `lvgl_project/` | WSL LVGL 模拟器工程（`lv_port_pc_vscode_official`） | 见第四节 |

约定：
- 根目录**只保留** `.clangd`、`.zcodeignore` 与上述目录；新的临时产物直接写进分类目录
  （日志→`_dev/logs`、截图→`_dev/shots`、调试图→`_dev/frames`、裸帧→`_dev/raw`）。
- 固件工程内的临时文件放 `jc_p4_app/_dev/`。
- `.zcodeignore` 已忽略 `_dev/`，搜索不会被调试产物淹没。
- 遗留：根目录有一个 Windows 把 `> nul` 当文件名造出来的 `nul` 幽灵文件（无法用常规手段删除，
  不影响任何流程；需要清理时在 **cmd** 里执行 `del \\?\D:\esp32project\ESP32-P4\nul`）。

---

> 以下是**历史交接内容**（2026-10-05 及更早），结论仍有效但语境是当时的：
>
> **⚡ 新会话速览（2026-10-05 接手状态）**：任务 1~17 全部完成 ✅（真机验证）；
> 白天人脸识别第 1 轮 + 缓存一致性修复完成 ✅（第廿一节，回归 19/19）。
> 当前固件能力：SC2336 采集（白天 50fps/AE/AWB/CCM、夜视阶梯自动降帧+降噪+门控识别）
> → esp-dl 人脸检测/多帧注册/双阈值 1:N 识别 → JCLINK 0x02 结果 + 0x05 硬件 JPEG 预览
> → LVGL SDL 模拟器（WSL，`./bin/main --host 192.168.33.2 --port 3333`）；
> 网口 192.168.33.2 + WiFi（`{"cmd":"wifi"}`）+ BLE 广播 "JC-P4" 三路可达。
> **最近三层修复**（第廿一节，改缓存/线程相关代码前必读）：P4 双核 L1 互不一致 →
> 跨核缓冲写侧 C2M+读侧 M2C+消费者全钉 CPU0；IDF CMake 早期展开丢弃 if(CONFIG) 内的
> REQUIRES 追加（依赖放无条件基础列表）；⚠️ "esp-dl 混沌野值"**已查明大半是深探自己造的**
> （每帧多跑 1~2 次 `s_feat->run()` 污染后续帧，已用 `APP_FACEAI_DIAG_DEEP` 门控），
> 剩余部分是**执行历史/内存布局决定的"模式"**（同一构建跨会话自匹配会在 0.79 与 0.995
> 之间切换，**但陌生人在每个模式下都稳定在 0.01 附近**，分离度足够）——所以阈值必须
> **同一会话内相对标定**，不能用跨构建的绝对值。
> **待办**：⚠️ 本节待办已**上移到文首「🚀 新 agent 从零接手 → 3. 待办」**（唯一维护处），
> 避免两处不同步。下面 ③④⑤ 是当时（10-05/10-06 早）的条目，仍有参考价值：
> ③~~第 2 轮开工前移除/门控诊断字段（sim_q2/sim_f2/kp/ai_crc2）~~ ✅
> **已于 2026-10-05 完成**（`APP_FACEAI_DIAG_DEEP`，默认 n）——⚠️ 而且是**决定性修复**：
> 摘掉深探后自匹配 0.72→**0.997**、陌生人野值 0.839→**0.01**、分离度 gap 0.987，
> 单阈值方案重新可行。"esp-dl 混沌野值"的真凶是**探针自己**（每帧多跑 1~2 次
> `s_feat->run()`，模型执行历史相关 → 污染后续帧）。详见第廿一节。
> ④⚠️ **F11 检测模型：ESPDet+MNP 已实现，但当前不可用（2026-10-05）**——检出率
> **0~7% → 100%**（真机、同一构图，`infer_ms` 15~22ms → **71ms**），但**特征自匹配从
> 0.997 崩到 0.34**（PC 路径同图，0/14 ≥0.90）→ 识别废了。怀疑 ESPDet 框偏松导致
> MNP 关键点漂移、112×112 对齐错位。**下次开工先决定：修关键点（dump MSR vs ESPDet 的框+kp）
> 还是回退 MSRMNP**，定之前不要跑标定。⚠️ ESPDet 不出关键点，直接换会 panic，必须配
> MNP 二级。新增运行时检测阈值 `{"cmd":"mode","det_thr":0.35}`（不必重烧）。
> ⑤✅ **PC 端取景中枢 `pc_host/board_hub.py`（2026-10-06，真机验收 ✅）**：板子只服务
> 一个 TCP 客户端，模拟器一占就看不到画面 → hub 独占上游 + 本机 `127.0.0.1:13333` 透明
> 转发 + Tkinter 常驻窗口（画框/关键点/`box_w` 脸宽/结果状态栏）。**脚本一行不改**，
> 只把 `--tcp` 指向本机端口；关流 3~5s 自愈、板子重启自动恢复、多脚本并行画面不断。
> 详见第廿一节末「PC 端取景中枢」。⚠️ 纪律：**只允许 hub 连板子**，且**不要同时开 WSL 模拟器**
> （它默认 host 就是板子 IP 且每帧自动重连，会静默抢连接）。
>
> **里程碑史**：任务一~五 + 七 已完成，真机端到端跑通 ✅：
> 板载 SC2336（640x480@50fps）→ 图像管道 → esp-dl 检测/识别 → 0x02 JSON 回包；
> 板端新增 **0x05 图像回传**（硬件 JPEG）→ **LVGL SDL 模拟器**显示画面 + 检测框 +
> 名单 + 按钮操作（替代板子触摸屏），UI 与板端同源（`ui_shared.c` 只用 LVGL）。
> ⚠️ **图像采集只在板端做（用户 2026-09-30 定）**：PC 端不再采集、不再发 JPEG；`src=pc` 仅作备用分支。
> 任务六（host_app.py UDP 桥）**不再必需**：模拟器在 WSL 里直接 TCP 连板子。
>
> **2026-10-01 新增三大能力（全部真机验证，详见「十、本地化组件与驱动 patch」）**：
> ① **帧率/夜视可调**：esp_video/esp_cam_sensor 已本地化+patch，帧率 1~50fps 运行时可调
>   （改 VTS：帧率越低曝光上限越高，`{"cmd":"cam","fps":N}`）；15fps+拉满曝光 = 夜视。
> ② **时域降噪（DN）**：8x8 分块运动自适应多帧平均，静止场景噪声 -53%；档位 0~3、
>   UI 有 DN 循环按钮；高帧率（>25fps）自动跳过。
> ③ **运动自适应帧率（AUTO）**：运动 50fps ↔ 静止档自动切换（UI 第 4 行 AUTO 按钮），
>   切换耗时 ~2ms；独立运动估计（mv_peak/mv_ratio 每帧实时回传）。
>   ⚠️ 静止档不再是固定 15fps：自第十二节起由**夜视阶梯**决定（暗场 5fps），
>   关掉 AUTO 也把帧率交还给阶梯；探测判据 2026-10-02 重做过（见第十三节）。
> ⚠️⚠️ **两个必须知道的坑**（详见第十节）：A) faceai 任务里做 V4L2 ioctl 与 camera 任务
>   大数据处理（运动估计/降噪）并发会**无日志整板重启**——切换帧率时已规避，新增同类代码要警惕；
>   B) 已知遗留：AUTO 降档后 600ms 的 exp 拉满偶发不生效（画面偏暗），手动
>   `{"cmd":"cam","exp":3327,"gain":192}` 补救。
> **2026-10-01 下午新增（详见「十一、app 级 AE/AWB」）**：
> ④ **AE/AWB 自动曝光/白平衡**（app 级软件闭环，默认开）：帧内稀疏采样亮度/RGB → AE 调
>   exp/gain（50Hz 抗频闪量化）+ AWB 灰世界；**白平衡已下放 ISP 硬件 WBG**（零 CPU）；
>   IPA pipeline controller 已确认关死（其 metadata 会覆盖手动参数）。UI 第 3/4 行有
>   AE/AWB 按钮。手动操作自动退出自动模式。
> ⑤ **性能架构修复（camera 卡 26~38fps 的根因）**：CPU0 单核饱和——camera 任务已钉 CPU1
>   （`APP_CAM_CORE`）、WB 下放 ISP 硬件、netlink TCP 发送改非阻塞+整帧语义（慢客户端不再
>   拖死 AI，lwip 发送缓冲已扩 32KB）。实测 camera 50fps / AI 33fps / 无 WDT。**所有传感器
>   写操作（自动路径）走 camera 任务信箱**，faceai 不再直接 ioctl。
> ⑥ **夜视增强（2026-10-01 晚）**：app 层恢复 IPA 丢失的 ISP 低光增强——gamma 暗部提亮
>   （自适应，亮时无感/暗时 0.65，实测 2.8~4.3 倍提亮）+ BF 拜耳降噪（随增益 3~6 档），
>   走 /dev/video20 信箱。命令 `{"cmd":"cam","enh":bool,"gamma":x,"bf_level":N}`。详见第十一节。
> ⑦ **夜视阶梯（2026-10-01 晚第二轮）**：**自动**夜视真正吃满进光量 —— 暗度驱动降帧率
>   （默认常开：luma<45→5fps、<25→5fps 且降噪抬到 3 档；曝光上限 66ms→200ms，变亮/运动自动回升。
>    下限 2026-10-02 由 2fps 提到 5fps，见第 ⑧ 条）
>   + 夜视档自动开时域降噪（dn 2/3）。与运动自适应正交共用一套切换编排，手动 fps/dn 让位。
>   结果帧新增 `night`/`ntier`。真机验收 14/14（`pc_host/night_accept_test.py`）。详见第十二节。
> ⑧ **AUTO 运动切换卡顿修复（2026-10-02）**：夜视阶梯把静止档压到 2fps 后，
>   AUTO 的"运动探测窗口"（300ms）比帧周期（~950ms）还短，判定只能读陈旧值 →
>   50/2fps 每 ~1s 来回振荡 = 用户看到的 1~2 秒卡顿。已修（可用样本判据 + 单位 bug
>   + 帧率交还 + 下限 2→5fps），振荡归零、端到端 4517ms→1259ms。详见第十三节。
> ⑨ **光照双模式（2026-10-02）**：低光照=纯夜视——夜视档（ntier≥1）时 AI 不取帧
>   不推理、帧池整个让给 0x05 预览，以 2Hz 发 event="night" 状态帧；光照回 tier0
>   自动恢复人脸识别。`{"cmd":"cam","face_night":bool}` 可覆盖（默认开，Kconfig
>   `APP_FACEAI_NIGHT_OFF`）；结果帧新增 `face_night`。详见第十四节。
> ⑩ **白天曝光余量阶梯（2026-10-02，真机验证）**：白天 tier0 恒 50fps；exp 顶满且
>   增益超标持续 5s → 自动 50→20→10fps（20fps 与 50Hz 抗频闪对齐），余量恢复自动回升；
>   夜视档/运动自适应语义不变。修复"白天整体偏暗"。详见第十六/十七节。
> ⑪ **白天颜色根治（2026-10-02，真机验证）⚠️⚠️ 全文档最大教训**：困扰多日的"白天画面
>   颜色与人眼差距极大"真凶 = **0x05 JPEG 编码器对 RGB565 输入的通道序解读与标准相反**——
>   所有经 JPEG 的画面（模拟器预览、img 抓帧）红蓝对调（黄↔天蓝、橙红↔深蓝、肤色青蓝、
>   黑白绿正常），而 raw 直出/AI 路径一直正确。修复：app_stream.c 编码前 R/B 通道交换
>   （编码 4→44ms）。⚠️ 此前一切基于 JPEG 的颜色调参/结论（WB 1320/1645、高光偏粉、
>   CCM 观感）都隔着这层对调；同节含厂商标定 CCM 下发（`APP_CAM_CCM_MIX`，色彩增强）。
>   **颜色类调试第一步永远先 raw 直出对比**。详见第十八节。
> ⑫ **模拟器 UI 功能分组（2026-10-02）**：按钮按 识别×2/模式/帧率/画质 分组 + 状态栏
>   DAY/NIGHT 徽标 + Auto All 一键全自动 + 滑块 AE 互斥 + 连接即同步 + 预览等比修复
>   （原 480×300 裁切 + 检测框 Y 错位）。详见第十五节。
> ⑬ **WiFi 恢复（2026-10-02，真机验证）**：板载 C6 版本对钉死 **esp_wifi_remote 0.14.2 +
>   esp_hosted 2.12.13**（配出厂从机固件 2.3.2；3.0.x 宿主 major mismatch）。运行时
>   `{"cmd":"wifi","ssid":X,"pass":Y}` 连接（NVS 保存、开机自动重连）；实测关联（-64dBm）
>   + DHCP（192.168.194.206）+ PC ping 通。⚠️ TF 卡槽位错配（原 SLOT_1 与 C6 SDIO 同槽
>   互踩）曾是启动循环真凶，已归位 SLOT_0。详见第十九节。
> ⑭ **BLE 实施（2026-10-04，真机验证 ✅ 见第二十节）**：NimBLE host 跑 P4 + BT controller
>   在 C6（esp-hosted VHCI over SDIO），广播名 "JC-P4" + demo GATT（读/写/notify）；
>   `APP_ENABLE_BLE` 门控；HCI 同步 + 广播 + WiFi 共存实测通过（老从机固件 BT controller
>   自启，host 侧 init/enable RPC 失败可忽略）。⚠️ 实施过程踩平两个大坑：IDF CMake
>   早期展开丢弃 if(CONFIG) 内的 REQUIRES 追加（第二十节）；本工程内部 RAM 启动窗口
>   贴零线，BT 使能需同步瘦身（SDIO_RX_Q_SIZE 20→12 + NimBLE 内存池迁 PSRAM）。
> ⑮ **白天识别第 1 轮（2026-10-04，见第廿一节）**：多帧注册向导（shots+质量门控）、
>   连续 K 帧确认（tracking/match/reject 稳定语义）、双阈值 HI/LO、thr_calib.py 标定
>   工具、sim_raw/stable 结果帧字段。⚠️ 顺带发现**特征提取既有非确定性**（同图跨帧
>   sim 双模态漂移，疑似 PC 路径 JPEG 硬解缓存一致性，待台架排查）。
> ⚠️⚠️ **全局配置变更（2026-10-02）**：主任务栈 3584→**8192**（wifi 初始化链打穿默认栈）、
>   FREERTOS_HZ 100→**1000**（esp-hosted 要求；全系统 tick 粒度变细）、netlink 单帧发送
>   预算 100→**500ms**（raw 帧 614KB 需要）、bin 体积 **5.32MB**/8MB（剩 ~33%）。
> 写文件策略：write_to_file 失败时（报 "missing required parameter filePath"），立即改用
> PowerShell 数组 + base64 + WSL 管线：`$b=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((@('line1','line2') -join [char]10))); wsl -e bash -c "echo $b | base64 -d > /mnt/d/.../file.c"`
> （长内容分段追加 `>>`；命令过长会被拆解报错，此时拆小块重试）

## 一、总体计划（原任务 1~5、7、9~11 完成 ✅；6 废弃；8 已用 app 级方案完成 ✅）

**2026-10-02 起新增能力全部完成**：光照双模式（夜视不做人脸）、白天曝光余量阶梯、
白天颜色根治（JPEG 编码器红蓝对调修复 + 厂商 CCM）、模拟器 UI 功能分组、**WiFi 恢复
（真机验证，见第十九节）**。**2026-10-04：BLE 实施并真机验证通过（见第二十节，
任务 1~17 全部完成 ✅）；白天人脸识别第 1 轮完成 + 跨核缓存一致性修复（见第廿一节）**。
仍待办：①真机阈值标定（thr_calib.py，需真人在场）；②识别第 2/3 轮（face_recog_day_plan.md）；
③白天自然光下白参考重标（wb_kr/kb + ccm_mix）；④手机端 nRF Connect BLE 交互验收。

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
| 8 | ISP AE/AWB（画面偏绿偏暗）→ ✅ 2026-10-01 app 级闭环 + WB 下放 ISP 硬件（见第十一节）；PPA 缩图后再回传仍 pending | ✅/partial |
| 9 | **夜视三件套**（2026-10-01 ✅）：帧率可调（改 VTS，1~50fps）+ 时域降噪 DN 0~3（8x8 运动自适应，静止噪声 -53%）+ 运动自适应帧率 AUTO（静止 15fps ↔ 运动 50fps，切换 ~2ms）。详见第十节。⚠️ 遗留：AUTO 降档 exp 拉满偶发不生效 | ✅ |
| 10 | **夜视阶梯：自动夜视真正吃满进光量**（2026-10-01 晚 ✅，见第十二节）：暗度驱动降帧率（默认常开，15/5fps）+ 夜视档自动开 DN；与运动 AUTO 正交。真机验收 14/14 | ✅ |
| 11 | **AUTO 运动切换卡顿修复**（2026-10-02 ✅，见第十三节）：夜视阶梯压低静止档后，AUTO 探测窗口比帧周期还短导致 50/2fps 每秒振荡（=用户看到的 1~2s 卡顿）。振荡归零、端到端 4517ms→1259ms；下限 2→5fps | ✅ |
| 12 | **光照双模式**（2026-10-02 ✅，见第十四节）：低光照=纯夜视不做人脸，白天自动恢复识别 | ✅ |
| 13 | **白天曝光余量阶梯**（2026-10-02 ✅，见第十六/十七节）：白天 50fps 恒定 + exp 顶满自动 50→20→10fps，修"白天整体偏暗" | ✅ |
| 14 | **白天颜色根治**（2026-10-02 ✅，见第十八节）：**0x05 JPEG 编码器红蓝通道对调修复** + 厂商 CCM 下发——⚠️ 开发过程疑难全记录在本节 | ✅ |
| 15 | **模拟器 UI 功能分组**（2026-10-02 ✅，见第十五节） | ✅ |
| 16 | **WiFi 恢复**（2026-10-02 ✅，见第十九节）：板载 C6，版本对钉死，运行时 `{"cmd":"wifi"}`，实测关联+DHCP+ping 通 | ✅ |
| 17 | **BLE**（2026-10-04 ✅ 真机验证，见第二十节）：NimBLE host on P4 + C6 controller（esp-hosted VHCI），广播 "JC-P4" + demo GATT；HCI 同步/广播/WiFi 共存实测通过 | ✅ |
| 18 | **白天人脸识别第 1 轮 + 缓存一致性**（2026-10-04~05 ✅ 真机验证，见第廿一节 + `docs/face_recog_day_plan.md`）：多帧注册向导/连续帧确认/双阈值/标定工具；跨核缓存修复（写侧 C2M+读侧 M2C+全消费者钉 CPU0）；预览撕裂修复；回归 19/19。待办：真机标定、第 2/3 轮 | ✅/部分 |

## 二、已核实的技术结论（新会话直接用，勿重复调研）

1. **esp-dl**：官方支持 P4（IDF≥5.3）；模型 `models/human_face_detect` + `models/human_face_recognition`；例程 `examples/human_face_detect|human_face_recognition` 是 app_faceai 的直接模板。⚠️ v3 API 与 v2 差异大，以组件源码为准。加依赖：`espressif/esp-dl`（建议同时在 examples 里看它怎么引模型组件，模型可能要单独加 `espressif/esp-dl` 下的子组件或整仓）。
2. **esp_tinyusb ^2.0.0**：P4 官方支持。本地模板例程：`D:\ESP-IDF\v5.5.5\esp-idf\examples\peripherals\usb\device\tusb_serial_device\`。关键配置 `CONFIG_TINYUSB_CDC_ENABLED=y`（已写入 sdkconfig + defaults，漏了会报 "TinyUSB CDC driver must be enabled in menuconfig"）。
3. **JPEG 硬解**：用 IDF 5.5.5 **内置** `esp_driver_jpeg`（`driver/jpeg_decode.h`，P4 硬件 2D-DMA 解码），
   无需 `espressif/esp_new_jpeg`。参考例程 `esp-idf/examples/peripherals/jpeg/jpeg_decode`。
   PPA 同样内置（`esp_driver_ppa`）。
4. **PC LVGL 模拟器**：`D:\esp32project\ESP32-P4\lvgl_project\lv_port_pc_vscode_official` 已编译成功，WSL 目录下 `./bin/main` 启动。结构：`config/ FreeRTOS/ lvgl/ main/`。任务七把 ui_shared/net_link 加进去。
5. **agent-browser CLI**：已装 v0.38.1（`& "$env:APPDATA\npm\agent-browser.cmd" <cmd>`），复用 C 盘已有 Playwright Chromium（ms-playwright/chromium-1228）。⚠️ C 盘空间紧张，勿再往 C 盘装大件；Python 包装包用 `pip install --target D:\...`。
6. 参考报告：`jc_p4_app/docs/pc_link_references.md`（esp-dl/tinyusb/jpeg/lv_port 全部核实过）。
7. **⚠️ 颜色调参铁律（2026-10-02）**：0x05 JPEG 编码器通道序与标准相反（已修，见第十八节），
   修复前所有经 JPEG 的颜色观察/调参都是"隔着对调"看的。**颜色类调试/标定第一步必须
   raw 直出对比**（`{"cmd":"stream","value":"on","fmt":"raw"}` + `tools/rgb565_to_png.ps1`）。
8. **WiFi 组件版本对（2026-10-02）**：esp_wifi_remote **0.14.2** + esp_hosted **2.12.13**
   配出厂 C6 从机固件 2.3.2（3.0.x 宿主 major mismatch；2.0.13/2.0.17 有 RX 队列竞态
   启动循环）。registry 已恢复可达；组件经 managed_components 正常解析。
9. **esp_hosted 2.12.13 自带 BLE**：NimBLE host-only + VHCI 例程
   （managed_components/espressif__esp_hosted/examples/host_nimble_bleprph_host_only_vhci），
   BLE 已照此实施（2026-10-04，真机验证，实现细节与三个坑见第二十节）。

## 三、工程现状（D:\esp32project\ESP32-P4\jc_p4_app，IDF v5.5.5）

**编译状态：✅ build 通过 + ✅ 已烧录 COM4 真机跑通**（esp_tinyusb CDC + esp_driver_jpeg 硬解 +
esp-dl 人脸检测/识别 + esp_hosted WiFi），bin **5.32MB / 8MB 分区（剩 ~33%）**。烧录：`idf.py -p COM4 flash`，
看日志见下面「八、真机实测记录」。

⚠️ **离线依赖（2026-10-02 更新）**：组件注册表 `components.espressif.com` **已恢复可达**
（实测 HTTP 200；esp_wifi_remote/esp_hosted/eppp_link 即经 registry 正常解析）。esp-dl 全家桶
仍走本地检出 + override_path（历史原因，工作正常无需迁移）。⚠️ 若 registry 再次不可达，
新组件的获取方式 = GitHub 按 tag clone 到本地 + override_path（见 main/idf_component.yml
的 esp-dl 注释）。
既有本地检出（仍生效）：两个外部仓库在项目**上一级目录**：
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
| app_imgpipe.c/h | ✅ 新增 | 图像管道：摄像头/USB 帧 → 帧池（双缓冲最新帧语义）→ sink/ack。含 AE 亮度采样、时域降噪 DN（第十节）、独立运动估计（AUTO 用，输出 `mv_peak`/`mv_ratio` + **`mv_stats_us`/`mv_ready`** 供低帧率下判"样本是否新鲜可用"，第十三节）、`set_auto_ack()` |
| app_faceai.cpp/h | ✅ 新增 | 边缘 AI（HumanFaceDetect/HumanFaceFeat + `/storage/face.db`）；**运动自适应帧率 AUTO**（第十/十三节）与**夜视阶梯**（第十二节）状态机都在这里；命令队列 + 0x02 JSON 回传 |
| app_camauto.c/h | ✅ 新增(10-01) | AE/AWB/夜视增强自动引擎（第十一节）：目标亮度闭环、灰世界 AWB、gamma/BF 自适应；所有 sensor 写走 camera 任务信箱（`app_camera_post_*`），自身零 ioctl |
| app_ble.c/h | ✅ 新增(10-04) | BLE 外设（第二十节）：NimBLE host on P4 + C6 controller（esp-hosted VHCI）；广播 "JC-P4" + demo GATT；sync 看门狗 + pre-sched 堆水位探针 |
| app_netlink.c/h | ✅ 改(10-01) | JCLINK over TCP server(:3333)；发送改**非阻塞+整帧完成循环**（预算 100ms，慢客户端丢帧不阻塞 AI，撕裂靠 CRC 重同步） |
| app_status.c/h | ✅ 改 | 新增 `APP_MOD_USB`/`APP_MOD_PIPE`/`APP_MOD_FACE`（共 13 磁贴，显示实况） |
| main.c | ✅ 改 | 初始化顺序：nvs→i2c→**usb_link**→display(守卫)→sdcard→eth→wifi→audio→rs485→battery→camera→**imgpipe**→**faceai**；各守卫已加 |
| Kconfig.projbuild | ✅ 改 | `APP_ENABLE_*`、FACEAI 阈值、`APP_CAM_*`（WB/HFLIP/DN/MOTION/AUTO_FPS/DEF_GAIN/**AE/AE_TARGET/FLICKER50/AWB/AWB_MIN_LUMA/NIGHT_ENH/GAMMA_MIN/CORE**/**NIGHT_FPS(_LOW/_MIN)/NIGHT_LUMA_DARK(_ER)/NIGHT_DN_LOW(_MIN)**） |
| idf_component.yml | ✅ 改 | 加 `espressif/esp_tinyusb ^2`（任务四）+ 5 个 esp-dl 系依赖（全部 `override_path` 指向本地检出，**不走注册表**） |
| CMakeLists.txt | ✅ 改 | SRCS 加 app_usb_link.c、app_imgpipe.c、app_camauto.c、app_faceai.cpp；REQUIRES 加 esp_tinyusb、esp_driver_jpeg、json、wear_levelling、esp-dl、human_face_detect、human_face_recognition |
| sdkconfig(+defaults) | ✅ 改 | APP_ENABLE_*=y、TINYUSB_CDC_*、FACEAI 阈值；**AI 性能预设**：`COMPILER_OPTIMIZATION_PERF` + `CACHE_L2_CACHE_256KB` + `CACHE_L2_CACHE_LINE_128B`；`ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`=n、`LWIP_TCP_SND_BUF_DEFAULT`=32768（0x05 整帧必须装得下） |

### 其他既有模块（均已编译验证过）
app_i2c（I2C1 SDA7/SCL8）、app_display（DSI+JD9365+GT911+LVGL9.3 状态页）、app_wifi（esp_wifi_remote 经 C6）、app_audio（ES8311/ES7210）、app_sdcard、app_eth（RMII IP101）、app_rs485、app_battery（GPIO53 ADC）、app_camera（SC2336 持续采集+V4L2 控制封装+**写操作信箱**，camera 任务钉 CPU1）、app_stream（0x05 硬件 JPEG 回传）

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

⚠️ **改 Kconfig.projbuild 后必须 `idf.py reconfigure`**：增量 build 不会重新生成
sdkconfig.h——新符号在代码里取不到（bool=n 时宏干脆不定义），轻则取默认值、重则编译错
（2026-10-01 踩了两次；代码里 app_camauto.c 顶部有 `#ifndef` 兜底示范）。另外在 Git Bash
里调 PowerShell 编译要先 `Remove-Item Env:MSYSTEM`，否则 IDF 的 python 报
"MSys/Mingw is no longer supported"。烧录前 `chcp 65001` + `PYTHONIOENCODING=utf-8`。

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

## 七、环境备忘

- IDF 激活：`. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`（idf.py 为别名）
- Python：宿主机 3.14（C:\Users\cxk\AppData\Local\Programs\Python\Python314），pip 装包优先 venv 或 --target 到 D 盘；IDF venv 勿污染
- WSL 可用（bash/python3/base64 齐全）；C 盘空间紧张，避免大文件落 C 盘
- 板卡硬件要点、SC2336 适配结论、全部引脚表：见 `D:\esp32project\ESP32-P4\docs\JC-ESP32P4-M3-DEV-开发要点.md` 与 `docs\项目框架构建指南.md`
- **Git Bash 调 PowerShell 编译/烧录**：必须先 `Remove-Item Env:MSYSTEM`（否则 IDF Python 报
  MSys 不支持）+ `chcp 65001` + `PYTHONIOENCODING=utf-8`（中文 Windows GBK 会让烧录静默失败）
- **串口工具**：`tools/serial_capture.ps1`（抓日志，⚠️ 打开会 RTS 复位板子）；
  `tools/serial_read_passive.ps1`（**被动读取不复位**，DTR/RTS 拉低，抓连接类事件的实时日志）
- **状态机调试探针**：`pc_host/_cycle_probe.py`（单连接、按时间表发命令、打印结果帧跳变与
  板端日志）——调试夜视/白天阶梯这类带保持时间的状态机必用，`watch N` 的短窗口会误判
- ⚠️ **板端 netlink 单客户端**：模拟器/jc_test 快速连断会短暂残留槽位（下一连接超时），
  连接间隔留 2~3s 或重启板子
- ⚠️ **pkill 自匹配**：`pkill -f '[b]in/main'` 且 kill 与 launch 必须分两次调用（详见第十五节）

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
powershell -NoProfile -ExecutionPolicy Bypass -File D:\esp32project\ESP32-P4\jc_p4_app\tools\serial_capture.ps1 -PortName COM4 -Seconds 25 > D:\esp32project\ESP32-P4\_dev\logs\_boot_log.txt 2>&1
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
   - 📌 2026-10-02 注：WB 已下放 ISP 硬件 WBG（第十一节），且**白天颜色问题的最终根因
     是 0x05 JPEG 编码器红蓝对调**（见第十八节"真相"）——本条的观感结论是在对调未修复时
     得出的；`1320/1645` 增益推导恰好仍与 CCM 配套（第十八节有数学验证），但**白天自然光
     下白纸重标 wb_kr/kb 仍建议做一次**。
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
- **8x8 分块**（2026-10-01 由 16x16 改小：块太大时局部小运动会被块内均值稀释掉），
  每块取 4x4=16 个采样点，用**块内最大差**（G 通道 6bit 差）判运动 ——
  用最大差而不是均值：块里只要有一小片在动就必须判为运动，否则会留半透明拖影
- 静止块强混合、运动块按差线性衰减权重、**超过门限完全跳过**
- **跳过的块把当前帧刷进 prev**（关键）：否则旧影像会"冻"在历史里，等物体停下
  （差值落回阈值内）再被混回画面，表现就是"画面滞留"—— 这正是用户实测反馈的成因之一
- 像素混合 `out = cur + ((prev-cur)*wq)>>8`，**按 R/G/B 分量分别混合**
  （⚠️ 对 RGB565 整包做线性插值会**跨分量借位**：实测噪声反而涨 2.6 倍、出彩色伪影）
- 混合结果写回 prev（IIR，省掉一次 600KB memcpy）；`dn=0` 完全不进这段，零开销
- 失效重置：首帧 / 尺寸变化 / 换档 / 换图像源 / 改曝光·增益·帧率（`app_imgpipe_reset_prev()`）
- **帧率守卫**：`app_camera_get_fps() > CONFIG_APP_CAM_DN_MAX_FPS`（默认 25）时跳过 ——
  高帧率下几十 ms/帧的成本会把 camera 任务拖垮（实测 `dn_skip` 正常累加、帧率不受影响）

**档位**：`s_dn_tab[]` = {关, 低 50%, 中 75%, 高 92%} × **最大差**门限
（低 14..28 / 中 16..32 / 高 18..38）。板端 `{"cmd":"cam","dn":0..3}`；
UI 第 4 行循环按钮 `DN Off → Low → Mid → High`，结果行显示 `dnN mvNN%`。

**实测（真机 2026-10-01，8x8+最大差 版本）**：
- 静止场景：`dn_mmax: 13`（< 高档门限 18 → 全混 ✓）、`dn_mv: 5%`（噪声没被误判成运动 ✓）、
  `dn_blk: 4800/4800`、`noise2(lap)` **85.54 → 40.42（-53%）**（改造前 16x16 均值版是 -42%）
- `dn_ms: 39`（10fps 帧周期 100ms，占 39%）—— **这就是必须要有帧率守卫的原因**
- 50fps：守卫跳过；10fps/25fps 稳定：`dropped: 0`、无 task_wdt、0x02 流不中断
- 截图：`docs/img/ui_sim_denoise_20261001.png`（16x16 版）、`ui_sim_mv_20261001.png`
  （8x8 版，结果行 `cam10fps dn3 mv4%`）
- ⚠️ **运动场景的"无拖影"结论需要用户实拍确认**（挥动手掌/移动物体，观察 `mv` 升高时
  物体边缘是否还有半透明残影）；门限若需微调就改 `s_dn_tab` 的 `mmax_lo/hi`

**调试字段**（结果帧，仅在 dn>0 时带）：`dn` 档位、`dn_ms` 耗时、`dn_blk` 混合块数、
`dn_sad` 差均值口径（旧）、`dn_mmax` 最大差口径（当前判据）、`dn_mv` 运动块占比 %、
`dn_skip` 被守卫跳过帧数。调门限靠 `dn_mmax`/`dn_mv`：静止时应 `mmax < lo` 且 `mv` 接近 0，
挥手时应 `mv` 明显升高。

**物理限制（务必向用户说明）**：运动物体的模糊来自"曝光时间 ≤ 帧周期"这一物理约束
（10fps→约 100ms、25fps→约 40ms、50fps→约 20ms），软件无法消除；时域降噪只能消除
**自己造成的算法拖影**。所以**低帧率长曝光只适合静态/慢速场景**，动态场景请用 25/50fps。

**推荐组合**：

| 场景 | 帧率 | 降噪 | 说明 |
|---|---|---|---|
| 静态弱光（夜视） | 10fps | DN High | 最亮最干净；`mv` 应接近 0 |
| 静态但想稍快点 | 15/25fps | DN Mid/High | 曝光上限降低，亮度略降 |
| 有人走动/物体运动 | 25/50fps | DN Off/Low | 优先保证不拖影；50fps 会自动跳过降噪 |
| 想看清运动细节 | 50fps | DN Off | 20ms 曝光，物理模糊最小 |

新增工具：`tools/raw2png.py`（RGB565→PNG，纯标准库）与 `rgb_stats.py` 的
`noise2(lap)` 二阶差分指标（对噪点敏感、对线性边缘不敏感，比一阶梯度更能区分二者）。

### 运动自适应帧率（AUTO 模式，2026-10-01）

> ⚠️ 本节写于夜视阶梯之前。**静止档后来改由夜视阶梯决定**（暗场 5fps 而不是固定 15fps，
> 见第十二节），探测窗口的判据也在 2026-10-02 重做过（见第十三节，原文描述的"按时间
> 判 300ms 窗口"已被证明在低帧率下失效）。读本节时以那两节为准。

用户需求：静止 15fps（夜视+降噪），画面出现运动自动切 50fps（无拖影），两全其美。

**mv 恒 0 的根因（重要教训）**：`s_cam_dn_*` 统计缓存**只在收到 cam 命令时**读一次
`app_imgpipe_get_stats()`，之后流式结果帧一直重复那次快照 —— UI 看到的 mv 是"点击
瞬间的冻结值"。修复：新增 `cam_state_tick()`，在 faceai 主循环每帧取帧后刷新
（一次结构体拷贝，成本可忽略）。⚠️ 任何"结果帧回读的实时值"都必须走每帧刷新，不能缓存快照。

**独立运动估计**（`app_imgpipe.c` → `pipe_motion_estimate()`，与降噪解耦）：
- 8x8 块 + 4x4 采样，每帧**无条件**把当前帧刷进**独立的**参考帧缓冲（PSRAM ~600KB，
  仅自动模式启用时分配）——只比相邻帧，不需要 IIR 历史，也不受降噪开关/守卫影响
- 输出：`mv_peak`（全帧块最大差的**峰值** 0..63，比均值灵敏 —— 少数运动块的大差值
  不会被几千个静止块稀释）与 `mv_ratio`（块差 ≥ 阈值的块占比 %）
- 仅自动模式启用（`app_imgpipe_set_motion_detect`），省 ~2ms/帧与 600KB；
  块阈值 `{"cmd":"cam","mv_thr":N}` 可标定（默认 30；静止噪声实测块最大差 ~13-20，挥手 30+）

**状态机**（faceai 任务，按时间迟滞，`auto_fps_tick()`）：
- 运动持续 **100ms** → 切 50fps；静止持续 **1.5s** → 回静止档（现为 `night_idle_fps()`）；切换后 300ms 冷却
- 切换联动（缺一不可）：`app_camera_set_fps()` + `app_imgpipe_reset_prev()` +
  **exp/gain 拉到新上限**（帧率上限随 VTS 变，不拉满的话 50→15 回落时画面只有 30% 亮度）
- 命令：`{"cmd":"cam","auto_fps":true|false}`、`{"cmd":"cam","mv_ratio":N}`；
  结果帧**恒带** `auto`（0/1）、`mv`（自动模式用运动估计占比 / 关闭时回退 dn_mv）、`mv_peak`
- 手动 fps 命令会自动退出自动模式（板端与 UI 都同步）

**⚠️ 并发坑（实测触发整板重启，根因未明、已绕开）**：在 faceai 任务里做
`app_camera_get_ctrl/set_ctrl`（8 组 V4L2 ioctl）时，若 camera 任务的运动估计
（600KB 分配/每帧拷贝）同时在跑，会触发**无 panic 日志的整板重启**（稳定复现）。
规避：`auto_fps_apply` 里先 `set_motion_detect(false)` 暂停估计，主循环 500ms 后恢复。
**后续凡是"faceai/命令上下文做 V4L2 ioctl + camera 任务做大数据处理"的组合都要警惕。**

**实测（真机 2026-10-01）**：开启自动 → 15fps、exp 自动 3327/3327；`mv_thr=1` 伪造运动 →
数帧内切 50fps、exp 自动 994/994；`mv_ratio=100` 伪造静止 → 1.5s 回 15fps；全程无崩溃。
UI：第 4 行 `AUTO OFF/ON` 按钮（原 10fps 档移除，命令行仍可设）、结果行
`cam15fps auto mv0% dn2`。截图 `docs/img/ui_sim_autofps_20261001.png`。

**卡顿优化（同日第二轮）**：切换路径原为 ~19 个同步 V4L2 ioctl（set_fps 1 + get_ctrl 16 +
set_ctrl 2），与 camera 任务取流争设备锁 → UI 冻结数秒。优化后：**升档 1 个 ioctl**
（利用 sc2336_set_fps 内置 exp clamp：VTS 变小自动把 exp 压到新上限=天然拉满）、
**降档 1 个 ioctl + 600ms 后补 2 个**（exp_max 用公式 `1000*50/fps-6` 算、gain 用缓存，
拉满挂在恢复运动估计的同一时机）。实测 `af_ms=1~2ms`（结果帧新增 `"af_ms"`）。
另发现 50fps 下运动估计持续跑会拖垮 camera 任务（帧率掉到 ~26fps、长时间后无日志重启）
→ **active 状态改为脉冲探测**：每 2s 开 300ms 估计（回落检测用），其余关闭。
**已知遗留**：降档后 600ms 的 exp 拉满（boost）偶发不生效（exp 停在 994，画面偏暗），
用户可手动 `{"cmd":"cam","exp":3327}` 补救；待查（怀疑与 IPA/AE 或 ioctl 失败有关，
串口日志未抓到 S_EXT_CTRLS failed）。

**烧录环境坑（2026-09-30 踩到）**：
- `idf.py -p COM4 flash` 在中文 Windows 上可能因 esptool 进度字符（`ϵ`）触发
  `UnicodeEncodeError: 'gbk' codec can't encode ...` 而**静默失败**（表现为"没有 Hash of data verified"）。
  先 `chcp 65001` + `Set-Item Env:PYTHONIOENCODING utf-8` 再烧。
- 烧录前先确认串口在位：`[System.IO.Ports.SerialPort]::GetPortNames()`；
  CH340 的 COM4 若在设备管理器里是 `Unknown`，说明 USB 数据线断了（板子可能仍在跑，以太网照常）。

## 十一、app 级 AE/AWB 自动曝光/白平衡（2026-10-01，真机验证 ✅）

### 结论与背景（重要，纠正旧认知）

- **P4 没有"硬件直接改 sensor 曝光"的 HW AE**：ISP 硬件只做统计（AE 5x5 亮度块/AWB 白块
  /HIST），AE 闭环必然是软件。esp_video 的 IPA pipeline controller（`CONFIG_ESP_VIDEO_ENABLE_
  ISP_PIPELINE_CONTROLLER`，闭源 libesp_ipa.a + sc2336_default.json）就是 Espressif 的软件 3A。
- **IPA 控制器已确认关死**（sdkconfig 与 defaults 一致）。之前 sdkconfig 里它其实开着（WB 实验
  忘了关），实测它：①AGC 不抬增益（gain 恒 0，曝光被推到 994 也不加增益）；②AWB 校正不足
  （偏绿只从 1.53 修到 1.46）；③**每帧用 metadata 覆盖手动 exp/gain**（`esp_ipa_pipeline_init`
  的初始 metadata 会把传感器写回 519/0，运行中也会改）——Night/AUTO 档"参数莫名变暗/不生效"
  的可疑根源之一。它还带一个 **prio 11 的 isp_task** 每帧做 IPA 运算+写回，与应用抢 CPU/锁。
- **旧"WB 三条路全 FAIL"的真正根因**：`app_camera_set_wb()` 把 `V4L2_CID_USER_ESP_ISP_WB`
  (0x009819E5) 发给了 **/dev/video0（CSI 设备）**，而这个 control 属于 **/dev/video20（ISP 设备）**
  ——CSI 设备把它转发给 sensor 查表失败。0x00980914/0x00980915 也不是 RED/BLUE_BALANCE，
  而是 **HFLIP/VFLIP**（QUERY/SET 在 sc2336 侧可用、GET 未实现——所以"设置后读回没变"是 GET
  缺分支，SET 可能已经生效）。
- 用户选择方案：**app 级软件闭环**（不用闭源 IPA），AE/AWB 默认开启，手动操作自动退出自动。

### 实现（新模块 `main/app_camauto.c/h`）

- **AE**：imgpipe 在 publish_raw 里（降噪/镜像/WB 之前）对摄像头帧做稀疏采样（每 8px×每 8 行，
  RGB565 位段拆取），产出 `ae_luma/ae_r/ae_g/ae_b`（0..255，stats 新字段 + `ae_stats_us` 时戳）。
  faceai 主循环每帧调 `app_camauto_tick()`（内部节流 250ms，写后 500ms 冷却）：亮度出死区
  （±6）才动作；**乘法律单步限幅 0.5x~2x**；太暗先加 exp 后加 gain、太亮先降 gain 后降 exp；
  exp 上限跟帧率 `1000*50/fps-6`；**50Hz 抗频闪把 exp 量化到 500 行（=10ms）整数倍**
  （`AE_FLICKER_STEP`，代价：量化边界上有 ±7% 的静态误差，属预期）。
- **AWB（灰世界）**：软件路径量的是 WB 前的 R/G/B → `kr=G/R、kb=G/B` 绝对灰世界；**ISP 硬件
  WB 路径**（见下）统计在上游已被校正 → 在当前增益上做相对修正 `kr*=G/R`。EMA 1/8 平滑 +
  3% 死区 + 钳位 800..2500，亮度 <40 不校（暗帧不可信）。
- **白平衡统一入口** `app_camauto_wb_apply/GET`：**ISP 硬件 WB 优先**——打开 `/dev/video20`，
  信箱下发 `V4L2_CID_USER_ESP_ISP_WB`（WBG 增益乘进 CCM，零 CPU）；video20 打不开才退回
  imgpipe 软件 WB（此时软件层 kr/kb 自动置 1000 防双重校正）。手动 wb_kr/wb_kb、UI WB 按钮、
  AWB 引擎全走这一个口。
- **手动优先**：手动 exp/gain/fps（cam 命令/Night/Day 预设/滑块）→ AE 退出；手动 wb_kr/wb_kb
  → AWB 退出（与"手动 fps 退出 auto_fps"同规则）；`{"cmd":"cam","ae":bool,"awb":bool,
  "ae_target":N}` 随时切。
- 结果帧恒带 `ae/awb/luma/ae_tgt`；ack msg 扩展 `ae=x awb=x luma=x tgt=x`。
- 模拟器 UI 第 3/4 行改为各 6 按钮：新增 **AE ON/OFF**（第 3 行末）与 **AWB ON/OFF**（第 4 行末），
  激活绿色高亮、随板端状态同步（被手动操作踢掉会变灰）。顺带修了一个旧 bug：UI 找
  `"auto_fps"` 字段同步 AUTO 按钮，但板端发的是 `"auto"`——从未同步成功过。

### 传感器写操作信箱（治卡顿的关键，务必保持的纪律）

`app_camera` 新增 pending-mailbox：`app_camera_post_exp_gain/post_fps/post_wb` 投递写请求，
**camera 任务在自己循环的安全点（QBUF 后、DQBUF 前）串行执行**（自旋锁保护合并，同类请求
取最后一次）。自动路径（AE 调节、auto_fps 切换/boost、ISP WB）全部走信箱——faceai 上下文
不再有任何写 ioctl，"faceai ioctl + camera 任务大数据并发=整板重启"的坑类被整体绕开。
手动命令仍是直接写（低频、用户动作）。读操作（cam_state_tick 每帧 get_fps、handle_cmd
get_ctrl）维持现状。

### 性能架构修复（本次最大收获，2026-10-01 实测）

**症状**：关闭 IPA 控制器后 camera 卡 38fps + IDLE0 WDT + faceai 模型加载卡死（prio 低的
faceai 被 camera 饿死）；开回控制器则全家降到 26fps（prio 11 的 isp_task 抢走 CPU）。
**根因：CPU0 单核饱和**——camera 每帧 600KB 拷贝(7ms)+软件WB+镜像(15~20ms)，faceai 推理
~20ms/帧，全部挤在 CPU0（CPU1 一直闲着）。
**修复**（三件套）：
1. **camera 任务钉到 CPU1**（`CONFIG_APP_CAM_CORE` 默认 1）——采集/拷贝/后处理整链挪走，
   AI 独占 CPU0；
2. **白平衡下放 ISP 硬件 WBG**（见上）——软件 WB 的每帧 614KB LUT 遍历归零；
3. netlink **TCP 发送改非阻塞 + 整帧完成循环**（`app_netlink.c tcp_write`）：阻塞 send 被慢
   客户端（WSL NAT 链路）拖住时整个 AI 链路跟着客户端排水速度走（实测 5fps）。⚠️ 单纯
   "满即丢帧"不行——**lwip 发送缓冲默认只有 ~5.7KB，一帧 0x05 JPEG 有 10~20KB，非阻塞
   send 必然只发出一半，帧被撕裂、接收端全丢（实测画面一张都到不了）**。最终方案：
   `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768`（装得下整帧）+ 非阻塞 send 的**整帧完成循环**
   （EAGAIN 时 1 tick 重试、总预算 100ms，超时才放弃）；一个字节没发出去时放弃才是干净
   丢帧。`s_tx_lock` 保留防多任务发送帧交错，socket 设 O_NONBLOCK（recv 的 EAGAIN 分支
   要 vTaskDelay(1) 防空转）。

**实测效果**：camera **50.0fps 稳定**、模型加载 293ms（曾卡死）、无 WDT、AI 33.4 帧/s
（原生 Windows 客户端）。

### 真机验收记录（2026-10-01）

```
INIT    exp=500 gain=58 luma=112 ae=1 awb=1 wb=1518/1760 cam_fps=50
STEADY  5s: 33.4 帧/s, luma 恒 112（无振荡）
NIGHT   手动 gain192/exp994 -> ae 自动退出(0) ✓
AE 重开 从 (994,192) 过曝 252 收敛：gain 先降 192->33，8s 内回落 ✓（先降增益后降曝光正确）
WB 手动 1000/1000 <-> 1320/1645 经信箱到 ISP 硬件 ✓（awb 正确让位）
AUTO-FPS 0.10s 切换，15<->50 双向 ✓
截图    docs/img/（_sim_final_001.png：画面自然不偏绿、AE/AWN 按钮高亮、滑块同步）
```
已知小特性：①抗频闪量化下 AE 有 ±7% 静态亮度误差（量化步长 500 行所致，可关
AE_FLICKER_STEP 换精度）；②15fps 档 + stream 开启时 AI 约 9~15 帧/s——2 槽帧池被 AI(15/s)
+stream(10/s) 两个消费者分抢，属帧池设计特性，夜视档建议关 Preview。

### 夜视增强：app 层恢复 ISP 硬件 Gamma 提亮 + BF 降噪（2026-10-01 第二轮，真机验证 ✅）

**用户反馈"低光夜视不如修改前"的根因**：修改前 IPA 控制器（虽是误开着）每帧往 ISP 写
低光增强；关闭它时这些增强整体消失——① **gamma 暗部提亮**（sc2336_default.json aen 段
gamma_param=0.688，画面发暗发灰的主因）；② **BF 拜耳域降噪**（adn 段随增益 level 3→7，
高增益噪点更重）；③ sharpen/demosaic/对比度自适应（次要，未恢复）。app 层 AE 只推高
exp/gain，补不回 gamma 的观感。

**恢复方式**（`app_camera.c` 信箱扩展 + `app_camauto.c` 自适应，均经 /dev/video20）：
- `V4L2_CID_USER_ESP_ISP_GAMMA(0x009810E1)`：payload `esp_video_isp_gamma_t{enable,
  points[16]{x,y}}`。**两个硬坑（均已踩平）**：① IDF 驱动校验 x 首点必须 >0（x[0]=0 直接
  INVALID_ARG）、相邻差为 2 的幂、末点 x=255——用固定网格
  `{1,2,4,8,16,32,64,96,128,192,224,240,248,252,254,255}`；② 曲线语义是 **x=输入位置、
  y=输出值**（受控 A/B 确认：线性完全透明，y=x^0.65 提亮 2.8 倍）——曾按"输出基准"
  反写导致压暗，已回退。
- `V4L2_CID_USER_ESP_ISP_BF(0x009810E2)`：`{enable, level∈[2,20], matrix 3x3∈[0,15]}`，
  矩阵取 IPA 的 {2,4,2;4,5,4;2,4,2}；esp_video 对 BF 是 stop+start live 重配。
- **自适应策略**（`app_camauto` ENH 状态机，默认 `CONFIG_APP_CAM_NIGHT_ENH=y`）：
  gamma 按 EMA 亮度（1/4 平滑、2s 节流、量化 50）——≥70 线性 / ≤45 最强
  （`APP_CAM_GAMMA_MIN`=650）/ 中间线性过渡；BF 按增益分档（<8→3、≥8→4、≥48→5、
  ≥120→6）。与 AE 的测量耦合（提亮抬高实测亮度）靠慢节流+EMA+宽迟滞压制。
  手动命令让自适应让位：`{"cmd":"cam","gamma":0.65}`（<3 按浮点否则千分比）、
  `{"cmd":"cam","gamma_en":false}`、`{"cmd":"cam","bf_level":N}`、总开关
  `{"cmd":"cam","enh":true|false}`（重开=恢复自适应）。结果帧带 `gamma`（千分比，
  0=禁用）/`bf`（level，0=禁用）。ENH 与 AE/AWB 开关独立——手动 Night 档（AE 关）
  照样自适应。

**真机实测（2026-10-01 晚，真实暗光环境）**：
- 固定曝光 (994,192) 下 A/B：线性 avg_luma 14.7 → gamma0.65 **41.3（2.8×）** →
  gamma0.5 **63.5（4.3×）**（`tools/img_check.ps1` 量 JPEG）。
- 自动模式暗场收敛落点：`luma=33 exp=994 gain=192 gamma=650 bf=6`——AE 到顶后
  gamma 自动最强、BF 自动 6 档，原始亮度 ~8 提到 33。
- 白天无回归：luma>70 时 gamma=1000（线性）。
- ⚠️ 新观察：暗场 AE 满曝光（exp=VTS-6=994@50fps）时 camera 实际帧率掉到 ~26fps
  （传感器曝光占满帧周期的固有特性；需要帧率时切 25/15/10fps 档让 VTS 变大即可恢复）。

### 遗留

- 传感器 HMIRROR SET 实测返回失败（errno 3，sc2336 驱动拒绝），软件镜像仍兜底（性能已够：
  只剩镜像一趟 8ms）；后续可查 sc2336_set_mirror 的调用条件。
- sc2336 驱动 get_para_value 缺 HMIRROR/VFLIP get 分支（本地检出可补）。
- AE 参数（死区/步长/量化）按当前环境标定，换场景若不满意先调 `ae_target` 与 `AE_DEADZONE`。
- 旧遗留"AUTO 降档 exp 拉满偶发不生效"未再复现（IPA metadata 覆盖嫌疑已随控制器关闭消除），
  继续观察。
- **暗场满曝光时 camera 实际帧率 ~26fps**（exp=VTS-6@50fps 的传感器固有特性，非回归）；
  需要帧率时切 25/15/10fps 档（VTS 变大）恢复。若要修，可把 AE 的 exp 上限收窄到 ~90% VTS。
- **netlink 偶发 ack 丢失**：客户端频繁快连快断时，板端单客户端槽位偶有残留（recv 断开检测
  依赖 recv 返回 0，半开连接要等 keepalive）。现象是偶发"命令无回复、重连即好"。帧流本身
  不受影响；要根治可在 accept 前探活旧连接或缩短 keepalive。
- **-night 增强手动/自动切换语义**：手动 `gamma`/`bf_level` 后自适应永久让位，需
  `{"cmd":"cam","enh":true}` 恢复——若想要"手动 N 秒后恢复自动"，在 `app_camauto_enh_resume_auto`
  上加定时即可。⚠️ 2026-10-01 已修：原来 `enh_set()` 在"本来就是开着"时直接早退，
  导致手动 gamma 之后再发 `enh:true` **恢复不了**自适应（画面会一直停在线性 gamma、luma 掉到个位数）。
  现在 `enh:true` 无论当前状态都会清手动让位并重建 EMA。
- 工具：`pc_host/ae_accept_test.py`（AE/AWB 真机验收脚本，收敛性/手动切换/auto_fps 一键回归）、
  `pc_host/night_accept_test.py`（夜视阶梯验收，见第十二节）、
  `pc_host/stutter_probe.py`（切换卡顿量测：注入运动并逐帧记录到达间隔与 cam_fps 变化，见第十三节）。

## 十二、夜视阶梯：暗度驱动的降帧率 + 自动时域降噪（2026-10-01，真机验证 ✅）

### 动机（先纠正一个旧认知）

第十/十一节做完后，自动夜视在**很暗**的场景仍然不理想。实测定位到根因**不是曝光不够**，
而是两件事：

1. **自动档从来不会降帧率**。AE 在暗场 1 秒内就把 exp/gain 顶到上限（50fps 下 exp=994=20ms）
   然后永远停在那里 —— 而实测"亮"的最大余量其实在**降帧率换长曝光**上：50fps→15fps 是 3 倍进光量，
   →5fps 是 10 倍（理论值；实际超过约 5fps 后继续降就不再变亮，见第十三节，所以下限就取 5）。
   旧的 `auto_fps`（AUTO 按钮）只由**运动**驱动，静止时最多到 15fps，
   而且默认是关的，所以"自动夜视"根本没用上这块。
2. **最有效的降噪工具默认是关的**。长曝光把画面拖进噪声主导区（实测：50fps 时一片糊，
   5fps 时能看清门框/面板但彩色噪声淹没画面；**同一档开 DN=3 后噪声汤变成可看画面**，
   JPEG 体积从 56KB 掉到 27KB）。而 `CONFIG_APP_CAM_DN` 默认 0，自适应夜视路径也从不碰它。

### 实现（`main/app_faceai.cpp`，与 auto_fps 并列）

- **一个帧率阶梯，两个驱动源**（方向相反、正交）：
  - 运动（AUTO 开关）只往**高**帧率升档 → `AUTOFPS_ACTIVE_FPS`(50)；
  - 暗度（夜视阶梯，**默认常开**）只往**低**帧率降档。
  - 静止时的目标帧率统一走 `night_idle_fps()`：tier0 → `AUTO_IDLE_FPS`(15)，tier1 → 5，tier2 → 5。
    （⚠️ 2026-10-02 起下限由 2 提到 5，见第十三节：帧率越低 AUTO 升档延迟越大，而实测 2fps
    并不比 5fps 亮；当前 tier1/tier2 帧率相同，区别只在自动降噪档。）
    **运动状态机的回落分支也已改走这个函数**，否则运动结束后会错误回到 15fps（这是本次最关键的交互点）。
  - 两者共用 `auto_fps_apply()` 的切换编排（暂停运动估计 / 改 VTS / 复位时域历史 / 拉满新曝光上限 / 通知 AE），
    没有第二份实现。
- **判定**：输入 `s_cam_luma`（AE 帧内采样，每帧刷新）做 1/8 EMA，带迟滞（进入用阈值，
  退出要越过"阈值+10"）与**保持时间**（变暗/变亮各持续 3s 才切），防阈值边缘抖动。
  阈值沿用 ENH 口径：`APP_CAM_NIGHT_LUMA_DARK`=45、`..._DARKER`=25。
- **手动让位**：`{"cmd":"cam","fps":N}` 既退 auto_fps 也退阶梯（`night` 变 0）；
  `{"cmd":"cam","night":true}` 恢复，并且**会立刻把当前档位的帧率/降噪拉回来**
  （否则用户在夜视档手动改过 fps 后会一直停在手动值 —— 首版漏了这步，被验收 T5 抓到）。
- **自动 DN**：`night_apply()` 按档下发 `dn`（tier1→2、tier2→3、tier0→用户手动值）；
  手动 `{"cmd":"cam","dn":N}` 置 `s_dn_manual` 让自动让位，`night:true` 复位。
- **boost 解耦（踩过的坑）**：降档后"延迟 600ms 把 exp/gain 拉到新上限"原先挂在
  `auto_fps_tick()` 的运动估计恢复时机里，而那个函数在 `!s_auto_fps` 时直接早退 ——
  阶梯降档时 AUTO 可能是关的，boost 就永远不会执行（画面停在旧曝光、偏暗）。
  现抽成 `auto_boost_maybe()`，由主循环每帧调用，与 AUTO 开关解耦。

### 配置与命令

| Kconfig | 默认 | 说明 |
|---|---|---|
| `APP_CAM_NIGHT_FPS` | y | 阶梯总开关（默认常开） |
| `APP_CAM_NIGHT_FPS_LOW` / `_MIN` | 5 / 5 | tier1 / tier2 的目标帧率（`_MIN` 原为 2，见第十三节） |
| `APP_CAM_NIGHT_LUMA_DARK` / `_DARKER` | 45 / 25 | 档位阈值（退出另有 +10 迟滞带） |
| `APP_CAM_NIGHT_DN_LOW` / `_MIN` | 2 / 3 | tier1 / tier2 的自动降噪档 |

命令：`{"cmd":"cam","night":true|false}`；结果帧恒带 `"night"`(0/1) 与 `"ntier"`(0/1/2)。

### 真机验收（`pc_host/night_accept_test.py`，14/14 通过）

暗场静止实测（自动档）：开机约 7s 后 `night: luma 33 (ema 26) -> tier 1` →
`auto-fps -> 5 fps` + `night: auto dn -> 2` + `sc2336: fps -> 5 (vts 10000, exp_max 9994)`，
600ms 后 `boost: fps change -> exp=9994 gain=192`；继续变暗则 tier 2 → 5fps（当前下限，
即帧率不再变、只有 `dn` 抬到 3）。下限若调回 2fps，则是 `exp_max 24994`。

验收脚本的**设计要点（值得复用）**：环境照度不可控，所以不假设房间多亮 ——
造暗用"关夜视增强 + gamma 线性"，造亮用"手动强提亮 gamma 0.35"（都是 ISP 真实通路，
且不跟阶梯抢曝光）；某一方向被环境光卡住时记为 **SKIP 而非 FAIL**。硬判据是**不变量**：
只要阶梯在管帧率，`cam_fps` 必须等于当前档位目标帧率、`dn` 不得低于档位要求 ——
按"同一档位的连续样本段"取中位数，滤掉"档位已变、帧率还在 camera 任务信箱里"的异步窗口
（直接逐帧比对会被这个窗口刷出一堆假失败）。

| 段 | 内容 | 结果 |
|---|---|---|
| T1 | 手动 15fps → 打开阶梯 → 自动降档 | PASS（tier1/5fps，exp_max 9994） |
| T2 | 夜视档自动开 DN | PASS（dn 2→3、dn_ms 39ms、dn_blk 4800） |
| T3 | 反向：主动变亮 → 档位回落 | PASS（luma 21→88，tier 1→0，fps 5→15） |
| T4 | 运动优先：伪造运动升 50fps，静止后回落**夜视档**而非 15 | PASS |
| T5 | 手动 fps 让位 / `night:true` 恢复 | PASS |
| INV | 稳态段帧率/降噪必须跟随档位 | PASS（每轮 3~5 个稳态段） |

回归：`pc_host/ae_accept_test.py` 仍通过。⚠️ **两个预期内的语义变化**：①AUTO 打开时
初始/回落帧率现在是 `night_idle_fps()`（暗场即 5fps），不再是固定 15；②**关掉 AUTO 会把帧率
交还给夜视阶梯**（不再是"保持当前帧率不动"），否则会把人留在 50fps —— 手动 fps 命令不受影响
（它会先关阶梯）。

### 遗留 / 注意

- **"更暗档是否值得"已定论（2026-10-02）**：单看亮度，超过约 5fps 之后继续降帧率并不更亮 ——
  50fps→1fps 的 JPEG 均值 39.6→13.5（峰值 48→80、结构更清楚），2fps vs 5fps 同场景连续抓帧
  则是 avg_luma 3.2 vs 4.9、JPEG 20KB vs 31KB。加上低帧率会成倍放大 AUTO 的升档延迟，
  所以下限取 5fps 是**没有亮度代价**的更优解。要极限进光量再把 `APP_CAM_NIGHT_FPS_MIN`
  调回 2（延迟翻倍）。
- 暗场满曝光时 camera 实际帧率低于标称（传感器固有：exp≈VTS 时约只有一半），
  所以"5fps 档"实测约 2.6fps，属预期。
- 夜视档 AI 只有几帧/秒，且与 0x05 回传分抢 2 槽帧池 —— 夜视档建议关 Preview。

## 十三、AUTO 运动切换"卡顿 1~2 秒"的根因与修复（2026-10-02，真机验证 ✅）

### 现象与结论

用户反馈：**静态低光下 AUTO ON 夜视效果最佳，但识别到运动物体切换帧率时有明显 1~2 秒卡顿。**

实测根因**不是切换动作慢**（`af_ms` 一直是 1~2ms），而是三层叠加：

1. **来回振荡（主症状）**。板端日志实测：`auto-fps -> 50 fps` 之后约 950ms 就 `auto-fps -> 2 fps`，
   如此每 ~1 秒反复一次，50/2 之间来回跳 —— PC 侧看到的就是反复卡顿。
   机理：AUTO 高帧率态靠"每 2s 开一个 ≤300ms 的探测窗口"判断是否还在动。
   第十二节的夜视阶梯把**静止档从 15fps 压到 2fps 后帧周期变成 ~950ms**，
   **窗口里根本不会产生新帧**，判定只能读切换瞬间被复位成 0 的陈旧运动值 → 误判"不动了"
   → 掉回 2fps → 下一拍又检测到运动 → 升回 50fps。即：夜视阶梯打断了 AUTO 探测窗口的隐含前提
   （静止档帧周期 ≪ 窗口长度）。这是一个**跨模块的隐含假设**，改任一侧都要重新验证。
2. **判定依据不对**。判定按"时间到没到"而不是"窗口内有没有拿到可用样本"；
   而重开运动估计后的**第一帧只是基准帧，mv 恒为 0**，拿它判定等于"没动"。
3. **单位 bug（老代码就有）**：`now - s_auto_last_probe_us >= AUTOFPS_PROBE_PERIOD_MS` 里
   `now` 是 `esp_timer_get_time()` 的**微秒**，常量却是 `_MS`（2000），实际等于 2ms ——
   探测窗口一直在 40ms 级反复开关，运动估计被反复复位，`s_auto_last_probe_us` 也形同虚设。

### 修复

- **`app_imgpipe`**：统计新增 `mv_stats_us`（采样时刻）与 `mv_ready`（本样本是否来自**真实比较**，
  基准帧恒 0 不算）——判"有没有新样本"和"能不能据此下结论"都必须靠它们。
- **`app_faceai` AUTO 状态机**：
  - 探测窗口只在拿到**可用样本（新 + 真实比较）**或超时后才关闭；
    拿不到就不下结论、保持 50fps（低帧率下窗口太短是常态，不能当成"静止"）。
  - 升档后加**最短驻留 1.5s**（`AUTOFPS_MIN_ACTIVE_MS`）。
  - 修单位：按 ms 比较（`× 1000`）。
  - **保持时间**在"一帧本身就比保持时间长"时跳过（夜视档 1s/帧 ≫ 100ms 的保持，白等一整帧）。
  - `auto_fps_tick/night_tick/camauto_tick` 改**每轮主循环都跑**（含 `get_frame` 超时轮），
    只在拿到新帧才跑的话，决策会被多拖一整个帧周期。
  - `auto_fps_set(false)` 现在**把帧率交还给夜视阶梯**（并清掉脏的 `s_auto_active`）：
    AUTO 只管"运动升档"，静止帧率一向是阶梯的职责；顺带手动 fps 命令改成**先关阶梯再退 AUTO**，
    否则交还动作会把用户刚设的手动帧率顶掉。
- **`Kconfig`**：`APP_CAM_NIGHT_FPS_MIN` **2 → 5**。帧率越低，升档延迟越大（≈识别 1 帧 + 排写 1 帧），
  而实测**2fps 并不比 5fps 亮**（同场景连续抓帧 avg_luma 3.2 vs 4.9，JPEG 20KB vs 31KB；
  与最早那次探针"超过约 5fps 后继续降帧率均值反而下降"一致）——所以这一档**没有亮度代价**。

### 实测（真机）

| 指标 | 修复前 | 修复后（2fps 档） | 修复后（5fps 档，当前默认） |
|---|---|---|---|
| 50/2 来回振荡 | 每 ~1.5s 一次 | 无（120s 内只升档 1 次） | 无 |
| 帧到达最大间隔 | 990ms | — | **389ms** |
| 运动→升档端到端 | 4517ms | 2903ms | **1259ms** |
| 其中"排写"段（决策→写寄存器） | 910ms | 910ms | 340ms |

（端到端数字含探针自身的 `mv_thr` 改值造成的重新建基准 ≈ 1 帧；真实运动少约一帧。
15fps 静止档下实测约 340ms。）

回归：`pc_host/night_accept_test.py` 14/14、`pc_host/ae_accept_test.py` 通过；
连续观察无 `task_wdt`、无复位、`drop` 无异常增长。

### 遗留

- **"排写"段仍是一个帧周期**：`camera_task` 阻塞在 `DQBUF(portMAX_DELAY)`
  （`esp_video_ioctl.c:191` 里 `ticks` 被写死，`esp_video` 也没有 `.poll_p`），
  信箱里的写请求只能等 QBUF 之后执行。要再砍这段得给本地检出的 `esp_video` 打补丁
  （非阻塞 DQBUF 或支持 poll），属于驱动改动，本次没做。
- **验收脚本要注意的两个前提**（都踩过）：探针注入用 `mv_thr` 会让 imgpipe 重建基准、
  多花一帧；AUTO 运动接管期间夜视档位是**冻结**的（阶梯按设计让位），
  所以"帧率必须跟随档位"的不变量校验要排除 `auto=1 && cam_fps>=50` 的样本。
- `night_accept_test.py` 的 `TIER_FPS` 已随下限改成 `{0:15, 1:5, 2:5}`；
  若把 `APP_CAM_NIGHT_FPS_MIN` 调回 2，记得同步改回 `{2: 2}` 与 `INV-曝光上限` 的期望（~24994）。

## 十四、光照双模式：低光照夜视（不做人脸）/ 白天人脸识别（2026-10-02）

### 需求与设计

用户需求：摄像头功能按光照划分——**低光照只做夜视、不做人脸；光照充足做白天人脸识别**。
夜视阶梯（第十二节）已经按亮度自动定档（tier 0=亮 / 1=暗 / 2=很暗），所以不新造模式状态机，
直接**用档位门控 AI**（`night_face_gated()`，app_faceai.cpp）：

- **门控条件**：`face_night 开 && 夜视阶梯开 && src=cam && ntier>=1`，缺一不可：
  - 阶梯关着（用户走手动 Night/Day 预设/手动 fps）不门控——手动模式尊重用户选择；
  - **src=pc 永不门控**：阶梯 luma 只在摄像头路径采样（`app_imgpipe_publish_raw` 在
    PC 源下第一行就早退，tier 对 PC 帧无意义），且 PC 送帧的窗口=1 每帧应答语义必须保持；
  - AUTO 运动接管期间档位冻结（tier 保持 ≥1），夜视档下运动升 50fps 也**不恢复识别**，
    与"低光照不做人脸"语义一致。
- **夜视档下 AI 完全不取帧**（不是"取了帧跳过推理"）：`get_frame` 换成 `vTaskDelay(200)`。
  帧池 2 槽整个让给 0x05 预览——原来夜视档 AI 与 preview 分抢帧池（第十二节的已知问题），
  现在 preview 独享，暗场预览帧率约翻倍。
  **关键前提（改前已到源码验证）**：AE luma 采样、运动估计、时域降噪都在 camera 任务的
  `publish_raw()` 里做，与 AI 取帧无关——所以门控后夜视阶梯/AUTO/AE 照常运转；
  `cam_state_tick()` 改为**每轮主循环都跑**（不取帧也刷新统计快照），亮度回升就能自动出档
  （否则 s_cam_luma 冻结、永远出不了夜视档——这是本功能最容易埋的坑）。
- **状态上报**：夜视档下以 2Hz 发 `event:"night"` 状态帧（faces=0、无框，msg 带 tier，
  gain/exp/dn/luma/ntier 等字段照常带），UI/PC 据此显示模式；进入/退出夜视档各打一条
  0x04 日志（`face: night mode (tier N)` / `face: day mode`）。AI fps 口径在夜视档失效
  （s_fps 清零），画面帧率看 `cam_fps`。光照回 tier0 自动恢复识别，无专门 day 心跳。

### 命令 / 配置 / 字段

- `{"cmd":"cam","face_night":true|false}`：运行时开关（默认 Kconfig `APP_FACEAI_NIGHT_OFF=y`，
  sdkconfig 已生成；bool=n 时宏不定义，代码里 #ifndef 兜底为 0）。
  结果帧恒带 `"face_night"`(0/1)；cam 查询 ack msg 末尾带 `face_night=N tier=N`。
- 夜视档下 `register` 仍武装成功，ack 追加 "(night mode: face off, runs when light
  returns)"——特征提取要等光照恢复才有帧可吃。
- UI（ui_shared.c）：结果行 ntier≥1 时追加 ` nightN` 标记（ASCII，LVGL 默认字体无中文字形）；
  事件行直接显示 night 状态帧的 msg。

### 验证状态与验收方法

- ✅ 固件编译通过（bin 5.34MB / 8MB，剩 36%）、✅ 模拟器编译通过。
- ✅ **真机验证（2026-10-02，TCP 192.168.33.2）**：房间环境光太亮（最低曝光 luma 仍 ~90，
  任何软件造暗手段都会被阶梯 boost 反杀），故采用**临时抬高阈值法**（sdkconfig
  `NIGHT_LUMA_DARK/DARKER` 45/25→150/100，让 AE 目标亮度 120 真实落入 tier1），
  完整链路真机确认后改回重烧：
  - 进档：`night tier 1 -> fps 5` → **`face: night mode (tier 1) - face off, preview only`**
    → 0x02 变为 2Hz `{"faces":0,"event":"night","msg":"face off, preview only (tier 1)"}`
    （一次长观察抓到 61 条 night 帧，AI 取帧确实停了）；
  - 回档：`night tier 0 -> fps 15` → **`face: day mode - face recognition on`**
    → 正常识别帧立即恢复（50fps、event:"none"、infer_ms 正常）；
  - 门控随档位 0↔1 多次往返，每次进入/退出日志与帧类型都正确切换；
  - 结果帧 `face_night:1` 字段、cam 查询 ack 的 `face_night=1 tier=0` 均正常。
  - ⚠️ 亮房间+人为阈值下 boost 会把画面推到过曝导致档位 0↔1 来回（约 10s 一个周期），
    这是阶梯 boost 的既有行为（真实暗场不存在），与门控无关。
- 结果帧里 night/ntier/face_night 等 cam 字段在**开机后第一次 cam 命令之前不带**
  （`s_cam_state_valid` 语义，与 gain/exp 等字段一致）；UI 连接时自动发 cam 查询，实际无影响。
- 已知边界：夜视档+stop 时无状态帧（stop 语义优先，start 后 500ms 内恢复）；
  `detect off` 与门控叠加时夜视优先；src 切到 pc 立即恢复识别（tier 冻结不影响 pc 路径）；
  AUTO 运动接管结束回落后若仍在暗档，门控保持（识别不恢复）。
- 真实暗场（不抬阈值）的验收待用户在夜间熄灯做：暗场开机 ~7s 进档 → UI 出 ` nightN` 标记、
  event 行显示 night → 开灯 ~7s 内恢复识别。`{"cmd":"cam","face_night":false}` 可对照
  （夜视档也出识别帧）。

## 十五、模拟器 UI 整理：功能分组 + 模式徽标 + 等比预览（2026-10-02）

只改 `lvgl_project/.../main/src/ui_shared.c`（纯 LVGL，板端可复用；固件/main.c 零改动）。

### 新布局（480x800，内容区 468px 宽）

```
[状态栏 30：✓ connected | 模式徽标 | src cam]
[预览 flex_grow（等比 letterbox + 检测框，~366px 高）]
[结果行] [事件行]
[Start][Stop][Register][Delete]        ← 识别操作
[List][Clear][Camera][Preview ON]      ← 识别操作
[Auto All][Night Hi][Night Lo][Day]    ← 模式
[50fps][25fps][15fps][AUTO OFF]        ← 帧率
[Mirror ON][WB ON][AE ON][AWB ON][DN]  ← 画质（5 键各 ~89px，不再截断）
[gain 滑块 28] [exp 滑块 28]（AE 开时置灰 + "(auto)" 后缀）
[lib 14] [日志 56]（高度未按用户要求加大）
```

### 新行为

- **模式徽标**（状态栏中间）：night=1 && tier0 → "DAY"（青）；tier≥1 → "NIGHT n"（琥珀
  `C_NIGHT 0xE8A33D`）；阶梯关着不显示。字段缺失（未发过 cam 命令）时为空。
- **Auto All 按钮**：`{"cmd":"cam","night":true,"auto_fps":true,"ae":true,"awb":true}` 一键回
  全自动；板端回报 night=1&&auto=1&&ae=1&&awb=1 时按钮青色高亮（`set_result` 驱动）。
- **滑块与 AE 互斥**：AE 开 → 两滑块 `LV_STATE_DISABLED` 置灰 + 标签 "(auto)"；按下滑块若
  AE 开则先发 `{"cmd":"cam","ae":false}` 并同步按钮/置灰，再正常拖动。滑块标签统一走
  `slider_label_refresh()`（三处写标签的地方都走它，否则结果帧回写会抹掉后缀）。
- **连接即同步**：`ui_shared_set_link()` 连上后多发一条 `{"cmd":"cam"}`——滑块位置、全部
  开关按钮、徽标立即对齐板端（实测连接即显示 gain 0/exp 500/L104/DAY）。
- **等比预览修复**：`img_holder` 原 480px 宽 > 内容区 468px（真溢出）且 640x480 画面按宽
  缩放成 480x360 后被 300px 容器裁掉底部 60px、检测框 Y 坐标按 300/480 压缩错位。
  改为：holder 宽 100% + flex_grow 占满剩余高度；图像统一缩放
  `scale=min(holder_w/rgb_w, holder_h/rgb_h)`（lv_image 默认 pivot=中心，`lv_obj_center`
  即 letterbox 居中）；检测框 = `原图坐标 × scale + letterbox 偏移`（`disp_scale/ox/oy`）。
  顺带修"名字标签画在所有框上"→ 只标第一个框（与原注释意图一致）。IMG_W/IMG_H 宏已删。

### 实测（真机 TCP + 截图）

- 连接后自动同步：DAY 徽标、gain 0/exp 500 滑块位置、AUTO OFF/AE ON/AWB ON 按钮态全部
  来自板端（截图 `_ui_fix_001.png`）；
- 命令置 auto_fps 后下一帧 Auto All 变青、AUTO 按钮变 ON、结果行 `cam15fps auto`
  （截图 `_ui_auto_001.png`）；恢复 auto=0 后高亮消失（同一代码路径）。
- 工程坑（再次踩）：`pkill -f '[b]in/main'` 与**启动命令写进同一个 bash -c** 时，
  外层命令行含 `./bin/main` 字样会被自己的 pkill 杀掉（exit 15、啥也没跑）——
  kill 与 launch 必须分两次调用。

## 十六、白天/夜视的 fps·gain 自动调节分离（2026-10-02，真机验证 ✅）

### 需求（用户定）

**白天（tier0）不做低光照的那套 fps/gain 自动调节**——降帧率换静态可见度、运动升档保
流畅、增益拉满，都是夜视的逻辑；白天恒定满帧率。AE（亮度闭环）保留：那是常规曝光调节，
白天画质需要它。

### 现状里白天被低光照逻辑接管的三条路径（都已修）

1. **白天静止帧率被压到 15fps**：`night_idle_fps()` tier0 原返回 `AUTOFPS_IDLE_FPS(15)`，
   夜→日回落、AUTO 开关都会把白天打到 15fps（UI 截图 `cam15fps` 的来源）。
2. **白天也跑运动自适应**：AUTO 开着时白天照样 15↔50 切换，且运动估计持续跑
   （50fps 下持续估计把 camera 拖到 ~26fps，第十节实测）。
3. **白天误挂 boost**：白天 `auto_fps_apply(50)` 因等值被判成"降档"→ exp/gain 拉满 →
   白天过曝闪一下。

### 修复（app_faceai.cpp）

- **`night_idle_fps()` tier0 → `AUTOFPS_ACTIVE_FPS`(50)**：白天目标帧率恒定 50；tier1/2
  仍 5fps。夜→日转换、AUTO 开/关、白天进 AUTO 全部自然落到 50fps。
- **`auto_fps_apply()` 等值 no-op**：目标帧率 == `s_cam_fps` 时直接返回（不写 VTS、不挂
  boost）。tier1→2（同为 5fps）的冗余切换也在此跳过；自动 DN 在 night_apply 里继续处理
  不受影响。
- **`auto_fps_tick()` 白天门控**：阶梯开 && tier==0 → 清运动态 + **关运动估计** + 状态机
  不跑（AUTO 只在夜视档 tier≥1 工作）。切进夜视档时 night_apply→auto_fps_apply 重置恢复
  计时，估计自动恢复。**阶梯关着（手动模式）不门控**，保留原行为。
- **`APP_CAM_AUTO_IDLE_FPS`（15fps 静止档）退役**：Kconfig 项、宏、sdkconfig 行已删
  （reconfigure 已跑）。
- 结果：白天的帧率/增益完全稳定（AE 除外），boost/降档/运动估计只在夜视档存在。

### 配套同步

- `pc_host/night_accept_test.py`：`TIER_FPS = {0:50, 1:5, 2:5}`（tier0 不再是 15）、
  T3 fallback 50、T4 回落判据 `<= FPS_LOW`。
- `pc_host/stutter_probe.py`：静止档就绪判据 `cam_fps < 50`（白天恒 50 不会就绪，属预期）。
- `ui_shared.c`：AUTO 按钮日志改为"夜视档专用；白天恒 50"。

### 真机验证（2026-10-02）

- **白天（正式阈值）**：`auto_fps:true` 后 198 帧全部 `cam_fps=50 / exp=500 / gain=0`
  纹丝不动（改前会掉 15fps + boost 跳变）；最终固件 158 帧恒 50fps / tier0 / 识别正常。
- **昼夜循环（临时阈值 150/100）**：串口全流程 —— `tier1 -> 5fps + dn2 + face: NIGHT`
  → boost → AE 压回 → `tier0 -> 50fps + face: DAY` → 之后 40s 无任何 auto-fps 动作
  （白天 AUTO 休眠）。夜视档的 5fps/DN/门控与运动升档逻辑未改动（第十三节已验证）。
- ⚠️ 亮房间+人为阈值下 boost 过曝导致 tier 0↔1 每 ~11s 来回（第十二节既有行为，
  真实暗场不存在），与本次改动无关。
- 已知小特性：AE 关闭时夜→日只恢复 50fps + exp 被 clamp，**gain 不会自动降**（boost 只管
  拉满不管回落）——夜视手动档回到白天若嫌过亮，按 Day 按钮或开 AE。

## 十七、白天曝光余量阶梯：50 -> 20 -> 10fps（2026-10-02，真机验证 ✅）

### 动机（用户反馈"白天整体偏暗了"）

第十六节把白天定成恒 50fps 后，**曝光上限从 66ms（15fps 时代）砍到 20ms**（exp 994 =
50fps 的 VTS 上限）。偏暗房间实测 exp 顶满 994（零余量）、亮度靠增益 67~140 硬提——噪点
让画面"发暗发脏"。且 50fps 抗频闪量化下 exp 实际只有 10ms/20ms 两档，没有任何过渡余地。
（板端画面亮度本身正常：多帧 avg_luma 117~156——问题在噪声观感与余量耗尽。）

### 设计（`day_expo_tick()`，app_faceai.cpp；常规相机的自动快门，非夜视逻辑）

- **降档**：exp 顶满上限（`>= exp_max-2`）且**增益 EMA ≥ `APP_CAM_DAY_GAIN_GATE/2`（24）**
  持续 5s → 50 -> 20（`DAY_FPS_MID`，帧周期 50ms=5×10ms 与 50Hz 抗频闪对齐，曝光上限
  2.5 倍）→ 仍顶满再 -> 10（`DAY_FPS_LOW`）。
- **升档**：exp ≤ 下一档曝光上限的 80%（10->20 用 1995 行、20->50 用 795 行）且增益 EMA
  < 24，持续 8s。**分级阈值是必须的**：统一 795 会把"需要 30ms"的场景永久卡在 10fps。
- **防振荡三件套**：保持时间（降 5s/升 8s）+ 切换后冷却 10s + 增益 EMA（1/8 平滑 AE 在
  量化边界的打摆——瞬时增益条件曾让 5s 保持时间永远凑不齐，实测踩到）。
- **约束**：只在 tier0（白天侧）+ AE 开启 + cam 源时工作；夜视档（tier1/2 -> 5fps）、运动
  自适应（仅夜视档）、boost 语义全部不动（`auto_fps_apply` 拆出 `auto_fps_apply_ex(fps,
  want_boost)`，白天降档传 false——曝光交给 AE 连续调，强制拉满只会过曝）；手动 fps 命令
  照旧让整个阶梯让位；AE 关闭时阶梯自动休眠。
- `night_idle_fps()` tier0 返回 `day_fps()`——夜→日恢复落到当前白天档（记忆档位，exp 余量
  恢复后自动回升）。
- **Kconfig**：`APP_CAM_DAY_STEP`（默认 y）/ `DAY_FPS_MID`=20 / `DAY_FPS_LOW`=10 /
  `DAY_GAIN_GATE`=48。

### 踩过的坑（改这套代码前必读）

1. **AE 在量化边界打摆**（exp 500<->994、gain 0<->70）：瞬时值条件凑不齐保持时间 → 增益
   必须 EMA；降档判据的 exp 顶满部分稳定可靠。
2. **`s_cam_exp_max` 开机是 0**（只有 cam 命令/切档才更新）——白天阶梯在第一个客户端连接前
   永远早退。已在主循环加一次性播种（get_ctrl 读真实上限）。
3. **观察窗口陷阱**：`watch 8` 在 AI 25fps 下只覆盖 0.3s，而保持时间 5s——点查询会得出
   "没触发"的假结论。验证状态机要用**单连接长窗口探针**（`pc_host/_cycle_probe.py`：同一
   条连接按时间表发命令 + 打印状态跳变，注意 board 是单客户端，watch 占线时 cmd 连不上）。
4. **ae_target 强到 20 的病态输入**：夜视 boost（gain 192）在半亮房间会过曝到白（luma 252）
   → 档位被踢回 tier0 → AE 又压暗 → 循环。真实夜晚 boost 不会过曝（用户已验收夜视），
   这是人为压目标的产物，不修。
5. **验证期间怀疑过崩溃**（帧率像"重新爬阶梯"）——串口 60s 确认无 rst/panic，是场景/灯光
   被手动改变的观测混淆。怀疑崩溃时先抓串口数 `rst:`。

### 真机验证（2026-10-02，探针全程记录）

- 暗房间开机 ~15s：自动 50 -> **20fps**（exp 994/2494 = 40% 余量，luma 121 稳在目标，
  gain 64）；
- ae_target 250 强顶：exp 顶格 2494 + gain 97 → **20 -> 10fps**（exp 2500/4994 = 50ms）；
- target 120 恢复：gain 99->14 收敛，exp 50% 未顶格 → **正确停在 10fps**（迟滞有效）；
- auto_fps:true 在白天无任何动作（AUTO 仅夜视档工作，第十六节语义保持）；
- 夜视档回归：ae_target 20 压暗 → tier 1 -> 5fps + DN2 + face 门控 → 与第十六节行为一致；
- 升档路径（10->20->50）未在本轮环境复现（房间一直偏暗）——阈值保守（80%），亮度恢复后
  0x04 会出现 `cam: day fps -> N (exposure headroom)`，属预期观察项；
- 单帧进光对比：50fps 顶格 20ms/gain67 -> 10fps 90ms/gain0（**4.5 倍进光、零增益**）。

### 遗留

- 边界房间（场景曝光需求 16~20ms）会停在 20fps 不回 50（量化 exp ∈ {500,1000} 跨不过
  795 判据）——画面亮度一致，仅帧率保守，接受。
- ae_target 无法企及（如 250 对着暗室）时阶梯会一路降到 10fps——按设计工作，属人为输入。
- `pc_host/_cycle_probe.py` 保留作状态机调试探针（单连接、时间表发命令、跳变打印）。

## 十八、白天颜色校正：厂商标定 CCM 下发（2026-10-02）

### 背景

用户反馈白天颜色与人眼差距大，问是否需要 IR-cut。**确认：模组镜头镀膜泛红 = 已带
IR-cut 滤光片**，IR 污染排除。真正的软件缺口：**IPA 控制器关闭后（第十一节），厂商标定
的 CCM（颜色校正矩阵）从未下发**——与当年 gamma/BF 丢失同因。

已核实的事实：
- 厂商标定在 `esp_cam_sensor/sensors/sc2336/cfg/sc2336_default.json`：`acc/ccm/matrix =
  [1.408,-0.094,-0.314, -0.13,1.28,-0.15, -0.072,-0.173,1.245]`（三行系数和均为 1.0 =
  **亮度守恒**，灰/白不动只校色相饱和度）+ `acc/saturation = 138`（意图不明，未采用）。
- 驱动支持 `V4L2_CID_USER_ESP_ISP_CCM`（0x009810E0），payload
  `esp_video_isp_ccm_t{enable, float matrix[3][3]}`，范围 (-4,4)，live 重配。
- **WB 与 CCM 的组合方式（关键，读驱动源码确认）**：`isp_reconfigure_white_blance()`
  就是调 `isp_reconfig_ccm()`——WBG 按**列**缩放进 CCM（第 0/2 列乘 kr/kb，= 数学上
  "先 WB 后 CCM"，教科书式正确组合）。写 CCM 不会冲掉 WB（AWB 下一次写会重新组合）。
- **理论中性点推导**：用经验 WB（1320/1645）把场景白点的原始值中性化，代入 CCM 解出
  的中性 WB 恰好仍是 (1320,1645)——即**厂商 CCM 与既有经验 WB 是配套的**（CCM 行和 1.0
  保白点）。实测该组合下暗部/彩色物体色相正常。

### 实现（照 gamma/BF 信箱模式）

- `app_camera.c`：`CAM_MREQ_CCM` + `isp_apply_ccm(mix)`（`M' = I + mix/1000×(M−I)`，
  钳 (-4,4)，0=禁用模块）+ `app_camera_post_ccm()` + **init 里随初始 WB 一起下发默认
  `CONFIG_APP_CAM_CCM_MIX`**；`app_camera.h` 声明。
- Kconfig：`APP_CAM_CCM_MIX`（默认 1000 = 完整标定矩阵；0=禁用；>1000 过饱和方向）。
- 命令 `{"cmd":"cam","ccm_mix":N}`；结果帧恒带 `"ccm"` 字段；cam 查询 ack msg 带 `ccm=%d`。

### 开发过程疑难全记录（⚠️ 新会话接手颜色问题前必读）

白天颜色问题前后折腾了两轮、走了一次大弯路。以下是完整的诊断时间线、验证方法与
踩坑记录——**再遇到任何"颜色不对"的问题，先读完这段再动手**。

**阶段一：误诊为白平衡/滤光片问题（2026-09-30 ~ 10-02 上午）**

- 最初症状"画面偏绿" → 灰世界 WB（1320/1645）修复了平均值 → 以为颜色问题已解决。
- 用户反馈"白天画面颜色与人眼差距极大" → 第一反应怀疑 **IR 污染**（无 IR-cut 时近红外
  等量进 RGB 三通道、去饱和、场景相关、白平衡校不掉）。用户斜看镜头镀膜泛红 =
  **模组已带 IR-cut**，排除硬件。⚠️ 这一步的启示：问用户一个 10 秒能做的物理检查，
  比盲改软件便宜得多。
- 转向软件：发现 IPA 关闭后**厂商标定 CCM 从未下发**（与 gamma/BF 丢失同因）→ 实现
  CCM 信箱下发（见上）。修复本身正确，但**评估方法有问题**——见阶段三。

**阶段二：用户四色实测锁定互换特征（关键转折）**

- 用户拿实物逐色测试：**黄→天蓝、橙红→深蓝、天蓝→黄、紫（偏蓝）→橙红；黑白灰、绿
  完全正常**。这是教科书级的 **R/B 通道互换补色模式**（G 居中不动所以绿/灰/白不受影响）。
- **矩阵验算法（可复用）**：对四种颜色取传感器原始值 → 乘 WB → 分别代入
  "P·M（CCM 输出行 R/B 互换）"与"M·P（输入列互换）"两种通道错位模型逐一验算，
  **四色全部吻合**——以此锁定"通道互换"而非"色温偏差"或"CCM 系数错"。
  （单纯的 M 转置不吻合——验算排除了矩阵转置嫌疑。）
- 但 P·M 与 M·P 都吻合 → **无法从颜色本身判断对调发生在哪一层**（拜耳序/CCM 应用/
  JPEG 链路都可能），需要绕过链路的分层实验。

**阶段三：raw 直出裁决（决定性实验，5 分钟）**

- 同一场景（尹黄啤酒罐=实物金色、白袋上橙红 III 标记）**同时抓 raw 与 JPEG**：
  - raw（`{"cmd":"stream","value":"on","fmt":"raw"}` + `tools/rgb565_to_png.ps1`）：
    罐**金黄**、标记**橙红**、桌面暖棕——**摄像头路径（拜耳/ISP/WB/CCM）完全正确**；
  - JPEG（`fmt:"jpeg"`）：同样的罐显示浅蓝、标记深蓝——**文件本身红蓝对调**；
  - PIL 与模拟器（tjpgd）两个**独立解码器**结果一致 → 排除解码端 →
    **硬件 JPEG 编码器对 RGB565 输入的通道序解读与标准相反**（驱动无配置项；
    `pixel_reverse` 只管字节序，它当年修的"彩色噪声"是另一回事）。
- **修复**：app_stream.c 编码前 R/B 通道换回（RGB565 位域交换 / RGB888 首尾字节交换，
  私有缓冲顺带 stride 重排）。只影响 0x05 JPEG 链路；AI/raw/相机路径不经过这里。
  代价：编码 4ms→44ms（PSRAM 遍历，preview 10fps 可接受）。
- **顺带修复**：raw 帧 614KB 超过 netlink 单帧发送预算 100ms → 永远发不出（表现为
  "8s 收不到 0x05 帧"）→ `app_netlink.c` 预算提 **500ms**。⚠️ raw 调试手段本身被
  这个预算卡死过——做 raw 对比前先确认预算够。

**陷阱与教训（每条都真实浪费过时间）**

1. **显示链路说谎**：识别路径（AI/raw）与显示路径（JPEG）颜色不一致，且无任何报错——
   导致所有基于 0x05 JPEG 的颜色观察与调参（WB 1320/1645、"高光偏粉"、CCM 效果评估）
   都隔着对调，结论全部作废。**颜色类问题的第一步永远是 raw 直出对比**（一条命令，
   能省数小时误诊）。
2. **修复 CCM 后立刻做 A/B 会被 AWB 重收敛污染**：CCM 切换后 AWB（灰世界）要 10~30s
   重新收敛（EMA+死区+节流），切换后立刻抓帧对比会得到被 WB 漂移污染的假差异。
   A/B 前等 AWB 稳定，或临时关 AWB 钉死 wb_kr/kb。
3. **"高光偏粉"是暖光裁剪**：CCM 修复后当晚混合光源下高光仍偏粉——采样确认 R 超出
   ~11% 且集中亮部 = 暖光高光非对称裁剪（CCM 的 1.408 对角元放大它），**CCM 之前就
   存在**，白天自然光 + 白参考重标 WB 才能最终解决（仍待做）。
4. **理论中性点推导可用**：用既有经验 WB 把场景白点原始值中性化、代入 CCM 解中性
   WB——解出的恰好仍是 (1320,1645)，证明厂商 CCM 与经验 WB 配套（行和 1.0 保白点）。
   这类线性推导能避免盲目试参。
5. **netlink 单客户端**：模拟器/jc_test 快速连断会短暂残留槽位（现象：下一连接超时），
   连接间隔留 2~3s 或复位板子。
6. **遗留待办**：①白天自然光下白纸重标 `wb_kr/kb` + `ccm_mix`（1000↔1380 对应厂商
   saturation 138 的可能意图）+ `{"cmd":"cam","sat":N}` 微调；②编码 44ms 优化
   （LUT/双缓冲）；③AWB 相对修正loop在 CCM 大切换后收敛慢（~30s），必要时加快速
   重收敛路径。

## 十九、WiFi 恢复（板载 C6，2026-10-02 真机验证 ✅）

### 根因（两层叠加，纠正"等 C6 固件升级"的旧结论）

板载 **ESP32-C6**（Wi-Fi 6 + BLE 5，SDIO slot 1 接 P4，GPIO14-19 + 复位 GPIO54 高有效，
出厂已烧 esp-hosted 从机固件 2.3.2）就是无线模块。之前摘除的真实原因有两层：

1. **宿主组件版本错配**：解析到 esp_hosted 3.0.7，与从机固件 2.3.2 "major version
   mismatch"。厂商 wifi_scan 模板的 dependencies.lock 给出配套版本对：
   **esp_wifi_remote 0.14.2 + esp_hosted 2.0.13**（registry 现已恢复可达，钉版本即可）。
2. **TF 卡槽位错配引发共享 SDMMC 主机冲突（启动循环的真凶）**：app_sdcard.c 原来配
   `SDMMC_HOST_SLOT_1`，但 TF 卡引脚（CLK43/CMD44/D0-3=39..42）是 P4 SDMMC **slot 0**
   的原生映射；C6 SDIO（GPIO14-19）是 slot 1。两者同抢 slot 1：esp_hosted 构造函数
   （app_main 之前）spawn 的 RX 任务、sdcard 的失败清理（删除共享信号量）、transport
   任务的二次 init 交织 → `xQueueSemaphoreTake(NULL)` 启动循环。**修复：sdcard 归
   slot 0，C6 归 slot 1**（2.0.13/2.0.17/2.12.13 三个版本都验证过此冲突存在）。

### 最终配置

- `main/idf_component.yml`：`esp_wifi_remote "0.14.2"` + `esp_hosted "2.12.13"`
  （2.12.13 相比 2.0.13 修复了 RX 任务/队列初始化的竞态，且与从机固件同主版本兼容；
  registry 里没有 2.1~2.6 系列的组件版本）
- `main/CMakeLists.txt`：门控内显式补 injected 垫片头目录——IDF 5.5.5 组合下
  esp_wifi_remote 对 esp_wifi 的 injected/esp_wifi.h 注入不传播到 main（实测
  "esp_wifi.h not found"），用 `idf_component_get_property` 取两个组件目录加进 INCLUDE_DIRS
- `sdkconfig.defaults`：esp_hosted 板级块（SDIO slot1/宽4/复位54 高有效/40MHz）+
  **CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192**（esp_wifi_init+hosted 初始化链把默认 3584
  打穿——Stack protection fault）+ **CONFIG_FREERTOS_HZ=1000**（esp-hosted 推荐值，
  100 会报 bus jitter 警告）+ CONFIG_APP_ENABLE_WIFI=y
- `app_wifi.c`：+ `esp_wifi_set_storage(WIFI_STORAGE_FLASH)`（凭据 NVS 保存）+
  `app_wifi_set_credentials(ssid, pass)` API
- `app_faceai.cpp`：新命令 **`{"cmd":"wifi","ssid":"X","pass":"Y"}`**——运行时配置并连接
  （NVS 保存、断线自动重连），无需重烧

### 真机验证（2026-10-02 深夜，全链路 ✅）

开机：SDIO master slot 1 初始化 → 无 mismatch、无断言 → **`=== 2 APs found ===`**
（SSID3 -56、HUAWEI-MFVHA4_Guest -57）→ C6 链路完全恢复。

**连接实测**（用户热点 "man"）：`{"cmd":"wifi","ssid":"man","pass":"33333344"}` →
关联成功（**wrssi -64**，结果帧 `wrssi` 字段，2s 节流探针）→ DHCP 拿到
**wip = 192.168.194.206**（结果帧 `wip` 字段）→ **PC ping 192.168.194.206 通**
（2/2 回包 ~109ms）。板子双 IP 双路可达：网口 192.168.33.2（有线）+ WiFi DHCP，
:3333 两个口都监听。

### 补齐过程中发现的三块缺口（厂商 demo 只做扫描，从未覆盖）

1. **STA netif 从未创建**：`esp_netif_create_default_wifi_sta()` 必须在 esp_wifi_init
   前调用——没有 netif，关联成功也拿不到 IP（DHCP 无处运行）。app_wifi.c 已补
   （s_sta_netif 缓存句柄）。
2. **开机自动重连**：凭据存 NVS 后，开机读回 `esp_wifi_get_config` 判断 ssid 非空 →
   `s_have_cfg=true` → STA_START 事件自动 connect；断线自动重试。无需每次开机重发命令。
3. **探针节流**：结果帧的 wrssi 探针（esp_wifi_sta_get_ap_info 是一次 RPC）未关联时
   逐帧调用会刷屏 esp_hosted 错误日志——已按 2s 节流 + 缓存值。

### 注意

- esp_hosted 建议 FREERTOS_HZ=1000（已改）——全系统 tick 粒度变细，历史"1 tick=10ms"
  的注释仍在但行为更优
- WiFi/hosted 任务默认 CPU0（与 AI 同核）——当前实测 AI 帧率无回归；如掉帧再调绑核
- esp_wifi_remote 0.14.2 的 injected 头注入在 IDF 5.5.5 不传播到 main 的坑（见上），
  升级 esp_wifi_remote 版本时需复查 CMakeLists 的显式补丁是否仍需要
- netlink 单客户端：模拟器/jc_test 快速连断会短暂残留槽位（连接间隔留 2-3s 即可）

### BLE

esp_hosted 2.12.13 **自带 BLE 支持**（组件内含
`examples/host_nimble_bleprph_host_only_vhci` 例程：P4 跑 NimBLE 主机栈、C6 做
controller、HCI 走 VHCI/SDIO）。实施 = 启用 bt 组件 + 该例程的 Kconfig 配置 + app_ble.c
（广播/扫描）。**已于 2026-10-04 实施并真机验证通过**，广播 "JC-P4" + demo GATT；
实施过程的三个大坑（CMake 早期展开 / 内部 RAM 贴零线 / 老从机固件 RPC）见第二十节。

### 注意

- esp_hosted 建议 FREERTOS_HZ=1000（已改）——全系统 tick 粒度变细，历史"1 tick=10ms"
  的注释仍在但行为更优
- WiFi/hosted 任务默认 CPU0（与 AI 同核）——当前实测 AI 33fps 无回归；如掉帧再调绑核
- esp_wifi_remote 0.14.2 的 injected 头注入在 IDF 5.5.5 不传播到 main 的坑（见上），
  升级 esp_wifi_remote 版本时需复查 CMakeLists 的显式补丁是否仍需要

## 二十、BLE 实施（2026-10-04，真机验证 ✅）

### 拓扑与实现（main/app_ble.c/h）

- **拓扑**：NimBLE host 跑在 P4（IDF `bt` 组件，`CONFIG_BT_CONTROLLER_DISABLED=y`），
  BT controller 跑在 C6 从机上（`esp_hosted_bt_controller_init/enable` 走 SDIO RPC），
  HCI 数据经 esp-hosted 的 VHCI 传输与 WiFi 共用同一条 C6 链路。移植自 esp_hosted
  2.12.13 例程 `host_nimble_bleprph_host_only_vhci`：去掉 console/CLI（Just Works 不需要
  passkey 输入）与扩展广播（P4 上例程默认也是 legacy 广播）。
- **广播**：设备名 **"JC-P4"**（公共地址来自 C6），flags + TX power + 完整名 + Alert
  服务 UUID 0x1811；连接失败/断开/广播完成自动重新广播。
- **demo GATT**：1 个 128-bit UUID 服务 + 1 字节特征（读/写/notify/indicate）+ 自定义
  只读描述符；写入特征会向所有订阅者 notify——手机 nRF Connect 可验证全链路。
- **观测点**：结果状态磁贴 `BLE`（adv/conn）；8s HCI sync 看门狗（超时打
  `no HCI sync` 日志并置磁贴 "no hci sync"——老从机固件下 BT 通路不通的明确信号）；
  构造器打印 `ble: pre-sched heap: internal free=...`（见下"内部 RAM 贴零线"）。
- **命令/配置**：`Kconfig APP_ENABLE_BLE`（默认 y，depends on APP_ENABLE_WIFI——
  BLE 与 WiFi 共用 C6/esp_hosted 链路）；结果帧未新增字段。

### ⚠️ 坑 A：IDF CMake 早期展开丢弃 if(CONFIG) 内的 REQUIRES 追加（全工程性！）

**症状**：首次编译 `app_ble.c: fatal error: nimble/nimble_port.h: No such file or directory`，
且 libbt.a 未链接——但 `app_ble.c` 明明进了 SRCS。

**根因**（读 `tools/cmake/component.cmake` + `scripts/component_get_requirements.cmake` 确认）：
IDF 在**独立 CMake 脚本实例**里早期展开每个组件的 CMakeLists.txt 收集
`REQUIRES`/`PRIV_REQUIRES`（该脚本里**没有 CONFIG_* 变量**），真实 configure 阶段
`idf_component_register` 传入的 REQUIRES 参数**被静默丢弃**（`__component_set_properties()`
不回填 `__REQUIRES` 属性，链接依赖只用早期展开捕获值）。所以
`if(CONFIG_APP_ENABLE_BLE) list(APPEND requires bt esp_hosted) endif()` **从不生效**，
而 `list(APPEND srcs/include ...)` 走真实注册**有效**——正是"源文件在编译列表里、
依赖组件的头目录/链接缺失"的矛盾来源。

**顺带澄清历史**：第十九节 WiFi 块里 `list(APPEND requires esp_wifi_remote esp_wifi)`
也一直是死代码——wifi 能编译纯靠 `idf_component.yml` 托管依赖兜底（managed 组件以
managed_priv_reqs 注入，给 main 自己的编译提供 include，但属 PRIVATE）；`bt` 不在
yml 里没有兜底，故 BLE 首当其冲暴露。

**修复**：`main/CMakeLists.txt` 把 `esp_wifi_remote/esp_wifi/esp_hosted/bt` 全部移入
**无条件的**基础 `set(requires ...)`（bt/esp_wifi 组件在功能关闭时注册为空库，无副作用），
if 块只留 `list(APPEND srcs ...)` 与 WiFi 的 includes 修复。**以后任何组件想条件性
依赖别人，都必须放进无条件基础列表**。验证手段：看 `build/project_description.json`
的 `build_component_info.main.reqs/priv_reqs`。

### ⚠️ 坑 B：内部 RAM 启动窗口贴零线，BT 使能即无限重启（本坑最凶险）

**症状**：配置好后烧录，板子在 `sleep_gpio` 后无限重启：
`assert failed: vApplicationGetIdleTaskMemory port_common.c:53 (pxStackBufferTemp != NULL)`。

**排查**（逐层收窄，值得复用的方法论）：BT=n 构建正常启动（排除代码错误）→ 两版
heap_init 区域只差 1.2KB（排除静态内存剧增）→ 在 app_ble.c 加 `__attribute__((constructor))`
探针（main 链接在最后，跑在 esp_hosted 构造器之后、调度器之前）：堆剩余 142KB、
试分配 6KB OK、请求仅 1536B——排除"请求病态"→ 给本地 IDF `port_common.c` 的断言
前插诊断打印（⚠️ printf 在此阶段会因 newlib 需要 malloc 而静默无输出，必须用
`esp_rom_printf`）→ 拿到决定性数据：

```
IDLEALLOC-FAIL size=1536 free=1300 largest=1024
```

**根因**：调度器启动前（esp_hosted 构造器的 mempool/任务栈 + 主任务栈等约 140KB
分配之后）内部堆只剩 ~1.3KB，空闲任务栈要 1536B——BT 使能的构建比 no-BT 恰好多占
~2KB（静态 +1.2KB + hosted BT 支持），把空闲任务栈挤出堆。**本工程内部 RAM 在启动
窗口本来就贴着零线，BLE 只是压垮它的最后一根稻草**（no-BT 构建的余量也只有几 KB）。

**修复**（两处都写进 `sdkconfig.defaults`）：
- `CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE` 20→**12**（启动窗口省 12KB；streaming 模式按需
  分配，WiFi 吞吐实测无回归）；
- `CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`（NimBLE host 内存池迁 PSRAM，省 40~60KB
  内部 RAM——**没有这条，就算过了启动窗口，app_main 里 nimble_port_init 也会 OOM**）；
  注意实际符号是 `CONFIG_NIMBLE_MEM_ALLOC_MODE_*`（无 BT_ 前缀）。

### ⚠️ 坑 C：老从机固件（2.3.x）不认识新版 BT controller RPC

**症状**：`E ble: C6 bt controller init failed (check slave fw / SDIO)`。

**根因**：`esp_hosted_bt_controller_init/enable` 走 feature-control RPC
（FEATURE_BT_INIT/ENABLE）——"BT controller 默认关闭、host 必须显式 init/enable"
是**新版从机**才有的语义；出厂从机 2.3.x 的 BT controller **默认自启**（其能力位图
明确广播 `HCI over SDIO / BLE only`），只是不认识这条新 RPC。

**修复**：app_ble_init 里 init/enable 失败**降级为警告并继续**初始化 NimBLE——
HCI 通路是否真正可用由 sync 回调裁决。实测老固件上 HCI 正常同步（device address
cc:8d:a2:f7:c8:16 即来自 C6）。**若将来刷新版从机固件**，这些 RPC 会成功，同一份
代码两个世界通吃。⚠️ 从机固件升 3.x 前先看第十九节的版本对。

### 真机验证（2026-10-04，COM4 + 串口日志）

```
ble: pre-sched heap: internal free=142840 largest=102400   ← 启动窗口水位（修后与 no-BT 持平）
H_SDIO_DRV: sdio_data_to_rx_buf_task started                ← 不再 assert
main_task: Calling app_main()                               ← 启动循环消失
...
NimBLE: GAP procedure initiated: stop advertising.          ← HCI sync 成功的标志
ble: device address: cc:8d:a2:f7:c8:16
ble: advertising as 'JC-P4'                                  ← 广播开始
wifi: === 3 APs found ===                                    ← WiFi 扫描共存正常
main: all modules launched
35s 稳定观察：camera 10.5fps（夜视阶梯生效中）、无 rst、无 task_wdt、0 回归
```

bin 5.45MB / 8MB 分区（剩 ~32%，BLE 增量约 140KB）。**手机端验收**（待用户）：
nRF Connect 扫到 "JC-P4" → 连接 → 找 128-bit 服务 → 读/写特征（写 1 字节会触发
notify）→ 断开后板子自动恢复广播。

### 遗留 / 注意

- esp_hosted 的 VHCI HCI 包路径按需从内部堆分配（每包一次 malloc_align）；老固件 +
  BLE 广播态实测无压力，若以后 BLE 大流量（GATT 长包/高连接率）再观察内部堆。
- `CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=12` 若将来 WiFi 吞吐不济可回调（每 +1 档启动窗口
  少 1.5KB 余量，需对照 `ble: pre-sched heap` 日志）。
- NimBLE host 任务栈 4096（内部 RAM）不可迁 PSRAM（任务栈限制），已够用。
- 手机端 nRF Connect 交互验收待用户执行（板端证据齐全）。

## 廿一、白天人脸识别第 1 轮（2026-10-04，真机验证 ✅）

规划见 `docs/face_recog_day_plan.md`（含社区对标：esp-dl 例程 / ESP-WHO / esp-box
factory_demo 门禁）。本轮 = F1 多帧注册 + F2 连续帧确认 + F3 双阈值 + F5 标定工具。
验收脚本 `pc_host/_face_r1_test.py`（单连接、PC 送 esp-dl 官方人脸图，可全自动回归）。

### 实现（main/app_faceai.cpp + Kconfig）

- **F1 多帧注册**：`{"cmd":"register","name":X,"shots":3}`（1~5，缺省 1 = 旧单帧路径
  完全兼容）。采集态对每帧做**质量门控**（检测分 ≥0.6、脸宽 ≥80px、关键点齐全，低质量
  帧 msg 报 `quality low` 不入库）+ **400ms 帧间隔**（拉开拿自然姿态变化）；逐帧回
  `event:"enrolling", msg:"collected 2/3"`，满额 `event:"registered"`。`stop`/`clear`
  取消采集。注意：注册武装后"下一个带脸的帧"即被采集——PC 源测试必须先
  `{"cmd":"src","value":"pc"}`（真机摄像头注册不受影响）。
- **F2 连续 K 帧确认**：最近 K=5 帧 top-1 结果环形缓冲（人脸离开即清空）；同 id 命中
  M=3 帧（灰区 M+1=4）才 `event:"match"`，否则 `tracking`（不报名字防误报闪名）；整窗
  都 < LO → 稳定 `reject`（陌生人）。结果帧新增 `stable`(0/1) 与 `sim_raw`（本帧无门槛
  top-1 相似度，低于 LO 也如实带出——标定数据源）。空库直接 reject 不等窗。
- **F3 双阈值**：`APP_FACEAI_MATCH_THR_HI=80`（>=HI 命中 M 帧即确认）/
  `_LO=70`（[LO,HI) 灰区需 M+1 帧；<LO 未命中）。旧命令 `{"cmd":"mode","thr":x}` 兼容
  （设 LO=x、HI=x+0.10）；独立调节 `thr_hi/thr_lo`；ack msg 带 `thr=lo/hi`。
- 查询改为 **query_feat(f, 0.0, 1) 无门槛取 top-1**（阈值分类全部在应用层做）——
  这是 sim_raw 能覆盖全区间的前提。delete/clear 会重置平滑窗。

### 真机验证（_face_r1_test.py，**19/19 全绿** 2026-10-04 晚）

P1 多帧注册（enrolling×2→registered 3/3）✅；P2 单帧注册兼容 ✅；P3 HI 快路径
（第 3 帧 match，M=3）✅；**P4 灰区 M+1**（musk2 稳定聚 0.772~0.775，第 4 帧 match，
匹配帧野值 0.496 被窗口最大值吸收）✅；P5 抬高 thr_lo 稳定 reject ✅；阈值命令回读 ✅。

### ⚠️ 非确定性根因链（已查清，三层叠加）

**第一层（已修，2026-10-04）——P4 双核 L1 互不一致且 `esp_cache_msync` 无跨核 IPI**：
- `jpeg_decoder_process` 的 invalidate 只作用于调用核（jpeg_decode.c:313/333，覆盖整个
  outbuf_size——我们传 sl->cap 已 64B 对齐，驱动 assert 吞错坑不适用）；
- imgpipe 与 faceai 原先都没绑核，解码核 ≠ 读像素核时消费者核 L1 残留旧 cache 行；
- **CAM 路径同样中招**：pipe_copy_into 的 memcpy 在 camera 任务（CPU1）留脏行。
- 修复：写侧 C2M 回写（pipe_copy_into）+ 读侧 M2C 失效（get_frame，消费者上下文，
  整槽 cap——不能按 stride*height，任意高度下 size 未必 64B 对齐）+ imgpipe/faceai
  钉 CPU0。诊断埋点常驻：`frm_crc/ai_crc/ai_crc2/dec_core/ai_core/sim_q2/sim_f2/kp`。
- 修复后：解码输出 frm_crc 恒定、AI 输入 ai_crc 恒定、ai_crc2==ai_crc（推理期间无人
  踩输入缓冲）、同帧两次查询一致（查询/存储侧无辜）。

**第二层（esp-dl 内在，未修，已定界）——模型执行历史相关（混沌野值）**：
同帧内相同输入+相同关键点，**两次特征提取从不一致**（14/14），关键点也逐帧漂移
（检测同样历史相关）；sim 围绕真值混沌跳变（musk1 真值 ~0.997，野值 0.08~0.94；
musk2 真值 ~0.775，野值 0.05~0.51），且查询/存储侧已被排除。结论：esp-dl v3.3.11
在 P4 上的模型执行读取了未初始化/上一次运行的张量残留（嫌疑：中间 tensor padding
或算子未完全覆盖输出）。上游无已知 issue（检索 2026-10-04）。
**F2 窗口最大值正是这类混沌的正确抑制器**——判定分布收窄到真值邻域
（musk1 0.996~1.0 / musk2 0.772~0.778），野值被 max 吸收。
后续可选：esp-dl 深挖（同图 dump 特征向量 + 二分算子）或升级 esp-dl 版本。

**第三层**：跨核缓存教训与驱动 assert 吞错隐患见上一节（通用坑，已修/已记录）。

### ⚠️ 发现的既有问题：特征提取非确定性 → 已定位为跨核缓存一致性（2026-10-04 修复待网验）

同一 JPEG 反复送帧，特征与库内特征（含自图）的相似度呈**双模态漂移**：
- musk1 自图：某次运行 0.99/1.0/1.0（干净），另一次 0.69/0.70（整体漂移）；偶发单帧
  野值（0.06/0.44/0.154）；
- musk2 跨照片：历史 0.773~0.78，出现过 0.51 模态与 0.05 模态（8 帧一致的 0.05）。

**根因（读源码定位，非猜测）——P4 双核 L1 互不一致且 `esp_cache_msync` 无跨核 IPI**：
1. `jpeg_decoder_process` 的 invalidate 只作用于**调用核**的 L1（jpeg_decode.c:313/333，
   覆盖整个 outbuf_size——我们传 sl->cap 已 64B 对齐，驱动 assert 吞错坑不适用）；
2. imgpipe 与 faceai 任务原先都**没绑核**（裸 xTaskCreate），解码核 ≠ 读像素核时，
   消费者核 L1 残留该缓冲上一帧的 cache 行 → 新旧像素混合 → 特征崩坏。两任务调度
   分合解释了"同图时准时不准"与运行间差异；
3. **CAM 路径同样中招**：pipe_copy_into 的 memcpy 在 camera 任务（CPU1）留脏行，
   faceai（CPU0）读 L2/RAM 旧值——真实摄像头识别同样受影响。
4. esp-dl 无辜：预处理/模型无跨帧状态、feat 默认 RUNTIME_MODE_SINGLE_CORE；
   ⚠️ 但 esp-dl 的 conv 在 AUTO 模式且输入 ≥100x50 时会拆双核、其双核路径对 PSRAM
   tensor 无缓存维护——今后若改 RUNTIME_MODE_AUTO/MULTI_CORE 会复发，记住这点。
5. 顺带发现驱动健壮性缺陷（本工程不触发）：`esp_cache_msync` M2C 要求 addr/size
   64B 对齐，失败仅被 assert 保护，release 下静默跳过——调用方若传非对齐 size
   （如 width*height*bpp）就会踩坑；必须用 `jpeg_alloc_decoder_mem` 的
   *allocated_size 作 outbuf_size（esp-dl 自己的 dl_image_jpeg.cpp 是正确示范）。

**修复（已实现并烧录，回归 19/19 ✅）**：
- **写侧回写**：⚠️ **位置铁律——C2M 必须放在该帧所有 CPU 写入之后、pipe_publish 之前**。
  第一版放在 pipe_copy_into（拷贝）里，但 publish_raw 随后还会在槽内就地写入
  （DN、pipe_postprocess 的白平衡+**镜像**）——镜像的脏行滞留 CPU1 L1，消费者经
  L2/RAM 读到"部分行镜像、部分行未镜像"的**持续性撕裂画面（上下区域左右排序相反，
  即用户实测的报告）**。现统一放在 publish_raw 末尾（postprocess 之后）。
- **读侧失效**：app_imgpipe_get_frame 里（消费者上下文）对整槽 `DIR_M2C` 失效
  （读者核 L1；缓冲无脏行安全；cap 64B 对齐满足 M2C 要求——注意不能按 stride*height
  失效，任意高度下 size 未必 64B 对齐）。
- **绑核**：imgpipe/faceai/**stream** 全部 `xTaskCreatePinnedToCore(..., 0)`（camera 仍钉
  CPU1）。⚠️ **读侧失效只在"失效与读取同核"时有效**——未绑核的消费者在失效与读像素
  之间被调度迁移到另一核，该核 L1 的陈旧行照样被读走（开机窗口 JPEG 横带混拼实测复现，
  钉核后消除）。所有经 app_imgpipe_get_frame 消费帧的任务都必须绑核，新增消费者同理。
- 诊断埋点常驻：img_frame_t 新增 `crc32/dec_core`；结果帧带人脸时附
  `frm_crc/ai_crc/ai_crc2/dec_core/ai_core/sim_q2/sim_f2/kp`。
- main/CMakeLists 基础 requires 新增 `esp_mm`（esp_cache.h）/`esp_rom`（esp_rom_crc.h）。

**开机窗口 AE 横带（已知瞬态，非缓存问题）**：AE 大步调整曝光的瞬间，若寄存器写
落在滚动快门一帧的积分中途，画面出现"上半帧旧曝光、下半帧新曝光"的亮度带（实测
复现于开机后 ~8s 的 JPEG，raw 同窗口干净、AE 稳定后自愈）。根因在传感器寄存器
写入时机与滚动快门的交互，属 sc2336 驱动层（可用 group-hold/VSYNC 对齐修复，
本地检出可打 patch）；瞬态仅 1~2 帧，暂不处理。

### 遗留 / 注意

- `thr_calib.py`（F5）已就绪并**以窗口最优 sim（匹配判定量）为主分布**——该量已被
  F2 窗口平滑收窄到真值邻域，可直接标定；单帧 sim_raw 分布天然宽（混沌野值），仅作
  参考。真实光照/距离的标定待用户在场执行：镜头前站注册者跑 Phase A，离开跑 Phase B。
- **2026-10-05 标定前干跑发现并修掉两个问题（闸门未过就交棒，新会话照此收尾）**：
  1. **致命 bug**：`recv_result()` 返回 `(type, seq, payload)` 三元组，脚本却当 dict 用
     （`j.get("event")` → `AttributeError: 'tuple' object has no attribute 'get'`）——
     该脚本此前只在纸面"就绪"，从未真机跑过。已修（`json.loads(frame[2])`）。
  2. **Phase B 样本几乎为空的设计缺陷**：固件只在 match/tracking 状态把窗口最优值写进
     `sim`，一旦整窗判成稳定陌生人（reject）就归零（app_faceai.cpp:780-782）→ 陌生人
     相位只有前 K 帧有 `sim`。已改为**用每帧都带出的 `sim_raw` 滑窗 K 帧重建 win_max**
     （与固件 smooth_tally 同义，人脸消失清窗）。
  - 同一次自测（PC 送图，同连接）实测：Phase A 22 样本 / Phase B `sim` 仅 5 个而重建后
    21 个 → 不修就必然误报 "insufficient data"。
  - 顺带暴露一个**判定量本身的隐患**：陌生人（bill2 对 musk）单帧 sim_raw 中位数 0.026，
    但野值冲到 **0.839**，重建 win_max 达 0.839 → 固件 5 帧里 3 帧过阈 → **真的误报了
    3 帧 match**。即 **win-max 对"真值很低"的陌生人反而是野值放大器**（K=5 窗口只要
    有 1~3 帧野值就能抬到达标）。真机标定的 thr_lo 因此大概率要顶到野值上沿，或改判定
    统计量（中位数/计数）——这也是 F11 换 MBF 的决策输入。⚠️ 标定完必须核对这一点。
  - 新增 `--swap N` 相位间倒计时（真人在场时两相位换人需要时间）。
  - 自测脚本为一次性的，已删（不入库）。
- **2026-10-05 首次真机标定（真人执行，结果待重做）**：
  - 注册者 `me` 已入库 3 特征（真机摄像头多帧注册成功，`enrolled 3/3`）；
    板端当前库 = `['me']`（特征 3 条），`mode` 仍 0.70/0.80 默认值。
  - ❗**B 相位无效**：现场无人可换，Phase B 站的还是注册者本人 → 上一轮
    "A.p05=0.501 ≤ B.p95=0.876 OVERLAP" 的结论**作废**（两相位同一人，
    0.757 vs 0.821 只是同人自身噪声）。**判别陌生人必须换一个真的人**。
  - ✅ 但 A 相位数据有效且**很值得注意**：注册者本人 20s 内 win-max 只有
    median 0.757 / p95 0.816 / max 0.816，20s 里仅 3 帧 match。
    **本人自匹配都上不到 0.85** —— 与 PC 路径自测（musk1 对 musk1 median 0.724）
    一致，说明当前固件 MFN 特征质量整体被压低（不是阈值问题，调阈值救不回来）。
  - 同时实测到的成像条件（`{"cmd":"cam"}` 回读）：
    `exp=4994/4994`（=10fps 档上限 ≈**100ms 积分 = 整帧周期**）、`cam_fps=10`、
    `gain 17~54`、`luma 120/120`、`wb_kr=1534/kb=1672`（偏品红）。
    **AE 认为曝光正常，但代价是整帧积分 → 人一动就糊**（第十/十六/十七节的物理限制）。
  - 检出率也偏低：正式相位里只有 ~19~23%（单测最好 67%），站位/补光仍然勉强。
  - **结论/下一步判断**：**阈值标定在当前条件下无意义**（本人自匹配 0.76 < 默认 LO 0.70 边缘），
    先要 ①照度提上去让 `exp ≤994`（50fps 档 ≈20ms，消除运动模糊）+ ②摘掉探针
    （每帧多跑 2 次特征提取，esp-dl 执行历史相关，嫌疑最大）后再重标。
  - `thr_calib.py` 已支持**两段式**（真人换人最稳，A 跑完可停下确认再接 B）：
    `--phase a --a 20` 存样到 `pc_host/_thr_calib_A.json` → `--phase b --b 20` 读回出建议；
    另加 `--enroll NAME --shots N`（真机多帧注册）。
- **2026-10-05 决定性证据：当前固件的特征提取在同一人身上逐帧乱跳（探针嫌疑最大）**
  - 实验条件：真机 640x480、`fps` 手动钉 10（`exp=4994 gain≈2 luma=123`、AE 被手动模式关掉）、
    同一次会话内**刚用 3 帧重新注册 `me`**、受试者原地不动（脸宽 273~395px）、25s。
  - 结果：检出仅 20/124=**16%**；`sim_raw` 逐帧 trace（同人同位置）：
    `0.522 0.489 0.840 0.688 0.350 0.559 0.812 0.573 0.778 0.361 0.259 0.594
     0.457 0.542 0.563 0.454 0.420 0.455 0.426 0.604`
    → **跨度 0.259~0.840（0.58！），中位 0.532**；
  - **20 帧全部 `tracking`，一次 `match` 都没到**（连"3 帧同 id"都凑不齐 → 连 top-1 的
    **身份**都在逐帧乱变，不只是分数抖动）。
  - 对照：PC 路径（零运动模糊）同图自匹配 `sim_raw` 0.599~0.879 / 中位 0.721 —— 同样发散，
    只是没这么夸张。
  - **结论**：这是**特征提取本身的不稳定**，不是阈值/光照/曝光问题。每帧多跑 2 次
    `s_feat->run()` 的深探（`sim_q2`/`sim_f2`）是"esp-dl 执行历史相关"的头号嫌疑——
    **摘探针（第 2 轮第一步）不是优化，是标定的前提**。
  - 顺带实测坑：①`{"cmd":"cam","fps":N}` 手动定帧率会**同时关掉 AE**（`camauto: manual
    param, AE off`），定格曝光后不再自适应；②软复位（串口 DTR/RTS 拉 RTS）后**以太网链路会
    掉**（PC 侧 Killer E2600 变 Disconnected、无载波），几秒后自行重协商恢复；TCP 侧
    netlink 卡死（前一个客户端占住唯一的 accept 槽）时**只有复位能救**——`pc_host` 侧
    连不上且板端日志无 `netlink: client ... connected` 就是这个症状。
    复位不会丢库（`/storage/face.db` FAT + NVS 名字表；实测复位后 `lib=['me'] lib_n=3` 完好）。
- **2026-10-05 摘探针 = 决定性修复 ✅（"混沌野值"真凶是探针自己）**
  - 改动：新增 `APP_FACEAI_DIAG_DEEP`（默认 **n**，已写入 `sdkconfig.defaults`）门控
    `sim_q2`/`sim_f2`/`kp`/`ai_crc2` 四个深探（声明/计算/上报三处都 `#if`）；
    轻量 `frm_crc`/`ai_crc`/`dec_core`/`ai_core` 常驻不受影响。
    改后 `idf.py reconfigure` + build + flash COM4，bin **5.46MB**/8MB（剩 32%）。
  - 复测（PC 送图、同一条连接，库内只注册 musk1；对比探针开启时的旧数据）：
    | 输入 | 探针开（旧） | 探针关（新） |
    |---|---|---|
    | musk1 对自身 | med 0.72、野值漫天 | **med 0.997 / max 1.000**（10 帧里 6 帧 ≥0.997）|
    | musk2 对 musk1（同人异照片）| med 0.77 | **med 0.771**（0.769~0.774，极稳）|
    | bill2（陌生人）| med 0.026、**野值冲到 0.839** | **0.009~0.011（spread 0.002）** |
    逐帧：自匹配 `0.998 0.998 0.998 0.722 1.000 0.112 0.998 0.749 0.667 0.997`；
    同人异照 `0.437 0.774 0.772 0.772 0.090 0.769`；陌生人 `0.010 0.011 0.009`。
  - **结论**：**"esp-dl 混沌野值"主要不是 esp-dl 的锅，而是深探自己造的**——每帧多跑
    1~2 次 `s_feat->run()`，而模型执行历史相关，重复 run 污染后续帧。摘掉后：
    ①自匹配回到 **0.997**（与本文档原文记录的"真值 ~0.997"完全吻合）；
    ②**陌生人野值基本消失**（旧版正是这些野值把 5 帧窗口的 max 抬到 0.84 → 误 match）；
    ③分离度 gap=**0.987** → **单阈值方案重新可行**（`thr_lo` 取 ~0.6 即可，不必顶野值上沿）；
    ④残留同人侧野值（0.09~0.75）都在**低**方向，F2 窗口取 max 正好吸收。
  - ⚠️ 因此**之前用探针开启状态注册的特征库（`me` 3 帧）与 A 相位样本都作废**，
    已 `clear`；真机 A/B 标定需在探针关闭状态下**重注册 + 重跑**（仍需注册者 + 另一个真人）。
  - 教训（写进规矩）：**任何"诊断探针"只要会额外调用模型，就不能长期常驻**——
    它测的量本身会被它改变。深探只做一次性归因，用完即关。
- **2026-10-06 真机 A/B 阈值标定（首次有效；探针关闭状态 + 取景中枢在环）**
  - 流程：取景中枢 `pc_host/board_hub.py`（上游 192.168.33.2:3333，脚本连
    127.0.0.1:13333，窗口实时看取景、可边走边调站位）→ 清库 → `--enroll me --shots 3`
    → `--phase a --a 20`（注册者）→ **停下等确认** → `--phase b --b 20`（换成另一位真人）。
  - 分布（判定量 win-max(k=5)；A 为旧样本无 trace，B 逐帧）：
    | 相位 | n | median | max |
    |---|---|---|---|
    | A 注册者 `me` | 46 | **0.673** | 0.877 |
    | B 陌生人（第 1 次） | 26 | 0.433 | **0.587** |
    | B 陌生人（第 2 次） | 24 | 0.450 | 0.485 |
  - **B 上沿 = 0.587（两次取最大）→ 建议 `thr_lo = 0.61`**（+0.023 余量）；A 在该阈值
    的模型命中率 83%。
  - ✅ **判定量重建被实测精确验证**：A 相位结果帧里 `sim>0` 的帧数 = **23**，与模型
    `win-max>=0.70` 的帧数 **23 完全一致**（固件 tracking 分支 `sim = win_max`）——
    `thr_calib.py` 用 `sim_raw` 滑窗重建判定量是**精确**的，不是近似。
  - ⚠️ **真正的限速器不是阈值，而是 F2 的窗口票数规则**：A 相位 23 帧判定量达标，
    实测只有 **8 帧**走到 match（票数系数 **0.35**）——固件要求窗口内同 id 票数
    ≥M(3)、灰区 ≥M+1(4) 帧，而检出是**间歇**的（A 检出 65%、B 仅 25%；B 的窗口
    占用分布 `o=0:73 / 1:9 / 2:4 / 3:2 / 4:1 / 5:8`）。→ 要提高识别率必须先把
    **检出连续性**做上去（补光/取景/检测侧改进）；单降阈值最多把"报名字的帧占比"
    从 11%（8/71 @0.70）提到 ~29%（@0.61）。
  - ⚠️ **B 的成像条件比 A 差**（检出 25% vs 65%；第一版甚至因"仰头+瞳孔上翻+背光"
    检出掉到 2%）→ 陌生人的相似度可能被**低估**，0.61 的余量偏薄；落地后需在
    光照更好、条件与 A 相当的陌生人身上复验 B。
  - 工具升级（`thr_calib.py`，已真机跑通）：`collect()` 返回**逐帧 trace**（含无人脸的
    空隙帧，模拟固件 `smooth_reset`）；`report()` 按固件 F3 规则
    （`best_cnt>=need && best_sim>=thr_lo`）复算逐帧命中率 + 阈值扫描表；
    **B 相位也存盘** `_thr_calib_B.json`。新增 `FW_SMOOTH_M=3` 常量。
  - 踩坑：`FW_SMOOTH_M` 一开始没定义（只定义了 `K`）→ 首次运行直接 `NameError`
    （默认参数在 def 时求值）。教训：新加常量/默认参数后，先 `--b 2` 跑 2 秒干跑。
  - ✅ **0.61/0.61 下现场验证（陌生人）**：他调整姿态后检出 **95%**（36/38，脸宽 224px），
    Phase B 20s **59 帧全部 `reject`、`match`=0、`sim`=0**，win-max max=**0.565**
    → 实际余量 **+0.045**（比离线估计的 +0.025 更宽）。**零误报得到真机确认**。
  - 📐 **检出率 vs 取景/姿态（现场实测汇总，供第 2 轮 UI 向导 F8 / F4 用）**：
    | 场景 | 脸宽 | 检出率 |
    |---|---|---|
    | 陌生人站好（整头入画、睁眼、脸迎光） | 224px | **95%** |
    | 陌生人贴镜头 + 仰头 + 瞳孔上翻 + 背光 | 256~440px | **2%** |
    | 同一个陌生人，手臂挡住右半画面 | — | **0%** |
    | 注册者本人贴太近（整脸失焦糊） | 430px | **0%** |
    | 注册者本人站住、睁眼正视镜头 | 316~358px | **12%** |
    | 注册者本人在最初成功那轮 | 250~400px（median 315） | **65%** |
    - **结论：脸宽 >400px 几乎必挂**（板载固定焦镜头，近了就失焦）；**200~350px 是可用区间**；
      另有三个独立的"必挂"条件：**闭眼/瞳孔上翻**（检测器要可见眼睛）、**肢体遮挡镜头**、
      **顶光/背光**（脸落在阴影里）。
    - 👉 第 2 轮 UI 向导应给**实时脸宽 + 是否检到脸**的反馈，而不是只写文字提示；
      本次现场就是靠"每秒报数字"才把陌生人的检出从 2% 拉到 95%。
  - 📌 样本文件：`pc_host/_thr_calib_A_070baseline.json`（A 基线，阈值 0.70 时代、46 帧、
    无 trace、**是验证判定量重建精确性的那份**）、`pc_host/_thr_calib_B.json`（B，
    0.61/0.61 现场验证那次、59 帧、带 trace）。A 的真实命中率（带 trace）**尚未采到**
    —— 注册者本人几次都因取景/闭眼导致检出 0~12%，需在其姿态稳定后补测。
  - 🔬 **否证一个诱人的"软件兜底"**：现场把运行时检测阈值 `det_thr` 降到 **0.30 / 0.20**
    （readback 确认已生效）→ 检出**仍是 0%**。说明这种"检不到脸"是**结构性失败**
    （脸没被判成有效人脸：闭眼/俯视/失焦/遮挡），**不是分数卡在阈值边缘**——
    降 det_thr 救不了，只能改姿态/取景/光照。
  - 📌 **现场最典型的失败姿态**（抓帧确认）：**低头看屏幕而不是看镜头**（视线向下、
    上眼睑半合），且**镜头在脸下方仰拍**（画面里能看到鼻孔/下巴）→ 检出 0~2%。
    → 板子应架到**眼平高度**，受试者必须**注视镜头本体**（不是注视取景窗口）。
- **2026-10-06 第 2 轮开工：F4 轻跟踪 + 特征提取节流（已实现并真机验证 ✅）**
  - 实现（`app_faceai.cpp` + `Kconfig.projbuild`）：同一张脸（与上一帧识别框
    IoU > `APP_FACEAI_TRACK_IOU`(40%) 且中心位移 < 脸宽 `TRACK_MOVE_PCT`(10%)）且在
    `TRACK_HOLD_MS` 内时，**跳过 93ms 的 `s_feat->run()`**，沿用上一帧结论、只更新框
    （esp-who/esp-box 的"检测每帧、特征按需"）。新字段：结果帧 `reuse`（1=本帧跳过提特征）
    与 `feat_hz`（每秒提特征次数，**验收指标**）；`mode` ack 带 `trk=<ms>/iou<..>/sh<..>%`；
    **运行时可调**：`{"cmd":"mode","trk_hold_ms":400}`（0=关，照 det_thr 的先例）。
  - ⚠️ **跳过帧不进 F2 平滑窗**（跳过不是新测量，推进去会让 K 帧票数被同一值灌满、
    多帧确认就失去意义）→ 代价：**确认延迟随 HOLD 线性变长**（K=5 帧 × HOLD，
    HOLD=400ms 时整窗要 ~2s）。**F4/F2 的这个耦合是选 HOLD 时的核心权衡。**
  - 🔴 **踩坑：编译默认 150ms 是死值** —— AI 处理一帧要 **175ms（相机+预览流）/ 244ms
    （PC 送图）**，比 150ms 还长 → 保持窗**永远等不到触发**（先有鸡先有蛋：只有跳过提
    特征才能变快，跑不进 150ms 就永远不跳）。真机实测 HOLD=150ms 时 **reuse=0%**。
    已把编译默认改为 **250ms**（`Kconfig.projbuild` + `sdkconfig` + `sdkconfig.defaults`）。
  - 实测（PC 送图 20fps、同一张脸，`pc_host/_f4_sweep.py`）：
    | trk_hold_ms | 结果帧 | reuse | feat_hz | 结果 fps |
    |---|---|---|---|---|
    | 150 | 32 | **0%** | 3.9 | 3.9 |
    | 250 | 46 | 37% | 3.5 | 5.3 |
    | 400 | 57 | 65% | 2.7 | 7.3 |
    | 600 | 64 | 78% | 1.9 | 7.2 |
    | 1000 | 81 | 90% | 0.9 | **9.9** |
    → 提特征频率 ≈ **1/HOLD**；结果帧率随 HOLD 上升（最高 +154%）。相机路径（预览流开着）
    的 AI 基础节拍只有 5.7fps，HOLD=250ms 下提特征 ≈5.7→3.5 次/秒、帧率不变（帧源限制）。
  - ✅ **不引入回归**：`pc_host/_face_r1_test.py` 同条件对比 —— **F4 关 = 9/15、
    F4=250ms = 10/15，失败项完全相同**（P2 单帧注册 / P3 HI 快路径 / P4 灰区 /
    P5 陌生人稳定 reject）。这些是**既有的 harness 时间偏移**（每轮断言吃到上一轮的
    残留结果帧 → 逐帧计数与事件断言错位；trace 里同人 `sim_raw` 0.77~0.97 正常）。
    **待修：套件每轮发图前先 drain 残留结果帧**（否则它作为"安全网"信噪比太低）。
  - ⚠️ **局限（实测）**：跟踪判据只看几何 → **两个不同的人在同一位置交替出现也会命中
    复用**（实测 reuse 70% @HOLD=400ms）→ HOLD 内换人**会张冠李戴**（沿用上一个人的
    结论）。真人换位远慢于 400ms，当前可接受；但 **F7 多人脸 / F8 UI 时要重新审视**
    （可能要把"身份结论"也纳入跟踪判据）。
  - ✅ **顺带查清并修掉一个吞吐瓶颈（比 F4 更值钱）：0x05 预览流与 AI 抢帧池 + 抢 CPU0**
    - 现象：相机 20fps 下 AI 结果帧率只有 ~5/s（`infer_ms` 仅 18ms，本该快得多）；
      开预览流后腰斩到 ~2.6/s。
    - 根因两层：**① 帧池只有 2 槽**，而 `pipe_take_slot()` 要求槽既非 `latest` 又不
      `busy`，AI 拿到一帧要占槽 ~190ms → 这期间相机**无法发布**，池发布率被锁死在
      ≈AI 消费率（实测 5.3fps，相机明明有 20fps）；0x05 流再进来和 AI **平分**这
      5.3 帧 → 两边都 2.6。**② 两者还在 CPU0 上分资源**（不是单纯抢帧）。
    - 受控实测（4 槽固件、**钉死 `cam fps`**、无脸、`trk_hold_ms=0`）：
      | 相机 | 预览流 | AI 结果帧率 |
      |---|---|---|
      | 50fps | off | **17.0/s** |
      | 50fps | 10fps | 11.5/s |
      | 20fps | off | 10.8/s |
      | 20fps | 10fps | 6.4/s |
      → AI 由**管线每帧开销（≈59ms）**限制，不是自己的算力；预览流吃掉 AI 1/3~40%。
      ⚠️ 踩过的测量坑：白天曝光阶梯会把 `cam_fps` 从 50 降到 20/10，**不钉死相机帧率
      的 A/B 会被档位漂移污染**（我还误把"流没关掉"的一轮当成 OFF 用），且
      `detect=0` 时结果帧的 `fps`/`feat_hz` 字段是**陈旧值**（只在 handle_frame 里更新）。
    - **已修**：新增 `APP_CAM_PIPE_SLOTS`（默认 **4**，Kconfig 2~6）—— 2 槽时 20fps 下
      AI 只有 5.2/s，4 槽后 **10.8/s（+108%）**、`drop` 归零。PSRAM 32MB 足够
      （每槽 640x480x2 ≈ 614KB；PC 送图路径按解码尺寸，最大 1920x1088x3 ≈ 6.3MB/槽）。
    - ⚠️ **尚未解决（下一步候选，按性价比排）**：① 每帧每个消费者的 614KB
      `esp_cache_msync`（`get_frame` 的 M2C 失效）＋发布侧 614KB memcpy，是那 ~59ms
      的主要嫌疑，值得加分段计时证实；② 预览流任务与 AI 同钉 CPU0（缓存一致性要求
      消费者在**自己核**上失效，不能随手挪核）；③ 预览流缺省 10fps 对取景够用，可降到
      5fps 把 CPU 让给 AI（注意取景中枢重连会补 `stream on fps=10`，要改得一起改）。
    - 回归：`_face_r1_test.py` 基线 9/15 → **11/15**（失败项仍是那 4 个既有的 harness
      时间偏移项），未引入新失败。
  - 工具：`pc_host/_stream_probe.py`（帧池/预览流争抢探针，含受控实验）。
  - 🎯 **分段计时定位到"管线每帧开销"的真凶（2026-10-06，`imgpipe/prof` 每秒日志）**
    给 `publish_raw`（相机→槽）与 `get_frame`（消费者取帧）逐段加了计时，实测每帧：
    ```
    pub[copy 6.9ms  ae 0.4  dn 39(夜间降噪)  post 6.5  crc 18.5  msync 0.02]
    get[inv 0.04ms  6MB/s  lat 0ms]
    ```
    （640x480 RGB565 = 614KB/帧；`get` 是消费者侧的 M2C 失效，**实测可忽略**）
    - 🔴 **真凶 1：每帧全帧 CRC32 = 18.5ms** —— `frame.crc32 = esp_rom_crc32_le(全帧)`
      是**当年查跨核缓存一致性的诊断探针**，一直常驻。按 17~22 帧/s 算 = **单核 ~30%
      CPU 白烧**。**已门控**：新增 `APP_CAM_FRAME_CRC`（**默认 n**）；要复查缓存一致性
      时再打开（并接受管线变慢）。实测关掉后（暗光 10fps 档）：流关时 AI 结果帧率
      2.4 → **8.2/s**。
    - 🔴 **真凶 2：夜间降噪 39ms/帧**（暗光自动开，属功能）——夜视档下它才是最大头；
      白天不该开（白天档 dn=0）。**下次改动前先看 `imgpipe/prof` 的 dn 值**。
    - 固有成本：copy 6.9ms（614KB 拷贝）+ post 6.5ms（白平衡+镜像一次遍历）。
      我原先怀疑的 `msync`/消费者失效**只有 0.02~0.04ms，完全不是问题**（假设错了，
      计量纠正）。
    - ⚠️ 教训（与"诊断探针不可常驻"同源）：**18.5ms/帧 的探针藏了很久没人注意**，
      因为它不报错、只在结果帧里多一个 `frm_crc` 字段。**凡是"每帧全帧扫描"的诊断，
      必须默认关 + 开门控**。
    - ⚠️ 另：白天曝光阶梯会把相机压到 20/10fps，暗光下 `exp` 顶到 9994（100ms）→ 实际
      采集只有 ~10fps，此时**帧率由曝光限制**而非 CPU。所以吞吐对比必须在**亮环境 +
      钉死相机参数**下做，否则会被"暗光→降档"淹没（今天就被绕进去过一次）。
    - 📊 **日光下的分段数据（关掉 CRC 后，50fps 档）**：
      `imgpipe/prof n=51 pub[copy 8.6 ae 0.55 post 8.3 crc 0 msync 0.5] get[inv 15us lat 5~20ms]`
      → ① 管线**每秒发布 51 帧**（帧池已不是瓶颈）；② 发布端每帧 ~18ms（copy+post）=>
      **CPU1 上约 87% 被"拷贝 + 白平衡镜像"占满**；③ AI 侧 `infer_ms` 18ms → **55~77ms**
      （CPU0 被预览流的 JPEG 编码挤占：`enc=51ms`/帧 ×10fps ≈ 半核）。**瓶颈已从"帧池"
      转移到"两个核被大量无效/重复工作占满"**。
      👉 **最明显的浪费**：管线每秒发布 51 帧，而消费者只需 ~7（AI）+10（预览）帧 ——
      **没人要的帧也在做全帧拷贝 + 白平衡镜像**。**下一步候选：发布端"按需发布"**
      （上一帧还没被取走且无消费者在等待 → 跳过 copy/post），估计能省 CPU1 的大头。
  - ⚠️ **踩坑（我自己的误判，务必记牢）：手动 `cam fps` 会关掉夜视阶梯 → 画面过曝发白**
    - 现场症状：抓帧是一片均匀浅粉（完全白掉）；状态 `luma=236`（目标 120，严重过曝）
      却仍 `ntier=2 / cam_fps=5 / exp=9994(100ms) / dn=3 / gamma=650 / bf=6`（全按暗光跑）。
    - **真因不是固件 bug，是我自己关掉了阶梯**：`night_tick()` 第一行就是
      `if (!s_night_fps) return;` —— **手动 fps 命令按设计让位**（`s_night_fps=false`）。
      我收尾时写的是**不存在的字段** `{"cmd":"cam","auto":true}`（正确是
      **`{"cmd":"cam","night":true}`**），于是阶梯一直关着、档位停在 2 档不回升。
      复位板子即恢复（tier0、exp 500、luma 120/120、50fps）。
    - 📌 **教训**：① 用 `cam fps`/手动曝光做过实验后，**必须**补 `{"cmd":"cam","night":true}`
      才算收尾（`night` 字段名别记错）；② 复现异常时**先抓一帧**：本次"发白"还有第二个
      物理原因——**镜头被仰着对准天花板**（抓帧就是天花板+顶灯），只查参数会查偏。
    - 📌 **对 UI 的要求（F8）**：既然手动相机参数会让夜视阶梯让位，UI 上必须同时给出
      "恢复自动夜视"入口，否则用户会卡在错档（现场就是这么卡住的）。
  - ⚠️ **运维坑：netlink 偶发"连着不响应"**（今天出现两次）：中枢显示"上游已连接"、
      画面照旧在流，但**命令无应答**（`cmd_and_wait` 全超时）；`{"cmd":"cam"}` 有时能回、
      `list` 不回。当时正是我"多次直连板子 ÷ 反复复位"之后。**处置：复位板子 + 重启中枢**，
      一次就好（复位不丢库/不丢编译默认值）。怀疑是单客户端会话在异常断开后没干净释放；
      以后排查类似"命令不通但画面在"的症状别在协议层绕。
  - 工具：`pc_host/_f4_sweep.py`（保持窗扫描，运行时可调）、`pc_host/_f4_test.py`
    （静态/交替/慢帧率三组对照）。
  - ⚠️ 踩坑：**打开 COM4 会把 DTR/RTS 拉高 → 复位板子**（EN/GPIO0 就接在这两根线上），
    所以"一边抓串口日志一边做测量"必然废掉测量（今天就踩了一次）；测量期间要看板端
    日志，用 JCLINK 的 0x04 通道（`Link.logs`）。
  - 📌 **顺带发现配置漂移**：`sdkconfig` 曾被上一轮会话切成 ESPDet（为标定提高检出率），
    而 `sdkconfig.defaults` 也写着 ESPDet；本次为验证 F4、并保持与"已标定固件"一致，
    **把 `sdkconfig` 切回 MSRMNP 编译烧录**（`sdkconfig.defaults` 里 ESPDet 的选择与
    理由**未改动**）。两者现在**不一致**，下次开工需按 ④ 的结论统一（要么修 ESPDet
    关键点、要么把 defaults 也改回 MSRMNP）。
  - ⏳ **A 侧真实命中率未采到（多次尝试，原因已彻底定位）**：注册者本人在镜头前的
    检出在 **0% ↔ 13% ↔ 65%** 之间剧烈摆动，决定因素是**姿态/视线**：
    失败帧的共同特征是**双眼视线向下 / 眼睑半合**（习惯看屏幕而非镜头）+ 镜头在脸下方
    仰拍；最后一次门控 12s **0/74 帧**（抓帧可见瞳孔完全被上眼睑遮住）。
    - **已排除**：脸宽（300~430px 都试过）、光照（luma 131~141 / AE 目标 120 正常）、
      检测阈值（**运行时降到 0.20 仍 0%**）、固件（同一块固件上陌生人 **95%** 检出）、
      距离/失焦（330px 时同一人曾达 65%）。
    - **结论：这是"人的因素"，不是固件缺陷**——同一人最初成功那轮（13:56，脸宽 median
      315）检出 65%，证明条件合适时完全能检到。
    - 👉 **对第 2 轮 UI 向导（F8）的硬需求**：必须**实时显示"是否检到脸 + 脸宽"**并明确
      提示"睁开眼、看镜头、别看屏幕"；否则用户会像本次一样反复失败且不知道原因。
      现场实测：**只是把"每秒检出率"反馈出来，就把陌生人的检出从 2% 提到 95%**。
    - 待补：A 的带 trace 样本（用于量出"F2 窗口票数规则"造成的真实命中率损失）。
- **2026-10-05 第一次真机 A/B 标定尝试：A 拿到、B 未完成（现场干扰为主）**
  - 固件：探针关闭版（`DIAG_DEEP=n`）、库已清空、`cam_fps` 由阶梯决定。
  - **Phase A 数据（注册者 `me`，20s，57 样本，探针关闭）**：
    win-max min 0.339 / p05 0.414 / **median 0.621** / p95 0.715 / max 0.816；
    events `none 69 / tracking 38 / reject 19`；检出率 45%（126 帧 57 帧有脸）。
    ⚠️ 两个待查点：①**注册与采样之间阶梯把帧率从 20fps 降到 10fps、曝光 2494→3500**，
    成像条件变了；②本人 win-max 中位只有 **0.621**，而 PC 路径自匹配是 **0.997**
    ——差异怀疑来自真机姿态/关键点抖动 + 注册帧与采样帧曝光不同，下次要钉住帧率重测。
  - **Phase B 未完成**：现场干扰（镜头被碰歪 3 次：键盘特写→显示器照片→严重运动模糊；
    Ethernet 掉 3 次；模拟器两实例抢连接）。**没有可用数据，阈值仍未标定。**
  - **现场操作教训（下次照做，比算法问题更耗时间）**：
    1. **必须用 LVGL 模拟器当取景器**：板子是 PC-as-HMI（板载屏关闭），用户**看不到画面**，
       盲调必然反复失败。模拟器要确保**只有一个实例**（实测两个进程同时存在会抢板端唯一的
       TCP 客户端槽，症状 = "connect 成功但一帧都收不到 / 连不上"，极易误判成板子卡死）。
    2. 板端 netlink **一次只服务一个客户端**：模拟器在跑时 pc_host 脚本会卡在 backlog。
       **每次测量前必须停模拟器**：`wsl -e bash -c "pkill -f '[b]in/main'"`。
       ⚠️ 同一条命令行里**不能再出现 `./bin/main` 字面量**，否则 pkill 会杀掉自己（实测踩过两次，
       表现为整条命令无任何输出）。
    3. 摄像头会被碰歪（今天 3 次）；板子/网线也要固定，别被线缆拽动。
    4. **Ethernet 链路易掉**：症状是 PC 网卡变 Disconnected、ping 100% 丢包；但
       **Windows 网卡状态不可靠**（实测出现过"ping 不通但 TCP 能连"）。多数几秒后自行
       重协商恢复；TCP 彻底卡死时才复位板子（复位不丢库）。
    5. **站位纪律**：脸宽 200~320px（≈画面宽 1/3~1/2）、整头入画（头顶别切）、脸迎光、
       采样期间别动。实测 419px（太近、头顶切）检出率仅 24%，也出现过整段 0 检出。
    6. **A/B 之间要钉住同一帧率**：`{"cmd":"cam","fps":N}`（注意会同时关掉 AE，
       曝光就不自适应了）；软复位后阶梯从 50fps 重来（`exp_max=994`），跑一会儿才降到 10fps。
    7. `pc_host/thr_calib.py` 是**两段式**的，真人换人最稳：
       `--enroll <name> --shots 3` → `--phase a --a 20`（存样到 `_thr_calib_A.json`）
       → 换人 → `--phase b --b 20`（读回 A 并给建议）。**注册与 A 之间不要改光照/距离。**
- **⚠️ 2026-10-05 追踪出：检测（MSRMNP）本身是当前最大瓶颈，且强烈依赖人/几何角度**
  - 实测对照（同一固件、同一链路、相似构图 face_w 280~330px）：
    | 输入 | 检出率 |
    |---|---|
    | PC 送图 bill2.jpg | **100%**（`faces=1`，`infer_ms` 21ms）|
    | 注册者本人（真机，Phase A）| **45%**（126 帧 57 帧有脸）|
    | 另一位真人（真机，同构图）| **0~7%**（连续 9 次尝试全部拿不到样本）|
  - **已排除的原因**（都实测过，别再重复）：
    ① 检测没跑？→ 否：`infer_ms` 每帧都是 **18~22ms**（检测确实在跑，只是返回空）；
    ② 人脸过曝？→ 否：AE 关闭、手动扫 `exp` 4994→3500→2500→1800→1200→900
       （luma 123→51），检出始终 **0~7%**；
    ③ AE 横带伪影？→ 否：`ae=0` 钉死曝光后仍然 0/43（但**确实抓到过一次人脸横带**
       —— 上半帧过曝下半帧正常，就是第十一节记的滚动快门/AE 写入时序坑，属实存在）；
    ④ 取景/距离/角度？→ 调了 8 轮（太近 419px→后退→偏右出框→居中），不是唯一原因。
  - 结论：**MSRMNP（mAP50-95 仅 0.366，全家族最弱）在这个视角/人脸上不可靠**。
    下一步要么 **F11 换检测模型**（ESPDET_PICO_224，0.504，15→50ms），要么先把
    **检测阈值做成运行时可调**（`{"cmd":"mode","det_thr":x}`）再扫一遍验证。
  - 顺带发现（可观测性缺口）：`frm_crc`/`ai_crc` 只在"本帧检到脸"时才随结果帧发出
    （在 `res && s_dbg_stable>=0` 块里），**没有人脸时没有任何逐帧指纹** ——
    排查"检测为什么失败"时看不到 AI 实际输入。要用时给非人脸帧也带一个轻量指纹。
  - 顺带确认：`{"cmd":"cam","fps":N}` 会**同时把 AE 关掉**（`ae=0`），这正好可以用来
    **钉死曝光做 A/B 一致条件**（但要记得手动调 `exp` 到合理亮度，否则整段过暗/过亮）。
- **✅ 2026-10-05 F11 落地：检测换成 ESPDet+MNP 混合，检出率 0~7% → 100%**
  - ⚠️ **换检测器的硬坑**：**ESPDet 只出人脸框、完全不产出关键点**（其后处理里没有任何
    keypoint 代码），而 MFN 特征提取必须用 5 个关键点做 112×112 对齐 → 直接换会
    `assert failed: FeatImagePreprocessor::preprocess ... (landmarks.size() == 10)`
    直接 panic（板子进入断言重启循环，实测踩了一次）。
  - **正解 = 两级混合**：`ESPDet Pico 224（出候选框，召回好）→ MNP（精修 + 补关键点）`，
    MNP 就是 MSRMNP 的第二级、模型本来已打包。改在 `app_faceai.cpp`：
    `s_mnp = new human_face_detect::MNP("human_face_detect_mnp_s8_v1.espdl", thr,
    MNP::default_nms_thr)`，检测处 `s_mnp->run(img, s_detect->run(img))`。
  - 开关：`APP_FACEAI_DETECT_MODEL`（Kconfig 二选一，默认仍 MSRMNP，当前已切 ESPDet）；
    `CONFIG_FLASH_ESPDET_PICO_224_224_FACE=y` 让 544KB 模型打进同一个
    `human_face_detect.espdl` rodata blob（MSRMNP 187KB 保留 → 可随时切回）。
    bin **6.0MB / 8MB（剩 25%）**。
  - **真机实测（同一人、同一构图 face_w 268~283px）**：
    | 检测 | 检出率 | `infer_ms` |
    |---|---|---|
    | MSRMNP | **0~7%** | 15~22ms |
    | **ESPDet+MNP** | **100%**（`det_thr` 0.50/0.35/0.25 三档全 100%）| **71ms** |
    → `det_thr=0.50` 已足够，**不要为了召回降阈值**（只会多引入误检框）。
    代价：AI 环从 ~5fps 掉到 **~2fps**（检测慢 3 倍多）——用 F4 跟踪节流补回来。
  - 新增**运行时检测阈值**：`{"cmd":"mode","det_thr":0.35}`（也接受 `35`），
    `mode` ack 回 `det_thr=x.xx <模型名>`；排查"检不到脸"时现场扫，不必重烧。
    ⚠️ 单阶段检测器只有 idx 0（传 idx 1 命中 assert）。
  - ~~**取景器"常开"的可行方式**：模拟器常驻 + 测量前 `pkill` 再重启（自动交接，画面中断 5~10s）~~
    → **已废弃（2026-10-06）**：改用 `pc_host/board_hub.py` 取景中枢（本机代理 + 常驻窗口），
    画面在测量期间**完全不断**，不再需要杀模拟器。详见本节末「PC 端取景中枢」。
    （保留原理供参考：板端 netlink **一次只服务一个 TCP 客户端**，`listen(backlog=1)` +
    单客户端 serve 循环——这就是必须由单一进程独占上游的原因。）
  - ⚠️⚠️ **2026-10-05 深夜补测（重要修正）：ESPDet+MNP 修好了检出率，但把特征质量搞坏了。**
    - PC 路径同图自匹配（`musk1` 对 `musk1`，n=14）：
      `0.330 0.340 0.340 0.341 0.342 0.343 0.343 0.344 0.636 0.638 0.663 0.684 0.707 0.834`
      → **med 0.344，≥0.90 的帧 0/14**；对照（摘探针 + MSRMNP）为 **med 0.997、6/10 ≥0.997**。
    - 真机 Phase A（注册者本人，ESPDet+MNP）：win-max 中位 **0.643**（10fps/`exp=4994`）
      与 **0.580**（50fps/`exp=994`）→ **曝光时长/运动模糊不是原因**（假设已用实测排除）。
    - 归因：**关键点质量下降**。两条路径的关键点都由 MNP 产出，唯一差别是喂 MNP 的**候选框
      来源**（MSR 紧框 vs ESPDet 框）→ 怀疑 ESPDet 框偏松/偏移，MNP 的 48×48 裁剪里人脸占比
      小、关键点漂移 → 112×112 对齐错位 → 特征自匹配从 0.997 崩到 0.34。
    - **因此 F11 当前实现不可用**（检出 100% 但识别废了）。下次开工第一件事：**打开
      `APP_FACEAI_DIAG_DEEP` 把 MSR 与 ESPDet 的框 + `kp` 都 dump 出来对比**。
    - 候选修法：①ESPDet 框按比例收紧后再喂 MNP；②两级门控（先 MSRMNP，检不到再用
      ESPDet+MNP 兜底）；③试 `ESPDET_PICO_416_416_FACE`（框更准？）。
    - 板端当前停留状态：**ESPDet+MNP 固件（检出 100%、自匹配 ~0.34）**——
      下次先决定"修关键点"还是"回退 MSRMNP"，别在没定之前跑标定。
  - 🔬 **2026-10-06 凌晨追溯（两个假设都被实测否掉，真凶指向"模型执行序列污染特征"）**
    1. **"ESPDet 框偏松/偏移"假设 → 否**：PC 送图实测 ESPDet+MNP 的框与 MSRMNP 时代
       **几乎一致且很稳**：`bill2` `[93,57,209,223]…[92,59,212,223]`（MSRMNP 记录
       `[88,60,212,223]`）；`musk1` 4 帧几乎不变 `[95,69,206,222]`。
    2. **"关键点抖动 → 对齐错位"假设 → 否**：新增**无副作用**探针
       `APP_FACEAI_DIAG_KP`（只上报 `kp`，不额外跑模型，与会污染模型的
       `APP_FACEAI_DIAG_DEEP` 分开）后实测同图 14 帧：
       `kp` 每坐标跨度仅 **0~6px**（多数帧完全相同），而 `sim_raw` 在
       **0.233~0.963** 之间乱跳。→ **输入相同 + 关键点相同，特征相似度仍然乱跳**，
       说明不确定性在**特征模型执行内部**，与检测/对齐无关。
    3. **对照**：MSRMNP（小模型：MSR 60KB+MNP 127KB）典型值 **0.998**（偶发低野值）；
       ESPDet+MNP（检测模型 544KB）典型值 **0.76**。→ **模型执行序列/张量残留会污染
       特征质量，且模型越大越明显**（正是第一节记的"esp-dl 执行历史相关"）。
    - **由此得到的最实用路线**：**回退 MSRMNP（特征干净）** + 用新加的运行时
      `{"cmd":"mode","det_thr":0.2~0.3}` 把检测阈值压下去救活体检出率，
      避免引入大模型对特征的污染。
    - 长期正解（择机立项）：查 esp-dl 特征模型的**张量/缓存残留**——给特征模型独立
      内存 arena、或在提特征前后做 cache 维护；否则"换更大的模型"永远伴随特征退化。
  - 🔬 **2026-10-06 凌晨后半：把非确定性定量化 + 分离度结论（重要）**
    - 现象：**同一个 MSRMNP 干净构建，跨会话自匹配的典型值会在 0.79 与 0.995 之间切换**
      （`kp` 跨度恒为 **0**，关键点完全稳定，所以与对齐无关）；
      同一会话内则相当稳定（例如一次会话里 0.787~0.795 成簇，另一次里 0.995~0.998 成簇）。
      → 结论：esp-dl 的特征提取会落在**几个稳定的"模式"之一**，模式由执行历史/内存布局决定，
      **跨会话/跨构建不可复现**；但模式内部自洽（同帧内自比较仍 ≈1.0，所以注册时的
      `query_feat(f, 0.999f, 1)` id 回查照样能过 —— 这掩盖了问题）。
    - **分离度实测（PC 路径，同一会话内）**：
      | 对比 | 结果 |
      |---|---|
      | 自匹配 musk1 vs musk1 | med **0.995**（min 0.363 单帧野值）|
      | 同人不同照片 musk2 | med 0.397 |
      | **陌生人 bill2** | med **0.008**（0.007~0.009）|
      | **gap** | **0.986** |
      → **即使在 0.79 那个模式里，陌生人也稳定在 0.01 附近**，分离度在观测到的每个模式下都很大。
    - **实践结论（改写标定思路）**：阈值必须是**同一会话内相对标定**的（不能用跨构建的固定
      绝对值）；只要陌生人的簇远低于本人的簇，识别就可行 —— 所以 **A/B 必须在同一会话、
      同一固件、同一光照与帧率下连续做完**（`thr_calib.py` 两段式正好满足）。
    - 下一步验证清单（按性价比）：
      1. **MSRMNP 的 `det_thr` 现场扫**（现在运行时可调，无需重烧）——若 0.2~0.3 能把活体
         检出率救到够用，就同时拿到"检出够用 + 特征干净"，**首选**；
      2. 若救不回来，用 ESPDet+MNP 构建（检出 100%）做 A/B，接受压缩后的自匹配簇
         （分离度可能仍够）；
      3. 真机 A/B 必须**同一会话内跑完**，且记下当时的 `kp` 是否稳定。
    - ❌ **2026-10-06 实测：第 1 条被否**。MSRMNP + 运行时 `det_thr` 现场扫
      **0.50 / 0.35 / 0.25 / 0.15 四档检出率全是 0%**（各 66~69 帧零检出）——
      即 MSRMNP 对"另一位"那张脸**连候选框都产不出来**，与阈值无关。
      已知对照：MSRMNP 对注册者 45%、对另一位 0%；**ESPDet+MNP 对两者都 100%**。
      → **活体路径必须用 ESPDet+MNP**；结合上一条"分离度在每个模式下都很大"，
      结论是：**用 ESPDet+MNP 构建、在同一会话内做 A/B 标定**（接受压缩后的自匹配簇，
      `thr_lo` 会比 PC 路径低）。MSRMNP 只能留在 PC 送图场景。
    - ⚠️⚠️ **2026-10-06 白天复测：上面这条被**推翻一半**（重要教训：夜里的结论不可外推）**
      - 夜间那批 0~7% 是**在 `exp=4994`（≈100ms 整帧积分）下测的**，被运动模糊掩盖；
      - **白天实测（exp 500~994 ≈ 10~20ms、fps 回到 50）**：MSRMNP 活体检出
        **32%（21/66）与 29%（20/69）两轮**，`face_w` 281~283px，`infer_ms` 26~30ms。
      - 即 **MSRMNP 在白天是可用的**（不如 ESPDet 的 100%，但对采样足够：20s 约 30 个样本）。
      - 另一条要注意的坑：**AE 从上一状态收敛需要几秒**——复测第一次跑出来 0%，
        回头看是开局 `luma=231` 严重过曝，等它落到 `luma=113` 后才正常。
        **任何"测检出率"的动作都要先读 `luma`，确认 AE 已收敛再下结论。**
      - 于是两条路都通：**MSRMNP（检出 ~30%、特征干净 0.995、26ms、无需重烧）**
        vs **ESPDet+MNP（检出 100%、特征被压缩 0.34~0.79、71ms、需重烧）**。
        标定的看家本领是**分离度**，故优先用 **MSRMNP** 跑 A/B；不行再换 ESPDet。
      - ⚠️ 另注：那次 det_thr 扫描**并没有换人**（现场一直是同一个人），
        所以"MSRMNP 对第二位 0%"这句不成立；结论只应表述为
        "夜间长曝光下 MSRMNP 对该面孔 0%（任何阈值）、ESPDet 100%"。**换人步骤必须
        停下来等用户确认**（`thr_calib.py --phase a/b` 两段式正是为此设计）。
- 名表容量（10 人/32 特征）、UI 向导、备份导出 = 第 3 轮（F8/F9/F10）。
- Kconfig：`APP_FACEAI_MATCH_THR` 已删除（被 HI/LO 取代），sdkconfig.defaults 同步；
  改后已 reconfigure。
- 诊断字段现状（2026-10-06 更新；旧的"第 2 轮开工时移除"条目**已完成**）：
  `frm_crc/ai_crc/dec_core/ai_core` 常驻（轻量）；`sim_q2/sim_f2/ai_crc2` 由
  `APP_FACEAI_DIAG_DEEP`（默认 n）门控；`kp` 由**新增的** `APP_FACEAI_DIAG_KP` 门控
  （**无副作用**：只复制关键点并上报，不额外跑模型——与 DIAG_DEEP 分开，正是为了
  能"开着探针测"而不改变被测对象）。板子当前 `DIAG_KP=y`、`DIAG_DEEP=n`。
- 🖥️ **PC 端取景中枢 `pc_host/board_hub.py`（2026-10-06 新增，真机验收 ✅）**
  - **动机**：板端 netlink 一次只服务一个 TCP 客户端，WSL 取景器会独占连接 → 跑 A/B 时
    看不到画面，调试极难（用户原话："测 A B 时不能看到画面太难调试"）。
  - **做法**：hub 成为**唯一**连板子的客户端，并在本机 `127.0.0.1:13333` 开一个**协议完全
    一致**的端口做透明转发 + 向所有本地客户端广播；同时用 Tkinter 窗口**常驻显示**画面。
    现有脚本**一行不改**，只把 `--tcp` 指向本机端口。
  - **用法**：
    1. `python pc_host/board_hub.py`（窗口 + 本机代理；可选 `--zoom 1|2|3`、`--fps`、`--fmt`）
    2. 脚本一律连本机端口，例：`python pc_host/thr_calib.py --tcp 127.0.0.1:13333 --phase a --a 20`
    3. 无人值守自检：`python pc_host/board_hub.py --selftest`（退出码 0 = 通过）；
       窗口化验证：`--exit-after 6`（跑满 6s 自动退出）
  - **窗口显示**：0x05 画面 + 检测框（命中画绿框并标"名字+sim"）+ 5 关键点（黄点带编号）
    + 状态栏（`event/faces/box_w/sim/sim_raw/fps/infer_ms/lib_n/src/stable`，以及上游帧率、
    本机客户端数、重连次数、RX 字节）。**`box_w` 就是脸宽像素**——取景时直接对着它调到
    200~320px。按键：`1/2/3` 缩放、`b` 框、`k` 关键点、`t` 文字、`s` 截图（存 `pc_host/shots/`）、`q` 退出。
  - **"板子任何状态都不断画面"（逐项实测）**：
    | 场景 | 实测结果 |
    |---|---|
    | 多脚本并行（jc_test / thr_calib / watch 同时）| 都正常收发，画面不停（各收到 31 图 + 32/34 结果）|
    | 有脚本把 `stream` 关掉 | **3~5s 内自动补发 `stream on`**，画面恢复（关流 10s 后仍收到 24 图）|
    | 板子复位/重启 | 自动重连 → `首帧到达 seq=0`，画面恢复，窗口不退出（重启后收到 37 图）|
    | 上游"连上但一个字节没收到" | 8s 后判定"没被服务"并强制重连，日志给出明确告警 |
  - **纪律（避免又互相抢连接）**：
    1. **只允许 hub 直连板子**；所有 pc_host 脚本一律 `--tcp 127.0.0.1:13333`
       （现有脚本的默认值仍写着板子地址，**故意不改**——没起 hub 时仍可直接连板子调试）；
    2. **不要同时开着 WSL 模拟器**：它源码默认 host 就是 `192.168.33.2:3333`
       （`lv_port_pc_vscode_official/main/src/main.c:128-129`），且 `net_link_tick()` **每帧自动重连**
       → 会静默抢走唯一客户端槽（症状 = hub 报"已连上但无任何数据"）。若确实要用它，
       指向 hub：`./bin/main --host 127.0.0.1 --port 13333`（从 WSL 访问宿主需 hub `--bind 0.0.0.0`）；
    3. 直连板子的默认值清单（要盯的地方）：`thr_calib.py:235`、`night_accept_test.py:107`、
       `protocol.py:123/126`、`_face_r1_test.py:28`、`stutter_probe.py:65`、`_cycle_probe.py:16`、
       `ae_accept_test.py:6`、`jc_test.py` 文档示例。
  - **实现要点 / 两个必须记住的坑（改这个文件前必读）**：
    1. **`Hub.shutdown()` 不能持 `clients_lock` 再调 `client.close()`**——`close()` →
       `_drop_client()` 会再取同一把**非重入锁** → **死锁**（首版实测：进程卡死不退出，
       还占着板子唯一连接，害得后面 hub 报"已连上但无数据"，排查了半天）。
    2. **保活判定必须每轮循环都做，不能只在 socket 超时分支做**——板子在 0x05 停流期间
       **仍持续发 0x02 结果帧（~10fps）**，socket 永不超时 → 只在超时分支判定会导致
       "`stream` 被关掉后画面再也回不来"（首版实测踩到）。
  - 环境事实更正：**本机 Python 3.14 已装 `cv2 5.0.0`**（旧记录"OpenCV 未装"已过期）；
    `tkinter`、`Pillow 12.3.0`、`numpy 2.4.3` 均可用。本工具按用户选择用 Tkinter+Pillow。
- ⚠️ **当前"板子实际固件"与"仓库配置"不一致（2026-10-06 交接时的真实状态，下次先处理）**：
  - **板子实际在跑**：`msrmnp_s8_v1`（末次已烧录的固件），`det_thr=0.50`，`DIAG_KP=y`、`DIAG_DEEP=n`；
  - **仓库 sdkconfig / sdkconfig.defaults 已是**：`APP_FACEAI_DETECT_ESPDET_PICO_224=y`
    ——切 ESPDet 的那次**只跑完 `idf.py reconfigure`，build+flash 被中途取消**，所以配置领先于固件；
  - 下次开工先决定用哪个：要 ESPDet 就 `idf.py build` + `idf.py -p COM4 flash`（约 6 分钟）；
    要回 MSRMNP 就把 sdkconfig 里那两行改回 MSRMNP 再编译烧录；
  - **拿不准时用 `{"cmd":"mode"}` 回读**：ack 会带当前模型名与 `det_thr`
    （例：`det_thr=0.50 msrmnp_s8_v1`）——**这是确认板子真实状态的权威手段**，别靠记忆猜。

## 廿二、第 2 轮 F6/F7 + 发布端按需发布 + 回归套件修复（2026-10-06 晚，真机验证 ✅）

本节 = 一次会话的全部改动与数据。开工时板子状态与第廿一节收尾一致（MSRMNP、
thr 0.70/0.80、lib=['musk']、镜头仍对天花板——所以本轮所有验证都走 PC 送图或
钉死参数的暗场相机路径，**没有真人介入**）。

### 1. 回归套件修复（待办 6）——先修 harness 再动固件（顺序是对的）

- **修复**：`_face_r1_test.py` 新增 `drain_results()`，**每张测量图发送前先清空板端
  残留的 0x02 结果帧**（上一轮迟到回包/在途帧）。前提经验证：`src=pc` 空闲时板子
  3 秒推 0 条结果帧（实测），所以 drain"读到安静即停"（quiet 0.35s）。
- **结果：同一块旧固件上 9~11/15 → 19/19 全绿**。文档的诊断完全正确：4 个失败项
  全是"每轮断言吃到上一轮残留结果帧"。
- 顺带：P3/P4/P5 循环跳过 `reuse=1` 的 F4 复用帧（不是新测量，不进确认帧计数），
  循环上限放宽（8/10/10）留余量。

### 2. P4 改自适应阈值（会话模式鲁棒）

- **踩坑**：新固件首轮回归 P4 失败——本会话 musk2（同人跨照片）相似度聚在
  0.42~0.59，而固定灰区 0.70/0.85 只在 0.77 模式下成立。这正是第廿一节记录的
  esp-dl"会话模式"双模态（0.77 或 0.40，执行历史决定）；本轮还实测到**同一会话内
  混跳**（probe 3 帧 = 0.776 / 0.407 / 0.253）。
- **修复**：P4 改三步——①用"不可能命中"的 0.98/0.99 探 musk2 ×3（窗口不满 5 帧
  不会误触发稳定 reject）；②按中位数设灰区 LO=med-0.08 / HI=med+0.05；③发一张
  **无脸图**（PIL 生成纯色 JPEG）清空平滑窗，从空窗验证"灰区需 M+1 帧"。
  无脸图清窗这个手法值得记住：`!big → smooth_reset()`，等价于"人脸离开"，
  PC 送图就能触发，不用 clear（清库）。
- trace 里的 `('reject',…)` 后跟一串 `('none',…)` 是 **F6 在正确工作**（去重窗口内
  稳定 reject 抑制为 none）——P5 因此改为先 `{"cmd":"mode","dedup_ms":0}` 再断言。

### 3. F6 事件去重（规划 P1-F6，`app_faceai.cpp`）

- **语义**：同一人的 match / 任意稳定 reject 在 `s_dedup_ms`（编译默认 5000，运行时
  `{"cmd":"mode","dedup_ms":N}`，0=关）内只报**一次事件**；窗口期内 event 回落
  `"none"`，但 `match`/`sim`/`stable` 字段照常带——UI 名字不闪断，host 联动靠事件边沿。
- **实现要点**：门加在 `handle_frame` 末尾（`track_cache` **之后**）——F4 复用帧回放的
  结论会重新过门，**不会绕过去重刷屏**。match 按名字去重、reject 按时间统一去重
  （陌生人没有 id 可作键；T 内换一个陌生人也不重报，门禁语义可接受）。
- clear/delete/register 三个库操作复位去重状态（否则跨阶段抑制）。
- 回归断言（P3 内）：match 后再发同图 → `event=none && match=musk && stable=1` ✓。

### 4. F7 多人脸（规划 P1-F7，`app_faceai.cpp`）

- **语义**：按面积取前 `s_multi_max`（编译默认 2，运行时 `{"cmd":"mode","multi":N}`，1=旧行为）
  张脸分别提特征。**主脸（第 1 大）走完整 F2/F4/F6**（顶层 match/sim/stable 语义不变）；
  **副脸每帧裸比对**（无平滑窗口），`raw_sim >= thr_lo` 才报名字（esp-dl 野值靠这条兜底）。
- **结果帧**：`boxes[i]` 在第 5/6 位追加 `name`/`sim`（旧解析只读前 4 个坐标，兼容）；
  未提特征的脸不加元素（len=4）。`infer_ms` 重算为含副脸的总耗时。
- **F4 协同**：复用帧整帧不提特征（主脸+副脸都省），副脸标签从 `s_multi_cache` 快照回放
  （标签不闪断）；注册/删除/清库时快照作废。
- **验收（`pc_host/_f7_test.py`，bill2+musk1 水平拼图 PC 送图）**：双人检出 10/10、
  box 结构 10/10、双命名 5/10（野值波动，见局限）、**名字跟随正确的脸 14/14（零张冠李戴）**、
  主脸 F2 确认到达 ✓。
  - 拼图配方：每张 2 倍放大（脸宽 112→224px，MSRMNP 可用区间）+ `det_thr` 临时 0.35
    （MSRMNP 对双人小目标漏检，0.50 时 10 帧全只出 1 张脸）+ 间隔 40px。
  - ⚠️ **已知局限**：副脸无平滑窗，esp-dl 单帧野值偶发闪错名（一轮实测 1/16，复跑未现）。
    根治需按脸平滑（F8/F7 联合立项时定）。主脸不受影响（F2 窗口抑制）。

### 5. 发布端按需发布（待办 5，`app_imgpipe.c/h`）

- **机制**：`publish_raw()`（camera 任务/CPU1）在「没有消费者正在 `get_frame` 里等帧
  **且** 距上次真实发布 < `APP_CAM_PUB_MIN_MS`(100)」时**整条链路跳过**（不取槽、
  不拷贝、不 AE/DN/post，帧在源头丢弃）。消费者侧在 `get_frame` 的等帧短窗内维护
  `s_waiters` 计数（启发式，无需加锁）。
- **⚠️ 强制下限必须有**：AE 亮度采样 / 运动估计 / 时域降噪全挂在发布路径上——
  全跳过 = 夜视阶梯与 AE 失去输入（luma 冻结 = 判定状态机瞎掉）。100ms（≥10 帧/s）
  足够喂饱全部判定节流（AE 250ms / AUTO 探测窗 300ms）。
- **运行时开关**：`{"cmd":"cam","pub_demand":false}`（A/B 用）；prof 日志新增 `skip N`。
- **A/B 实测（暗场、手动钉 50fps——⚠️ 暗场满曝光下传感器实际只出 ~26fps，固有特性）**：
  | 指标 | A 关 | B 开 |
  |---|---|---|
  | 实际发布（prof n） | 27 帧/s | **18 帧/s（skip 10/s）** |
  | 发布侧 CPU1 | ~46%（17ms×27） | **~30%** |
  | AI 结果帧率 | 10.8/s | 9.0/s |
  | 预览流 | 9.7/s | 8.0/s |
  | 排队延迟 lat | 15ms | 0ms（帧更鲜，但要现做） |
- **解读（务必读懂再动这块）**：省的是"发布了但没人等"的帧；代价是消费者要等
  下一次发布（≤相机周期 + ~17ms 管线）。**暗场无脸饥饿场景**（AI 60ms 一循环、
  相机只有 26fps）是**最坏情形**：~15% CPU 换 ~15% 消费者吞吐，不划算——用开关关掉。
  **白天 50fps + F4 节流**（AI 175~244ms/帧、忙时根本没人等帧）才是目标场景：
  跳过率应达 ~25-35 帧/s、省 CPU1 ~40%、消费者无感（AI 忙时的帧反正会被跳过，
  它闲下来时下一帧 ≤37ms 内就到）。**镜头对准拍摄位后用 `imgpipe/prof` 复核。**

### 6. 配置统一（待办 3 关闭）

- `sdkconfig.defaults` 检测模型 ESPDet → **MSRMNP**（理由与决策依据写进了 defaults 注释：
  白天检出 ~30% 够 A/B 采样、特征干净 0.995、标定的看家本领是分离度；ESPDet 特征被压到
  0.34~0.79）。`sdkconfig` 本就是 MSRMNP，两边一致，无需重烧。
- 本次编译烧录：bin **6.0MiB**/8MB（剩 25%）——与第廿一节 F11 之后"双模型打包"的尺寸一致
  （544KB ESPDet 仍在 flash 里，可随时切回），不是异常。

### 7. 新固件真机状态（收工实况）

- `mode` ack：`detect=1 match=1 thr=0.70/0.80 (lo/hi) det_thr=0.50 trk=250ms/iou40/sh10%
  dedup=5000ms multi=2 msrmnp_s8_v1`；**lib 空**（F7 测试收尾 clear，`--enroll` 随时重注册）。
- 回归 **24/24**（含 F6/F7 新断言）+ F7 双人 5/5 + 按需发布 A/B + 相机自动态已恢复
  （`{"cmd":"cam","night":true,"ae":true}`——手动 fps 实验后必做，见必读坑 3）。
- 新增命令汇总：`{"cmd":"mode","dedup_ms":N}` / `{"cmd":"mode","multi":N}` /
  `{"cmd":"cam","pub_demand":bool}`；mode ack 新增 `dedup=<ms> multi=<N>`。

### 8. A 侧命中率补测完成（2026-10-06 晚，取景中枢在环，待办 2 关闭）

- **流程**：中枢起在 127.0.0.1:13333 → 抓帧预检（发现镜头仍对暗处、画面纯噪声 gain 144，
  提醒用户后 AE 收敛到 luma ~96 / exp 3000 / gain 0）→ `--enroll me --shots 3`（60s 窗口内
  采满 3 帧）→ `--phase a --a 30`（带 trace 存 `_thr_calib_A.json`）。
- **A 相位数据（30s，74 帧）**：检出 19 帧（**连续性仅 26%**——姿态/光照仍是主因）；
  win-max(k=5) median **0.641** / p95 0.863 / max 0.876；板端事件 none 55 / tracking 21 / match 1。
- **固件 F3 规则复算（thr_calib.hit_frames，判定量 = win-max + 窗口占用票数）**：
  | thr_lo | 报 match 帧 | 占有脸帧 | 占全部帧 |
  |---|---|---|---|
  | 0.55 | 2 | 9% | 3% |
  | **0.61** | **2** | **9%** | **3%** |
  | 0.70 | 1 | 5% | 1% |
- **📌 决定性发现（与第廿一节的判断一致并定量化）**：19 个判定帧里 **10 个（53%）分数过 0.61**，
  但票数达标的只有 2 个——**F2 窗口票数规则吃掉 8 个**；窗口占用分布 `{1:11, 2:5, 3:1, 4:1, 5:1}`
  （检出间歇 → 凑不满 M+1=4 票）。**识别率的真瓶颈 = 检出连续性，不是阈值**；继续降阈值无益。
  提命中的方向：补光/眼平/看镜头（物理），检测侧改进，或给 F2 加"占用不足时的单帧高阈值快速通道"。
- **✅ 决策：`thr_lo=0.61 / thr_hi=0.61` 已持久化**——`sdkconfig` 与 `sdkconfig.defaults`
  **两处都改**（⚠️ defaults 只对全新 sdkconfig 生效，当前构建的编译默认在 sdkconfig 里；
  下次烧录后复位不再丢），运行时也已下发（`mode` ack 回读 `thr=0.61/0.61`）。
  依据：B 上沿 0.565（旧 B 样本，真机零误报）余量 +0.045；A 分数 53% 过线，0.61 严格优于 0.70。
- **遗留**：B 相位在新固件上复验（换一位未注册真人，`--phase b --b 30` 读回 A 出建议）。

### 9. B 相位复验完成（2026-10-06 晚，照片展示方式，0.61 最终确认）

- **方式**：无真人在场，用 esp-dl 官方陌生人照片（musk，库里只有 `me`）**全屏显示在
  显示器/手机上对镜头展示**——走真实相机路径（与 A 同口径）；⚠️ PC 直接送图不可用
  （文档实测 PC 路径陌生人 sim ~0.01 vs 相机路径 0.43~0.59，会高估余量）。
- **过程事故（处置记录）**：第一次 phase b 报 "library: []"——实为**板端 netlink 挂死 +
  上游 TCP 连不上**（hub 日志刷"上游连接失败"，ping 100% 丢包 3 分钟+），不是库丢了。
  处置按文档：打开 COM4 复位板子（顺带抓启动日志无 panic）→ hub 自动重连 →
  `list` 回读 `['me']` lib_n=3 完好（复位不丢库再次实证）→ 重发运行时 0.61（复位丢）。
- **B 相位数据（照片展示，64 帧，检出连续性 91%——照片不眨眼不动，与真人 A 的 26% 形成对照）**：
  win-max(k=5) median **0.144** / p95 0.189 / **max 0.189**；单帧 sim_raw max 0.189；
  事件 none 67 / reject 5 / **match 0**（现场零误报）。
- **与 A 的分离**：A median 0.641 vs B median 0.144（中位差 0.50）；**0.61 在本 B 样本上
  余量 +0.42**，全阈值扫描 B 误命中 0/64（0.50~0.80 全部 0%）。
- **⚠️ 工具建议 thr_lo=0.21 被否决（重要）**：那是"贴着 B 上沿"的朴素最优——余量只剩
  +0.021，而本 B 是**照片展示**（平面、固定光照、无微动），上沿 0.189 显著低于上一轮
  **真人 B 的 0.565/0.587**（照片展示低估了真人的上沿）；且 A 命中 3/74 vs 2/74 几乎无收益
  （F2 票数规则才是瓶颈，见第 8 节）。**最终定值维持 0.61/0.61**（真人 B 余量 +0.045、
  照片 B 余量 +0.42、两轮现场零误报），已在 sdkconfig + defaults + 运行时三处生效。
- **遗留**：若将来要压更低阈值换命中率，必须先用**真人 B**（换一位未注册者）重采上沿；
  照片 B 可作为回归时的快速 sanity（检出连续性高，30s 就能跑）。

### 10. F2 快速通道（fast-confirm）落地（2026-10-06 晚第 2 段，真机验证 ✅）

- **动机**：第 8 节复算确认命中率真瓶颈 = F2 票数规则（53% 过线帧被吃掉 8/10）。
  用户拍板：物理环境暂不动，先做快速通道。
- **语义**：本帧裸相似度 `raw_sim >= fast_thr`（编译默认 0.80，运行时
  `{"cmd":"mode","fast_thr":N}` 接受 0~1/0~100，0=关）时**不等窗口票数直接确认
  match**；只信"本帧自己的强测量"——多人库时名字取本帧 top-1 的 id，不是窗口
  多数派。normal 路径完全不变；F4 复用帧回放缓存结论天然兼容；F6 去重照常作用。
- **离线复算（A/B trace）**：A 命中 2/19 → 8/19（fast=0.80），B 误报 0/58；
  真人陌生人上沿 0.587 → 余量 +0.21。
- **真机验证**：回归 **25/25 全绿**（P3 自匹配 0.93、P4 灰区 4 帧、P4.5
  `fast=0.72 sim=0.776 frame#1` 单帧直达 match、P5 bill2 稳定 reject、P6 恢复读回
  `thr=0.61/0.61 dedup=5000ms multi=2 fast=0.80`）；手动实验单帧 0.964 直达 match。
- **实现落点**：`Kconfig APP_FACEAI_FAST_THR`(80) + `app_faceai.cpp` 匹配分支
  fast_hit + mode 命令/ack（`fast=%.2f`）+ sdkconfig/defaults 两处。
- **⚠️ 过程揪出并修掉一个上线以来就潜伏的固件竞态（比快速通道更值钱）**：
  **stream 任务在 src=pc 模式下与 AI 抢帧**——每帧只交付一次，stream 抢到 =
  AI 无结果 = 该帧不回 0x02、PC 端读超时。以前从没暴露：直连调试从不开
  stream；**取景中枢上线后其保活把 stream 钉在开**（实测每 3~10s 补发一次，
  累计 111 次），经中枢跑 PC 送图必被间歇偷帧（回归 13~15/21、注册把摄像头
  里的真人收进库等乱象）。**修复**：`app_stream.c` stream_task 加源模式守卫
  （src!=CAM 时 sleep 不取帧——PC 源下 0x05 回传本无意义）。经验证帧结果
  0.24~0.31s 稳定返回。
- **配套修复 pc_host 两处协议健壮性（经中枢跑测试的必修课）**：
  1. `protocol.py cmd_and_wait`：ack 改**按命令名精确匹配**——hub 补发的 stream
     ack 也带 ack 字段，只认"有 ack"会把它错当成自己命令的应答（P6 曾收到
     stream 的 ack、msg 全错）；
  2. `_face_r1_test.py send_img_get_result`：跳过带 ack 字段的帧 + 加 `--tcp`
     参数（中枢在跑时必须连 127.0.0.1:13333，脚本原来硬编码直连板子）。
- **P5 换 bill2 当陌生人**：musk2 是同人跨照片，在 0.99 会话模式里它与 musk1
  同分（实测 0.997），"全窗 < LO" 前提不成立；bill2 各模式单帧 0.01~0.31 恒 < 0.85。
- **⚠️ esp-dl 模式今晚进一步恶化（重要观察）**：从"每 boot 稳定"恶化到
  "boot 内逐帧双峰乱跳"（同图连续帧 0.351/0.772/0.283；一个 boot 里 musk1
  自匹配 0.11~0.16——史上最差）。一次 boot 一个模式如抽签（重启后恢复 0.70
  模式）。回归的 P4 自适应阈值 + P4.5 的 75 分位校准 + 两轮重试就是为此设计。
  长期正解仍是 esp-dl 张量 arena 隔离（F11 追溯）。
- **待办遗留**：真人 A 相对比（快速通道对命中率的实际提升）——需要注册者本人
  在镜头前重新 `--enroll me`（回归清库）+ `--phase a --a 30`，对比基线
  （19:54 那轮 30s 仅 1 次 match）。

### 11. 快速通道真人 A 相对比（2026-10-06 晚第 3 段；⚠️ 注意 _thr_calib_A.json 已被本轮覆盖）

- **流程**：重注册 me×3（3.7s 采满）→ Phase A 30s（fast=0.80 生效）。
- **本轮数据**：71 帧检出 12 帧（连续性 17%，比基线轮还低——站位/光线波动）；
  win-max median 0.649 / **max 0.677**；占用分布 {1:3, 2:2, 3:2, 4:2, 5:3}。
- **离线复算**：现行规则命中 **5/12（42%）**——对比基线轮 2/19（11%）显著提升，
  **但这不是快速通道的功劳**：本轮 max 0.677 < 0.80，快速通道一帧都没触发；
  提升来自本轮检出的**成簇性**更好（占用 ≥4 的帧 5 个 vs 基线 2 个）。
- **📌 关键洞察：F2 票数规则吃的是"成簇性"而非总量**——检出只需在时间上聚成
  ≥4 帧的簇就能过票数；稀疏散点检出再多也没用。这解释了"检出 26% 但命中 11%"
  与"检出 17% 却命中 42%"的矛盾。
- **fast 阈值对这张脸的实测**：用户本人 win-max 簇在 0.64~0.68（本轮）~0.88（基线轮），
  跨模式波动大。0.80 只在高档模式触发；**fast=0.65 可把本轮提到 8/12（67%）**，但
  对真人陌生人上沿 0.587 余量只剩 +0.06，**不推荐**；0.70（+0.11）勉强。维持 0.80，
  真正的杠杆仍是检出连续性/成簇性（补光、眼平、看镜头）。
- **登记一个运维坑**：thr_calib 的 `--phase a` 会**覆盖** `_thr_calib_A.json`——基线样本
  要存档请先改名（今晚 19:54 基线文件已被覆盖，关键数字在本文档第 8/11 节）。

### 12. 统一 A 相测试：曝光上限假设拿到正面证据（2026-10-06 晚第 4 段）

- **条件**：镜头重新对准人（脸宽 323px），相机 fps 50 档（白天阶梯降到 ~13-16fps），
  **exp 994 = 20ms 上限**（锐利曝光制度），me×3 重注册（3/3 一次通过）。
- **Phase A（30s，219 帧）**：检出 45 帧（连续性 21%）；win-max **median 0.753 / p95 0.849 /
  max 0.891**；事件 match 3（F6 去重压制了重复报名）。
- **三规则离线复算（同一份 trace）**：
  | 规则 | 命中 |
  |---|---|
  | 0.61 + 票数（基线规则） | 9/45 = **20%** |
  | + fast 0.80（本轮实际生效） | 16/45 = **36%** |
  | + fast 0.70 | 32/45 = 71%（对陌生人 0.587 余量仅 +0.11，不建议） |
- **⭐ 曝光上限假设获证**：短曝光（20ms）轮的 sim 簇显著高于长曝光轮（60~100ms）——
  median 0.753 vs 0.638/0.649，max 0.891 vs 0.677/0.876。**锐利的噪声图优于模糊的干净图**：
  特征质量提高 → 更多帧过 0.80 → 快速通道从"无事可做"变成"+16 个百分点"。
  👉 **产品化改动候选：AE 增加曝光上限（如 ≤20~40ms），暗光下宁可提增益保锐度**（新立项）。
- **仍是短板**：连续性 21%、最长连续检出段仅 6 帧、占用 1~2 的判定帧占 30/45——
  成簇性依旧不足，票数规则仍在吃分。根本杠杆依旧是补光（硬件）与检出模型。
- 运维备注：曝光上限 A/B 实验首次尝试作废（镜头对着显示器拍了 206 帧空白屏）；
  恢复相机自动时 **fps 命令必须在 night/ae 之前发**（顺序反了会把它们再次关掉，踩第二次了）。

### 13. AE 曝光上限落地（2026-10-06 晚第 5 段，真机验证 ✅）

- **语义**：AE 的曝光天花板 = min(fps 档位上限, 上限值)，超过上限转提增益保锐度。
  默认 **40ms**（Kconfig `APP_CAM_AE_EXP_MAX_MS`=40，0=解除）；运行时
  `{"cmd":"cam","ae_exp_max_ms":N}`（ms，0..1000）；cam ack 新增 `ae_exp_max=<ms>`。
  **夜视档（tier>=1）自动解除**（faceai 传 0）——夜视要亮度且人脸已门控关闭；
  白天曝光阶梯在积分触到上限后自然停步（50->20 仍发生，20->10 不再发生：
  上限之下降帧率换不来亮度）。
- **实现**：`app_camauto_tick` 加第 5 参 `exp_cap_lines`（1ms=50 行），在 tick 里钳
  `s.exp_max`——AE 的加曝光/量化/限幅全走这个值，一处钳住全部路径；手动 exp 与
  夜视 boost 走信箱直写、不经引擎，天然不受影响。
- **真机验证**：`ae_target 250` 强顶 → 白天阶梯 50->20fps 后 **exp 精确停在 2000 行
  （40ms）**，AE 转提增益 32->192，luma 233（人为目标的预期过曝）；恢复 target 120
  正常。cam ack 全程带 `ae_exp_max=40ms`。
- **遗留**：`ae_accept_test.py`/`night_accept_test.py` 未在本次改动后重跑（前者需
  特定光照脚本环境）——其中 ae 收敛类断言若依赖"exp 顶到 fps 最大值"需按上限
  调整；夜视 INV 的 exp_max 期望不受影响（夜视档上限解除）。

### 14. esp-dl 张量残留清零实验（2026-10-06 深夜，假设被细化否定 ✅重要的排除性结论）

- **目的**：验证"特征非确定性 = 模型读了上一次运行残留的 arena 工作区内存"（此前 HANDOVER
  多处记载的"启动抽签/执行历史"假设），若证实则清零即修复、阈值可移植、ESPDet 可启用。
- **实验基建（保留，默认关）**：
  1. **esp-dl patch**（本地检出 `esp-dl/esp-dl/dl/model/`，升级需重打）：`Model::zero_workspace()`
     （调 `ModelContext::root_reset()`——库内**现成但全库零调用**的全 arena memset(0)，
     覆盖全部中间张量；权重不在 arena）+ `Model::poison_workspace()`（esp_fill_random
     填充，含 C2M 回写）。第二轮起 memset/fill 后统一 `esp_cache_msync(C2M|UNALIGNED)`
     回写（排除"脏行滞留 L1、算子直读内存"的缓存不一致变体）。
  2. **app_faceai**：`feat_ws` 三态（0=关 1=清零 2=毒化，运行时 `{"cmd":"mode","feat_ws":N}`，
     ack 带 `ws=N`）；`feat_run()` 统一入口替换全部 4 个 `s_feat->run` 调用点；
     `detect_ws_clear()` 在 ws=1 时同时清 MSR+MNP 两个检测子模型（关键点漂移与特征同源）。
- **实验数据（PC 送图 musk1，同图确定性输入；直连板子）**：
  | 配置 | sim_raw spread | 备注 |
  |---|---|---|
  | ws=0 基线 | **0.89**（0.106~0.998） | 混沌全貌 |
  | ws=1 只清特征 | 0.37（0.578~0.951，簇状） | 改善但不确定 → 检测 kp 漂移嫌疑 |
  | ws=1 检测+特征全清 | 0.86（0.133~0.997） | **kp 稳定（12 帧 2 变体）但输出仍乱跳** |
  | ws=1 全清 + C2M 回写 | ~1.0（0→0.9985） | 缓存不一致假设也否定 |
  | ws=2 毒化 | 0.04（n=5）但 **~15s/帧病态慢路径** | 疑浮点 denormal/算子走入毒化数据 |
- **决定性分层证据（frm_crc/ai_crc/kp/box 四指纹）**：同图连发 10 帧，**解码输出、AI 输入、
  关键点、框全部唯一恒定**，工作区已清零 + 回写——**输出仍 0.29~0.997 乱跳**。
- **结论（假设链逐层否定）**：
  1. ❌ 特征 arena 残留 → 清零后仍混沌；
  2. ❌ 检测 arena 残留（kp 漂移）→ kp 已 crc 级稳定，输出仍混沌；
  3. ❌ 缓存不一致（CPU 脏行 vs 算子直读）→ C2M 回写后仍混沌；
  4. ✅ **非确定性发生在 esp-dl 算子执行内部**——与输入、关键点、arena 内容全部无关；
  5. ⚠️ 毒化 15s/帧的病态说明**确有算子会读 arena 里非自己写的数据**（走了毒化数据），
     但该读取并非输出混沌的充分解释。
- **下一步候选（按代价排）**：
  1. **esp-dl 逐层 dump 二分**：同输入跑两遍，逐层比对中间张量，找第一个分歧的算子
     （DIAG_DEEP 思路的深挖版；需要 get_intermediate——⚠️ minimize() 清了名字表，
     要么去掉 minimize 要么走 m_variables——需再补一个 patch 透传）；
  2. **向 Espressif 上报**（esp-dl issue，附本实验复现协议：同图+全 arena 清零+C2M 仍混沌）；
  3. **工程绕行（现状已够用）**：F2 窗口投票 + 快速通道 + 每次开机相对标定——本实验证明
     "跨 boot 固定阈值"在 esp-dl 修复前不可达，文档此前"每会话相对标定"的纪律继续有效。
- **基建保留价值**：feat_ws/zero_workspace/poison_workspace 是后续所有 esp-dl 深挖的
  仪表；默认 feat_ws=0（重启即回默认），对生产行为零影响。

### 15. 逐层 dump 二分：元凶不是某个算子，是执行级硬件竞态（2026-10-07 凌晨，实验收口 ✅）

- **新增基建（延续 14 节，默认关）**：
  1. esp-dl `Model::run_trace(trace)`：按执行计划逐算子 forward，每算子后对其**输出张量**
     （`m_outputs_index` → `m_model_context->m_variables`）的逻辑字节做 CRC32——比整 arena
     CRC 干净（滤掉 padding/无效区噪声）；末项 = 整 arena CRC。`Model::module_name(i)`。
  2. esp-dl `FeatImpl` trace 开关（`set_feat_trace/get_feat_trace`，Feat 接口默认空实现 +
     FeatWrapper 透传），run() 中以 run_trace 替代 run（预处理/后处理不变）。
  3. app_faceai：`{"cmd":"mode","feat_bisect":K}` 武装 K 轮（强制 ws=1），每个带脸帧跑
     run A（本帧正常结论）+ run B（同帧第二次 traced run），0x04 日志报 A/B 首分歧算子
     与 A/参照（第一轮 A）跨帧首分歧算子。
- **二分数据（musk1 ×8，103 个算子，ws=1 清零+C2M 制度）**：
  | 轮 | A/B（同帧双跑）首分歧 | vs 参照（跨帧）首分歧 | 本帧 sim |
  |---|---|---|---|
  | 1 | module#68 | （参照建立） | 0.9979 |
  | 2 | module#9 | module#9 | 0.9964 |
  | 3 | module#8 | module#68 | 0.4064 |
  | 4 | module#13 | module#13 | 0.2394 |
  | 5 | module#4 | module#4 | 0.9977 |
  | 6 | module#2 | module#0 | 0.9596 |
- **⭐ 决定性结论**：
  1. **同帧两次 run 就分歧**（A/B），排除一切"跨帧状态/执行历史"类解释；
  2. **首分歧算子逐轮随机**（#2/#4/#8/#9/#13/#68），排除"某个内核实现有 bug"——
     若是特定算子的软件错误，同一算子应稳定成为首分歧；
  3. 结合输入/kp/工作区全部 crc 级恒定（第 14 节），唯一自洽的解释是
     **内存读出层在执行期发生随机分歧**——P4 上 esp-dl 的 DSA/DMA/缓存交互竞态
     或 PSRAM 200MHz 时序边际，属**硬件/驱动层问题，app 与模型内存卫生均无法修复**；
  4. 毒化 15s/帧病态（14 节）与此一致：毒化数据被算子读入后触发异常慢路径。
- **对产品的最终语义**：
  - 单帧特征不可信、跨帧**投票**（F2）与**单帧高分快通道**（F2 快通道）是唯一正确的
    工程绕行——已实现且验证；
  - 阈值只能每次开机相对标定（A/B 同会话），"跨 boot 固定阈值"在 esp-dl/P4 修复前不可达；
  - **强烈建议向 Espressif 上报**（esp-dl issue）：附本节复现协议（同 JPEG 反复发、
    frm_crc/ai_crc/kp 恒定、arena 清零+C2M、run_trace 逐算子输出 CRC 随机分歧）与
    全部数据——这是完整的 minimal repro。
- **esp-dl patch 登记表（本地检出 esp-dl/esp-dl，升级必须重打）**：
  | 文件 | 内容 |
  |---|---|
  | `dl/model/include/dl_model_base.hpp` + `src/dl_model_base.cpp` | `Model::zero_workspace()` / `poison_workspace()`（root_reset + esp_fill_random + C2M 回写）/ `run_trace()`（逐算子输出 CRC）/ `module_name()`；include esp_cache.h、esp_random.h、esp_rom_crc.h |
  | `vision/recognition/dl_feat_base.hpp/cpp` | Feat/FeatWrapper/FeatImpl 加 `set_feat_trace/get_feat_trace`；FeatImpl::run 支持 traced run |
  | （调用方）`jc_p4_app/main/app_faceai.cpp` | `feat_ws`（0/1/2）与 `feat_bisect`（K 轮）运行时命令 + `feat_run()` 统一入口 + `detect_ws_clear()` + 双 traced run 分析 |
- **📤 上报材料已备（⏳ 挂起待办：用户定 2026-10-07 起以后再处理）**：`docs/esp-dl_issue_draft.md`
  （英文正文 + 中文提交自查清单）。提交入口 github.com/espressif/esp-dl/issues/new；
  可附材料 = esp-dl 三补丁文件（登记表见上）+ 两份完整 trace；**提交后把 issue 链接回填本节**。

### 16. F8 模拟器 UI 升级（2026-10-07 凌晨，实现完成 + 主界面截图验证 ✅；弹窗交互见第 17 节）

- **改动一（固件 app_faceai.cpp）**：结果帧 lib 数组旁新增**平行数组 `lib_feats`**（每个唯一
  名字的特征条数，send_result 与 list 回包都带）——名单显示"每人特征数"的数据源。向后兼容
  （旧 UI/脚本忽略新字段）。回归 25/25 确认无影响。
- **改动二（模拟器 ui_shared.c，全部纯 LVGL）**：
  1. **Register 弹窗升级**：shots 选择行（1/3/5 单选高亮，默认 3）；OK 后多帧注册弹窗转
     "注册中"态（收键盘/输入/OK，显示进度条 + "collected N/M"），enrolling 事件驱动进度
     （sscanf msg），registered/error 自动关闭；采集期间 Cancel 发 `{"cmd":"stop"}`（取消采集）；
  2. **结果行稳定态**：`stable==1&&match` → "OK 名字 sim"；tracking → ".. 名字"；
     stable reject → "?? stranger"；enrolling → "enrolling"（ASCII——LVGL 内置字体无 ✓）；
  3. **检测反馈（硬需求）**：结果行新增 `w323px`（faces>0 时第一框宽度）——"是否检到脸+脸宽"；
  4. **每框名字/相似度标签**：json_boxes 扩展解析 F7 的 boxes 第 5/6 位（name/sim），
     update_boxes 改为"有名字就画在该框上"（此前只画首框且用顶层 match）；
  5. **名单特征数**：json_lib 扩展解析 lib_feats → "lib: musk(1)"。
- **验证**：固件回归 25/25；WSL 模拟器构建+运行截图 `docs/img/ui_sim_f8_20261007.png`
  （名单 musk(1) ✓、DAY 徽标/滑块 auto/按钮同步正常）；检测反馈/稳定态/每框标签需人脸
  入画（镜头当时对着线缆）。**弹窗交互已由第 17 节补齐**（4 个缺陷修复 + 合成点击逐项验证，
  截图 `docs/img/ui_sim_f8b_*_20261007.png`）。
- **踩坑（修复过程）**：json_lib 的 lib_feats 偏移写死 `pf += 12`，实际模式串
  `"lib_feats":[` 是 **14** 字节——少加 2 导致 atoi(`:[`) = 0，名单显示 musk(0)。
  修复 = `pf += strlen(模式串)`（别手写偏移长度）。工具层教训：Bash heredoc 会折叠
  `\`——含反斜杠的批量代码改动要用 Write 落 .py 脚本再执行（本节两次失配都是它）。
- **模拟器连接纪律**：WSL 后台启动用 `DISPLAY=:0 setsid nohup ... &`（普通 nohup 后台
  会随 wsl -e 会话退出被杀）；`pkill -f '[b]in/main'` 与启动命令必须分两次调用
  （同命令行内出现 ./bin/main 字面量会自杀——第 3 次踩）。
- **截图工具**：`SIM_SHOT=<前缀> SIM_SHOT_MS=5000` 每 5s 存 BMP（WSLg 路径直接写 /mnt/d/），
  `tools/bmp2png.ps1` 转 PNG。

### 17. F8b 注册弹窗 UI 修复（2026-10-07，模拟器，合成点击逐项真实验证 ✅）

用户报 4 个 UI 问题，**全部根因读源码定位后修掉**（只改 PC 模拟器侧：`ui_shared.c`
+ `main.c` 一行；板端固件零改动、无需重烧——已核实 `jc_p4_app/main` 不引用 ui_shared）：

| # | 现象 | 根因（源码） | 修法 |
|---|---|---|---|
| 1 | 下方键盘点不动 | `lv_msgbox_create(NULL)` 会在 `lv_layer_top()` 上自动建 **100%x100% 背板**（lv_msgbox.c:117-121），而 `lv_obj` 默认带 `LV_OBJ_FLAG_CLICKABLE`（lv_obj.c:167）→ 全屏可点击遮罩在最上层吞掉一切；键盘却建在**屏幕层**（旧 ui_shared.c:514） | 键盘改 `lv_keyboard_create(lv_layer_top())` + `lv_obj_move_foreground`（同层、创建在后 → 在遮罩之上） |
| 2 | 空输入点 OK 也"成功" | `input_ok_cb` 只判 `name[0]`，无论空否都落到 `close_input()` → 静默关窗 | OK **初始置灰** + 输入框 `LV_EVENT_VALUE_CHANGED` 实时开关（空/纯空白=灰），`do_input_ok()` 里再挡一道空判据 |
| 3 | "1/3/5" 含义不明 | 裸数字按钮（注册采集帧数） | 加英文标题 **`Shots:`** + 1/3/5（行宽 100%） |
| 4 | 关弹窗后主 UI 被透明层盖住、点不动 | `close_input()` 用 `lv_obj_delete(s_ui.mbox)`，而背板是 mbox 的**父对象**；只有 `lv_msgbox_close()` 会连父一起删（lv_msgbox.c:284-288）→ 50% 灰全屏遮罩**永久留在 layer_top** | 改 `lv_msgbox_close()`，并把 6 个弹窗控件指针全部置 NULL（原来关窗后全是悬空指针） |

- **顺带**：①键盘右下 **✓（LV_EVENT_READY）接到与 OK 同一条路径**（`do_input_ok` 抽取；
  否则 target 是键盘时 `lv_obj_get_user_data` 取到 NULL cmd 会崩），顺带让只写不读的
  `s_ui.reg_cmd` 派上用场；②模拟器 `main.c` 鼠标光标从 `lv_screen_active()` 改到
  **`lv_layer_sys()`**（否则弹窗期间指针被遮罩压暗）；③三处 CJK 日志/提示串改 ASCII
  （LVGL 内置字体无中文字形，实测渲染成空框）：`register: face the camera` /
  `src -> pc (PC frames)` / `waiting for image...\n(press Preview ON + Camera)`。
- **验证方法（可复用）**：模拟器自带 `SIM_SHOT`（app 内部 SDL_RenderReadPixels 存 BMP，
  480x800 全幅、不受遮挡/缩放影响）+ **WSL 侧 XTest 合成点击**（`libXtst.so.6` 已在，
  ctypes 即可：XQueryTree 找名为 "LVGL Simulator" 的窗口 → XTranslateCoordinates 取
  root 位置 → XTestFakeMotionEvent/ButtonEvent。**X 坐标 == LVGL 坐标（480x800 1:1）**，
  比走 Windows API 干净得多——Windows 侧受 RDP DPI 缩放、`SetForegroundWindow` 被拒、
  窗口有一部分在屏幕外等一堆干扰）。
- **逐项实测证据**（截图存 `docs/img/ui_sim_f8b_*_20261007.png`）：
  | 步骤 | 结果 |
  |---|---|
  | 点 Register | 弹窗 + 键盘**全亮在遮罩之上**、`Shots: 1 3 5`（3 高亮）、**OK 置灰** |
  | 点键盘 `a` | 字符进输入框，**OK 变实心可点**（键盘点击确实生效） |
  | 点 ⌫ | 清空后 **OK 重新置灰**（VALUE_CHANGED 双向都对） |
  | 选 1 shot + OK | 弹窗关闭、**主 UI 无残留遮罩**、日志 `register: face the camera` |
  | 再点 Register | **弹窗重新打开**（证明关窗后主 UI 可点，问题 4 直证） |
  | 默认 3 + OK | 转"注册中"态（收键盘/输入/OK，进度条 + `collected 0/3`） |
  | 注册中点 Cancel | 弹窗关闭 + 无遮罩 + 日志 `register: cancelled`（发 stop） |
  | 键盘 ✓ 键 | 与 OK 同路径（日志出现、转注册中态） |
- **遗留（按用户选择未做）**：`char js[160]` 遇超长名字（>~120 字符）会被 `snprintf`
  截断 → 发出残缺 JSON；要堵只需 `lv_textarea_set_max_length(ta, UI_NAME_MAX-1)` 一行。
  另：`s_ui.btn_cancel` 仍是只写字段（注册中不隐藏 Cancel，属既有设计）。
- 回归：模拟器 `cmake --build` 通过（仅剩两处**既有**的 `-Wformat-truncation` 告警，
  在 `ui_shared_set_result` 里，与本次无关）。

## 廿三、F9 容量提升：名表迁移 /storage/names.json（2026-10-11，真机验证 ✅）

规划见 `docs/face_recog_day_plan.md` P2-F9。动机：人数上限被自己的名表编码卡死
（`app_faceai.cpp` 曾硬编码 `MAX_PERSONS=10` / `MAX_FEATS=32`，名字存 NVS 单值字符串
≤2048B）。本轮把名表主存储迁到 `/storage/names.json`（NVS 仅只读兜底 + 首次开机自动迁移）。

### 1. 改动
| 文件 | 改动 |
|---|---|
| `main/app_faceai.cpp` | 名表持久化重写：`names_load_json`/`names_save_json`（cJSON 数组 `[{id,name}]`，tmp+rename 原子写 + 直写兜底）、`names_load_nvs`/`names_save_nvs`（旧格式兜底）、`names_load()`（names.json 优先，缺失则从 NVS 迁移并落盘）；上限改走 `CONFIG_APP_FACEAI_MAX_PERSONS`(64)/`MAX_FEATS`(320)；**名表加载从 `app_faceai_init()` 移到 `faceai_task` 的 storage 挂载之后** |
| `main/Kconfig.projbuild` | 新增 `APP_FACEAI_MAX_PERSONS`(64, 1..256) / `APP_FACEAI_MAX_FEATS`(320, 1..1024) |
| `sdkconfig` / `sdkconfig.defaults` | 新符号 + **`CONFIG_FATFS_LFN_HEAP=y`** |
| `docs/face_recog_day_plan.md` | F9 标记完成 |

### 2. ⚠️ 踩坑（关键）：FATFS 默认不支持长文件名 → names.json 写不进去
- 首次实现后真机启动日志：`names.json missing -> migrated 12 name(s) from NVS` 紧跟
  `names.json write failed (fallback NVS only)`——**names.json 从未写入成功**，全靠 NVS
  兜底才没丢名字（兜底机制救了场）。
- 根因：`sdkconfig` 里 `CONFIG_FATFS_LFN_NONE=y`（/storage 的 FATFS 不启用长文件名），
  而 **"names.json" 扩展名 `json` 是 4 字符**（非 8.3 合法名）→ `fopen`/`rename` 用该名字
  全部失败（`names.tmp` 是 8.3 合法，故临时文件能建、rename 到目标失败）。
- 修复：`CONFIG_FATFS_LFN_HEAP=y`（堆版 LFN）；另在 `names_save_json` 加"tmp+rename 失败
  则直写目标文件"的兜底。
- 教训：**在 FAT 分区上写文件前先确认 FATFS 的 LFN 配置**（`esp_vfs_fat` 默认关闭 LFN）；
  文件名 >8 或扩展名 >3 都会静默失败。新增"文件存储"类功能必须跑一次真机写入验证。

### 3. 名表加载时机（易埋的坑）
原 `names_load()` 在 `app_faceai_init()` 里调用，早于 `faceai_task` 内
`esp_vfs_fat_spiflash_mount_rw_wl` → 挂载前读 names.json 必然失败，"空名表"会覆盖真实名表。
已移到 mount 之后（`faceai_task` 内）调用。

### 4. 设计
- **主存储** `/storage/names.json`：cJSON 数组 `[{"id":1,"name":"musk"},...]`，字段名稳定，
  供 F10 备份导出复用；id = esp-dl DataBase 自增 id，同名多条特征即多行（与旧 `s_names` 语义一致）。
- **写入**：写 `names.tmp` → `rename` 原子替换（FATFS rename 不覆盖目标，故失败时先 `remove`
  再改名；仍失败则直写目标）。库变更（register/delete/clear/迁移）时才写，**不在每帧热路径**。
- **NVS 兜底**：`faceai/names` 旧格式 `"id:name;..."` 尽力同步（拼接 >4000B 则告警跳过，不阻断主路径）。
- **上限**：`MAX_PERSONS=64`（唯一名字数）/ `MAX_FEATS=320`（特征行数）；register 达人数上限回
  `lib full`。协议语义不变（`lib`/`lib_feats`/`list`/`lib_n` 全部照旧）。

### 5. 验证（真机，2026-10-11）
| 项 | 结果 |
|---|---|
| 编译/烧录 | ✅ bin 6.0MB；回归 `_face_r1_test.py` **25/25** |
| 容量 | ✅ 连续注册 12 个不同名字全部成功（旧上限 10 已突破），`list` 列出 12 人、`lib_feats` 全 1、无 `lib full` |
| names.json 作为权威来源 | ✅ 复位启动日志 `names loaded from /storage/names.json: 12`（不再走 NVS） |
| 自动迁移 | ✅ `esptool erase_region 0x810000 0x7F0000`（只擦 storage、保留 NVS）后启动：`Formatting FATFS partition` → `names.json missing -> migrated 12 name(s) from NVS`，且**不再报写入失败**；再复位 → `names loaded from /storage/names.json: 12` |
| 收尾 | 库已恢复为测试前的 `['musk']`（1 特征） |

### 6. 备注
- 复跑回归时遇到一次 **24/25**（P4.5 fast-confirm 失败）：本 boot 的 esp-dl 会话模式里
  `musk2 sim≈0.19`（远低于 fast 阈值 0.72），复位重摇后 25/25——**已知的 esp-dl 会话模式
  抖动，非 F9 引入**（见第廿一节 / 第廿二节 14、15 节）。
- ⚠️ 新增的 `CONFIG_FATFS_LFN_HEAP=y` 是本工程首次开启 LFN，占少量 flash/堆；若在意可改用
  8.3 安全文件名（如 `/storage/names.js`）替代。
- **F10 备份**已按"0x06 块传输 + 二进制信封"落地（见第廿四节）：规划文档里写的
  "0x02 分片回 base64 JSON"被弃用（会与流式结果帧混流、且 base64 膨胀 33%）。

## 廿四、F10 特征库备份 / 恢复（2026-10-11，真机验证 ✅）

规划见 `docs/face_recog_day_plan.md` P2-F10。用户确认的语义：**覆盖式恢复**、**原样打包**
（face.db 原始字节 + 名表，id/名字映射逐位一致，可跨设备迁移）、**导入前自动 .bak**。

### 1. 改动
| 文件 | 改动 |
|---|---|
| `main/app_usb_link.h/c` | 新增帧类型 **0x06（双向块数据）**：`JCL_TYPE_BLOB` + `JCL_BLOB_CHUNK_DEF`(16KB)；`parser_feed` 的 `ST_CRC1` 增加 `0x06 → s_lk.blob_cb(payload,len)` 分支 + `app_usb_link_set_blob_cb()`。发送端复用 `app_usb_link_send(JCL_TYPE_BLOB,...)` |
| `main/app_faceai.cpp` | `db_export` / `db_import` / `db_import_end` / `db_import_abort`；抽 `read_file_all` / `names_to_json` / `names_parse_buf` 复用（名表 JSON 生成/解析单一来源）；`send_result` 增加一次性 `dbx` 附加字段挂点（`s_extra_obj`） |
| `pc_host/protocol.py` | `T_BLOCK=0x06` + `TYPE_NAME` + `send_block(offset,data)` / `recv_block()` |
| `pc_host/jc_test.py` | 子命令 `db_backup [out.bin]` / `db_restore <file.bin>`（信封解析、CRC 校验、成员打印） |
| `pc_host/_f10_test.py` | **新增**验收脚本（正向 + 负向 CRC + 恢复后识别） |
| `docs/usb_link_protocol.md` | 类型表加 0x06 + 独立"0x06 块数据帧"章节（格式/命令/背压说明） |

### 2. 协议与格式（关键契约）
```
0x06 payload : offset(u32 LE) | data       （单片 ≤ JCL_MAX_PAYLOAD，**不允许空载荷**）
备份信封     : magic "JCDF"(4) | ver u32=1 | face_len u32 | names_len u32 | names(JSON) | face.db
```
- **导出**：`{"cmd":"db_export"}` → ack（0x02 带结构化 `dbx:{size,crc,parts,chunk}`）→ 板端连续
  发 `parts` 个 0x06 分片 → PC 按 offset 拼接 + `crc32`(zlib) 校验 → 落盘 `.bin`。
- **导入**：`{"cmd":"db_import","size":N,"crc":C}` → ack（**先自动备份** `/storage/face_bak.db`
  + `/storage/names.bak`）→ PC 连发分片（不逐片 ack）→ `{"cmd":"db_import_end"}` → 板端校验
  长度/CRC32/信封头/名表 JSON → 全通过才替换 `face.db`（写 `face_new.db` → remove 旧 → rename）
  → 写名表 → **重建 DataBase** → 复位 F2/F4/F6/F7 缓存 → 回 `lib/lib_feats`。
  任一校验失败：`event:"error"` 且**库保持原样**。`db_import_abort` 主动放弃。

### 3. ⭐ 设计要点（改动前必读）
1. **不用 0x02/0x03 分片**：0x02 是每帧都在流的结果通道（会互相污染）；0x03 受
   `CMD_JSON_MAX=256` 限制（传 656KB 需数千次往返）；base64 还会膨胀 33%。0x06 每片 16KB。
2. **回调零共享状态**：0x06 回调**只做"按偏移顺序追加到接收文件"**——"接收文件是否存在"
   就是会话标记，数据全部经 VFS 落盘。由此**完全绕开廿一节的跨核 L1 缓存坑**（不需要
   C2M/M2C、不需要钉核），也不需要 ~700KB PSRAM 缓冲；所有校验在收尾时由 faceai 任务
   **读回文件**完成（fail-closed）。
3. **文件名 8.3 安全**（`face_new.db`/`face_bak.db`/`names.bak`/`dbx_recv.bin`）：延续 F9 教训，
   不引入新的 LFN 依赖。
4. **重建特征库而非重启**：`delete s_db; new DataBase(FACE_DB_PATH, feat_len)`（构造时从
   storage 载入）——用户无需重启即生效。
5. **兼容性逐点核实过**（改动前用 subagent 审计）：板端未知 type 静默丢弃、`protocol.py`
   `recv()` 对未知 type 忽略、WSL 模拟器 `net_link.c` dispatch 是 `default: break`、
   `board_hub.py` 上行/下行**逐字节透明转发** ⇒ 新增 0x06 不打断任何既有链路。

### 4. 真机验证（2026-10-11，COM4 烧录 + 直连 192.168.33.2:3333）
| 项 | 结果 |
|---|---|
| 编译/烧录 | ✅ bin 6.0MB，无新增警告 |
| `_f10_test.py` | ✅ **12/12 PASS**：注册 f10a/f10b → backup（4169B / 1 part / CRC 一致）→ clear → restore → **lib 与基线逐项一致** → 恢复后送 musk1 **match=f10a** → 错误 CRC 被拒且库不变 → `.bak` 日志可见 |
| 回归 | ✅ `_face_r1_test.py` **25/25**（0x06 未影响 0x01~0x05） |
| 收尾 | 库恢复为 `['musk']`（1 特征） |

### 5. 实测数据
- 2 人 / 2 特征：`face.db` 4106B、信封 4169B、**1 片**（16KB 档）；导出→校验→落盘亚秒级。
- 满配外推：64 人 × 5 特征 = 320 条 × 2050B ≈ **656KB → 41 片**，TCP 下秒级。
- 上限：`size` > 2MB 直接拒绝（`DBX_MAX_BYTES`）。

### 6. 使用与注意
```powershell
python pc_host/jc_test.py --tcp 192.168.33.2:3333 db_backup face_db_backup.bin
python pc_host/jc_test.py --tcp 127.0.0.1:13333 db_restore face_db_backup.bin   # hub 在跑时
python pc_host/_f10_test.py --tcp 192.168.33.2:3333                             # 12 项验收
```
- ⚠️ 导入中途别断开：半截会话会在下一次 `db_import` 时被清掉（`remove(DBX_RECV_PATH)`），库不受影响。
- ⚠️ 经取景中枢跑大库时，hub 下行队列 64 满且非图像帧会**断开本地客户端**（既有背压策略）——
  大库建议先 `stream off` 并直连板子。
- `.bak` 是覆盖式恢复的**回滚副本**：误导入后可人工把 `face_bak.db`/`names.bak` 改名回去，
  或再用 `db_restore` 导回旧备份。
