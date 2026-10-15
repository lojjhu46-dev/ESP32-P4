#!/usr/bin/env python3
"""_face_r1_test.py -- 白天识别第 1 轮（F1 多帧注册 / F2 连续帧确认 / F3 双阈值）
端到端验收脚本。单条 TCP 连接，用 esp-dl 官方测试人脸图（PC 源）跑完整流程。

⚠️ 测试方法要点：
- 注册武装后"下一个带脸的帧"会被采集（含 PC 送帧间隙的摄像头帧）——必须先
  {"cmd":"src","value":"pc"} 再注册。
- 平滑窗跨注册/跨阶段会残留，每个独立阶段前 clear（clear 会重置平滑窗）。
- 偶发单帧解码异常（sim 跳变，~1/5 概率）属既有现象，F2 平滑窗的设计目的
  就是滤掉它——若某帧 sim_raw 跳变，多数帧投票仍应给出正确结论。

流程（每阶段独立）：
  P1 多帧注册: clear -> register bill shots=3 -> bill2 反复送 -> enrolling x2 -> registered
  P2 单帧注册: clear -> register musk -> musk1 -> registered（兼容旧路径）
  P3 HI 快路径: musk 自图 sim~0.99 >= HI -> 第 3 帧起 match（M=3）
     + F7 断言: 主脸 box[0] 第 5/6 位带 name/sim
     + F6 断言: match 后的后续帧 event 回落 none（match/stable 字段照常）
  P4 灰区路径（自适应阈值）: 先用"不可能命中"的 0.98/0.99 探测 musk2 本会话
     的相似度分布（esp-dl 会话模式决定它聚在 ~0.77 或 ~0.42，跨构建不可复现，
     见 HANDOVER 廿一节），据此设灰区 LO=med-0.08 / HI=med+0.05；发一张无脸图
     清空平滑窗后，从空窗验证灰区需 M+1=4 帧 -> match
  P4.5 F2 快速通道: thr 0.98/0.99（正常路径必不触发）+ fast=中位数-0.05
     （P4 观测的 musk2 raw，跨会话模式鲁棒）-> 单帧即 match（不等票数）
  P5 陌生人路径: 先关 F6 去重与 F2 快速通道（避免上一段事件/高分帧干扰），
     thr_lo 抬到 0.85 -> musk2 全窗 < LO -> 稳定 reject
  P6 阈值恢复(0.61/0.61) + F6 去重恢复 + F2 快速通道恢复(0.80) + src=cam
"""
import json
import os
import sys
import time

from protocol import Link

FACES = r"D:\esp32project\ESP32-P4\esp-dl\examples\human_face_recognition\faces"
TCP = "192.168.33.2:3333"
NOFACE_JPG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_noface.jpg")

results = []


def make_noface_jpg():
    """无脸图（发一张让板端 smooth 窗清空，模拟"人脸离开"）：纯色块，不会出脸。"""
    from PIL import Image
    Image.new("RGB", (320, 240), (40, 44, 52)).save(NOFACE_JPG, quality=80)


def check(tag, ok, detail=""):
    results.append((tag, ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {tag} {detail}")


def drain_results(link, quiet=0.35, max_s=2.5):
    """清掉板端残留的 0x02 结果帧（上一轮迟到的回包、切换瞬间在途的摄像头帧）。

    ⚠️ 这是本套件历史失败（9~11/15）的修复：每轮断言吃到上一轮的残留结果帧，
    逐帧计数与事件断言整体错位一帧。src=pc 空闲时板子不推帧（实测 3s 0 条），
    所以读到安静即停。每张测量图发送前都必须先 drain——保证"读到的那条结果
    一定是刚发的这张图的回复"。
    """
    n = 0
    deadline = time.monotonic() + max_s
    while time.monotonic() < deadline:
        if link.recv_result(timeout=quiet) is None:
            break
        n += 1
    return n


def send_img_get_result(link, path, timeout=8.0, drain=True):
    if drain:
        drain_results(link)
    with open(path, "rb") as fp:
        link.send_jpeg(fp.read())
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = link.recv_result(timeout=1.0)
        if frame is None:
            continue
        j = json.loads(frame[2].decode("utf-8", "replace"))
        if "ack" in j:
            # 命令应答不是帧结果：中枢保活会补发 stream on，其 ack（faces=0,
            # event=none）会混进结果流——不过滤会把这些帧当成"没有脸的帧"，
            # 读数整体错位（2026-10-06 经中枢跑回归踩到）
            continue
        return j
    return None


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", default=TCP,
                    help="板子或取景中枢地址；中枢在跑时必须连 127.0.0.1:13333"
                         "（板子一次只服务一个客户端）")
    args = ap.parse_args()
    make_noface_jpg()
    link = Link(tcp=args.tcp)
    try:
        link.cmd_and_wait({"cmd": "src", "value": "pc"})

        # ---- P1 多帧注册 shots=3 ----
        print("[P1] multi-shot enrollment")
        link.cmd_and_wait({"cmd": "clear"})
        ack = link.cmd_and_wait({"cmd": "register", "id": "bill", "shots": 3})
        check("register shots=3 ack", "collect 3" in (ack.get("msg") or ""), f"msg={ack.get('msg')}")
        got_enrolling, got_registered = 0, False
        for i in range(10):
            j = send_img_get_result(link, os.path.join(FACES, "bill2.jpg"))
            if j is None:
                continue
            if j.get("event") == "enrolling":
                got_enrolling += 1
            if j.get("event") == "registered":
                got_registered = True
                print(f"        final: {j.get('msg')}")
                break
        check("enrolling progress events", got_enrolling >= 1, f"x{got_enrolling}")
        check("registered after 3 shots", got_registered)

        # ---- P2 单帧注册（兼容旧路径）----
        print("[P2] single-shot enrollment (legacy)")
        link.cmd_and_wait({"cmd": "clear"})
        ack = link.cmd_and_wait({"cmd": "register", "id": "musk"})
        check("register single ack", "collect 1" in (ack.get("msg") or ""), f"msg={ack.get('msg')}")
        j = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
        check("single-frame registered", j is not None and j.get("event") == "registered",
              f"event={j and j.get('event')}")

        # ---- P3 HI 快路径：thr=0.50/0.60，M=3 帧确认 ----
        # （提取管线存在既有非确定性：同图跨帧 sim 会漂移 ±0.3，故用低 HI 只验证
        #   状态机力学；匹配帧的 sim_raw 可能是野值——窗口最大值携带 match 是设计行为）
        print("[P3] HI fast path (thr=0.50/0.60, M=3)")
        link.cmd_and_wait({"cmd": "clear"})          # clear 同时重置平滑窗
        link.cmd_and_wait({"cmd": "register", "id": "musk"})
        j = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
        check("  re-enrolled musk", j is not None and j.get("event") == "registered")
        r = link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.50, "thr_hi": 0.60,
                               "fast_thr": 0})
        check("set thr 0.50/0.60 (fast off)", "thr=0.50/0.60" in (r.get("msg") or "")
              and "fast=0.00" in (r.get("msg") or ""), f"msg={r.get('msg')}")
        seq = []
        mj = None
        for i in range(8):
            j = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
            if j is None:
                continue
            if j.get("reuse") == 1:
                continue          # F4 复用帧：沿用上帧结论，不是新测量，不计入确认帧数
            seq.append((j.get("event"), j.get("match"), j.get("sim"), j.get("sim_raw"), j.get("stable")))
            if j.get("event") == "match":
                mj = j
                break
        matched = [s for s in seq if s[0] == "match"]
        check("HI match reached", bool(matched), f"seq={[(s[0], round(s[2] or 0, 2)) for s in seq]}")
        if matched:
            check("  name=musk", matched[-1][1] == "musk")
            check("  stable=1", matched[-1][4] == 1)
            check("  window sim>=HI(0.60)", (matched[-1][2] or 0) >= 0.60,
                  f"sim={matched[-1][2]:.3f} raw={matched[-1][3]:.3f}")
            check("  confirmed on frame 3 (M)", len(seq) == 3, f"frames={len(seq)}")
            # F7 多人脸：主脸 box 第 5/6 位带 name/sim（单脸图只有主脸一个条目）
            b0 = ((mj or {}).get("boxes") or [[]])[0]
            check("  F7 primary box name/sim", len(b0) >= 6 and b0[4] == "musk",
                  f"box0={b0}")
        # F6 事件去重：match 后的后续帧 event 回落 none（match/stable 字段照常，
        # UI 名字不闪断）——复用帧回放的结论同样会被门住，两种帧都应满足
        jd = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
        check("  F6 dedup: repeat match -> none (match kept)",
              jd is not None and jd.get("event") == "none" and jd.get("match") == "musk"
              and jd.get("stable") == 1,
              f"event={jd and jd.get('event')} match={jd and jd.get('match')} "
              f"stable={jd and jd.get('stable')} reuse={jd and jd.get('reuse')}")

        # ---- P4 灰区路径（自适应阈值，跨会话鲁棒）----
        # esp-dl 的"会话模式"决定 musk2（同人跨照片）的相似度聚在 ~0.77 或 ~0.42
        # （执行历史/内存布局决定，跨构建不可复现，HANDOVER 廿一节）——固定
        # 0.70/0.85 只在 0.77 模式下成立。做法：先用"不可能命中"的 0.98/0.99 探测
        # musk2 本会话分布（窗口不满 5 帧不会触发稳定 reject），据此设灰区
        # LO=med-0.08 / HI=med+0.05，再发一张无脸图清空平滑窗，从空窗验证 M+1。
        print("[P4] gray zone (adaptive thr around musk2's session-mode sim)")
        link.cmd_and_wait({"cmd": "clear"})          # 清掉 P3 的窗口历史
        link.cmd_and_wait({"cmd": "register", "id": "musk"})
        j = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
        check("  re-enrolled musk", j is not None and j.get("event") == "registered")
        r = link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.98, "thr_hi": 0.99,
                               "fast_thr": 0})
        check("  probe thr 0.98/0.99 (cannot match)", "thr=0.98/0.99" in (r.get("msg") or ""),
              f"msg={r.get('msg')}")
        probe = []
        for i in range(6):
            if len(probe) >= 3:
                break
            j = send_img_get_result(link, os.path.join(FACES, "musk2.jpg"))
            if j is None or j.get("reuse") == 1:
                continue
            probe.append(j.get("sim_raw") or 0.0)
        check("  probed musk2 x3", len(probe) == 3, f"sims={[round(s, 3) for s in probe]}")
        med = sorted(probe)[1] if len(probe) == 3 else 0.75
        lo = round(max(0.30, med - 0.08), 2)
        hi = round(min(0.97, med + 0.05), 2)
        print(f"        musk2 session sims med={med:.3f} -> gray LO={lo} HI={hi}")
        j = send_img_get_result(link, NOFACE_JPG)    # 无脸帧：板端 smooth 窗清空
        check("  no-face frame clears window", j is not None and j.get("faces") == 0,
              f"faces={j and j.get('faces')}")
        hi_eff = max(lo, hi)      # 板端恒保持 hi >= lo（med 很低时 hi 会被钳到 lo）
        r = link.cmd_and_wait({"cmd": "mode", "thr_lo": lo, "thr_hi": hi,
                               "fast_thr": 0})
        check("  adaptive thr set", f"thr={lo:.2f}/{hi_eff:.2f}" in (r.get("msg") or ""),
              f"msg={r.get('msg')}")
        seq = []
        for i in range(10):
            j = send_img_get_result(link, os.path.join(FACES, "musk2.jpg"))
            if j is None:
                continue
            if j.get("reuse") == 1:
                continue          # F4 复用帧不计数（同 P3）
            seq.append((j.get("event"), j.get("sim_raw")))
            if j.get("event") == "match":
                break
        print(f"        trace: {[(e, round(s or 0, 3)) for e, s in seq]}")
        check("gray-zone match (musk)", seq and seq[-1][0] == "match", f"frames={len(seq)}")
        # 从空窗起：灰区（best_sim<HI）需 M+1=4 帧；单帧野值冲过 HI 时 need 降为
        # M=3 -> 第 3 帧确认，同属状态机正确行为 -> 3~5 帧均算过
        check("  gray confirm within 5 frames (M+1)", len(seq) in (3, 4, 5), f"frames={len(seq)}")

        # ---- P4.5 F2 快速通道：单帧高分不等票数直接确认 ----
        # fast 值按 P4 观测 raw 的 75 分位 - 0.05 校准（比中位数鲁棒：esp-dl 的
        # 会话模式可能逐帧双峰乱跳，中位数会落在两簇之间偏低，max 又被单帧
        # 野值抬高）；thr 0.98/0.99 时正常路径需 best_sim>=0.98（从未达到），
        # 本段任何 match 必来自快速通道。dedup 关掉（P4 刚报过 musk 的 match，
        # 5s 去重窗口会把它抑制成 none——F6 正确行为，机制验证必须解耦）。
        print("[P4.5] F2 fast-confirm (single high frame, no votes needed)")

        def p75(xs):
            s = sorted(xs)
            return s[min(len(s) - 1, int(len(s) * 0.75))]

        all_raws = [s for s in probe if s] + [s for _, s in seq if s]
        fired, detail = False, ""
        for round_i in range(2):
            fast = round(max(0.30, min(0.97, p75(all_raws) - 0.05)), 2) if all_raws else 0.75
            link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.98, "thr_hi": 0.99,
                               "fast_thr": fast, "dedup_ms": 0})
            for i in range(4):
                j = send_img_get_result(link, os.path.join(FACES, "musk2.jpg"))
                if j is None or j.get("reuse") == 1:
                    continue
                if j.get("sim_raw"):
                    all_raws.append(j["sim_raw"])
                if j.get("event") == "match" and j.get("match") == "musk":
                    fired = True
                    detail = f"round{round_i} fast={fast} sim={j.get('sim'):.3f} frame#{i + 1}"
                    break
            if fired:
                break
        check("  fast-confirm fired (single frame)", fired, detail)

        # ---- P5 陌生人路径：thr_lo 抬到 0.85，全部帧 < LO -> 稳定 reject ----
        # 陌生人用 bill2（不同人，各会话模式下单帧 sim 0.01~0.31，恒 < 0.85）；
        # ⚠️ 不能用 musk2：同人跨照片，在 0.99 会话模式里它与 musk1 同分（实测
        # 0.997），"全窗 < LO" 的前提不成立。先关 F6 去重与 F2 快速通道：
        # 上一阶段刚报过 match/reject 事件，5s 窗口会把本段的稳定 reject 抑制成
        # none（F6 的正确行为）——那是事件层语义，不属于本段断言。
        print("[P5] stranger (thr_lo raised, all frames below LO)")
        link.cmd_and_wait({"cmd": "mode", "dedup_ms": 0, "fast_thr": 0})
        r = link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.85, "thr_hi": 0.90})
        check("set thr_lo=0.85", "thr=0.85/0.90" in (r.get("msg") or ""), f"msg={r.get('msg')}")
        seq = []
        for i in range(10):
            j = send_img_get_result(link, os.path.join(FACES, "bill2.jpg"))
            if j is None:
                continue
            if j.get("reuse") == 1 and j.get("event") != "reject":
                continue          # F4 复用帧不计数；但复用出的稳定 reject 仍是有效结论
            seq.append((j.get("event"), j.get("stable")))
            if j.get("event") == "reject" and j.get("stable") == 1:
                break
        check("stranger stable reject", bool(seq) and seq[-1][0] == "reject" and seq[-1][1] == 1,
              f"seq={[s[0] for s in seq]}")

        # ---- P6 恢复：阈值（编译默认 0.61/0.61）+ F6 去重 + F2 快速通道 + cam 源 ----
        r = link.cmd_and_wait({"cmd": "mode", "thr_lo": 0.61, "thr_hi": 0.61,
                               "dedup_ms": 5000, "fast_thr": 0.80})
        check("mode thr lo/hi readback",
              "thr=0.61/0.61" in (r.get("msg") or "") and "dedup=5000ms" in (r.get("msg") or "")
              and "fast=0.80" in (r.get("msg") or ""),
              f"msg={r.get('msg')}")
        link.cmd_and_wait({"cmd": "src", "value": "cam"})
    finally:
        link.close()

    fails = [t for t, ok in results if not ok]
    print(f"\n=== {len(results) - len(fails)}/{len(results)} PASS ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
