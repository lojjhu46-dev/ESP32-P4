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
T_BLOCK = 0x06          # 双向：块数据分片 offset(u32 LE)+data（F10 特征库备份/恢复）
T_H264 = 0x07           # board -> PC H.264 访问单元（见 jc_p4_app/main/app_h264enc.c）

MAX_PAYLOAD = 1024 * 1024   # 与板端 JCL_MAX_PAYLOAD 保持一致（0x05 raw 帧 614KB）
TYPE_NAME = {T_JPEG: "JPEG", T_RESULT: "RESULT", T_CMD: "CMD", T_LOG: "LOG",
             T_IMAGE: "IMAGE", T_BLOCK: "BLOCK", T_H264: "H264"}

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


# 0x07 载荷头（小端）：codec u8 | flags u8 | stream_id u16 | pts_us u32 |
#                     frame_seq u32 | width u16 | height u16 | nal_len u32
H264_HDR_LEN = 20
H264_FLAG_KEYFRAME = 0x01
H264_FLAG_SPS_PPS = 0x02
H264_FLAG_LAST_FRAG = 0x04


class H264Frame:
    """0x07 H.264 访问单元（板端 app_h264enc 发出）"""

    __slots__ = ("codec", "flags", "stream_id", "pts_us", "frame_seq",
                 "width", "height", "nal_len", "data")

    def __init__(self, payload: bytes):
        if len(payload) < H264_HDR_LEN:
            raise ValueError(f"h264 payload too short: {len(payload)}")
        (self.codec, self.flags, self.stream_id, self.pts_us, self.frame_seq,
         self.width, self.height, self.nal_len) = struct.unpack("<BBHIIHHI", payload[:H264_HDR_LEN])
        if H264_HDR_LEN + self.nal_len > len(payload):
            raise ValueError(f"h264 nal_len {self.nal_len} > payload {len(payload)}")
        self.data = payload[H264_HDR_LEN:H264_HDR_LEN + self.nal_len]

    @property
    def is_keyframe(self) -> bool:
        return bool(self.flags & H264_FLAG_KEYFRAME)

    def __repr__(self) -> str:
        return (f"<H264Frame stream={self.stream_id} {self.width}x{self.height} "
                f"seq={self.frame_seq} key={self.is_keyframe} {len(self.data)}B>")


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
        # 同一批 read 里可能同时包含 ack 和紧随其后的 0x06 分片；recv() 命中 ack
        # 就返回，会把同批剩余帧丢掉（F10 备份实测稳定丢第 1 个 16KB 分片）。
        self._pending: list = []
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
            # H.264 0x07 峰值 ~200KB/s；默认 4KB RX 缓冲 + 逐帧写盘会丢参考帧。
            try:
                self.ser.set_buffer_size(rx_size=256 * 1024, tx_size=64 * 1024)
            except Exception:
                pass
            self.desc = f"serial {serial_port}"
        else:
            raise ValueError("need tcp= or serial_port=")

    # ---- io ----
    def _read(self) -> bytes:
        try:
            if self.tcp:
                return self.tcp.recv(65536)   # 图像帧可达数十 KB，尽量一次多读
            n = self.ser.in_waiting or 1
            return self.ser.read(min(max(n, 1), 65536))
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

    def send_block(self, offset: int, data: bytes) -> None:
        """发一个 0x06 块分片：offset(u32 LE) + data（F10 备份导入用）"""
        self.send(T_BLOCK, struct.pack("<I", offset) + data)

    def recv(self, timeout: float | None = None, want=(T_RESULT, T_LOG)):
        """收帧；(type, seq, payload) 或 None(超时)。0x04 日志会被收集到 self.logs 并打印"""
        for i, frame in enumerate(self._pending):
            if frame[0] in want:
                return self._pending.pop(i)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            data = self._read()
            if data:
                frames = self.parser.feed(data)
                for ptype, _seq, payload in frames:
                    if ptype == T_LOG:
                        line = payload.decode("utf-8", "replace").rstrip("\n")
                        self.logs.append(line)
                        print(f"  [board] {line}")
                for i, frame in enumerate(frames):
                    # 同一批 read 里 ack 后面常紧跟 0x06 分片；提前 return 会丢掉
                    # 剩余已解析帧（F10 备份实测稳定丢中间 16KB 块）→ 先入队。
                    # 注意：这里只能给“小帧”入队，但返回必须是 want 里的任意类型
                    # （0x05 图像 / 0x07 H.264 也走 recv）。
                    if frame[0] in want:
                        self._pending.extend(
                            f for f in frames[i + 1:] if f[0] in (T_RESULT, T_BLOCK))
                        return frame
                    if frame[0] in (T_RESULT, T_BLOCK):
                        self._pending.append(frame)
                        if len(self._pending) > 32:
                            self._pending.pop(0)
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

    def recv_h264(self, timeout: float | None = None) -> H264Frame | None:
        """等一条 0x07 H.264 访问单元（板端 M0 自动发送）"""
        frame = self.recv(timeout=timeout, want=(T_H264,))
        if frame is None:
            return None
        return H264Frame(frame[2])

    def recv_block(self, timeout: float | None = None):
        """等一个 0x06 块分片，返回 (offset, data) 或 None（F10 备份导出用）"""
        frame = self.recv(timeout=timeout, want=(T_BLOCK,))
        if frame is None or len(frame[2]) < 4:
            return None
        off = struct.unpack_from("<I", frame[2], 0)[0]
        return off, frame[2][4:]

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

        ⚠️ ack 必须**按命令名精确匹配**：板端回显 ack=<命令名>，而取景中枢
        （board_hub.py）保活会补发 stream on，其 ack 也带 "ack":"stream"——
        只认"有 ack 字段"会把中枢的 ack 错当成自己命令的应答（2026-10-06
        经中枢跑回归实测踩到：mode 命令收到 stream 的 ack，msg 全错）。
        期间收到的其他帧（流式结果/外来 ack）被跳过，超时返回最后收到的一帧。
        """
        import json

        self.send_json_cmd(obj)
        expected = obj.get("cmd") if isinstance(obj, dict) else None
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
                if expected is None or reply.get("ack") == expected:
                    return reply
                continue        # 外来命令的 ack（如中枢补发的 stream）：跳过
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
