# qt_host —— JCLINK 上位机（C++/Qt6）

ESP32-P4 边缘人脸识别终端的 PC 上位机：**同时支持网口（TCP）与高速 USB（CDC 串口）**，
接收 0x05 视频流并显示，叠加 0x02 检测结果，内置全功能控制面板（流/名单/识别/相机/备份）。

> **新会话接手先读**：[`../jc_p4_app/docs/HANDOVER.md`](../jc_p4_app/docs/HANDOVER.md) 第廿五节
> （USB 视频流 + Qt 上位机的实测数据与坑）。

## 1. 依赖与环境（本机已就位）

| 项 | 路径 | 说明 |
|---|---|---|
| Qt | `D:\qt\qt\Qt\6.12.0\mingw_64` | **6.12.0 mingw_64**（含 Core/Gui/Widgets/Network） |
| MinGW | `D:\qt\qt\Qt\Tools\mingw1310_64` | 13.1.0（qt_host 用它的 g++ 编译） |
| Ninja | `D:\qt\qt\Qt\Tools\Ninja` | 构建器 |
| CMake | `C:\Program Files\CMake\bin\cmake.exe` | ≥3.21 |

⚠️ **本机 Qt 未安装 `Qt6SerialPort` 模块** → 串口用 **Win32 API 自实现**
（`src/serial_transport.cpp`：`CreateFile` + `SetupComm(1MB)` + `SetCommTimeouts` + 独立读线程），
不依赖该模块、也不需要联网装组件。

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
  板端 link 强制态（auto·usb·tcp）/ 开始-停止流 / 截图 / 缩放
- **左侧**：视频区（黑底等比 letterbox，叠加命中绿框+名字+相似度、未知黄框、关键点红点，
  右上角角标实时显示 `fps / KB-s / 丢帧 / 链路`）+ 板端日志区
- **右侧 Tab**：
  - **流**：fps(1-30)、quality(1-100)、格式 JPEG/raw、图像源 cam/pc、raw 字节序交换
  - **名单**：表格（名字/特征数）、Register（1/3/5 帧）、Delete、Clear（二次确认）
  - **识别**：detect/match、thr_lo、thr_hi、fast_thr、dedup_ms、multi
  - **相机**：gain/exp 滑块、帧率档、镜像、AE、AWB、夜视、低光照停人脸、降噪档
  - **备份**：F10 特征库导出 `.bin` / 导入（覆盖式，板端自动 `.bak`）+ 进度条
- **快捷键**：`S` 截图 · `空格` 开始/停止流 · `R` 重连
- 截图存到 **exe 同目录的 `shots/`**（PNG，含检测框叠加层，按原始分辨率出图）

连接成功后会自动发一轮状态查询（`mode`/`cam`/`stream`/`link`/`list`）把控件同步成板端真实值。

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

⇒ **路线 = JPEG 预览（8~11fps）+ raw 按需单帧抓取**：
- UI「流」页有 **`抓一帧 raw → PNG`** 按钮（自动 开流[raw,fps=5] → 取第一帧 → 停流 → 存 `shots/raw_*.png`，约 0.5s）
- 脚本：`qt_host.exe --selftest --serial COM5 --grab-raw out.png` → `grab-raw: ok=1 640x480 data=614400B`

⚠️ 测吞吐时**必须持续读**（CDC 背压），且**不能在流中发查询**（0x02 洪泛会让 ack 匹配失败）——
细节见 `jc_p4_app/docs/HANDOVER.md` 第廿五节 §3.2。

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
