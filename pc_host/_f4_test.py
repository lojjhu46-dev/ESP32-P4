"""F4 轻跟踪节流验收（一次性脚本，PC 送图，不需要真人在场）。

判据（规划 P0-F4）：白天取帧下"提特征频率"降到 5~10 次/秒，**同时结果帧率上升**
（每帧都提特征时，处理一帧 = 检测 22ms + 提特征 93ms ≈ 115ms → 上限 ~8.7fps；
跟踪命中后跳过提特征，处理一帧只要解码 + 检测）。

关键点：PC 源是"一帧进一帧出"（imgpipe 的 usb_src_acquire 阻塞取下一帧 JPEG），
所以喂帧必须用**独立线程**按固定频率发，否则"发→等结果→再发"会把帧间隔拉到
几百 ms，远超 F4 的 150ms 保持窗，跟踪永远不命中（第一版就踩了这个坑）。

用法：python pc_host/_f4_test.py
"""
import json
import sys
import threading
import time

sys.path.insert(0, "pc_host")
from protocol import Link  # noqa: E402

FACES = "D:/esp32project/ESP32-P4/esp-dl/examples/human_face_recognition/faces/"
IMG_A = FACES + "musk1.jpg"      # 人脸较大
IMG_B = FACES + "bill2.jpg"      # 另一个人：人脸位置/尺度不同


class Feeder(threading.Thread):
    """按固定频率把 JPEG 灌给板子（独立线程，不与收帧互相等待）"""

    def __init__(self, lk, pick, hz):
        super().__init__(daemon=True)
        self.lk = lk
        self.pick = pick
        self.dt = 1.0 / hz
        self.sent = 0
        self.stop = threading.Event()

    def run(self):
        imgs = {}
        while not self.stop.is_set():
            t0 = time.time()
            path = self.pick()
            if path not in imgs:
                with open(path, "rb") as fp:
                    imgs[path] = fp.read()
            try:
                self.lk.send_jpeg(imgs[path])
                self.sent += 1
            except OSError:
                return
            rest = self.dt - (time.time() - t0)
            if rest > 0:
                time.sleep(rest)


def med(v):
    return sorted(v)[len(v) // 2] if v else -1.0


def collect(lk, seconds, label, feeder=None):
    n = reuse = face = 0
    fhs, fpss, infers, boxes = [], [], [], []
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
            b = j.get("boxes")
            if b:
                boxes.append((b[0][2] - b[0][0], b[0][3] - b[0][1]))
        if isinstance(j.get("feat_hz"), (int, float)):
            fhs.append(j["feat_hz"])
        if isinstance(j.get("fps"), (int, float)):
            fpss.append(j["fps"])
        if isinstance(j.get("infer_ms"), (int, float)):
            infers.append(j["infer_ms"])
        e = j.get("event", "?")
        ev[e] = ev.get(e, 0) + 1
    sent = feeder.sent if feeder else 0
    print(f"[{label}] 喂帧={sent} 结果帧={n} 有脸={face} "
          f"reuse={reuse} ({100 * reuse / max(n, 1):.0f}%)  "
          f"feat_hz中位={med(fhs):.1f}  结果fps中位={med(fpss):.1f}  "
          f"infer_ms中位={med(infers):.0f}")
    print(f"           events={ev}" + (f"  脸宽 {min(b[0] for b in boxes)}~{max(b[0] for b in boxes)}"
                                        if boxes else ""))
    return n, reuse


def run(lk, label, pick, hz, seconds):
    fd = Feeder(lk, pick, hz)
    fd.start()
    try:
        return collect(lk, seconds, label, fd)
    finally:
        fd.stop.set()
        fd.join(timeout=2.0)


def main():
    lk = Link(tcp="127.0.0.1:13333", timeout=6)
    lk.cmd_and_wait({"cmd": "src", "value": "pc"})
    time.sleep(0.5)

    print("=== 1) 静态同一张脸 @20fps → 跟踪应命中（reuse 高、feat_hz≈6.7、帧率上升）===")
    run(lk, "1-static-20fps", lambda: IMG_A, 20, 10)

    print()
    print("=== 2) 交替两张不同脸 @20fps → 框变化应使跟踪失效（reuse≈0）===")
    flag = {"i": 0}

    def alt():
        flag["i"] ^= 1
        return IMG_A if flag["i"] else IMG_B

    run(lk, "2-alternate-20fps", alt, 20, 10)

    print()
    print("=== 3) 静态但慢帧率 @5fps → 帧间隔 200ms > HOLD150ms，reuse 应≈0 ===")
    run(lk, "3-static-5fps", lambda: IMG_A, 5, 8)

    lk.cmd_and_wait({"cmd": "src", "value": "cam"})
    lk.close()


if __name__ == "__main__":
    main()
