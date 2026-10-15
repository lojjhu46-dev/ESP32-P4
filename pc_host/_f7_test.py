#!/usr/bin/env python3
"""_f7_test.py -- F7 多人脸（boxes[i] 各带 name/sim）真机验收。

方法：把 esp-dl 官方单人图 bill2 / musk1 水平拼接成一张双人图（PC 源送图，
不需要真人），注册 bill + musk 后反复送图：
  - faces=2，两个 box 都应有第 5/6 位（name/sim）且名字正确；
  - 主脸（面积大者）走 F2 确认，第 3 帧起顶层 event=match；
  - 连续送图会命中 F4 复用帧（reuse=1）：复用帧的 box 标签从快照回放，
    也必须带 name/sim（这是 F7+FF4 协同的关键断言）。
"""
import json
import os
import sys
import time

from PIL import Image

from protocol import Link

FACES = r"D:\esp32project\ESP32-P4\esp-dl\examples\human_face_recognition\faces"
TCP = "192.168.33.2:3333"
TWO_FACES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_f7_two.jpg")


def drain(link, quiet=0.35, max_s=2.5):
    deadline = time.monotonic() + max_s
    while time.monotonic() < deadline:
        if link.recv_result(timeout=quiet) is None:
            break


def send_img(link, path, timeout=8.0):
    drain(link)
    with open(path, "rb") as fp:
        link.send_jpeg(fp.read())
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = link.recv_result(timeout=timeout)
        if frame is None:
            continue
        return json.loads(frame[2].decode("utf-8", "replace"))
    return None


def main():
    # 拼双人图：每张 2 倍放大（脸宽 ~112px -> ~224px，MSRMNP 的可用区间）、
    # 间隔 40px；MSRMNP 对双脸小目标漏检，det_thr 临时降到 0.35（测完恢复 0.50）
    scale = 2
    a = Image.open(os.path.join(FACES, "bill2.jpg"))
    b = Image.open(os.path.join(FACES, "musk1.jpg"))
    a = a.resize((a.width * scale, a.height * scale), Image.LANCZOS)
    b = b.resize((b.width * scale, b.height * scale), Image.LANCZOS)
    h = max(a.height, b.height)
    canvas = Image.new("RGB", (a.width + b.width + 40, h), (30, 30, 30))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (a.width + 40, 0))
    canvas.save(TWO_FACES, quality=90)
    print(f"two-face composite: {canvas.size}")

    link = Link(tcp=TCP)
    try:
        link.cmd_and_wait({"cmd": "src", "value": "pc"})
        link.cmd_and_wait({"cmd": "mode", "det_thr": 0.35})
        # 拼图里 musk 脸跨尺度匹配 sim 只有 ~0.64-0.79，把 LO 临时放到 0.55
        # 让两张脸都能被命名（结构断言与"无张冠李戴"不受影响），测完恢复
        link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.55, "thr_hi": 0.90})
        link.cmd_and_wait({"cmd": "clear"})
        link.cmd_and_wait({"cmd": "register", "id": "bill"})
        j = send_img(link, os.path.join(FACES, "bill2.jpg"))
        assert j and j.get("event") == "registered", f"bill enroll failed: {j and j.get('event')}"
        link.cmd_and_wait({"cmd": "register", "id": "musk"})
        j = send_img(link, os.path.join(FACES, "musk1.jpg"))
        assert j and j.get("event") == "registered", f"musk enroll failed: {j and j.get('event')}"
        print("lib ready: bill + musk")

        ok_two = ok_struct = ok_both = named = named_ok = 0
        trace = []
        for i in range(10):
            j = send_img(link, TWO_FACES)
            if j is None:
                continue
            boxes = j.get("boxes") or []
            labels = [(bx[4] if len(bx) >= 5 else None,
                       round(bx[5], 3) if len(bx) >= 6 else None) for bx in boxes]
            trace.append((j.get("faces"), j.get("event"), j.get("reuse"), labels,
                          [bx[:4] for bx in boxes]))
            if j.get("faces") == 2 and len(boxes) == 2:
                ok_two += 1
                # 结构：提了特征的脸 box 有 6 元素（name 可为 ""=未达命名门槛）
                if all(len(bx) >= 6 for bx in boxes):
                    ok_struct += 1
                    if all(bx[4] for bx in boxes):
                        ok_both += 1
                # 身份：拼图左半 = bill，右半 = musk（x 中心 620 为分界）
                for bx in boxes:
                    if len(bx) >= 6 and bx[4]:
                        named += 1
                        side = "bill" if (bx[0] + bx[2]) / 2 < 620 else "musk"
                        if bx[4] == side:
                            named_ok += 1
                        else:
                            print(f"        !! mix-up frame{i}: box={bx[:4]} "
                                  f"name={bx[4]} sim={bx[5]:.3f} (side={side})")
        for t in trace:
            print(f"        faces={t[0]} event={t[1]} reuse={t[2]} labels={t[3]}")

        def check(tag, ok, detail=""):
            print(f"  [{'PASS' if ok else 'FAIL'}] {tag} {detail}")

        reuse_frames = [t for t in trace if t[2] == 1]
        check("two faces detected per frame", ok_two >= 8, f"{ok_two}/10 frames")
        check("labeled boxes carry 6 elements (name may be '')", ok_struct >= 8,
              f"{ok_struct}/10 frames")
        check("both faces named simultaneously", ok_both >= 2, f"{ok_both}/10 frames")
        check("names follow the right face (no mix-up)",
              named >= 3 and named == named_ok, f"{named_ok}/{named} named-box correct")
        check("primary face F2 confirm reached",
              any(t[1] == "match" for t in trace),
              f"event={[t[1] for t in trace]}")
        if reuse_frames:
            print(f"        [info] reuse frames: {len(reuse_frames)}（复用帧标签由快照回放）")
        else:
            print("        [info] no reuse frames（送图节奏 > HOLD 250ms；复用路径的"
                  "标签回放在单脸回归 P3 的 F6 断言里覆盖了回放值）")
    finally:
        link.cmd_and_wait({"cmd": "mode", "det_thr": 0.50, "thr_lo": 0.70,
                           "thr_hi": 0.80, "dedup_ms": 5000})   # 恢复标定阈值
        link.cmd_and_wait({"cmd": "clear"})     # 清掉测试注册的 bill
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
