"""昼夜闭环一次性探针：单连接，按时间表改 ae_target，全程打印状态跳变与板端日志。
用完即删（_ 前缀，非正式工具）。"""
import sys, time

sys.path.insert(0, r"D:\esp32project\ESP32-P4\pc_host")
from protocol import Link  # noqa: E402

DUR = 150
SCHED = [
    (6,   {"cmd": "cam", "ae_target": 120}),   # 基线
    (41,  {"cmd": "cam", "ae_target": 250}),   # 提亮
    (81,  {"cmd": "cam", "ae_target": 120}),   # 恢复默认
    (120, {"cmd": "cam", "auto_fps": True}),   # AUTO 回归检查（白天应休眠）
]

link = Link(tcp="192.168.33.2:3333", timeout=8.0)
t0 = time.time()
si = 0
last = [None]
print("start")
while time.time() - t0 < DUR:
    now = time.time() - t0
    if si < len(SCHED) and now >= SCHED[si][0]:
        link.send_json_cmd(SCHED[si][1])
        print(f"[{now:6.1f}] CMD {SCHED[si][1]}")
        si += 1
    f = link.recv_any(timeout=0.3)
    if not f:
        continue
    kind, data = f
    ts = time.time() - t0
    if kind == "log":
        print(f"[{ts:6.1f}] LOG {data}")
    elif kind == "result" and "ack" not in data:
        key = (data.get("cam_fps"), data.get("ntier"), data.get("night"),
               (data.get("exp") or 0) // 250, (data.get("gain") or 0) // 16,
               data.get("event"))
        if key != last[0]:
            last[0] = key
            print(f"[{ts:6.1f}] RES fps={data.get('cam_fps')} ntier={data.get('ntier')} "
                  f"night={data.get('night')} exp={data.get('exp')}/{data.get('exp_max')} "
                  f"gain={data.get('gain')} luma={data.get('luma')} ev={data.get('event')} "
                  f"auto={data.get('auto')}")
print("done")
