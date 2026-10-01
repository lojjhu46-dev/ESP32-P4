"""JCLINK v1 协议实现（PC 端）

与板端 main/app_usb_link.c 一一对应，见 jc_p4_app/docs/usb_link_protocol.md：
    'J''C' | type(1) | seq(1) | len(u32 LE) | payload | crc16(2 LE)
    crc16 = CRC-16/CCITT-FALSE(init 0xFFFF) over type+seq+len+payload

传输层可二选一（同一套帧）：
    * TCP     —— 网线直连（板端默认静态 IP 192.168.33.2:3333）
    * 串口    —— P4 原生 USB(OTG) 枚举出的 CDC 口（不是 CH340）
"""

from __future__ import annotations

import socket
import struct
import time

MAGIC = b"JC"
T_JPEG = 0x01
T_RESULT = 0x02
T_CMD = 0x03
T_LOG = 0x04
T_IMAGE = 0x05          # board -> PC 图像帧（见 jc_p4_app/main/app_stream.h）

MAX_PAYLOAD = 1024 * 1024   # 与板端 JCL_MAX_PAYLOAD 保持一致（0x05 raw 帧 614KB）
TYPE_NAME = {T_JPEG: "JPEG", T_RESULT: "RESULT", T_CMD: "CMD", T_LOG: "LOG", T_IMAGE: "IMAGE"}

# 0x05 载荷头（小端）：fmt u8 | flags u8 | width u16 | height u16 | seq u16
IMG_HDR_LEN = 8
IMG_FMT_JPEG = 0
IMG_FMT_RGB565 = 1


class ImageFrame:
    """0x05 图像帧（板端 app_stream 发出）"""

    __slots__ = ("fmt", "flags", "width", "height", "seq", "data")

    def __init__(self, payload: bytes):
        if len(payload) < IMG_HDR_LEN:
            raise ValueError(f"image payload too short: {len(payload)}")
        fmt, flags, width, height, seq = struct.unpack("<BBHHH", payload[:IMG_HDR_LEN])
        self.fmt = fmt
        self.flags = flags
        self.width = width
        self.height = height
        self.seq = seq
        self.data = payload[IMG_HDR_LEN:]

    @property
    def is_jpeg(self) -> bool:
        return self.fmt == IMG_FMT_JPEG

    def __repr__(self) -> str:
        kind = "jpeg" if self.is_jpeg else "rgb565"
        return f"<ImageFrame {self.width}x{self.height} {kind} seq={self.seq} {len(self.data)}B>"


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(ptype: int, payload: bytes, seq: int) -> bytes:
    hdr = MAGIC + bytes((ptype & 0xFF, seq & 0xFF)) + struct.pack("<I", len(payload))
    crc = crc16_ccitt(hdr[2:] + payload)
    return hdr + payload + struct.pack("<H", crc)


class FrameParser:
    """字节流 -> 帧 (type, seq, payload)"""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.bad_crc = 0

    def feed(self, data: bytes) -> list[tuple[int, int, bytes]]:
        self.buf += data
        out: list[tuple[int, int, bytes]] = []
        while True:
            if len(self.buf) < 8:
                return out
            i = self.buf.find(MAGIC)
            if i < 0:
                self.buf.clear()
                return out
            if i:
                del self.buf[:i]
            if len(self.buf) < 8:
                return out
            ptype, seq = self.buf[2], self.buf[3]
            (ln,) = struct.unpack("<I", self.buf[4:8])
            if ln == 0 or ln > MAX_PAYLOAD:
                del self.buf[:2]          # 滑一个字节重新找 magic
                continue
            if len(self.buf) < 10 + ln:
                return out
            payload = bytes(self.buf[8 : 8 + ln])
            (crc,) = struct.unpack("<H", self.buf[8 + ln : 10 + ln])
            if crc16_ccitt(bytes(self.buf[2 : 8 + ln])) != crc:
                self.bad_crc += 1
                del self.buf[:2]
                continue
            out.append((ptype, seq, payload))
            del self.buf[: 10 + ln]


class Link:
    """JCLINK 客户端（TCP 或串口）"""

    def __init__(self, tcp: str | None = None, serial_port: str | None = None, timeout: float = 3.0):
        self.timeout = timeout
        self.parser = FrameParser()
        self.seq = 0
        self.logs: list[str] = []
        self.tcp = None
        self.ser = None
        if tcp:
            host, _, port = tcp.partition(":")
            self.tcp = socket.create_connection((host, int(port or 3333)), timeout=timeout)
            self.tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.tcp.settimeout(0.2)
            self.desc = f"tcp {host}:{port or 3333}"
        elif serial_port:
            import serial  # pyserial

            self.ser = serial.Serial(serial_port, 115200, timeout=0.05)
            self.desc = f"serial {serial_port}"
        else:
            raise ValueError("need tcp= or serial_port=")

    # ---- io ----
    def _read(self) -> bytes:
        try:
            if self.tcp:
                return self.tcp.recv(65536)   # 图像帧可达数十 KB，尽量一次多读
            n = self.ser.in_waiting or 1
            return self.ser.read(min(max(n, 1), 4096))
        except (socket.timeout, TimeoutError, OSError):
            return b""

    def send(self, ptype: int, payload: bytes) -> None:
        self.seq = self.seq % 255 + 1
        frame = build_frame(ptype, payload, self.seq)
        if self.tcp:
            self.tcp.sendall(frame)
        else:
            self.ser.write(frame)
            self.ser.flush()

    def send_json_cmd(self, obj: dict) -> None:
        import json

        self.send(T_CMD, json.dumps(obj, separators=(",", ":")).encode())

    def send_jpeg(self, data: bytes) -> None:
        self.send(T_JPEG, data)

    def recv(self, timeout: float | None = None, want=(T_RESULT, T_LOG)):
        """收帧；(type, seq, payload) 或 None(超时)。0x04 日志会被收集到 self.logs 并打印"""
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            data = self._read()
            if data:
                for frame in self.parser.feed(data):
                    ptype, _seq, payload = frame
                    if ptype == T_LOG:
                        line = payload.decode("utf-8", "replace").rstrip("\n")
                        self.logs.append(line)
                        print(f"  [board] {line}")
                    if ptype in want:
                        return ptype, _seq, payload
            else:
                time.sleep(0.005)
        return None

    def recv_result(self, timeout: float | None = None):
        """等一条 0x02 结果帧，返回 (type, seq, payload) 或 None"""
        return self.recv(timeout=timeout, want=(T_RESULT,))

    def recv_image(self, timeout: float | None = None) -> ImageFrame | None:
        """等一条 0x05 图像帧（需先 {"cmd":"stream","value":"on"}）"""
        frame = self.recv(timeout=timeout, want=(T_IMAGE,))
        if frame is None:
            return None
        return ImageFrame(frame[2])

    def recv_any(self, timeout: float | None = None):
        """等任意 0x02 或 0x05，返回 ('result', dict) / ('image', ImageFrame) / None"""
        import json

        frame = self.recv(timeout=timeout, want=(T_RESULT, T_IMAGE))
        if frame is None:
            return None
        if frame[0] == T_IMAGE:
            return "image", ImageFrame(frame[2])
        try:
            return "result", json.loads(frame[2].decode("utf-8", "replace"))
        except ValueError:
            return None

    def cmd_and_wait(self, obj: dict, timeout: float | None = None):
        """发命令并等它自己的 ack（板端命令回复带 "ack":<cmd> 字段）。

        摄像头常驻推流时 0x02 里混着流式结果帧，靠 ack 字段区分；
        期间收到的流式帧会打印一行摘要，超时则返回最后收到的那帧。
        """
        import json

        self.send_json_cmd(obj)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        last = None
        while time.monotonic() < deadline:
            frame = self.recv_result(timeout=0.5)
            if frame is None:
                continue
            try:
                reply = json.loads(frame[2].decode("utf-8", "replace"))
            except ValueError:
                continue
            last = reply
            if "ack" in reply:
                return reply
            print(f"  [stream] faces={reply.get('faces')} event={reply.get('event')} "
                  f"src={reply.get('src')}")
        return last

    def drain_logs(self, seconds: float = 0.3) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            data = self._read()
            if data:
                for ptype, _s, payload in self.parser.feed(data):
                    if ptype == T_LOG:
                        line = payload.decode("utf-8", "replace").rstrip("\n")
                        self.logs.append(line)
                        print(f"  [board] {line}")
            else:
                time.sleep(0.005)

    def close(self) -> None:
        if self.tcp:
            self.tcp.close()
        if self.ser:
            self.ser.close()
