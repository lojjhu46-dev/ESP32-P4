#!/usr/bin/env python3
"""thr_calib.py -- 白天人脸识别阈值标定工具（规划 F5，2026-10-04）

原理：固件匹配判定用的是**窗口最优相似度**（最近 K=5 帧里同 id 的最大 sim，
结果帧的 "sim" 字段）；本脚本分两个阶段采集该决策量的真实分布：

  Phase A（注册者）：让已注册的人保持在镜头前，采集 N 秒
  Phase B（陌生人） ：让镜头前是没有注册过的人（换陌生人上来），采集 N 秒

两个阶段**都必须有脸在画面里**（脚本靠 faces>0 取样本；Phase B 空画面会
没数据直接报 insufficient）。

⚠️ 结果帧 "sim" 字段的坑（2026-10-05 干跑实测）：固件只在 match / tracking
状态下把窗口最优值写进 "sim"；一旦整窗判定为稳定陌生人（reject）就归零。
所以陌生人相位的 "sim" 只在前 K 帧有值。本脚本因此**额外用每帧都带出的
"sim_raw" 重建窗口最大值**（滑窗 K 帧取 max，人脸消失即清窗，与固件
smooth_tally/win_max 同义），两相位都稳定有样本，建议阈值以它为准；
"sim"（固件字段）与 "sim_raw"（单帧无门槛 top-1）作为参考。

已知 esp-dl 提取存在混沌野值（围绕真值跳变，HANDOVER 廿一节）：单帧
sim_raw 分布天然比窗口分布宽，阈值应以窗口分布为准。

前置条件：板端已 start、match 开、src=cam，且库里**已有注册者特征**。

推荐用法（两段式，A 跑完可以停下来换人，再接 B——真人在场时最稳）：
  python thr_calib.py --enroll me          # 1) 先注册（可选，库非空即可）
  python thr_calib.py --phase a --a 20     # 2) 注册者站镜头前 20s，样本存 --json
  python thr_calib.py --phase b --b 20     # 3) 换成陌生人，读回 A 并给出建议阈值
一次跑完（中间用倒计时换人，容易换不及）：
  python thr_calib.py --tcp 192.168.33.2:3333 --a 20 --swap 25 --b 20
依赖：仅标准库 + 同目录 protocol.py。
"""
import argparse
import collections
import json
import os
import sys
import time

from protocol import Link

# 必须与固件 main/app_faceai.cpp 的 FACE_SMOOTH_K 一致（窗口太短/太长都会
# 让"重建窗口最大值"与固件实际判据错位）
FW_SMOOTH_K = 5
# 必须与固件 FACE_SMOOTH_M 一致：窗口内同 id 票数达到该值（灰区需 +1）才确认 match
FW_SMOOTH_M = 3


def pct(sorted_vals, p):
    """p in 0..1，线性插值分位数"""
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def collect(link, seconds, phase, k=FW_SMOOTH_K):
    """收 seconds 秒结果帧，返回 dict：
      sims   = 结果帧自带的 "sim"（>0 才有值；陌生人相位通常只有前 K 帧）
      raws   = 每帧 "sim_raw"（单帧无门槛 top-1，覆盖全区间）
      winmax = 用 raw 滑窗 K 帧重建的窗口最优 sim（= 固件 best_sim / win_max）
      trace  = 逐帧 [{f: 人脸数, w: 重建 win_max, o: 窗口占用帧数}]，**含无人脸的
               空隙帧**（f=0,o=0，模拟固件 smooth_reset）——用于按固件 F3 规则
               （best_cnt>=need && best_sim>=thr_lo）复算逐帧命中率
      events = 事件计数
    """
    sims, raws, winmax, trace = [], [], [], []
    reused = 0                              # F4 节流帧数（沿用上帧结论，不计入分布）
    events = {}
    raw_win = collections.deque(maxlen=k)
    t_end = time.time() + seconds
    prompt = {
        "A": "Phase A: keep the ENROLLED face in view (one person, facing camera)",
        "B": "Phase B: only a NON-ENROLLED person in view (stranger / step away+back)",
    }[phase]
    print(f"--- {prompt} ({seconds}s) ---", flush=True)
    while time.time() < t_end:
        frame = link.recv_result(timeout=1.0)
        if frame is None:
            continue
        try:
            j = json.loads(frame[2].decode("utf-8", "replace"))
        except ValueError:
            continue
        if "ack" in j:                    # 命令应答不是流式帧
            continue
        ev = j.get("event", "?")
        events[ev] = events.get(ev, 0) + 1
        if j.get("reuse"):                  # F4 节流帧：沿用上帧结论，不是新测量
            reused += 1
            continue
        if j.get("faces", 0) > 0:
            s = j.get("sim")
            r = j.get("sim_raw")
            if isinstance(s, (int, float)) and s > 0:
                sims.append(float(s))
            if isinstance(r, (int, float)) and r > 0:
                raws.append(float(r))
                raw_win.append(float(r))
            w = max(raw_win) if raw_win else 0.0
            if raw_win:
                winmax.append(w)                # 重建固件的 win_max
            trace.append({"f": int(j.get("faces", 0)), "w": round(w, 4),
                          "o": len(raw_win)})   # o = 窗口占用帧数（≈固件 best_cnt）
        else:
            raw_win.clear()                     # 人脸离开：固件 smooth_reset
            trace.append({"f": 0, "w": 0.0, "o": 0})
    print(f"    samples: sim={len(sims)} raw={len(raws)} winmax={len(winmax)} "
          f"frames={len(trace)} reuse={reused} events={events}", flush=True)
    return {"sims": sims, "raws": raws, "winmax": winmax,
            "trace": trace, "events": events, "reuse": reused}


def summarize(vals, label):
    if not vals:
        return f"  {label}: (no samples)"
    v = sorted(vals)
    return (f"  {label}: n={len(v)}  min={v[0]:.3f}  p05={pct(v, 0.05):.3f}  "
            f"median={pct(v, 0.5):.3f}  p95={pct(v, 0.95):.3f}  max={v[-1]:.3f}")


def preflight(link):
    """检查板端状态：match 是否开、src 是否 cam、库里是否有注册者"""
    print("=== preflight ===", flush=True)
    mode = link.cmd_and_wait({"cmd": "mode"})
    print(f"  mode  : {mode.get('msg') if mode else '??'}", flush=True)
    lst = link.cmd_and_wait({"cmd": "list"})
    lib = (lst or {}).get("lib") or []
    print(f"  library: {lib} (n={len(lib)})", flush=True)
    if not lib:
        print("  !! library is EMPTY -> Phase A can never produce a match/sim.", flush=True)
        print("     register someone first, e.g.", flush=True)
        print("       python thr_calib.py --enroll <name> --tcp <host:port>", flush=True)
    if mode and "match=1" not in (mode.get("msg") or ""):
        print("  !! match is off; sending {\"cmd\":\"mode\",\"match\":true}", flush=True)
        link.cmd_and_wait({"cmd": "mode", "match": True})
    return lib


def enroll(link, name, shots):
    """在真机摄像头前做多帧注册（Phase A 的前置）"""
    print(f"=== enroll '{name}' with {shots} shots ===", flush=True)
    print("  >>> stand in front of the board camera NOW; keep your face in view",
          flush=True)
    link.cmd_and_wait({"cmd": "clear"})
    ack = link.cmd_and_wait({"cmd": "register", "id": name, "shots": shots})
    print(f"  ack: {ack.get('msg') if ack else '??'}", flush=True)
    t_end = time.time() + 60.0
    while time.time() < t_end:
        frame = link.recv_result(timeout=1.0)
        if frame is None:
            continue
        try:
            j = json.loads(frame[2].decode("utf-8", "replace"))
        except ValueError:
            continue
        ev = j.get("event")
        if ev == "enrolling":
            print(f"  [{j.get('msg')}]", flush=True)
        elif ev == "registered":
            print(f"  registered: {j.get('msg')} (match={j.get('match')})", flush=True)
            return True
        elif ev == "error":
            print(f"  !! error: {j.get('msg')}", flush=True)
            return False
    print("  !! enroll timeout (60s) - keep a well-lit face in view", flush=True)
    return False


DEFAULT_A_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "_thr_calib_A.json")


def save_phase(path, phase, data, k):
    d = dict(data)
    d["phase"] = phase
    d["k"] = k
    d["saved"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(d, fp)
    print(f"  -> saved phase {phase} to {path}", flush=True)


def phase_b_path(apath):
    """B 相位样本文件：与 A 同目录（_thr_calib_A.json -> _thr_calib_B.json）"""
    if apath.endswith("_A.json"):
        return apath[:-7] + "_B.json"
    return apath + ".B"


def load_phase(path):
    try:
        with open(path, encoding="utf-8") as fp:
            return json.load(fp)
    except (OSError, ValueError):
        return None


def hit_frames(d, thr_lo, m=FW_SMOOTH_M):
    """按固件 F3 规则复算该相位有多少帧会报 match：
      need = (win_max >= thr_hi) ? M : M+1；判定 = best_cnt >= need && best_sim >= thr_lo
    单人库时 best_cnt 就是窗口占用帧数（trace 的 o）；旧样本无 trace 则按满窗（上界）估。"""
    thr_hi = min(0.99, thr_lo + 0.10)
    tr = d.get("trace")
    if tr:
        n = 0
        for e in tr:
            if e["o"] <= 0 or e["w"] <= 0.0:
                continue
            need = m if e["w"] >= thr_hi else m + 1
            if e["o"] >= need and e["w"] >= thr_lo:
                n += 1
        return n
    return sum(1 for v in (d.get("winmax") or []) if v >= thr_lo)


def n_frames(d):
    tr = d.get("trace")
    return len(tr) if tr else len(d.get("winmax") or [])


def report(a, b, tcp):
    """a / b = 相位数据 dict（或 None）"""
    print("\n=== Phase A (enrolled in view) ===")
    if a:
        k = a.get("k", FW_SMOOTH_K)
        print(summarize(a["winmax"], f"win-max(k={k}) decision var"))
        print(summarize(a["sims"], 'firmware "sim"'))
        print(summarize(a["raws"], "single-frame sim_raw (noisy)"))
        print(f"  events: {a['events']}  (saved {a.get('saved', '?')})")
    else:
        print("  (not run)")
    print("=== Phase B (stranger only) ===")
    if b:
        k = b.get("k", FW_SMOOTH_K)
        print(summarize(b["winmax"], f"win-max(k={k}) 判定量"))
        print(summarize(b["sims"], 'firmware "sim"'))
        print(summarize(b["raws"], "single-frame sim_raw (noisy)"))
        print(f"  events: {b['events']}")
    else:
        print("  (not run)")

    if not a or not b or not a["winmax"] or not b["winmax"]:
        print("\n!! need BOTH phases with a face in view to recommend a threshold"
              "\n   (Phase A = the ENROLLED person, Phase B = a DIFFERENT, non-enrolled person)"
              "\n   -> run the missing phase with the same --json file")
        return 1

    aa = sorted(a["winmax"])
    bb = sorted(b["winmax"])
    a_med, b_med = pct(aa, 0.5), pct(bb, 0.5)
    na, nb = n_frames(a), n_frames(b)
    traced = bool(a.get("trace"))

    print("\n=== 按固件 F3 规则复算逐帧命中 ===")
    print(f"  判据: win-max(k={FW_SMOOTH_K}) >= thr_lo 且 窗口占用 >= M({FW_SMOOTH_M})"
          f"~M+1 帧(灰区更严)")
    print(f"  A({na} 帧) median {a_med:.3f}   vs   B({nb} 帧) median {b_med:.3f}"
          f"   [{'逐帧 trace 复算' if traced else 'A 为旧样本：无空隙标记，按满窗上界估'}]")
    print("    thr_lo      A 命中(本人，越高越好)        B 误命中(陌生人，越低越好)")
    for t in (0.50, 0.55, 0.58, 0.60, 0.62, 0.65, 0.68, 0.70, 0.75, 0.80):
        ha, hb = hit_frames(a, t), hit_frames(b, t)
        print(f"    {t:.2f}       {ha:4d}/{na} ({100 * ha / max(na, 1):5.1f}%)"
              f"              {hb:4d}/{nb} ({100 * hb / max(nb, 1):5.1f}%)")

    # B 相位"能触发判定"的窗口判定量上沿 —— thr_lo 必须高于它才可能不误报
    btr = b.get("trace")
    hi_b = max([e["w"] for e in btr if e["o"] > 0], default=None) if btr else bb[-1]
    lo_safe = min(0.95, (int(hi_b * 100 + 0.999) + 2) / 100.0)   # 上取整 0.01 + 0.02 余量
    hi_safe = min(0.98, lo_safe + 0.10)
    ha_s, hb_s = hit_frames(a, lo_safe), hit_frames(b, lo_safe)
    cmd = "{\"cmd\":\"mode\",\"thr_lo\":%.2f,\"thr_hi\":%.2f}" % (lo_safe, hi_safe)

    print("\n=== Recommendation ===")
    print(f"  B 相位触发判定的判定量上沿 = {hi_b:.3f}  ->  thr_lo 必须高于它")
    print(f"  suggested: thr_lo={lo_safe:.2f}  thr_hi={hi_safe:.2f}"
          f"    (A 命中 {ha_s}/{na} = {100 * ha_s / max(na, 1):.0f}%，"
          f"B 误命中 {hb_s}/{nb})")
    margin = lo_safe - hi_b
    print(f"  余量 = thr_lo - B 上沿 = {margin:+.3f}"
          + ("   (偏紧：建议再采一次 B / 加注册帧数)" if margin < 0.03 else ""))
    if hi_b >= 0.90:
        print("  !! B 上沿已到 0.90 → 单阈值分离不开：加注册帧数 / 改光照 / 考虑 MBF_S8_V1")
    if not traced:
        print("  ⚠️ A 是旧样本（无逐帧 trace）：上表 A 命中率是**未计窗口票数的上界**；"
              "用新版脚本重采 --phase a 才得到逐帧真值")
    print(f"  apply  : python jc_test.py --tcp {tcp} cmd '{cmd}'")
    print(f"  persist: CONFIG_APP_FACEAI_MATCH_THR_LO={int(round(lo_safe * 100))} "
          f"/ _HI={int(round(hi_safe * 100))} in sdkconfig.defaults")
    print("  (单人库是误报最坏情况：多人库要求同一 id 的票数更严；"
          "B 样本帧数有限，落地后需真机复验)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="face match threshold calibrator")
    ap.add_argument("--tcp", default="192.168.33.2:3333")
    ap.add_argument("--a", type=int, default=15, help="phase A seconds (enrolled in view)")
    ap.add_argument("--b", type=int, default=15, help="phase B seconds (stranger in view)")
    ap.add_argument("--k", type=int, default=FW_SMOOTH_K,
                    help=f"window K for reconstruction (must match firmware, default {FW_SMOOTH_K})")
    ap.add_argument("--phase", choices=("ab", "a", "b"), default="ab",
                    help="ab = both in one run (with --swap countdown); a = only A and save; "
                         "b = only B (loads A and prints the recommendation)")
    ap.add_argument("--json", default=None,
                    help=f"phase-A sample file (default {os.path.basename(DEFAULT_A_JSON)} "
                         f"next to this script)")
    ap.add_argument("--enroll", metavar="NAME",
                    help="first register NAME (multi-shot, camera) then exit")
    ap.add_argument("--shots", type=int, default=3, help="shots for --enroll (1~5)")
    ap.add_argument("--swap", type=int, default=15,
                    help="countdown seconds between phase A and B (--phase ab only)")
    args = ap.parse_args()
    ajson = args.json or DEFAULT_A_JSON

    link = Link(tcp=args.tcp)
    a = b = None
    try:
        if args.enroll:
            return 0 if enroll(link, args.enroll, args.shots) else 1

        lib = preflight(link)
        if not lib:
            return 1

        if args.phase in ("ab", "a"):
            da = collect(link, args.a, "A", args.k)
            save_phase(ajson, "A", da, args.k)
            da["k"] = args.k
            da["saved"] = time.strftime("%Y-%m-%d %H:%M:%S")
            a = da
            print("", flush=True)
            if args.phase == "a":
                print(f"Phase A done. Now get a DIFFERENT (non-enrolled) person in view,"
                      f" then run:\n  python {os.path.basename(__file__)} "
                      f"--tcp {args.tcp} --phase b --json \"{ajson}\"", flush=True)
        if args.phase == "ab" and args.swap > 0:
            print(f"=== SWAP NOW: enrolled person leaves, STRANGER steps in "
                  f"({args.swap}s) ===", flush=True)
            for left in range(args.swap, 0, -1):
                print(f"    ...{left}", flush=True)
                time.sleep(1.0)
        if args.phase in ("ab", "b"):
            db = collect(link, args.b, "B", args.k)
            db["k"] = args.k
            db["saved"] = time.strftime("%Y-%m-%d %H:%M:%S")
            save_phase(phase_b_path(ajson), "B", db, args.k)
            b = db
            if args.phase == "b":
                a = load_phase(ajson)
                if a is None:
                    print(f"  !! no phase-A file at {ajson} - run --phase a first",
                          flush=True)
    finally:
        link.close()

    return report(a, b, args.tcp)


if __name__ == "__main__":
    sys.exit(main())
