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

## 命令 (type 0x03)
```json
{"cmd":"start"}
{"cmd":"stop"}
{"cmd":"mode","detect":true,"match":true}
{"cmd":"register","id":"user1"}
{"cmd":"delete","id":"user1"}
{"cmd":"list"}
```

## 结果 (type 0x02)
```json
{"faces":1,"boxes":[[120,80,260,240]],"match":"user1","sim":0.87,
 "fps":12.5,"infer_ms":86,"dropped":0,"lib":["user1"],"event":"match"}
```
- event: none/match/reject/registered/deleted/list
- faces=0 时 boxes 为空数组，其余字段照发（心跳作用）

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
| `cam` +`dn` | **时域降噪档** `{"cmd":"cam","dn":0..3}`（0=关/1=低/2=中/3=高，仅摄像头路径生效）。结果帧带 `"dn"`，开启时另有 `"dn_ms"`（耗时）/`"dn_blk"`（混合块数）/`"dn_sad"`（平均 SAD）/`"dn_skip"`（被帧率守卫跳过帧数）。高帧率（>25fps）自动跳过以保证链路稳定 |
| `stream` | `{"cmd":"stream","value":"on"\|"off","fps":10,"quality":75}` 图像回传开关（0x05）；立即回 ack，msg 带 `sent/enc/KB/drop` |
| `start` | 立即回 0x02：`event:"none"` + `lib:[...]` |
| `stop` | 立即回 0x02：`event:"none"`；之后收到帧仍回 `{"faces":0,"event":"stopped"}` |
| `mode` | 立即回 0x02：`event:"none"`，`msg` 里带 `detect=/match=/thr=` |
| `register` | **立即回 ack**（`msg:"register armed for <name>..."`）；之后第一帧含人脸就入库，那帧回 `event:"registered"` + `match:"<name>"` |
| `delete` | 立即回 0x02：`event:"deleted"`（`msg` 带删掉几条特征）+ `lib:[...]`；不存在回 `event:"error"` |
| `list` | 立即回 0x02：`event:"list"` + `lib:[...]` |
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