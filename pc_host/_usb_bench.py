#!/usr/bin/env python3
"""_usb_bench.py -- JCLINK 视频流吞吐基准（**板端 tx 计数口径 + 后台持续读**）

┌ 三个坑（都是实测踩出来的，改这个脚本前先读）
│ 1) **CDC 是背压式的**：主机不读 → 驱动缓冲满 → 板端 FIFO 满 → 0x05 帧超 500ms 预算被丢。
│    所以测量期间**必须有线程持续读**（本脚本用后台 Reader 线程），否则测出的不是吞吐，
│    而是"背压下的丢弃率"（实测只数到 0x02 小帧，~7KB/s，读数全错）。
│ 2) **别用 PC 侧计数**：usbser.sys 突发交付 + 缓冲，按"首末帧时间差"算 fps/带宽抖动巨大
│    （实测同配置 786~1432 KB/s，±29%），还会出现 45fps 这种假读数。
│ 3) **别在流中发查询**：流开着时 0x02 结果帧洪泛，`cmd_and_wait` 会取到错帧
│    （实测读取全废）。本脚本只在**流关闭、Reader 停止**的静默态查 `link`。
└ 口径：`{"cmd":"link"}` 的 `tx_frames/tx_kb`（发送侧真实累计），并**减去空闲基线**
   （不含 0x05 时的 0x02/0x04 底噪），差值即 0x05 实际吞吐。

流程（每次测量）：
  静默段(基线) → 开流 → 后台读 N 秒 → 关流 → 静默段(终点)
用法：
  python pc_host/_usb_bench.py --serial COM5
  python pc_host/_usb_bench.py --tcp 127.0.0.1:13333             # 中枢在跑时（A/B 对比）
  python pc_host/_usb_bench.py --serial COM5 --repeat 3 --seconds 8 --tiers jpeg:20,raw:10

⚠️ 同一时刻只允许一个客户端连板子（关掉取景中枢 / WSL 模拟器 / Qt 上位机）。
⚠️ 吞吐与相机画面内容相关（人脸越多 AI 越忙）→ 每档 3 次取中位 + 打印极差。
"""
import argparse
import contextlib
import io
import re
import statistics
import sys
import threading
import time

try:
    from protocol import Link
except ImportError:
    sys.path.insert(0, ".")
    sys.path.insert(0, "pc_host")
    from protocol import Link


class Reader(threading.Thread):
    """后台持续读（丢弃内容）——CDC 背压要求的消费者"""

    def __init__(self, link: Link):
        super().__init__(daemon=True)
        self.link = link
        self.alive = True
        self.count = 0

    def run(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):   # 吞掉协议层的打印
            while self.alive:
                try:
                    if self.link.recv(timeout=0.05) is not None:
                        self.count += 1
                except Exception:  # noqa: BLE001
                    break

    def stop(self) -> None:
        self.alive = False
        self.join(timeout=1.0)


def quiet_drain(link: Link, seconds: float) -> None:
    """静默排空（不启动线程）"""
    deadline = time.monotonic() + seconds
    with contextlib.redirect_stdout(io.StringIO()):
        while time.monotonic() < deadline:
            link.recv(timeout=0.08)


def query_tx(link: Link, tries: int = 5) -> tuple[int, int]:
    """静默态查 link：返回 (tx_frames, tx_kb)。取到非 link ack 就重试。"""
    last = None
    for _ in range(tries):
        with contextlib.redirect_stdout(io.StringIO()):
            ack = link.cmd_and_wait({"cmd": "link"}, timeout=4.0)
        msg = (ack or {}).get("msg", "")
        last = msg
        m = re.search(r"tx_frames=(\d+)\s+tx_kb=(\d+)", msg or "")
        if m:
            return int(m.group(1)), int(m.group(2))
        quiet_drain(link, 0.3)
    raise RuntimeError(f"link ack 里没有 tx 统计（最后一次: {last!r}）")


def set_stream(link: Link, value: str, fmt: str, fps: int, quality: int = 75) -> None:
    """发 stream 命令（不等 ack：流起来后 ack 与结果帧混在一起，不依赖它）"""
    with contextlib.redirect_stdout(io.StringIO()):
        link.send_json_cmd({"cmd": "stream", "value": value, "fps": fps,
                            "fmt": fmt, "quality": quality})


def measure(link: Link, fmt: str, fps: int, seconds: float, idle_sec: float = 3.0):
    """返回 (payload_fps, payload_KB/s, drop几帧)。含空闲基线扣除。"""
    # --- 空闲基线（无 0x05，只有 0x02/0x04 底噪）---
    set_stream(link, "off", fmt, fps)
    quiet_drain(link, 1.0)
    b0f, b0k = query_tx(link)
    r = Reader(link)
    r.start()
    t = time.monotonic()
    time.sleep(idle_sec)
    dt_idle = time.monotonic() - t
    r.stop()
    quiet_drain(link, 0.3)
    b1f, b1k = query_tx(link)
    idle_fps = (b1f - b0f) / dt_idle
    idle_kbps = (b1k - b0k) / dt_idle

    # --- 流段 ---
    set_stream(link, "on", fmt, fps)
    r = Reader(link)
    r.start()
    t = time.monotonic()
    time.sleep(seconds)
    dt = time.monotonic() - t
    r.stop()
    set_stream(link, "off", fmt, fps)
    quiet_drain(link, 1.0)
    s1f, s1k = query_tx(link)

    fps_out = (s1f - b1f) / dt - idle_fps
    kbps_out = (s1k - b1k) / dt - idle_kbps
    return fps_out, kbps_out, idle_fps, idle_kbps


def main() -> int:
    ap = argparse.ArgumentParser(description="JCLINK 视频流吞吐基准（板端 tx 计数口径）")
    ap.add_argument("--tcp", default=None, help="如 127.0.0.1:13333 或 192.168.33.2:3333")
    ap.add_argument("--serial", default=None, help="如 COM5（板子 High speed USB 口）")
    ap.add_argument("--repeat", type=int, default=3, help="每档重复次数（取中位数）")
    ap.add_argument("--seconds", type=float, default=6.0, help="每次流段时长")
    ap.add_argument("--idle", type=float, default=3.0, help="每档空闲基线时长")
    ap.add_argument("--tiers", default="jpeg:10,jpeg:20,jpeg:30,raw:10",
                    help="档位列表，如 jpeg:10,raw:10")
    args = ap.parse_args()

    if not args.tcp and not args.serial:
        ap.error("必须给 --tcp 或 --serial")

    tiers = []
    for item in args.tiers.split(","):
        f, _, n = item.partition(":")
        tiers.append((f.strip(), int(n)))

    link = Link(tcp=(None if args.serial else args.tcp), serial_port=args.serial)
    iface = f"serial {args.serial}" if args.serial else f"tcp {args.tcp}"
    print(f"# JCLINK 吞吐基准 via {iface} | 每档 {args.repeat} 次 x {args.seconds}s"
          f" | 口径=板端 tx 计数 - 空闲基线")

    rows = []
    for fmt, fps in tiers:
        fps_list, kb_list = [], []
        for i in range(args.repeat):
            try:
                f, k, ifps, ikb = measure(link, fmt, fps, args.seconds, args.idle)
            except Exception as e:  # noqa: BLE001
                print(f"  {fmt}@{fps} run{i + 1}: 失败 {e}")
                continue
            fps_list.append(f)
            kb_list.append(k)
            print(f"  {fmt:4s}@{fps:<3d} run{i + 1}: {f:6.2f} fps {k:8.1f} KB/s"
                  f"   (底噪 {ifps:.2f} fps / {ikb:.1f} KB/s)")
        if fps_list:
            rows.append((f"{fmt}@{fps}", statistics.median(fps_list),
                         statistics.median(kb_list), min(fps_list), max(fps_list)))

    print()
    print(f"{'tier':>10} | {'fps(med)':>8} | {'KB/s(med)':>9} | {'fps spread':>13}")
    print("-" * 52)
    for name, fps, kb, lo, hi in rows:
        print(f"{name:>10} | {fps:8.2f} | {kb:9.1f} | {lo:6.2f}..{hi:5.2f}")

    set_stream(link, "off", "jpeg", 10)
    link.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
