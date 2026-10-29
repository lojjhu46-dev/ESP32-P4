# qt_host —— JCLINK 上位机（C++/Qt6）

ESP32-P4 边缘人脸识别终端的 PC 上位机：**同时支持网口（TCP）与高速 USB（CDC 串口）**，
接收 0x05（JPEG/raw）或 **0x07（H.264）**视频流并显示，叠加 0x02 检测结果，内置全功能控制面板（流/名单/识别/相机/备份）。

> **新会话接手先读**：[`../docs/library/HANDOVER.md`](../docs/library/HANDOVER.md) 第廿五节
> （USB 视频流 + Qt 上位机的实测数据与坑）。

## 1. 依赖与环境（本机已就位）

| 项 | 路径 | 说明 |
|---|---|---|
| Qt | `D:\qt\qt\Qt\6.12.0\mingw_64` | **6.12.0 mingw_64**（Core/Gui/Widgets/Network） |
| FFmpeg | `D:\ffmpeg\bin\ffmpeg.exe` | 0x07 H.264 解码（子进程输出 raw RGB） |
| MinGW | `D:\qt\qt\Qt\Tools\mingw1310_64` | 13.1.0（qt_host 用它的 g++ 编译） |
| Ninja | `D:\qt\qt\Qt\Tools\Ninja` | 构建器 |
| CMake | `C:\Program Files\CMake\bin\cmake.exe` | ≥3.21 |

⚠️ **本机 Qt 未安装 `Qt6SerialPort` 模块** → 串口用 **Win32 API 自实现**
（`src/serial_transport.cpp`：`CreateFile` + `SetupComm(1MB)` + `SetCommTimeouts` + 独立读线程），
不依赖该模块、也不需要联网装组件。

## 1b. 0x07 H.264 实时流（2026-10-23 打通）

- 板端 `app_h264enc` 发 0x07：20B 头（`codec/flags/stream_id/pts_us/frame_seq/w/h/nal_len`）
  + Annex-B 访问单元；SPS/PPS 随每个 IDR 携带，所以中途加入也能起播。
- 解码用 **ffmpeg.exe 子进程**（`H264LivePlayer`）：NAL 写 stdin，
  `ffmpeg -f h264 -i pipe:0 -f rawvideo -pix_fmt rgb24 pipe:1` 输出裸 RGB，
  Qt 按 `w*h*3` 切帧成 `QImage` 后复用现有 `video_view`。
- ⚠️ 不要用 `QMediaPlayer` 直接喂活管道：它的 FFmpeg 后端会把非 seekable 源卡在
  Buffering，约 1 秒后报 `Demuxing failed -1` 并冻结（实测）。
- ffmpeg 路径：优先环境变量 `FFMPEG_EXE`，其次 `PATH`，最后 `D:/ffmpeg/bin/ffmpeg.exe`。
- 控制台诊断日志：`[h264] ffmpeg decoder started` / `[h264] ffmpeg exited`。
- ⚠️ 板端默认**开机自动单档推流**（M2：1Mbps/QP30-38/GOP25），Qt 连接后即可看到画面；
  顶栏的“开始/停止流”按钮已删除。
- 运行时命令（FACEAI 关闭时板端注册的最小 handler）：
  `{"cmd":"stream","value":"on|off","bitrate":N,"qp_min":N,"qp_max":N,"i_period":N}`，
  回 `{"ack":"stream","value":bool,"fmt":"h264","bitrate":...,"qp_min":...,"qp_max":...,"i_period":...}`；
  参数变化会在下一帧重建编码器（短暂停顿）。
- **UI**：「流」页 `H.264 编码` 一组控件（码率 25-2500 kbps / QP 上下限 / I 帧间隔）+
  `下发编码参数` 按钮。只发参数、不发 `value`，所以不会改变流的开关；
  `qp_min >= qp_max` 本地拦截（相等会关闭码控）。连接时的 `stream` 查询会把当前值回填。

## 2. 构建与运行

```powershell
powershell -ExecutionPolicy Bypass -File qt_host\build.ps1          # 增量构建
powershell -ExecutionPolicy Bypass -File qt_host\build.ps1 -Clean   # 全量重建
powershell -ExecutionPolicy Bypass -File qt_host\build.ps1 -Run     # 构建后直接启动
```
- 产物：`qt_host\build\qt_host.exe`（`build.ps1` 会自动跑 `windeployqt` 并把 MinGW 运行库拷到 exe 旁，
  之后可脱离 PATH 直接双击运行；**别加 `--no-compiler-runtime`**，否则启动即 `0xC0000135`）。
- 默认带控制台窗口（便于看 qDebug）；`-DQT_HOST_CONSOLE=OFF` 可切成纯 GUI。

## 3. 连接方式

| 接口 | 填什么 | 注意 |
|---|---|---|
| **TCP（网口）** | 板子 IP:端口，默认 `192.168.33.2:3333` | 板子做 server，**一次只服务一个客户端** |
| **USB（高速口）** | 下拉选 COM 口（自动优先 Espressif 设备、标注 CH340） | 必须插板子的 **High speed USB(OTG)** 座；CH340 那个口是烧录/日志口，打开会复位板子 |

- 程序启动时**禁用系统代理**（`QNetworkProxy::NoProxy`）——否则直连局域网会报
  `The proxy type is invalid for this operation`。
- 同一时刻只允许一个客户端连板子：用 USB 时请先关掉取景中枢与 WSL 模拟器。

## 4. 界面与操作

- **顶栏**：接口选择 / 地址或 COM 口 / 连接断开 / **链路徽标**（`usb-cdc` 或 `tcp`，来自结果帧 `link` 字段）/
  板端 link 强制态（auto·usb·tcp）/ 截图 / 缩放
- **左侧**：视频区（黑底等比 letterbox，叠加命中绿框+名字+相似度、未知黄框、关键点红点，
  右上角角标实时显示 `fps / KB-s / 丢帧 / 链路`）+ 板端日志区
- **右侧 Tab**：
  - **流**：fps(1-30)、quality(1-100)、格式 JPEG/raw、图像源 cam/pc、raw 字节序交换；
    H.264 编码参数（码率/QP 上下限/I 帧间隔，仅 YUV420 固件）
  - **名单**：表格（名字/特征数）、Register（1/3/5 帧）、Delete、Clear（二次确认）
  - **识别**：detect/match、thr_lo、thr_hi、fast_thr、dedup_ms、multi
  - **相机**：gain/exp 滑块、帧率档、镜像、AE、AWB、夜视、低光照停人脸、降噪档
  - **备份**：F10 特征库导出 `.bin` / 导入（覆盖式，板端自动 `.bak`）+ 进度条
  - **云**：签到事件与抓拍上报、人脸库上传/下发（详见 §4b）
- **快捷键**：`S` 截图 · `R` 重连
- 截图存到 **exe 同目录的 `shots/`**（PNG，含检测框叠加层，按原始分辨率出图）

连接成功后会自动发一轮状态查询（`mode`/`cam`/`stream`/`link`/`list`）把控件同步成板端真实值。

## 4b. 云签到「云」页（2026-10-24 上线，10-26 公网真人实测验收）

**边界**：人脸比对全在板端；Qt 只做转发与抓拍裁剪，云端只存签到事件/照片/人脸库快照，**不做特征比对**。

| 控件 | 行为 |
|---|---|
| 服务器 / Token / 设备名 | 默认 `http://<CLOUD_IP>:8080`、设备名取本机名；Token 是服务器 `install.sh` 生成的那串（本机副本在 `_dev/.tok`，**别写进提交**） |
| ☑ 启用云签到（识别到 match 时上报） | 关 = 完全不联网；配置存 QSettings `cloud/enabled`、`cloud/url`、`cloud/token`、`cloud/device` |
| ☑ 签到抓拍（按人脸框裁剪 JPEG 上传） | 从当前画面裁人脸框 → `POST /api/events` 的 `photo_b64`；`cloud/photo` |
| ☑ 注册/删除后自动同步人脸库 | 收到板端 `registered`/`deleted` 结果帧 → 导出 JCDF 快照 → `POST /api/facebank`；`cloud/autoSync` |
| 「保存配置并测试连接」 | `GET /api/health` 探活，顺带读回 `dedup_window_s` 用于本地静默 |
| 「上传板端人脸库到云端」/「从云端同步人脸库到板端」 | 手动上传；下发走 `GET /api/facebank/latest` → 板端 `db_import`（覆盖前板端自动 `.bak`） |
| 「打开网页后台」 | 浏览器打开服务端 Web 台（事件/名单/快照/CSV） |

**去重是两层**：板端 5s 边沿去重（只压事件帧，实时 UI 仍每帧显示人脸）+ **服务端按 `person+device` 滚动去重**
（生产 1800s = 半小时一次；命中重复时事件与抓拍照片都不写入）。Qt 从 `/api/health` 读到 `dedup_window_s` 后，
本地静默取 `min(服务器窗口, 30min)`。

**断网补发**：未送达的任务落 `cloud_pending.jsonl`
（`%APPDATA%\jc_p4_app\qt_host\`，测试页另用 `cloud_pending_pctest.jsonl`），恢复后按队列重放；
补发可能乱序到达，所以服务端窗口是**对称 ±window**。

**已知缺口**：① 自动同步只由单条 best-effort `registered`/`deleted` 结果帧触发，USB 拥塞时该帧可能静默丢失
→ 云端库落后且无告警（注册后确认日志里有 `☁ 名单已变化`，否则手动点上传）；② 「从云端同步到板端」只拉最新版，
不支持选历史版本回滚（服务端 `?id=N` 已支持）。详见
[`../docs/library/H264重构交接与阶段计划.md`](../docs/library/H264重构交接与阶段计划.md) §15 与
[`../docs/library/嵌入式软件测试与修复实践手册.md`](../docs/library/嵌入式软件测试与修复实践手册.md) F 章；
服务端侧见 [`../cloud_server/README.md`](../cloud_server/README.md)。

免光照验证路径：固件 `CONFIG_APP_FACEAI_PC_TEST=y` 测试构建 + Qt `QT_HOST_PC_TEST`「PC测试」页
（原图直接作 `photo_b64`、独立 spool、阻断生产上报）+ `install.sh --test` 的 `127.0.0.1:18080` 实例。

## 5. 无界面自检（脚本化验收）

```powershell
# TCP
qt_host\build\qt_host.exe --selftest --tcp 192.168.33.2:3333 --seconds 8 --images 60 --fps 10
# USB（COM 口按实际填）
qt_host\build\qt_host.exe --selftest --serial COM5 --seconds 8 --images 60 --fps 10 --fmt jpeg
# raw 帧 + 存一帧 PNG（验证 RGB565 字节序）
qt_host\build\qt_host.exe --selftest --serial COM5 --seconds 7 --images 3 --fps 5 --fmt raw --dump raw.png
```
参数：`--seconds` 时长 · `--images` 通过所需最少帧数 · `--fps/--quality/--fmt` 流参数 ·
`--swap` raw 字节序交换 · `--dump <png>` 把最后一帧存图。
退出码 `0`=PASS、`1`=FAIL、`2`=连不上。

## 6. USB 实测数据（2026-10-16，COM5 = VID_303A:4001，协商 **HIGH 480Mbps**）

口径 = 板端 tx 计数 − 空闲基线（工具：`pc_host/_usb_bench.py`）；带宽可靠、帧率由带宽÷帧长推算：

| 档位 | 实测带宽（中位） | 平均帧长 | 推算帧率 | 解码失败 |
|---|---|---|---|---|
| JPEG 10fps | **217 KB/s** | ~26 KB | ~8.3 fps | 0 |
| JPEG 20fps | **292 KB/s** | ~26 KB | ~11.2 fps | 0 |
| JPEG 30fps | **281 KB/s** | ~26 KB | ~10.8 fps | 0 |
| **raw 10fps** | **1390 KB/s**（±0.5%） | 600 KB | **~2.3 fps** | 0（帧完整） |

**CDC FIFO 扫描结论：放大无益**（512 最优；1024/2048 更慢；4096/8192 开不了机——内部 RAM 余量 <3.5KB）。
把 FIFO 搬到 PSRAM 也不可行（P4 的 USB DMA 只能访问内部 DRAM：`SOC_DMA_LOW/HIGH=0x4ff00000..0x4ffc0000`）。

**USB vs TCP A/B（同场同工具）**：JPEG@10 = 229/217 KB/s、JPEG@20 = 306/327、JPEG@30 = 286/335、
raw@10 = **1389/1297 KB/s** ⇒ **两条链路吞吐同档（±10%）**。

⚠️ **强制切换的坑**：在 **TCP 客户端上**发 `link=usb` 会立即生效、但 ack 已改走 USB ⇒ TCP 侧收不到应答
并被断开（本上位机的「链路」下拉同理：切完请在目标接口重连/再看徽标）。反向（强制 usb 时连 TCP）会被拒。

⇒ **路线 = JPEG 预览（8~11fps）+ raw 按需单帧抓取**：
- UI「流」页有 **`抓一帧 raw → PNG`** 按钮（自动 开流[raw,fps=5] → 取第一帧 → 停流 → 存 `shots/raw_*.png`，约 0.5s）
- 脚本：`qt_host.exe --selftest --serial COM5 --grab-raw out.png` → `grab-raw: ok=1 640x480 data=614400B`

⚠️ 测吞吐时**必须持续读**（CDC 背压），且**不能在流中发查询**（0x02 洪泛会让 ack 匹配失败）——
细节见 `docs/library/HANDOVER.md` 第廿五节 §3.2。

## 6.1 吞吐基准工具

```powershell
python pc_host/_usb_bench.py --serial COM5                       # USB 四档（每档 3 次取中位）
python pc_host/_usb_bench.py --tcp 127.0.0.1:13333 --tiers jpeg:20,raw:10
```

## 7. 目录结构

```
qt_host/
├── CMakeLists.txt      Qt6 Widgets/Network；AUTOMOC；头文件必须进 target（否则 moc 缺失，链接报 ITransport vtable 未定义）
├── build.ps1           一键构建（ASCII-only！PowerShell 5.1 按 ANSI 读 .ps1，中文会解析失败）
├── README.md
└── src/
    ├── main.cpp            入口 + 深色主题 QSS + NoProxy + --selftest 分流
    ├── jclink.h/.cpp       帧常量 / buildFrame / 增量 FrameParser / crc16-ccitt / crc32 / 0x05 头
    ├── transport.h         传输抽象 ITransport + 串口枚举（SetupAPI）
    ├── tcp_transport.*     QTcpSocket 实现（NODELAY）
    ├── serial_transport.*  Win32 COM 实现（1MB 驱动缓冲 + 64KB 读线程）
    ├── link_session.*      收帧分发 / ack 精确匹配 / 统计 / F10 备份收发
    ├── image_frame.*       0x05 → QImage（JPEG 插件；RGB565 直映射 + 字节序开关）
    ├── video_view.*        letterbox 预览 + 检测框/关键点叠加 + 截图
    ├── main_window.*       顶栏 + 视频区 + 五个控制 Tab + 状态栏
    └── selftest.*          无界面自检（验收脚本用）
```
