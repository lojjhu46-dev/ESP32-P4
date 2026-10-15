"""量 AUTO 运动切换的卡顿：把系统压到低帧率档，注入运动，记录每帧到达间隔与 cam_fps 变化。

输出两段：
  1) 注入运动 -> 结果帧里 cam_fps 变成 50 的耗时（端到端，用户感知的那段）
  2) 期间的帧到达间隔（看有没有明显的长空档）
串口日志另抓（决策->写寄存器 的那段延迟从板端日志量）。
"""
import sys
import time

sys.path.insert(0, r"D:\esp32project\ESP32-P4\pc_host")
from protocol import Link


def run(tag, inject, seconds, link):
    """记录 seconds 秒；inject 是 (延迟秒, 命令) """
    print(f"\n===== {tag} =====")
    t0 = time.monotonic()
    injected_at = None
    t_prev = None
    first50 = None
    max_gap = 0.0
    gaps = []
    while True:
        now = time.monotonic()
        if now - t0 > seconds:
            break
        if inject and now - t0 >= inject[0] and injected_at is None:
            injected_at = now
            link.send_json_cmd(inject[1])
            print(f"    >>> t+{now - t0:.2f}s 注入 {inject[1]}")
        f = link.recv_any(timeout=0.2)
        if not f:
            if t_prev is not None:
                idle = (time.monotonic() - t_prev) * 1000
                if idle > 400:                     # 只在明显空档时打一行
                    print(f"    t+{time.monotonic() - t0:6.2f}s  ...静默 {idle:5.0f}ms")
            continue
        now = time.monotonic()
        dt = (now - t_prev) * 1000 if t_prev else 0.0
        t_prev = now
        if f[0] == "image":
            kind, extra = "IMG", ""
        else:
            r = f[1]
            if "ack" in r:
                continue
            cf = r.get("cam_fps") or 0
            kind = "RES"
            extra = f"cam_fps={cf:>3} ntier={r.get('ntier')} af_ms={r.get('af_ms')}"
            if cf == 50 and first50 is None and injected_at:
                first50 = now
        if injected_at:
            gaps.append(dt)
            max_gap = max(max_gap, dt)
        print(f"    t+{now - t0:6.2f}s  dt={dt:6.0f}ms  {kind} {extra}")
    if first50:
        print(f"    ==> 注入->cam_fps=50 端到端 {1000 * (first50 - injected_at):.0f} ms；期间最大帧间隔 {max_gap:.0f} ms")
    else:
        print(f"    ==> 未观察到 cam_fps=50；期间最大帧间隔 {max_gap:.0f} ms")
    return max_gap


def main():
    link = Link(tcp="192.168.33.2:3333", timeout=8.0)
    # 场景：AUTO ON + 夜视阶梯 ON + 暗场静止
    link.send_json_cmd({"cmd": "cam", "ae": True, "awb": True, "enh": True,
                        "night": True, "auto_fps": True, "mv_thr": 30, "ae_target": 120})
    print("等静止档稳定（确认 cam_fps 已不是 50）...")
    t0 = time.monotonic()
    idle_ok = False
    while time.monotonic() - t0 < 45:
        f = link.recv_any(timeout=0.5)
        if f and f[0] == "result" and "ack" not in f[1]:
            cf = f[1].get("cam_fps") or 0
            if cf and cf < 50:     # 已降档进夜视静止档（白天恒 50 不会就绪，属预期）
                idle_ok = True
                break
    print(f"   静止档就绪={idle_ok}（用时 {time.monotonic() - t0:.0f}s）")
    # 强制进最深夜视档（房间照度不可控：关掉自适应增强 + gamma 线性即可压暗）
    link.send_json_cmd({"cmd": "cam", "enh": False, "gamma": 1000})
    t0 = time.monotonic()
    tier_ok = False
    while time.monotonic() - t0 < 40:
        f = link.recv_any(timeout=0.5)
        if f and f[0] == "result" and "ack" not in f[1]:
            if (f[1].get("ntier") or 0) >= 2:
                tier_ok = True
                break
    print(f"   tier2 就绪={tier_ok}（用时 {time.monotonic() - t0:.0f}s）")
    time.sleep(3)

    # 阶段 A：静止 -> 注入运动（升档）
    run("A 升档（静止 -> 运动）", inject=(4.0, {"cmd": "cam", "mv_thr": 1}), seconds=22, link=link)

    # 阶段 B：恢复静止（降档）
    run("B 降档（运动 -> 静止）", inject=(4.0, {"cmd": "cam", "mv_thr": 30}), seconds=26, link=link)

    link.send_json_cmd({"cmd": "cam", "auto_fps": False, "enh": True, "night": True})
    link.close()
    print("\ndone")


if __name__ == "__main__":
    main()
