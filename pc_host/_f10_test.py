"""F10 验收：特征库备份 / 恢复（真机，PC 送图）。

流程：
  P1 注册两个人（f10a / f10b）→ 记录 lib 基线
  P2 db_backup → .bin 落盘 + 信封解析（人数/face.db 长度）
  P3 clear（库空）→ db_restore → lib 与基线逐项一致
  P4 恢复后识别仍可用（送 musk1，要求出现 match）
  P5 负向：故意错误的 CRC → 整批拒绝、库不变（并确认板端已写 .bak）

用法：python pc_host/_f10_test.py [--tcp 192.168.33.2:3333]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from protocol import Link  # noqa: E402

FACES = r"D:\esp32project\ESP32-P4\esp-dl\examples\human_face_recognition\faces"
BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_f10_backup.bin")

results: list[tuple[bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    results.append((bool(ok), name))
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {name} {detail}")


def drain_results(link: Link, quiet: float = 0.35) -> int:
    n = 0
    end = time.monotonic() + quiet
    while time.monotonic() < end:
        if link.recv_result(timeout=0.1) is None:
            continue
        n += 1
        end = time.monotonic() + quiet
    return n


def send_img_get_result(link: Link, path: str, timeout: float = 8.0, drain: bool = True):
    if drain:
        drain_results(link)
    with open(path, "rb") as fp:
        link.send_jpeg(fp.read())
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = link.recv_result(timeout=0.5)
        if frame is None:
            continue
        try:
            obj = json.loads(frame[2].decode("utf-8", "replace"))
        except ValueError:
            continue
        if obj.get("ack"):
            continue
        return obj
    return None


def enroll(link: Link, name: str, img: str) -> bool:
    link.cmd_and_wait({"cmd": "register", "id": name})
    j = send_img_get_result(link, os.path.join(FACES, img))
    return bool(j and j.get("event") == "registered")


def restore_raw(link: Link, path: str, crc_delta: int = 0, size_delta: int = 0) -> dict | None:
    """按 .bin 回传；crc_delta/size_delta 用于制造负向用例"""
    import struct
    import zlib

    with open(path, "rb") as fp:
        data = fp.read()
    crc = (zlib.crc32(data) & 0xFFFFFFFF) ^ crc_delta
    ack = link.cmd_and_wait({"cmd": "db_import", "size": len(data) + size_delta, "crc": crc},
                            timeout=20.0)
    if ack is None or ack.get("event") == "error":
        return ack
    for off in range(0, len(data), 16 * 1024):
        link.send_block(off, data[off:off + 16 * 1024])
        time.sleep(0.002)
    return link.cmd_and_wait({"cmd": "db_import_end"}, timeout=30.0)


def main() -> int:
    ap = argparse.ArgumentParser(description="F10 备份/恢复验收")
    ap.add_argument("--tcp", default="192.168.33.2:3333")
    args = ap.parse_args()

    link = Link(tcp=args.tcp)
    try:
        link.drain_logs(0.3)
        print("[P1] 注册两人（f10a / f10b）")
        link.cmd_and_wait({"cmd": "clear"})
        link.cmd_and_wait({"cmd": "src", "value": "pc"})
        check("enroll f10a", enroll(link, "f10a", "musk1.jpg"))
        check("enroll f10b", enroll(link, "f10b", "musk2.jpg"))
        base = link.cmd_and_wait({"cmd": "list"}) or {}
        base_lib = list(base.get("lib") or [])
        base_feats = list(base.get("lib_feats") or [])
        check("baseline lib", base_lib == ["f10a", "f10b"], f"{base_lib} feats={base_feats}")

        print("[P2] db_backup → .bin")
        from jc_test import DBX_HDR, db_backup

        check("db_backup ok", db_backup(link, BACKUP))
        ok_file = os.path.exists(BACKUP) and os.path.getsize(BACKUP) > DBX_HDR
        check("backup file written", ok_file,
              f"{os.path.getsize(BACKUP) if os.path.exists(BACKUP) else 0} B")

        print("[P3] clear → db_restore")
        link.cmd_and_wait({"cmd": "clear"})
        empty = link.cmd_and_wait({"cmd": "list"}) or {}
        check("lib cleared", not (empty.get("lib") or []), f"{empty.get('lib')}")
        ack = restore_raw(link, BACKUP)
        check("db_import_end ok", ack is not None and ack.get("event") != "error",
              f"{ack and ack.get('msg')}")
        cur = link.cmd_and_wait({"cmd": "list"}) or {}
        check("lib restored == baseline",
              list(cur.get("lib") or []) == base_lib
              and list(cur.get("lib_feats") or []) == base_feats,
              f"{cur.get('lib')} feats={cur.get('lib_feats')}")
        check("bak log seen", any("dbx backup" in ln for ln in link.logs))

        print("[P4] 恢复后识别仍可用")
        seen_match, seen_faces = None, 0
        for _ in range(8):
            j = send_img_get_result(link, os.path.join(FACES, "musk1.jpg"))
            if j is None:
                continue
            if j.get("faces"):
                seen_faces += 1
            if j.get("match") in base_lib and j.get("stable") == 1:
                seen_match = j.get("match")
                break
        check("match after restore", seen_match is not None,
              f"match={seen_match} face_frames={seen_faces}")

        print("[P5] 负向：错误 CRC 必须被拒绝且库不变")
        before = link.cmd_and_wait({"cmd": "list"}) or {}
        ack = restore_raw(link, BACKUP, crc_delta=0xFFFFFFFF)
        check("bad crc rejected", ack is not None and ack.get("event") == "error",
              f"{ack and ack.get('msg')}")
        after = link.cmd_and_wait({"cmd": "list"}) or {}
        check("lib unchanged after reject",
              list(after.get("lib") or []) == list(before.get("lib") or []))

        print("[P6] 收尾：清库")
        link.cmd_and_wait({"cmd": "clear"})
    finally:
        link.close()

    npass = sum(1 for ok, _ in results if ok)
    print(f"=== {npass}/{len(results)} PASS ===")
    for ok, name in results:
        if not ok:
            print(f"  - FAILED: {name}")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
