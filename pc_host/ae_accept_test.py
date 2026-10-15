"""AE/AWB 真机验收：收敛性 + 手动切换 + auto_fps 联动（安静版）"""
import sys, time
sys.path.insert(0, r"D:\esp32project\ESP32-P4")
from pc_host.protocol import Link

lk = Link(tcp="192.168.33.2:3333", timeout=5)
time.sleep(1.0)

def pump(sec):
    """收 sec 秒 0x02 结果帧"""
    out, t0 = [], time.time()
    while time.time() - t0 < sec:
        f = lk.recv_any(1.0)
        if f and f[0] == "result":
            d = f[1]
            if isinstance(d, dict) and "luma" in d:
                out.append(d)
    return out

def cmd(obj, timeout=3.0):
    """发命令等带 ack 的回复（跳过流式结果帧，不打印）"""
    lk.send_json_cmd(obj)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        f = lk.recv_any(0.5)
        if f and f[0] == "result" and isinstance(f[1], dict) and "ack" in f[1]:
            return f[1]
    return None

d = cmd({"cmd": "cam"})
if d:
    print("INIT   exp=%s gain=%s luma=%s ae=%s awb=%s wb=%s/%s cam_fps=%s" % (
        d.get("exp"), d.get("gain"), d.get("luma"), d.get("ae"), d.get("awb"),
        d.get("wb_kr"), d.get("wb_kb"), d.get("cam_fps")))
else:
    print("INIT   no ack!")

fr = pump(5)
lumas = [x.get("luma") for x in fr]
print("STEADY 5s: %d frames (%.1f/s), luma %s..%s last=%s" % (
    len(fr), len(fr) / 5.0, min(lumas) if lumas else "-",
    max(lumas) if lumas else "-", lumas[-1] if lumas else "-"))

d = cmd({"cmd": "cam", "gain": 192, "exp": 994})
print("NIGHT manual -> ae=%s (expect 0) exp=%s gain=%s" % (
    d.get("ae") if d else None, d.get("exp") if d else None, d.get("gain") if d else None))

d = cmd({"cmd": "cam", "ae": True})
print("AE re-enable -> ae=%s" % (d.get("ae") if d else None))
fr = pump(8)
lumas = [(x.get("luma"), x.get("exp"), x.get("gain")) for x in fr]
if lumas:
    print("RECONVERGE 8s: luma %s..%s, exp %s->%s, gain %s->%s, %.1f frames/s" % (
        min(l[0] for l in lumas), max(l[0] for l in lumas),
        lumas[0][1], lumas[-1][1], lumas[0][2], lumas[-1][2], len(lumas) / 8.0))

d = cmd({"cmd": "cam", "awb": False})
print("AWB off -> awb=%s wb=%s/%s (保持当前值)" % (
    d.get("awb") if d else None, d.get("wb_kr") if d else None, d.get("wb_kb") if d else None))
cmd({"cmd": "cam", "awb": True})

t0 = time.time()
d = cmd({"cmd": "cam", "auto_fps": True})
print("AUTO-FPS on -> auto=%s (%.2fs)" % (d.get("auto") if d else None, time.time() - t0))
fr = pump(6)
cfps = sorted(set(x.get("cam_fps") for x in fr))
print("AUTO 6s: %d frames (%.1f/s), cam_fps values=%s" % (len(fr), len(fr) / 6.0, cfps))
d = cmd({"cmd": "cam", "auto_fps": False})
print("AUTO off -> auto=%s" % (d.get("auto") if d else None))
lk.close()
print("DONE")
