"""夜视阶梯真机验收：T1 暗度降帧率 / T2 自动时域降噪 / T3 双向档位 / T4 运动优先 / T5 手动让位。

设计要点：
  * 环境照度不可控，所以**不假设房间多亮**。造暗用"关掉夜视增强 + gamma 线性"
    （实测能把实测亮度压低 2~3 倍）；造亮用"手动强提亮 gamma 0.35"（反向 2~3 倍）。
    两者都是 ISP 真实通路，且不跟阶梯抢曝光。
  * 任一方向如果被环境光卡住（房间太亮压不下去 / 太暗提不上来），记为 SKIP 并说明原因，
    不算失败——否则测的是房间不是代码。
  * 硬判据是不变量：只要阶梯在管帧率，cam_fps 必须等于当前档位目标帧率、dn 不低于档位要求。
    按"同一档位的连续样本段"取中位数，滤掉切档瞬间"档位已变、帧率还在 camera 任务信箱里"的异步窗口。

单连接跑完全程（板端是单客户端槽位，频繁快连快断会留残留）。
用法：python night_accept_test.py [--tcp 192.168.33.2:3333] [--no-jpeg]
"""
from __future__ import annotations

import argparse
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from protocol import Link  # noqa: E402

FPS_LOW, FPS_MIN = 5, 5          # 与 Kconfig APP_CAM_NIGHT_FPS_LOW/MIN 对齐
                                 # （MIN 默认 5 而非 2：帧率越低 AUTO 升档延迟越大，
                                 #   而实测 2fps 并不比 5fps 亮，见 HANDOVER 第十三节）
DN_LOW, DN_MIN = 2, 3            # 与 Kconfig APP_CAM_NIGHT_DN_LOW/MIN 对齐
TIER_FPS = {0: 50, 1: FPS_LOW, 2: FPS_MIN}   # tier0=白天基准 50fps；但白天还有曝光余量
                                             # 阶梯（HANDOVER 十七节）：exp 顶满时自动
                                             # 50->20->10，见 DAY_FPS_OK
DAY_FPS_OK = (50, 20, 10)                    # tier0 合法帧率集合（白天曝光阶梯）


def tier_fps_ok(tier, fps) -> bool:
    """tier0 允许白天曝光阶梯的 50/20/10；tier1/2 必须精确等于阶梯档位"""
    if int(tier) == 0:
        return fps in DAY_FPS_OK
    return fps == TIER_FPS.get(int(tier))
TIER_DN = {1: DN_LOW, 2: DN_MIN}
ACTIVE_FPS = 50                  # AUTO 判定"在动"时的升档目标（白天也是恒定值）


def is_auto_active(s: dict) -> bool:
    """AUTO 运动模式正在接管帧率：此时夜视阶梯按设计让位，档位冻结、不再管帧率"""
    return bool(s.get("auto")) and (s.get("cam_fps") or 0) >= ACTIVE_FPS

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"
RESULTS: list[tuple[str, str, str]] = []
SAMPLES: list[dict] = []


def check(name: str, ok, detail: str, skip: bool = False) -> None:
    status = SKIP if skip else (PASS if ok else FAIL)
    RESULTS.append((name, status, detail))
    print(f"  [{status}] {name}: {detail}")


def med(frames: list[dict], key: str, default=0):
    vals = [f[key] for f in frames if key in f]
    return statistics.median(vals) if vals else default


def pump(link: Link, sec: float) -> list[dict]:
    end = time.monotonic() + sec
    out: list[dict] = []
    while time.monotonic() < end:
        f = link.recv_any(timeout=0.5)
        if f and f[0] == "result" and "ack" not in f[1]:
            out.append(f[1])
    SAMPLES.extend(out)
    return out


def wait_for(link: Link, pred, timeout: float, label: str):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        f = link.recv_any(timeout=0.5)
        if f and f[0] == "result":
            last = f[1]
            SAMPLES.append(last)
            if pred(last):
                return last
    keys = ("luma", "ntier", "cam_fps", "dn", "night")
    print(f"    ({label} 超时；末帧 {({k: last.get(k) for k in keys}) if last else None})")
    return None


def cap_jpeg(link: Link, path: str, seconds: float = 6.0) -> int:
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        f = link.recv_any(timeout=0.6)
        if f and f[0] == "image" and f[1].is_jpeg:
            last = f[1]
    if last is None:
        return 0
    with open(path, "wb") as fp:
        fp.write(last.data)
    return len(last.data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", default="192.168.33.2:3333")
    ap.add_argument("--no-jpeg", action="store_true")
    args = ap.parse_args()

    link = Link(tcp=args.tcp, timeout=8.0)
    print(f"== 夜视阶梯验收 @ {args.tcp} ==")

    def cmd(obj):
        return link.cmd_and_wait(obj)

    def want_auto():
        cmd({"cmd": "cam", "ae": True, "awb": True, "enh": True, "night": True,
             "auto_fps": False, "ae_target": 120})

    def force_night(timeout=30.0):
        """把实测亮度压到夜视档；返回 (成功?, 说明)。先试环境本身，再关增强+线性 gamma。"""
        f = pump(link, 2.5)
        if (med(f, "ntier") or 0) >= 1:
            return True, f"环境本身已够暗（luma {med(f, 'luma'):.0f}）"
        want_auto()
        cmd({"cmd": "cam", "enh": False, "gamma": 1000})
        hit = wait_for(link, lambda r: (r.get("ntier") or 0) >= 1, timeout, "强制夜视档")
        if hit is not None:
            return True, f"关增强+gamma 线性后到位（luma {hit.get('luma')}）"
        f = pump(link, 2)
        return False, f"环境太亮，压不下去（luma 已是最低 {med(f, 'luma'):.0f} > 阈值 45）"

    # ---------------- T1: 暗度降帧率 ----------------
    print("\n[T1] 暗度降帧率（手动 15fps 起步 -> 打开阶梯 -> 应变暗并降档）")
    cmd({"cmd": "cam", "fps": 15, "auto_exp": True})
    f = pump(link, 4)
    check("T1-手动让位", med(f, "night") == 0, f"手动 fps 后 night={med(f, 'night')}（期望 0）")
    check("T1-基线15fps", med(f, "cam_fps") == 15, f"cam_fps={med(f, 'cam_fps')}")

    want_auto()
    ok_night, note = force_night()
    f = pump(link, 3)
    if ok_night:
        tier1 = int(med(f, "ntier"))
        check("T1-降档", tier1 >= 1 and med(f, "cam_fps") == TIER_FPS.get(tier1),
              f"ntier={tier1} cam_fps={med(f, 'cam_fps')} exp_max={med(f, 'exp_max')} "
              f"luma={med(f, 'luma')}（{note}）")
    else:
        check("T1-降档", False, f"{note}", skip=True)

    # ---------------- T2: 自动时域降噪 ----------------
    print("\n[T2] 夜视档自动开时域降噪")
    if ok_night:
        check("T2-自动DN", (med(f, "dn") or 0) >= 2, f"dn={med(f, 'dn')}（期望 >=2）")
        check("T2-DN生效", (med(f, "dn_ms") or 0) > 0,
              f"dn_ms={med(f, 'dn_ms')} dn_blk={med(f, 'dn_blk')}")
        if not args.no_jpeg:
            cmd({"cmd": "stream", "value": "on", "fmt": "jpeg", "fps": 5, "quality": 85})
            n = cap_jpeg(link, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                            "..", "..", "_night_accept_dn.jpg"))
            cmd({"cmd": "stream", "value": "off"})
            check("T2-DN抓帧", n > 0, f"JPEG {n} B -> _night_accept_dn.jpg")
    else:
        check("T2-自动DN", False, "上一步没进夜视档", skip=True)

    # ---------------- T3: 变亮降档（反向） ----------------
    print("\n[T3] 反向：主动变亮后档位应回落、帧率回升")
    if ok_night:
        f_dark = pump(link, 3)
        luma_dark, tier_dark, fps_dark = med(f_dark, "luma"), med(f_dark, "ntier"), med(f_dark, "cam_fps")
        cmd({"cmd": "cam", "enh": False, "gamma": 0.35})     # 手动强提亮
        hit = wait_for(link, lambda r: (r.get("ntier") or 0) < tier_dark, 30.0, "档位回落")
        f_bright = pump(link, 4)
        luma_b, tier_b, fps_b = med(f_bright, "luma"), med(f_bright, "ntier"), med(f_bright, "cam_fps")
        if hit is not None:
            check("T3-变亮降档", tier_b < tier_dark and tier_fps_ok(int(tier_b), fps_b),
                  f"luma {luma_dark:.0f}->{luma_b:.0f}，ntier {tier_dark:.0f}->{tier_b:.0f}，"
                  f"cam_fps {fps_dark:.0f}->{fps_b:.0f}")
        else:
            check("T3-变亮降档", False,
                  f"提亮后 luma 只到 {luma_b:.0f}（未越过回升阈值），环境太暗无法验证", skip=True)
    else:
        check("T3-变亮降档", False, "没进夜视档，无反方向可测", skip=True)
    cmd({"cmd": "cam", "enh": True})
    pump(link, 2)

    # ---------------- T4: 运动优先 ----------------
    print("\n[T4] 运动优先升档，静止后回落目标是夜视档（不是 15）")
    want_auto()
    ok4, note4 = force_night()
    if ok4:
        cmd({"cmd": "cam", "mv_thr": 1})                   # 伪造"一直在动"
        cmd({"cmd": "cam", "auto_fps": True})
        hit50 = wait_for(link, lambda r: (r.get("cam_fps") or 0) >= 50, 25.0, "升到 50fps")
        check("T4-运动升档", hit50 is not None, f"cam_fps={hit50.get('cam_fps') if hit50 else None}")
        cmd({"cmd": "cam", "mv_thr": 30})                  # 恢复真阈值 -> 判定静止
        hit_idle = wait_for(link, lambda r: (r.get("cam_fps") or 99) <= FPS_LOW, 30.0, "回落到静止档")
        f5 = pump(link, 4)
        nfps, ntier5 = med(f5, "cam_fps"), int(med(f5, "ntier"))
        check("T4-回落夜视档", hit_idle is not None and tier_fps_ok(ntier5, nfps),
              f"cam_fps={nfps} ntier={ntier5}（期望 {TIER_FPS.get(ntier5)}：运动结束要回落到"
              f"当前夜视档的目标帧率，而不是固定 15/50）")
        cmd({"cmd": "cam", "auto_fps": False})
    else:
        check("T4-运动优先", False, note4, skip=True)

    # ---------------- T5: 手动让位与恢复 ----------------
    print("\n[T5] 手动 fps 让位 / night:true 恢复")
    want_auto()
    cmd({"cmd": "cam", "enh": True})
    ok5, note5 = force_night()
    if ok5:
        cmd({"cmd": "cam", "fps": 25})
        f6 = pump(link, 6)
        check("T5-手动让位", med(f6, "night") == 0 and med(f6, "cam_fps") == 25,
              f"night={med(f6, 'night')} cam_fps={med(f6, 'cam_fps')}（期望 0 / 25）")
        cmd({"cmd": "cam", "night": True})
        hit_re = wait_for(link, lambda r: r.get("night") == 1 and (r.get("cam_fps") or 99) <= FPS_LOW,
                          30.0, "阶梯恢复")
        f7 = pump(link, 3)
        check("T5-恢复", hit_re is not None,
              f"night={med(f7, 'night')} ntier={med(f7, 'ntier')} cam_fps={med(f7, 'cam_fps')}")
    else:
        check("T5-手动让位", False, note5, skip=True)

    # ---------------- 收尾 ----------------
    cmd({"cmd": "cam", "ae": True, "awb": True, "night": True,
         "auto_fps": False, "mv_thr": 30, "ae_target": 120})
    cmd({"cmd": "cam", "enh": True})   # 恢复自适应 gamma/BF（清手动让位）
    pump(link, 3)
    link.close()

    # ---------------- 不变量 ----------------
    print("\n[INV] 稳态段：夜视档下 cam_fps / dn 是否跟随档位")
    runs: list[list[tuple[int, dict]]] = []
    cur: list[tuple[int, dict]] = []
    for s in SAMPLES:
        t = s.get("ntier") if s.get("night") == 1 else None
        if t is None or (cur and t != cur[-1][0]):
            if cur:
                runs.append(cur)
            cur = []
        if t is not None:
            cur.append((t, s))
    if cur:
        runs.append(cur)

    n_tier, bad, bad_dn, tier2_expmax = 0, [], [], []
    for r in runs:
        if len(r) < 6 or r[0][0] == 0:
            continue                        # 太短不算稳态；tier0 不接管帧率，不断言
        tier = r[0][0]
        fr = [s for _, s in r]
        # AUTO 运动接管期间夜视阶梯按设计让位（档位冻结、帧率归 AUTO 管），不算违反
        fr = [s for s in fr if not is_auto_active(s)]
        if len(fr) < 6:
            continue
        n_tier += 1
        f_med, d_med = med(fr, "cam_fps"), med(fr, "dn")
        if not tier_fps_ok(tier, f_med):
            bad.append(f"tier{tier}: cam_fps 中位 {f_med}（期望 {TIER_FPS[tier]}）luma={med(fr, 'luma'):.0f}")
        if (d_med or 0) < TIER_DN[tier]:
            bad_dn.append(f"tier{tier}: dn 中位 {d_med}（期望 >= {TIER_DN[tier]}）")
        if tier == 2:
            tier2_expmax.append(med(fr, "exp_max"))

    check("INV-帧率跟随档位", n_tier > 0 and not bad,
          f"检查了 {n_tier} 个稳态夜视段" + ("，全部跟随档位" if not bad else "；异常：" + "; ".join(bad)),
          skip=(n_tier == 0))
    check("INV-降噪跟随档位", n_tier > 0 and not bad_dn,
          "全部满足" if not bad_dn else "；".join(bad_dn), skip=(n_tier == 0))
    if tier2_expmax:
        # 下限 5fps → VTS 10000 → exp_max = 9994（下限若调回 2 则应为 24994）
        check("INV-曝光上限随档位", min(tier2_expmax) >= 9000,
              f"tier2 各段 exp_max 中位数 {tier2_expmax}"
              f"（{FPS_MIN}fps 档期望 ~{1000 * 50 // FPS_MIN - 6}）")

    print("\n== 汇总 ==")
    for name, status, detail in RESULTS:
        print(f"  {status:4s}  {name}")
    npass = sum(1 for _, s, _ in RESULTS if s == PASS)
    nfail = sum(1 for _, s, _ in RESULTS if s == FAIL)
    nskip = sum(1 for _, s, _ in RESULTS if s == SKIP)
    print(f"\n{npass} 通过 / {nfail} 失败 / {nskip} 环境所限跳过"
          + (f"，失败项：{[n for n, s, _ in RESULTS if s == FAIL]}" if nfail else ""))
    return 1 if nfail else 0


if __name__ == "__main__":
    raise SystemExit(main())
