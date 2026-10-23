"""M0 上位机侧：接收 JCLINK 0x07 H.264 访问单元并按 stream_id 落盘。

板端 `app_h264enc` 在 M0 参数扫描时把每个 NAL 作为 0x07 帧发出，
`stream_id` = 阶段号（1..3），每个阶段的流都以 IDR+SPS/PPS 开始，可独立播放。

用法（需独占板子：先停掉 board_hub / qt_host）：
    python pc_host/h264_capture.py                    # 自动找 VID_303A:4001，50s
    python pc_host/h264_capture.py --serial COM5 --seconds 55 --out-dir _dev/m0_link
校验：
    ffprobe -v error -count_frames -show_entries stream=width,height,avg_frame_rate,bit_rate \\
            -of default=nw=1 _dev/m0_link/h264_stream1.h264
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol import Link, T_H264, H264Frame  # noqa: E402


def auto_serial() -> str | None:
    """P4 原生 USB OTG 的 CDC 口（VID_303A PID_4001），不是 CH340。"""
    import serial.tools.list_ports as lp

    for p in lp.comports():
        if "303A" in p.hwid.upper() and "4001" in p.hwid.upper():
            return p.device
    return None


def wait_serial(name_hint: str | None, seconds: float = 20.0) -> str | None:
    """烧录/复位后 USB 会重新枚举，等端口出现后再打开。"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if name_hint:
            import serial.tools.list_ports as lp

            if name_hint in {p.device for p in lp.comports()}:
                return name_hint
        else:
            port = auto_serial()
            if port:
                return port
        time.sleep(0.4)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", help="CDC 口，如 COM5（缺省自动扫描 VID_303A:4001）")
    ap.add_argument("--tcp", help="host:port，走网线（与 USB 同协议）")
    ap.add_argument("--seconds", type=float, default=50.0)
    ap.add_argument("--out-dir", default="_dev/m0_link")
    ap.add_argument("--play", action="store_true", help="实时用 ffplay 播放（观察流畅度/清晰度）")
    args = ap.parse_args()

    port = args.serial
    if not args.tcp:
        port = wait_serial(args.serial)
        if not port:
            print("等不到 CDC 口（VID_303A:4001）；确认插的是 High speed USB 口且已枚举")
            return 2

    lk = Link(tcp=args.tcp, serial_port=port, timeout=2.0)
    print(f"# linked: {lk.desc} 采样 {args.seconds}s -> {args.out_dir}")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    player = None
    if args.play:
        import shutil
        import subprocess

        exe = shutil.which("ffplay")
        if not exe:
            print("找不到 ffplay（把 D:\\ffmpeg\\bin 加进 PATH）")
            return 3
        # 裸 H.264 从 stdin 播放；nobuffer/low_delay 尽量压低延迟
        player = subprocess.Popen(
            [exe, "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
             "-framerate", "25", "-f", "h264", "-", "-x", "960", "-y", "720",
             "-window_title", "P4 H.264 M0"],
            stdin=subprocess.PIPE)
        print("# ffplay 实时窗口已启动（关掉窗口即停止播放）")

    # 先全收进内存再落盘：逐帧写文件会卡住串口读取，丢掉参考帧导致 PPS 错误
    buffers: dict[int, bytearray] = {}
    stats: dict[int, dict] = {}
    t0 = time.monotonic()
    next_tick = t0 + 1.0

    try:
        while True:
            now = time.monotonic()
            if now - t0 >= args.seconds:
                break
            # want 必须显式带 T_H264：recv() 默认只回 0x02/0x04
            raw = lk.recv(timeout=0.3, want=(T_H264,))
            if not raw:
                continue
            if raw[0] != T_H264:
                continue
            try:
                fr = H264Frame(raw[2])
            except ValueError as e:
                print(f"  [warn] bad 0x07 frame: {e}")
                continue

            sid = fr.stream_id
            if sid not in stats:
                buffers[sid] = bytearray()
                stats[sid] = {"frames": 0, "bytes": 0, "keys": 0, "max": 0,
                              "first": fr.pts_us, "last": fr.pts_us, "t0": now}
                print(f"[cap] stream {sid}: {fr.width}x{fr.height}")
            buffers[sid] += fr.data
            if player is not None:
                try:
                    player.stdin.write(fr.data)
                    player.stdin.flush()
                except (BrokenPipeError, OSError):
                    print("# ffplay 已关闭，停止采集")
                    break
            st = stats[sid]
            st["frames"] += 1
            st["bytes"] += len(fr.data)
            st["max"] = max(st["max"], len(fr.data))
            st["last"] = fr.pts_us
            if fr.is_keyframe:
                st["keys"] += 1

            if now >= next_tick:
                total = sum(s["bytes"] for s in stats.values())
                frames = sum(s["frames"] for s in stats.values())
                dt = now - t0
                print(f"  [{dt:5.1f}s] streams={len(stats)} frames={frames} "
                      f"bytes={total} -> {total * 8 / dt / 1000:.0f} kbps, {frames / dt:.1f} fps")
                next_tick = now + 1.0
    except KeyboardInterrupt:
        print("\n# interrupted")
    finally:
        for sid, buf in buffers.items():
            path = out_dir / f"h264_stream{sid}.h264"
            path.write_bytes(buf)
        if player is not None:
            try:
                player.stdin.close()
                player.wait(timeout=5)
            except Exception:
                player.kill()
        lk.close()

    print("\n=== M0 上位机侧汇总 ===")
    if not stats:
        print("  没收到 0x07 帧：确认板端是 M0 固件、h264enc 已注册、CDC 口没被别的进程占用")
        return 1
    for sid in sorted(stats):
        st = stats[sid]
        dur_us = max(1, st["last"] - st["first"])
        dur_s = dur_us / 1e6
        print(f"  stream {sid}: frames={st['frames']} keys={st['keys']} "
              f"bytes={st['bytes']} avg={st['bytes'] // max(1, st['frames'])}B max={st['max']}B")
        print(f"            {st['frames'] / dur_s:.1f} fps  "
              f"{st['bytes'] * 8 / dur_s / 1000:.0f} kbps  ({dur_s:.1f}s)")
    print(f"\n  文件目录: {out_dir}")
    print(f"  校验: ffprobe -v error -count_frames -show_entries "
          f"stream=width,height,avg_frame_rate,bit_rate,nb_read_frames -of default=nw=1 "
          f"{out_dir / 'h264_stream1.h264'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
