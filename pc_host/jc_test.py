"""jc_test.py —— JCLINK 链路自测（板端任务四/五的验证工具，也是 host_app.py 的地基）

用法：
    # 网线（板端默认静态 IP）
    python jc_test.py --tcp 192.168.33.2:3333 start
    python jc_test.py --tcp 192.168.33.2:3333 frame faces/musk2.jpg
    python jc_test.py --tcp 192.168.33.2:3333 register bill faces/bill1.jpg
    python jc_test.py --tcp 192.168.33.2:3333 frame faces/bill2.jpg faces/musk2.jpg

    # USB CDC（P4 原生 USB 口枚举出来的 COM 口，不是 CH340）
    python jc_test.py --serial COM5 list

子命令：start / stop / list / clear / mode / frame / register / delete / demo
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from protocol import Link  # noqa: E402


def show(obj: dict | None, prefix: str = "<-") -> dict | None:
    if obj is None:
        print(f"{prefix} (超时，没有收到结果帧)")
        return None
    print(f"{prefix} {json.dumps(obj, ensure_ascii=False)}")
    return obj


def send_frames(link: Link, files: list[str], timeout: float = 15.0) -> None:
    for f in files:
        with open(f, "rb") as fp:
            jpeg = fp.read()
        t0 = time.monotonic()
        link.send_jpeg(jpeg)
        frame = link.recv_result(timeout)
        obj = None if frame is None else json.loads(frame[2].decode("utf-8", "replace"))
        show(obj, prefix=f"<- {os.path.basename(f)} {len(jpeg)}B")
        if obj is not None:
            print(f"   wall {1000 * (time.monotonic() - t0):.0f} ms, board infer_ms={obj.get('infer_ms')}")


# ---------------- F10：特征库备份 / 恢复 ----------------
# 备份信封（与板端 app_faceai.cpp 逐字节一致）：
#   magic "JCDF"(4) | ver u32 | face_len u32 | names_len u32 | names(JSON) | face.db
# 传输：0x06 分片，payload = offset(u32 LE) + data
DBX_MAGIC = 0x4644434A          # "JCDF"
DBX_VER = 1
DBX_HDR = 16
DBX_CHUNK = 16 * 1024


def db_backup(link: Link, path: str) -> bool:
    """db_export → 收齐 0x06 分片 → CRC32 校验 → 落盘 .bin"""
    ack = link.cmd_and_wait({"cmd": "db_export"}, timeout=20.0)
    if ack is None or "dbx" not in ack:
        print("<- db_export 失败（板端未返回 dbx 字段）")
        return False
    dbx = ack["dbx"]
    size, crc, parts = int(dbx["size"]), int(dbx["crc"]), int(dbx["parts"])
    print(f"<- db_export: {size} B / {parts} part(s) crc={crc:08x}")

    buf = bytearray(size)
    seen: set[int] = set()
    got = 0
    deadline = time.monotonic() + 60.0
    while got < size and time.monotonic() < deadline:
        blk = link.recv_block(timeout=2.0)
        if blk is None:
            continue
        off, data = blk
        if off in seen:
            continue
        if off + len(data) > size:
            print(f"! 分片越界 off={off} len={len(data)} (size={size})")
            return False
        seen.add(off)
        buf[off:off + len(data)] = data
        got += len(data)
    if got != size:
        print(f"! 分片不齐: {got}/{size}")
        return False

    calc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF
    if calc != crc:
        print(f"! CRC 不符: 板端 {crc:08x} / 本地 {calc:08x}")
        return False

    with open(path, "wb") as fp:
        fp.write(buf)
    magic, ver, face_len, names_len = struct.unpack_from("<IIII", buf, 0)
    names = []
    if names_len:
        try:
            names = json.loads(bytes(buf[DBX_HDR:DBX_HDR + names_len]).decode("utf-8", "replace"))
        except ValueError:
            names = []
    who = ", ".join(n.get("name", "?") for n in names) if names else "(空)"
    print(f"<- 已保存 {path}: {len(buf)} B, face.db {face_len} B, {len(names)} person(s): {who}")
    return True


def db_restore(link: Link, path: str) -> bool:
    """读 .bin → db_import → 分片回传 → db_import_end（覆盖式恢复）"""
    with open(path, "rb") as fp:
        data = fp.read()
    if len(data) < DBX_HDR:
        print("! 文件太小，不是备份文件")
        return False
    magic, ver, face_len, names_len = struct.unpack_from("<IIII", data, 0)
    if magic != DBX_MAGIC or ver != DBX_VER:
        print(f"! 信封格式不符 (magic=0x{magic:08x} ver={ver})")
        return False
    crc = zlib.crc32(data) & 0xFFFFFFFF

    ack = link.cmd_and_wait({"cmd": "db_import", "size": len(data), "crc": crc}, timeout=20.0)
    if ack is None or ack.get("event") == "error":
        print(f"<- db_import 被拒绝: {ack}")
        return False
    print(f"<- db_import ready ({len(data)} B, crc={crc:08x})")

    n = 0
    for off in range(0, len(data), DBX_CHUNK):
        link.send_block(off, data[off:off + DBX_CHUNK])
        n += 1
        time.sleep(0.002)       # 小睡：给板端 rx 环留喘息，别挤掉在途命令帧
    print(f"-> 已发送 {n} 块")

    ack = link.cmd_and_wait({"cmd": "db_import_end"}, timeout=30.0)
    show(ack)
    return ack is not None and ack.get("event") != "error"


def main() -> int:
    ap = argparse.ArgumentParser(description="JCLINK 链路自测")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--tcp", help="host:port，如 192.168.33.2:3333")
    g.add_argument("--serial", help="CDC 串口，如 COM5")
    ap.add_argument("--timeout", type=float, default=15.0, help="等结果帧的超时(秒)")

    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("start")
    sub.add_parser("stop")
    sub.add_parser("list")
    sub.add_parser("clear")
    p_mode = sub.add_parser("mode")
    p_mode.add_argument("--detect", type=int, choices=(0, 1), default=None)
    p_mode.add_argument("--match", type=int, choices=(0, 1), default=None)
    p_mode.add_argument("--thr", type=float, default=None)

    p_frame = sub.add_parser("frame")
    p_frame.add_argument("files", nargs="+")

    p_reg = sub.add_parser("register")
    p_reg.add_argument("name")
    p_reg.add_argument("files", nargs="+")

    p_del = sub.add_parser("delete")
    p_del.add_argument("name")

    p_src = sub.add_parser("src", help="切换图像源: cam(板载摄像头) | pc(PC 送帧)")
    p_src.add_argument("value", choices=("cam", "pc"))

    p_watch = sub.add_parser("watch", help="只收不发的监视模式：打印板端推来的结果帧")
    p_watch.add_argument("seconds", nargs="?", type=float, default=10.0)

    p_stream = sub.add_parser("stream", help="图像回传开关（0x05）：on/off/query")
    p_stream.add_argument("value", nargs="?", default="on", choices=("on", "off", "query"))
    p_stream.add_argument("--fps", type=int, default=None, help="回传帧率上限 1~30")
    p_stream.add_argument("--q", type=int, default=None, help="JPEG 质量 1~100")

    p_img = sub.add_parser("img", help="收一帧板端图像并存盘（自动开 stream）")
    p_img.add_argument("out", nargs="?", default="preview.jpg")
    p_img.add_argument("--fps", type=int, default=5)
    p_img.add_argument("--seconds", type=float, default=8.0)

    p_prev = sub.add_parser("preview", help="收 N 秒 图像+结果，统计帧率并保存最后一帧")
    p_prev.add_argument("seconds", nargs="?", type=float, default=8.0)
    p_prev.add_argument("out", nargs="?", default="preview.jpg")
    p_prev.add_argument("--fps", type=int, default=10)

    p_flip = sub.add_parser("fliptest", help="同会话连抓帧验证 hflip 是否生效（写 <prefix>_a*/_b*/*_c*.jpg）")
    p_flip.add_argument("prefix")

    p_cmd = sub.add_parser("cmd", help="发一条原始 JSON 命令并等 ack")
    p_cmd.add_argument("json", help='例如 \'{"cmd":"cam","gain":8}\'')

    p_dbb = sub.add_parser("db_backup", help="F10：导出特征库+名表到 .bin（0x06 分片）")
    p_dbb.add_argument("out", nargs="?", default="face_db_backup.bin")
    p_dbr = sub.add_parser("db_restore", help="F10：从 .bin 覆盖恢复（导入前板端自动 .bak）")
    p_dbr.add_argument("file")

    p_demo = sub.add_parser("demo")
    p_demo.add_argument("face_dir", help="含 bill1.jpg/bill2.jpg/musk1.jpg/musk2.jpg 的目录")

    args = ap.parse_args()

    link = Link(tcp=args.tcp, serial_port=args.serial, timeout=args.timeout)
    print(f"# JCLINK via {link.desc}")
    link.drain_logs(0.3)

    try:
        if args.cmd == "start":
            show(link.cmd_and_wait({"cmd": "start"}))
        elif args.cmd == "stop":
            show(link.cmd_and_wait({"cmd": "stop"}))
        elif args.cmd == "list":
            show(link.cmd_and_wait({"cmd": "list"}))
        elif args.cmd == "clear":
            show(link.cmd_and_wait({"cmd": "clear"}))
        elif args.cmd == "mode":
            req = {"cmd": "mode"}
            if args.detect is not None:
                req["detect"] = bool(args.detect)
            if args.match is not None:
                req["match"] = bool(args.match)
            if args.thr is not None:
                req["thr"] = args.thr
            show(link.cmd_and_wait(req))
        elif args.cmd == "frame":
            send_frames(link, args.files)
        elif args.cmd == "register":
            # 先等 ack（板端武装完注册再发帧，避免命令/帧到达顺序错配）
            show(link.cmd_and_wait({"cmd": "register", "id": args.name}))
            send_frames(link, args.files)
        elif args.cmd == "src":
            show(link.cmd_and_wait({"cmd": "src", "value": args.value}))
        elif args.cmd == "watch":
            t_end = time.monotonic() + args.seconds
            n = 0
            while time.monotonic() < t_end:
                # 只要 0x02（0x04 日志由 Link.recv 内部打印）
                frame = link.recv_result(timeout=1.0)
                if frame is None:
                    continue
                text = frame[2].decode("utf-8", "replace")
                try:
                    obj = json.loads(text)
                except ValueError:
                    print(f"[{n + 1}] (非 JSON) {text!r}")
                    continue
                n += 1
                print(f"[{n}] {json.dumps(obj, ensure_ascii=False)}")
            print(f"# {n} frame(s) in {args.seconds}s")
        elif args.cmd == "stream":
            req = {"cmd": "stream"}
            if args.value != "query":
                req["value"] = args.value
            if args.fps is not None:
                req["fps"] = args.fps
            if args.q is not None:
                req["quality"] = args.q
            show(link.cmd_and_wait(req))

        elif args.cmd == "img":
            req = {"cmd": "stream", "value": "on", "fps": args.fps}
            show(link.cmd_and_wait(req), prefix="<- stream")
            img = link.recv_image(timeout=args.seconds)
            if img is None:
                print(f"!! {args.seconds}s 内没收到 0x05 图像帧（检查 src 是不是 cam、stream 是否 on）")
                return 1
            out = args.out
            if img.is_jpeg and out.lower().endswith(".rgb565"):
                out = out[:-7] + ".jpg"
            elif (not img.is_jpeg) and out.lower().endswith(".jpg"):
                out = out[:-4] + ".rgb565"      # raw 帧存原始像素
            with open(out, "wb") as fp:
                fp.write(img.data)
            print(f"<- {img}  已保存到 {out}")
            # 一帧就够，顺手关掉
            show(link.cmd_and_wait({"cmd": "stream", "value": "off"}), prefix="<- stream off")

        elif args.cmd == "preview":
            show(link.cmd_and_wait({"cmd": "stream", "value": "on", "fps": args.fps}), prefix="<- stream")
            last = None
            t_end = time.monotonic() + args.seconds
            n_img = n_res = 0
            while time.monotonic() < t_end:
                item = link.recv_any(timeout=1.0)
                if item is None:
                    continue
                kind, obj = item
                if kind == "image":
                    n_img += 1
                    last = obj
                    if n_img <= 3 or n_img % 10 == 0:
                        print(f"[img {n_img}] {obj}")
                else:
                    n_res += 1
                    print(f"[result {n_res}] faces={obj.get('faces')} match={obj.get('match')} "
                          f"src={obj.get('src')} infer={obj.get('infer_ms')}")
            if last is not None:
                with open(args.out, "wb") as fp:
                    fp.write(last.data)
                print(f"# 最后一帧 {last} -> {args.out}")
            print(f"# {n_img} image(s) / {n_res} result(s) in {args.seconds}s")
            show(link.cmd_and_wait({"cmd": "stream", "value": "off"}), prefix="<- stream off")

        elif args.cmd == "fliptest":
            def grab(tag, n=3):
                seqs = []
                for i in range(n):
                    img = link.recv_image(timeout=6)
                    if img is None:
                        print(f"!! {tag}: 没收到帧")
                        break
                    with open(f"{args.prefix}_{tag}{i}.jpg", "wb") as fp:
                        fp.write(img.data)
                    seqs.append(img.seq)
                print(f"# {tag}: seq={seqs}")
                return seqs

            show(link.cmd_and_wait({"cmd": "stream", "value": "on", "fps": 10}), prefix="<- stream")
            grab("a")                                   # 基线（当前 mirror 设置）
            print(show(link.cmd_and_wait({"cmd": "cam", "mirror": False})))
            grab("b")                                   # mirror=off
            print(show(link.cmd_and_wait({"cmd": "cam", "mirror": True})))
            grab("c")                                   # mirror=on（恢复）
            show(link.cmd_and_wait({"cmd": "stream", "value": "off"}), prefix="<- stream off")
            print(f"# 比较: a0/a1(基线稳定性)  a0/b0(mirror 是否生效)  a0/c0(应一致)")
        elif args.cmd == "cmd":
            show(link.cmd_and_wait(json.loads(args.json)))
        elif args.cmd == "delete":
            show(link.cmd_and_wait({"cmd": "delete", "id": args.name}))
        elif args.cmd == "db_backup":
            print("# db_backup", "OK" if db_backup(link, args.out) else "FAILED")
        elif args.cmd == "db_restore":
            print("# db_restore", "OK" if db_restore(link, args.file) else "FAILED")
        elif args.cmd == "demo":
            d = args.face_dir
            show(link.cmd_and_wait({"cmd": "clear"}))
            print("# 注册 bill（bill1/bill2 两张特征）")
            for n in ("bill1.jpg", "bill2.jpg"):
                link.cmd_and_wait({"cmd": "register", "id": "bill"})    # 等 ack
                send_frames(link, [os.path.join(d, n)])
            print("# 注册 musk（musk1 一张特征）")
            link.cmd_and_wait({"cmd": "register", "id": "musk"})
            send_frames(link, [os.path.join(d, "musk1.jpg")])
            show(link.cmd_and_wait({"cmd": "list"}))
            print("# 识别 musk2（应 match=musk）")
            send_frames(link, [os.path.join(d, "musk2.jpg")])
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
