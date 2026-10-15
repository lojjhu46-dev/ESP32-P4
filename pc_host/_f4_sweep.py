"""F4 保持窗运行时扫描（一次性脚本，PC 送图 + 顺带抓板端串口日志）。

为什么扫：AI 处理一帧约 244ms（解码+检测+93ms 提特征），比编译默认的 150ms 还长，
保持窗在 150ms 下永远触发不了（只有跳过提特征才能变快，而跑不进 150ms 就永远不跳）。
所以要现场把 trk_hold_ms 扫一遍，看哪个值能同时做到：
  ① reuse 命中（节流真的生效）② 提特征频率落到 5~10 次/秒 ③ 结果帧率上升。

用法：python pc_host/_f4_sweep.py
"""
import json
import sys
import threading
import time

sys.path.insert(0, "pc_host")
from protocol import Link  # noqa: E402

FACES = "D:/esp32project/ESP32-P4/esp-dl/examples/human_face_recognition/faces/"
IMG = FACES + "musk1.jpg"
HOLDS = [150, 250, 400, 600, 1000]
SECONDS = 8.0
FEED_HZ = 20.0

_logs = []
_log_stop = threading.Event()


def serial_log_thread(port="COM4"):
    try:
        import serial
    except ImportError:
        return None
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        # ⚠️ 打开串口会把 DTR/RTS 拉高，而这两根线接的是板的 EN/GPIO0（烧录电路）
        # → 板子会被复位/按住。必须显式放开，否则整轮测量板子都在重启。
        s.setDTR(False)
        s.setRTS(False)
        time.sleep(0.2)
    except Exception as e:  # noqa: BLE001
        print(f"  (串口 {port} 打不开：{e}，跳过日志抓取)")
        return None

    def run():
        while not _log_stop.is_set():
            d = s.read(8192)
            if d:
                _logs.append(d.decode("utf-8", "replace"))
        s.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


class Feeder(threading.Thread):
    def __init__(self, lk, hz):
        super().__init__(daemon=True)
        self.lk = lk
        self.dt = 1.0 / hz
        self.sent = 0
        self.stop = threading.Event()
        with open(IMG, "rb") as fp:
            self.jpeg = fp.read()

    def run(self):
        while not self.stop.is_set():
            t0 = time.time()
            try:
                self.lk.send_jpeg(self.jpeg)
                self.sent += 1
            except OSError:
                return
            rest = self.dt - (time.time() - t0)
            if rest > 0:
                time.sleep(rest)


def med(v):
    return sorted(v)[len(v) // 2] if v else -1.0


def measure(lk, feeder, seconds, label):
    n = reuse = face = 0
    fhs, fpss, infers = [], [], []
    ev = {}
    t_end = time.time() + seconds
    while time.time() < t_end:
        f = lk.recv_result(timeout=0.5)
        if not f:
            continue
        try:
            j = json.loads(f[2].decode("utf-8", "replace"))
        except ValueError:
            continue
        if "ack" in j:
            continue
        n += 1
        if j.get("reuse"):
            reuse += 1
        if j.get("faces", 0) > 0:
            face += 1
        if isinstance(j.get("feat_hz"), (int, float)):
            fhs.append(j["feat_hz"])
        if isinstance(j.get("fps"), (int, float)):
            fpss.append(j["fps"])
        if isinstance(j.get("infer_ms"), (int, float)):
            infers.append(j["infer_ms"])
        e = j.get("event", "?")
        ev[e] = ev.get(e, 0) + 1
    print(f"  {label:>16s}  结果帧={n:3d} 有脸={face:3d} reuse={reuse:3d} "
          f"({100 * reuse / max(n, 1):3.0f}%)  feat_hz={med(fhs):4.1f}  "
          f"结果fps={med(fpss):4.1f}  infer_ms={med(infers):3.0f}  {ev}")
    return med(fpss), med(fhs), reuse, n


def main():
    # ⚠️ 不抓串口：打开 COM4 会把 DTR/RTS 拉高（接的是板子的 EN/GPIO0）→ **复位板子**，
    # 整轮测量全废（今天就踩了一次）。要看板端日志就单独读一次，别和测量同时进行；
    # 测量期间板端 0x04 日志会经 JCLINK 送到 lk.logs。
    lk = Link(tcp="127.0.0.1:13333", timeout=6)
    m = None
    for i in range(12):                     # 板子可能刚被串口打开复位，等它起来
        m = lk.cmd_and_wait({"cmd": "mode"}, timeout=4)
        if m:
            break
        print(f"  (等板子就绪… {i})")
        time.sleep(3)
    if not m:
        print("!! 连不上板子（mode 无应答）——检查取景中枢/网线后重跑")
        return None
    print(f"起始 mode: {m.get('msg')}")
    lk.cmd_and_wait({"cmd": "src", "value": "pc"})
    time.sleep(0.5)

    print(f"\n喂帧 {FEED_HZ:.0f}fps / 每档 {SECONDS:.0f}s / 同一张脸（跟踪应命中）")
    print(f"  {'trk_hold_ms':>16s}  结果帧率 / 提特征频率 / 节流命中")
    results = []
    for hold in HOLDS:
        lk.cmd_and_wait({"cmd": "mode", "trk_hold_ms": hold})
        fd = Feeder(lk, FEED_HZ)
        fd.start()
        try:
            r = measure(lk, fd, SECONDS, f"hold={hold}ms")
        finally:
            fd.stop.set()
            fd.join(timeout=2.0)
        results.append((hold,) + r)

    lk.cmd_and_wait({"cmd": "mode", "trk_hold_ms": 400})
    print("\n建议：取'feat_hz 落在 5~10 且结果帧率最高'的档（见上表）")
    print("已把板端 trk_hold_ms 暂设 400ms（要改随时再下发）")
    lk.cmd_and_wait({"cmd": "src", "value": "cam"})
    lk.close()

    _log_stop.set()
    lines = [ln for ln in lk.logs if ("imgpipe" in ln or "faceai" in ln)]
    if lines:
        print("\n=== 板端日志（imgpipe/faceai，最后 8 条）===")
        for ln in lines[-8:]:
            print("  " + ln)
    return results


if __name__ == "__main__":
    main()
