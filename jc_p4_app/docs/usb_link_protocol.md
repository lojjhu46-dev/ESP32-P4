# JCLINK v1 —— PC <-> ESP32-P4 帧协议

> **传输层可二选一，帧格式完全相同**：
> | 传输 | 物理层 | 端 | 说明 |
> |---|---|---|---|
> | USB CDC | P4 USB0 OTG-HS（480Mbps），esp_tinyusb | PC 用 pyserial 打开 CDC 串口 | 板子插「High speed USB」Type-C；没插就成了"发送失败: ERROR" |
> | **TCP** | RMII IP101 以太网 100Mbps，板子做 **server** | PC 用 socket 连 `192.168.33.2:3333` | 直连网线即可；有 TCP 客户端时回包走 TCP，断开自动回落 USB CDC |
>
> 板端静态 IP 由 `CONFIG_APP_ETH_STATIC_IP` 配置（DHCP 4s 无租约后回落，默认 192.168.33.2/23，
> 与直连 PC 的 192.168.33.53/23 同网段）；PC 端测试脚本见 `pc_host/jc_test.py`。

## 帧格式（小端）
```
offset  size  field
0       1     magic0 = 0x4A (J)
1       1     magic1 = 0x43 (C)
2       1     type
3       1     seq    (0-255 循环)
4       4     len    (uint32 LE, payload 字节数, 上限 512KB)
8       N     payload
8+N     2     crc16  (CRC-16/CCITT-FALSE, 初值 0xFFFF, 计算 type+seq+len+payload)
```
- CDC 为透明字节流，无需转义；magic + CRC 双重防误码
- 板端 RX：PSRAM 环形缓冲 64KB + 状态机找帧 + FreeRTOS 队列

## 帧类型
| type | 方向 | payload | 说明 |
|---|---|---|---|
| 0x01 | PC->板 | 标准 JPEG 字节流 | 摄像头帧，板端 esp_new_jpeg 硬解 |
| 0x02 | 板->PC | UTF-8 JSON | AI 结果 |
| 0x03 | PC->板 | UTF-8 JSON | 控制命令 |
| 0x04 | 板->PC | UTF-8 文本 | 日志透传（一行一条） |
| 0x05 | 板->PC | 8 字节头 + 图像数据 | **图像回传**（LUI/模拟器预览用，见下） |
| 0x06 | 双向 | 4 字节偏移(u32 LE) + 数据 | **块数据传输**（F10 特征库备份/恢复，见下） |

### 0x05 图像帧（板端 `app_stream.c` 发出，默认关闭）
```
off 0  fmt    u8    0=JPEG(默认)  1=RGB565(裸，预留)
off 1  flags  u8    预留
off 2  width  u16   图像宽（如 640）
off 4  height u16   图像高（如 480）
off 6  seq    u16   帧序号（UI 可据此丢旧帧）
off 8  data[]       图像数据
```
- 板端用 **P4 硬件 JPEG 编码器**（`esp_driver_jpeg` 的 jpeg_encode，RGB565 -> JPEG q75 4:2:0），
  640x480 约 **10~11KB/帧、编码 ~6ms**，回传限速（默认 10fps，可命令改）。
- 打开方式：`{"cmd":"stream","value":"on","fps":10,"quality":75}`（`value` 省略=只查状态）；
  关：`"off"`。ack 的 `msg` 里带 `sent/enc/KB/drop` 便于排查。
- ⚠️ 编码器输出缓冲**必须**用 `jpeg_alloc_encoder_mem()` 分配，否则
  `jpeg_encoder_process` 直接报 `bit stream is not aligned`。
- PC 端：`pc_host/protocol.py` 的 `Link.recv_image()` / `recv_any()`；
  命令行 `jc_test.py img out.jpg` / `jc_test.py preview 8 out.jpg`。
- LVGL 模拟器：`lv_port_pc_vscode_official/main/src/net_link.c` 收帧，
  `ui_shared.c` 用内置 tjpgd 解成 RGB888 直接显示（不依赖 LVGL 的 FS/MEMFS 解码）。

### 0x06 块数据帧（双向，F10 特征库备份 / 恢复）
```
off 0  offset u32 LE   本片数据在整个块里的偏移
off 4  N               分片数据（默认 16KB/片，板端 JCL_BLOB_CHUNK_DEF）
```
- 用途：把「名表 + face.db」整体备份到 PC / 从 PC 覆盖恢复。⚠️ **不允许空载荷**
  （`len==0` 三端都判非法帧），单片 ≤ `JCL_MAX_PAYLOAD`(1MB)。
- **导出**（板→PC）：`{"cmd":"db_export"}` → ack（0x02，带结构化 `dbx:{size,crc,parts,chunk}`）
  → 板端连续发 `parts` 个 0x06 分片。PC 按 offset 拼接、校验 `crc32`（与板端 esp_rom_crc32_le
  同算法，即标准 CRC-32/ISO-HDLC）后落盘。
- **导入**（PC→板，**覆盖式**）：`{"cmd":"db_import","size":N,"crc":C}` → ack（板端先把现有库
  备份到 `/storage/face_bak.db` + `/storage/names.bak`）→ PC 连续发 0x06 分片（**不逐片 ack**，
  可靠性靠 JCLINK 每帧 CRC16 + 整包 CRC32）→ `{"cmd":"db_import_end"}` → 板端校验
  长度 / CRC32 / 信封头 / 名表 JSON，**全部通过才**替换 `face.db` 并重建特征库；任一不符 →
  `event:"error"` 且**库保持原样**。`{"cmd":"db_import_abort"}` 主动放弃。
- **备份信封**（PC 侧 `.bin` = 所有分片数据按 offset 拼接后的内容）：
```
magic "JCDF"(4) | ver u32=1 | face_len u32 | names_len u32 | names(JSON) | face.db 原始字节
```
- PC 端：`python pc_host/jc_test.py --tcp <ip:port> db_backup out.bin` /
  `python pc_host/jc_test.py --tcp <ip:port> db_restore out.bin`；验收 `pc_host/_f10_test.py`。
- ⚠️ 0x06 是**新增**类型：旧解析器（WSL 模拟器 `net_link.c`、未升级的脚本）对未知 type
  是静默忽略，不影响既有 0x01~0x05 链路。

## 命令 (type 0x03)
```json
{"cmd":"start"}
{"cmd":"stop"}
{"cmd":"mode","detect":true,"match":true}
{"cmd":"register","id":"user1"}
{"cmd":"delete","id":"user1"}
{"cmd":"list"}
{"cmd":"link"}                          // 查询当前链路
{"cmd":"link","value":"usb"}            // 强制 USB / "tcp" / "auto"
```

## 链路选择与切换（2026-10-16，USB 视频流）
板端有两条**功能完全等价**的发送后端：内置 **USB CDC**（`"usb-cdc"`）与 **TCP**（`"tcp"`，
由 `app_netlink` 在客户端连上时注册）。上层（0x02 结果 / 0x04 日志 / 0x05 图像 / 0x06 分片）
不感知差异，全部经 `app_usb_link_send*()` 走"当前生效后端"。

| 强制态 | 语义 |
|---|---|
| `auto`（默认） | 与历史行为一致：**TCP 客户端连上则抢占**为 tcp，断开自动回落 usb-cdc |
| `usb` | 强制走 USB CDC；**新到的 TCP 客户端会被立即关闭**并打一行 0x04 说明；已连的 TCP 客户端会被主动断开 |
| `tcp` | 强制走 TCP（未连接时仍落到 usb-cdc，连上即生效） |

- 查询：`{"cmd":"link"}` → `event:"none"` + `msg:"link=auto active=usb-cdc tx_frames=.. tx_kb=.. tx_drop=.. last_ms=.."`
- 设置：`{"cmd":"link","value":"usb"|"tcp"|"auto"}` → 同上 msg（非法值回 `event:"error"`）
- **结果帧（0x02）恒带 `"link"` 字段**（`"usb-cdc"` / `"tcp"`），上位机用它显示链路徽标
- 发送统计：`tx_frames/tx_bytes/tx_drop/tx_last_ms`（在线路切换、算吞吐和排查丢帧时很有用）
- PC 端不需要"两条链路同时收"：切换 = 在目标接口重新连接（板子一次只服务一个 TCP 客户端）

### USB 通道实测（2026-10-16，ESP32-P4 USB0 OTG，协商 **HIGH 480Mbps**）
口径 = 板端 tx 计数 − 空闲基线（`pc_host/_usb_bench.py`）：

| 档位 | 实测带宽（中位） | 推算帧率 | 备注 |
|---|---|---|---|
| 0x05 JPEG | 217 KB/s @10fps | ~8.3 fps | 解码零失败 |
| 0x05 JPEG | 292 KB/s @20fps | ~11.2 fps | 与 30fps 同档上限 |
| 0x05 JPEG | 281 KB/s @30fps | ~10.8 fps | — |
| 0x05 raw RGB565 | **1390 KB/s**（±0.5%） | **~2.3 fps** | 帧完整（614400B/帧） |
| 0x01 PC 送图 / 0x02 / 0x03 / 0x04 / 0x06 | ✅ 全通 | — | 0x06 备份 8312B 导出 0.61s |
| 识别回归 `_face_r1_test.py --serial COM5` | ✅ **25/25** | — | USB 与网口功能对等 |

⚠️ **FIFO 扫描（勿再试）**：512（选定）> 1024 > 2048（放大反而更慢）；4096/8192 **开不了机**
（本板内部 RAM 余量 < 3.5KB：`assert failed: vApplicationGetIdleTaskMemory`）。
搬到 PSRAM 也不可行：P4 的 USB DMA 仅覆盖内部 DRAM（`SOC_DMA_LOW/HIGH=0x4ff00000..0x4ffc0000`），
且 esp_tinyusb 明确要求 USB DMA 缓冲 `DRAM_ATTR` + cache 行对齐。

### USB vs TCP 同档 A/B（2026-10-16，同场同工具，`_usb_bench.py`，每档 3 次取中位）
| 档位 | **USB**（COM5） | **TCP**（192.168.33.2:3333） | 差异 |
|---|---|---|---|
| 0x05 JPEG @10fps | **229 KB/s**（227~235） | **217 KB/s**（208~225） | ≈ 同档 |
| 0x05 JPEG @20fps | 306 KB/s（300~310） | **327 KB/s**（294~345） | TCP 略高（≤10%） |
| 0x05 JPEG @30fps | 286 KB/s（284~294） | **335 KB/s**（289~343） | TCP 略高（≤15%） |
| **0x05 raw @10fps** | **1389 KB/s**（1294~1394） | 1297 KB/s（1295~1299） | **USB 高 ~7%**（±0.2% 极稳） |

⇒ **两条链路吞吐同档（±10%）**：JPEG ≈9~13fps 等效、raw ≈2.2fps 等效，**功能完全对等**
（USB 侧已实测 0x01/0x02/0x03/0x04/0x05/0x06 全通 + 识别回归 25/25 + F10 备份恢复；TCP 侧同样全套）。

### 强制切换的实测行为（重要）
- 在 **TCP 客户端上**发 `{"cmd":"link","value":"usb"}`：状态**立即生效**，但那条命令的 ack 已经
  改走 USB 了 —— TCP 客户端会**收不到应答并被断开**（日志 `link forced to usb -> dropping client`）。
  ⇒ 从 TCP 侧切到 USB 时，请以 **USB 侧再查一次 `{"cmd":"link"}`** 为准（或直接重连）。
- 反向（强制 usb 时新 TCP 连接）会被拒绝并在 0x04 里说明原因（`netlink: client ... rejected`）。
- 断开 TCP 客户端后 `link=auto` 会自动回落到 `usb-cdc`（实测：TCP 块跑完，收尾 USB `list` 显示 `link: usb-cdc`）。

⚠️ **测量三坑**：① CDC 背压——测吞吐时主机**必须持续读**，否则测到的是丢弃率；
② 别用 PC 侧计数（突发交付，抖动 ±29%，还会出现 45fps 假读数）；
③ 别在流中发查询（0x02 洪泛会让 ack 匹配到错帧）。

✅ **结论/路线**：JPEG 连续预览可用（≈8-11fps）；**raw 连续推流 ~1.4MB/s（≈2.3fps），
6MB/s 不可达** → raw 改**按需单帧抓取**（0.5s/帧；UI 按钮 或
`qt_host.exe --selftest --serial COM5 --grab-raw out.png`）。

## 结果 (type 0x02)
```json
{"faces":1,"boxes":[[120,80,260,240]],"match":"user1","sim":0.87,
 "fps":12.5,"infer_ms":86,"dropped":0,"lib":["user1"],"event":"match"}
```
- event: none/match/reject/registered/deleted/list/night/day
- faces=0 时 boxes 为空数组，其余字段照发（心跳作用）
- `event:"night"` = 低光照夜视模式（夜视阶梯 tier>=1 且 `face_night`=1）：板端此档**不做人脸**，
  AI 不取帧不推理（帧池让给 0x05 预览），以 ~2Hz 低频发状态帧（`msg:"face off, preview only (tier N)"`）；
  光照回 tier0 自动恢复识别（恢复正常结果帧，无专门的 day 心跳）

## 流控
- 滑动窗口=1：PC 发一帧 JPEG 后等板端结果帧（0x02）再发下一帧；超时 500ms 重发同一帧
- 板端 RX 队列满时丢最旧帧，dropped 计数随结果帧上报
- 注册模式：PC 发 register 命令后，板端对下一张含人脸的帧提取特征入库并回 event=registered
- 首帧握手：PC 打开串口后先发 {"cmd":"start"}，板端回复含 "lib" 字段的结果帧即链路就绪

## 命令应答细节（板端 app_faceai 实现）
| 命令 | 板端应答 |
|---|---|
| `src` | `{"cmd":"src","value":"cam"\|"pc"}` 切图像源：板载摄像头 / PC 送帧；立即回 `event:"none"` + `msg:"src=cam"`。结果帧里有 `"src"` 字段 |
| `cam` | 摄像头参数：`gain`/`gain_max`/`exp`/`exp_max`/`mirror`/`wb`/`sat`/`ae` + **`fps`**（帧率 1~50，走改 VTS：帧率越低曝光上限越高，夜视长曝光用）。带 `"auto_exp":true` 时设完帧率顺手把 exp/gain 拉到新上限（UI 的夜视档）。结果帧里带 `"cam_fps"` |
| `cam` +`dn` | **时域降噪档** `{"cmd":"cam","dn":0..3}`（0=关/1=低/2=中/3=高，仅摄像头路径生效）。结果帧带 `"dn"`，开启时另有 `"dn_ms"`（耗时）/`"dn_blk"`（混合块数）/`"dn_mv"`（运动块占比 %）/`"dn_mmax"`（块内最大差均值）/`"dn_sad"`（差均值口径，历史对比用）/`"dn_skip"`（被帧率守卫跳过帧数）。运动区域会被判为"不混合"以避免算法拖影；高帧率（>25fps）整体跳过以保证链路稳定 |
| `cam` +`auto_fps` | **运动自适应帧率** `{"cmd":"cam","auto_fps":true\|false}`：运动 50fps ↔ 静止档自动切换（运动持续 ~100ms 升档、静止 ~1.5s 回落、切换后 exp 自动拉到新上限）。⚠️ **静止档不固定是 15fps**：由夜视阶梯（见下）决定，暗场为 5fps。附带标定参数 `mv_thr`（块运动阈值 1..63，默认 30）与 `mv_ratio`（触发百分比，默认 5）。手动 fps 命令会自动退出自动模式；**`auto_fps:false` 会把帧率交还给夜视阶梯**（不再"保持当前帧率不动"，否则暗场会停在 50fps） |
| 结果帧新增 | 恒带 `"auto"`(0/1)、`"af_ms"`（最近一次自动切帧耗时 ms，正常 ~1-2ms）、`"mv"`（运动块占比 %：自动模式用独立运动估计、关闭时回退 dn_mv）、`"mv_peak"`（块差峰值 0..63，标定用）—— 这些**每帧实时刷新** |
| `cam` +`night` | **夜视阶梯**（暗度驱动的降帧率 + 自动时域降噪，默认常开）`{"cmd":"cam","night":true\|false}`。开=按实测亮度自动降档：暗（luma<45）→5fps、很暗（luma<25）→5fps 且 `dn` 抬到 3（下限默认 5fps，可调 `APP_CAM_NIGHT_FPS_MIN`），变亮自动回升；每档同时把 `dn` 抬到 2/3。结果帧恒带 `"night"`(0/1 总开关) 与 `"ntier"`(0=正常/1=暗/2=很暗)。**手动 `{"cmd":"cam","fps":N}` 会让阶梯让位**（`night` 变 0），`night:true` 恢复；手动 `{"cmd":"cam","dn":N}` 让自动 DN 让位，`night:true` 一并恢复。与 AUTO 的关系：运动接管期间阶梯让位（档位冻结），静止档帧率由阶梯供给 |
| `cam` +`face_night` | **光照双模式（低光照夜视 / 白天人脸识别）** `{"cmd":"cam","face_night":true\|false}`（默认 true，Kconfig `APP_FACEAI_NIGHT_OFF`）。true=夜视档（`ntier`>=1 且 src=cam）AI 不取帧不推理：帧池整个让给 0x05 预览，结果帧以 ~2Hz 发 `event:"night"`；光照回 tier0 自动恢复。false=夜视档也照常识别（调试用）。结果帧恒带 `"face_night"`(0/1)。仅对 src=cam 生效（PC 送帧永不门控，窗口=1 应答语义不受影响）；夜视档下 register 会武装成功但在 ack 里提示"等光照恢复后执行" |
| `stream` | `{"cmd":"stream","value":"on"\|"off","fps":10,"quality":75}` 图像回传开关（0x05）；立即回 ack，msg 带 `sent/enc/KB/drop` |
| `start` | 立即回 0x02：`event:"none"` + `lib:[...]` |
| `stop` | 立即回 0x02：`event:"none"`；之后收到帧仍回 `{"faces":0,"event":"stopped"}` |
| `mode` | 立即回 0x02：`event:"none"`，`msg` 里带 `detect=/match=/thr=` |
| `register` | **立即回 ack**（`msg:"register armed for <name>..."`）；之后第一帧含人脸就入库，那帧回 `event:"registered"` + `match:"<name>"` |
| `delete` | 立即回 0x02：`event:"deleted"`（`msg` 带删掉几条特征）+ `lib:[...]`；不存在回 `event:"error"` |
| `list` | 立即回 0x02：`event:"list"` + `lib:[...]` |
| `link` | 查询/强制发送链路：`{"cmd":"link"}` 或 `{"cmd":"link","value":"usb"\|"tcp"\|"auto"}` → `event:"none"` + `msg:"link=<态> active=<后端> tx_frames=.. tx_kb=.. tx_drop=.. last_ms=.."`（详见上节「链路选择与切换」） |
| `clear` | 立即回 0x02：`event:"deleted"`，`msg:"cleared"` |

- **host 的正确姿势**：`register` → 等 ack → 再发帧。否则命令与帧的到达顺序不确定会错配
  （板端另加了发布时刻门限 `fr.ts_us > 武装时刻`，双保险）
- 一个人可以注册多张特征（多次 register + 发帧），`lib` 只列唯一名字；`delete` 删掉该名字的全部特征
- 额外字段：`lib_n`(库中特征条数)、`msg`(说明文本)、`src`(当前图像源 cam|pc)。
  **命令回复带 `"ack":"<命令名>"`**：摄像头常驻推流时 0x02 里混着流式结果帧，
  host 用 ack 字段区分命令回复（`pc_host/protocol.py` 的 `cmd_and_wait()` 已按此实现）。
  `lib` 只在 start/list/register 之后/delete/clear 的第一条里出现
- `mode` 可带 `thr`（0.7 或 70 两种写法都接受），覆盖默认余弦阈值（Kconfig 默认 0.70）
- 停止/未启用检测时板端仍消费帧并回一条最简结果，避免窗口=1 卡死
- 模型加载期（上电后几秒）由图像管道内置 ack 兜底，回的是 `{"faces":0,"boxes":[],"event":"none","pipe":{...}}`

## 实测（2026-09-30，网线直连 PC，300x300 测试图）
```
clear                                   -> event=deleted lib=[]
register bill + bill1.jpg               -> event=registered match=bill lib=["bill"]
register bill + bill2.jpg               -> event=registered match=bill lib_n=2   (一人两条特征)
register musk + musk1.jpg               -> event=registered match=musk lib=["bill","musk"]
musk2.jpg                               -> event=match match=musk sim=0.773      (1:N 命中)
每帧: imgpipe 硬解 1ms + faceai infer 20ms，端到端 167~357ms(含 300x300 JPEG 传输)
```