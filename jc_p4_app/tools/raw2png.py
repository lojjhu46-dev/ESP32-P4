# -*- coding: utf-8 -*-
"""RGB565(LE) raw 帧 -> PNG（纯标准库，不需要 PIL/OpenCV）。

用法: python tools/raw2png.py in.rgb565 out.png [width=640] [height=480]
说明: 配合 0x05 的 fmt=raw 抓帧使用，用来肉眼比对降噪/白平衡/曝光效果。
"""
import struct
import sys
import zlib


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 480

    data = open(src, "rb").read()
    n = w * h
    if len(data) < n * 2:
        print("file too small: %d < %d bytes" % (len(data), n * 2))
        return 1
    v = struct.unpack("<%dH" % n, data[: n * 2])

    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # PNG 每行前的 filter byte
        for x in v[y * w:(y + 1) * w]:
            raw.append(((x >> 11) & 0x1F) * 255 // 31)
            raw.append(((x >> 5) & 0x3F) * 255 // 63)
            raw.append((x & 0x1F) * 255 // 31)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)      # 8bit truecolor
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
           chunk(b"IEND", b""))
    with open(dst, "wb") as f:
        f.write(png)
    print("saved %s (%dx%d, %.0f KB)" % (dst, w, h, len(png) / 1024.0))
    return 0


raise SystemExit(main())
