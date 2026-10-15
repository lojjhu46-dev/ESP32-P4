"""0x05 预览流 vs AI 帧率争抢定位（探针，2026-10-06）。

现象：相机模式下开预览流，AI 结果帧率腰斩（直连实测 5.2 → 2.6 fps，infer_ms 不变）。
候选机制：
  A) 帧被"偷"走：stream_task 与 AI 抢同一个 2 槽帧池，且每帧只交付一次 →
     流拿走的帧 AI 就看不到。按此，相机帧率越高 / 流 fps 越低，影响应越小。
  B) 资源争抢（CPU0 同核 / 带宽）：与"偷多少帧"无关，只要开流就固定掉一半。

判别实验（本脚本）：
  · 相机 20fps（当前白天档）下：流 off / fps=10 / fps=1
  · 相机 50fps 下：流 off / fps=10 / fps=25
  若 50fps 时影响变小、fps=1 时影响变小 → 机制 A（帧被偷）；
  若六组两边都是"腰斩" → 机制 B（固定资源争抢）。

用法：python pc_host/_stream_probe.py   （需独占板子：先停掉取景中枢）
"""
import json
import sys
import time

sys.path.insert(0, "pc_host")
from protocol import Link  # noqa: E402

TCP = "192.168.33.2:3333"
SECONDS = 8.0

TRIALS = [
    ("cam20 流OFF", None, {"cmd": "stream", "value": "off"}),
    ("cam20 流10", None, {"cmd": "stream", "value": "on", "fps": 10}),
    ("cam20 流20", None, {"cmd": "stream", "value": "on", "fps": 20}),
]


def med(v):
    return sorted(v)[len(v) // 2] if v else -1.0


def measure(lk, seconds):
    lk.logs.clear()
    n = 0
    fpss, infers = [], []
    t_end = time.time() + seconds
    while time.time() < t_end:
        f = lk.recv_result(timeout=0.4)
        if not f:
            continue
        try:
            j = json.loads(f[2].decode("utf-8", "replace"))
        except ValueError:
            continue
        if "ack" in j:
            continue
        n += 1
        if isinstance(j.get("fps"), (int, float)):
            fpss.append(j["fps"])
        if isinstance(j.get("infer_ms"), (int, float)):
            infers.append(j["infer_ms"])
    pipes = [x for x in lk.logs if x.startswith("imgpipe")]
    return n, med(fpss), med(infers), pipes


def main():
    lk = Link(tcp=TCP, timeout=6)
    lk.cmd_and_wait({"cmd": "src", "value": "cam"})
    lk.cmd_and_wait({"cmd": "mode", "trk_hold_ms": 0})       # 排除 F4 影响
    print("无脸基线（不提特征）；trk_hold_ms=0")
    print(f"  {'实验':<12}{'AI结果帧':>8}{'AI fps':>9}{'infer_ms':>9}   板端 imgpipe")
    for label, cam_cmd, stream_cmd in TRIALS:
        if cam_cmd:
            lk.cmd_and_wait(cam_cmd)
        lk.cmd_and_wait(stream_cmd)
        time.sleep(1.5)
        n, fps, inf, pipes = measure(lk, SECONDS)
        tail = pipes[-1].split("imgpipe:", 1)[-1].strip() if pipes else "-"
        print(f"  {label:<12}{n:>8}{fps:>9.1f}{inf:>9.0f}   {tail}")
    # 收尾：恢复白天自动档 + 流 on@10 + F4=250
    lk.cmd_and_wait({"cmd": "cam", "auto": True})
    lk.cmd_and_wait({"cmd": "stream", "value": "on", "fps": 10})
    lk.cmd_and_wait({"cmd": "mode", "trk_hold_ms": 250})
    lk.close()


if __name__ == "__main__":
    main()
