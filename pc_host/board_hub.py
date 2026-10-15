#!/usr/bin/env python3
"""board_hub.py —— PC 端取景中枢（解决"测 A/B 时看不到画面"）

为什么需要它
------------
板端 app_netlink.c 是 `listen(backlog=1)` + 单客户端 serve 循环：**一次只服务一个 TCP
客户端**。所以"画面"和"采集脚本"各自连一次板子，必然把对方挤进 backlog（症状：connect
成功却一帧都收不到）。要让采集期间画面不断，只能**由同一个进程独占上游**。

本工具做的事
------------
1. **独占上游**：唯一连到 `board:3333` 的客户端（含断线重连退避）；
2. **本机代理**：在本机 `bind:port`（默认 127.0.0.1:13333）开一个**协议完全一致**的服务，
   把本地脚本的上行字节原样转发给板子，把板子的下行字节**广播**给所有本地客户端；
   → 现有脚本一行不改，只要 `--tcp 127.0.0.1:13333`；
3. **常驻画面**：Tkinter 窗口持续显示 0x05 图像，叠加检测框 / 5 关键点 / 结果状态栏；
   **板子任何状态（重启、拔网线、停流、分辨率或帧率变化）都不会让窗口退出**；
4. **保流自愈**：上游重连后自动补发 `stream on`；上游在线但停流超时且近期没有本地命令时，
   也会带冷却地补发，不与脚本的 `stream off` 打架。

用法
----
    python pc_host/board_hub.py                     # 常驻：窗口 + 本机代理
    python pc_host/board_hub.py --selftest          # 无界面自检（脚本化验收用）
    # 另开终端，脚本连本机端口即可（画面全程不断）：
    python pc_host/jc_test.py --tcp 127.0.0.1:13333 watch 5
    python pc_host/thr_calib.py --tcp 127.0.0.1:13333 --phase a --a 20

窗口快捷键：1/2/3 缩放、b 检测框、k 关键点、t 文字、s 截图、q 退出。
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import socket
import sys
import threading
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import protocol  # noqa: E402
from protocol import (FrameParser, ImageFrame, build_frame,  # noqa: E402
                      T_CMD, T_IMAGE, T_JPEG, T_LOG, T_RESULT)

try:  # 中文 Windows 控制台默认 GBK，板端日志/JSON 可能含非 ASCII
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:                                    # noqa: BLE001
    pass

UP_RECV_CHUNK = 65536
UP_POLL_S = 0.5            # 上游 socket 超时：兼作保活探针周期与线程退出粒度
CLIENT_POLL_S = 0.5
CONNECT_TIMEOUT_S = 4.0
BACKOFF_MIN_S = 1.0
BACKOFF_MAX_S = 5.0
NO_DATA_RECONNECT_S = 8.0  # 上游"连上但一个字节都没收到"超过这么久 → 判定没被服务，强制重连
KEEPALIVE_IDLE_S = 3.0     # 上游在线但这么久没收到 0x05 → 判定停流
KEEPALIVE_MIN_CMD_GAP_S = 3.0   # 距最近一次本地命令不足这么久 → 不抢着补发
KEEPALIVE_COOLDOWN_S = 5.0
SHOTS_DIR = os.path.join(_HERE, "shots")


def log(msg: str, tag: str = "hub") -> None:
    print(f"[{tag}] {time.strftime('%H:%M:%S')} {msg}", flush=True)


# --------------------------------------------------------------------------- #
# 共享状态
# --------------------------------------------------------------------------- #
class HubState:
    """上游/本地/统计的共享状态。图像与结果都是"只留最新"语义。"""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.image: ImageFrame | None = None
        self.image_seq = -1
        self.image_t = 0.0
        self.result: dict | None = None
        self.result_t = 0.0
        self.up_online = False
        self.up_since = 0.0
        self.reconnects = 0
        self.notes = "启动中"
        self.img_n = 0
        self.res_n = 0
        self.log_n = 0
        self.rx_bytes = 0
        self.img_fps = 0.0
        self.last_local_cmd_t = 0.0
        self.last_local_cmd = ""
        self.clients = 0

    def snap(self) -> dict:
        with self.lock:
            return {
                "image": self.image,
                "image_seq": self.image_seq,
                "image_t": self.image_t,
                "result": dict(self.result) if self.result else None,
                "up_online": self.up_online,
                "reconnects": self.reconnects,
                "notes": self.notes,
                "img_n": self.img_n,
                "res_n": self.res_n,
                "log_n": self.log_n,
                "rx_bytes": self.rx_bytes,
                "img_fps": self.img_fps,
                "last_local_cmd": self.last_local_cmd,
                "clients": self.clients,
            }


# --------------------------------------------------------------------------- #
# 本地客户端
# --------------------------------------------------------------------------- #
class LocalClient:
    """一个本地脚本连接。

    读到什么就原样转发给上游（透明），并用独立 FrameParser 旁路观察命令用于保活门控；
    下行走每客户端队列 + 写线程，队列满时丢**最旧的**（图像可丢，命令不丢）。
    """

    _ids = 0

    def __init__(self, sock: socket.socket, addr, hub: "Hub") -> None:
        LocalClient._ids += 1
        self.id = LocalClient._ids
        self.sock = sock
        self.addr = addr
        self.hub = hub
        self.alive = True
        self.q: queue.Queue[bytes] = queue.Queue(maxsize=64)
        self.parser = FrameParser()
        self.bytes_up = 0
        try:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock.settimeout(CLIENT_POLL_S)
        except OSError:
            pass
        self.rd = threading.Thread(target=self._read_loop, name=f"cli{self.id}rd", daemon=True)
        self.wr = threading.Thread(target=self._write_loop, name=f"cli{self.id}wr", daemon=True)
        self.rd.start()
        self.wr.start()

    # ---- 下行：上游字节 → 本客户端 ----
    def push(self, data: bytes, is_image: bool) -> None:
        if not self.alive:
            return
        try:
            self.q.put_nowait(data)
        except queue.Full:
            if is_image:                      # 图像可丢：丢最旧的，塞最新
                try:
                    self.q.get_nowait()
                except queue.Empty:
                    pass
                try:
                    self.q.put_nowait(data)
                except queue.Full:
                    pass
            else:
                log(f"本地客户端 #{self.id} 队列满，断开（它没在收）")
                self.close()

    def _write_loop(self) -> None:
        while self.alive:
            try:
                data = self.q.get(timeout=CLIENT_POLL_S)
            except queue.Empty:
                continue
            try:
                self.sock.sendall(data)
            except OSError as e:
                log(f"本地客户端 #{self.id} 发送失败：{e}")
                self.close()
                return

    # ---- 上行：本客户端 → 上游 ----
    def _read_loop(self) -> None:
        while self.alive:
            try:
                data = self.sock.recv(UP_RECV_CHUNK)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            self.bytes_up += len(data)
            self._observe(data)
            if not self.hub.up_write(data):
                break
        self.close()

    def _observe(self, data: bytes) -> None:
        """旁路解析上行帧，只为了记"最近有没有本地命令"（保活门控用）。"""
        for ptype, _seq, payload in self.parser.feed(data):
            if ptype == T_CMD:
                txt = payload.decode("utf-8", "replace")
                with self.hub.state.lock:
                    self.hub.state.last_local_cmd_t = time.time()
                    self.hub.state.last_local_cmd = txt[:120]
                log(f"本地 #{self.id} → 板子 cmd: {txt[:120]}", "cmd")

    def close(self) -> None:
        if not self.alive:
            return
        self.alive = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        self.hub._drop_client(self)


# --------------------------------------------------------------------------- #
# 中枢
# --------------------------------------------------------------------------- #
class Hub:
    def __init__(self, board_host: str, board_port: int, bind: str, port: int,
                 stream_fps: int, stream_fmt: str, stream_quality: int | None) -> None:
        self.board = (board_host, board_port)
        self.bind = (bind, port)
        self.stream_fps = stream_fps
        self.stream_fmt = stream_fmt
        self.stream_quality = stream_quality

        self.state = HubState()
        self.stop = threading.Event()
        self.up_sock: socket.socket | None = None
        self.up_lock = threading.Lock()
        self.up_parser = FrameParser()
        self.seq = 0
        self.clients: list[LocalClient] = []
        self.clients_lock = threading.Lock()
        self.last_image_t = 0.0
        self.last_keepalive_t = 0.0
        self.first_img_logged = False
        self.img_win: list[float] = []

    # ---------------- 上游 ----------------
    def up_online(self) -> bool:
        with self.state.lock:
            return self.state.up_online

    def _note(self, text: str) -> None:
        with self.state.lock:
            self.state.notes = text

    def _next_seq(self) -> int:
        self.seq = self.seq % 255 + 1
        return self.seq

    def up_write(self, data: bytes) -> bool:
        """把字节写到板子连接（所有本地客户端与 hub 自身的命令共用一把锁）。"""
        with self.up_lock:
            sock = self.up_sock
            if sock is None:
                return False
            try:
                sock.sendall(data)
                return True
            except OSError as e:
                log(f"上行写失败，准备重连：{e}")
                return False

    def send_cmd(self, obj: dict) -> bool:
        data = build_frame(T_CMD, json.dumps(obj, separators=(",", ":")).encode(), self._next_seq())
        return self.up_write(data)

    def stream_on(self, why: str) -> None:
        obj = {"cmd": "stream", "value": "on", "fps": self.stream_fps, "fmt": self.stream_fmt}
        if self.stream_quality is not None:
            obj["quality"] = self.stream_quality
        if self.send_cmd(obj):
            self.last_keepalive_t = time.time()
            log(f"补发 stream on（{why}）：fps={self.stream_fps} fmt={self.stream_fmt}")

    def _upstream_loop(self) -> None:
        backoff = BACKOFF_MIN_S
        while not self.stop.is_set():
            try:
                self._note("连接板子中…")
                sock = socket.create_connection(self.board, timeout=CONNECT_TIMEOUT_S)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.settimeout(UP_POLL_S)
            except OSError as e:
                with self.state.lock:
                    self.state.up_online = False
                    self.state.reconnects += 1
                self._note(f"板子不可达：{e}（{backoff:.1f}s 后重试）")
                log(f"上游连接失败：{e}")
                self.stop.wait(backoff)
                backoff = min(backoff * 2, BACKOFF_MAX_S)
                continue

            with self.up_lock:
                self.up_sock = sock
            self.up_parser = FrameParser()
            with self.state.lock:
                self.state.up_online = True
                self.state.up_since = time.time()
            self._note("已连接，等画流…")
            log(f"上游已连接 {self.board[0]}:{self.board[1]}")
            backoff = BACKOFF_MIN_S
            self.last_image_t = time.time()
            self.last_keepalive_t = 0.0
            self.first_img_logged = False
            rx_any_t = time.time()
            self.stream_on("上游重连")

            idle = True
            while not self.stop.is_set():
                try:
                    data = sock.recv(UP_RECV_CHUNK)
                except socket.timeout:
                    idle = True
                    data = b""
                except OSError as e:
                    if self.stop.is_set():
                        break                      # 正常关闭：socket 已被 shutdown 关掉
                    log(f"上游读取异常：{e}")
                    break
                if data:
                    idle = False
                    rx_any_t = time.time()
                    with self.state.lock:
                        self.state.rx_bytes += len(data)
                    self._broadcast(data, is_image=False)   # 先原样广播（透明）
                    self._consume_upstream(data)
                    # ⚠️ 保活判定必须**每轮都做**，不能只在 timeout 分支做：板子在
                    #    0x05 停流期间仍持续发 0x02 结果帧（~10fps），socket 永不超时，
                    #    只在超时分支判定会导致"stream 被关掉后画面再也回不来"（实测踩过）。
                    self._keepalive_tick()
                else:
                    if idle:                                # 真超时
                        # 「连上但一个字节都没收到」= 板子没服务我们（backlog 被别的进程占着，
                        # 或板子刚重启还没起 netlink）——今天反复踩的坑，必须显式报出来并重连。
                        if time.time() - rx_any_t > NO_DATA_RECONNECT_S:
                            self._note(f"已连上但 {NO_DATA_RECONNECT_S:.0f}s 无任何数据，重连中…")
                            log(f"⚠️ 上游已连接但 {NO_DATA_RECONNECT_S:.0f}s 内没收到任何数据——"
                                f"很可能板子的唯一客户端槽被别的进程占着（WSL 模拟器/残留脚本）。"
                                f"强制重连。")
                            break
                        self._keepalive_tick()
                    else:                                   # 对端关闭
                        log("上游被对端关闭")
                        break

            with self.up_lock:
                self.up_sock = None
            with self.state.lock:
                self.state.up_online = False
                self.state.reconnects += 1
            try:
                sock.close()
            except OSError:
                pass
            self._note("上游断开，重连中…")

    def _consume_upstream(self, data: bytes) -> None:
        """解析上游字节：更新最新图像/结果/日志（同时把 0x05 作为图像帧再广播一次？不——
        原样字节已经广播过了，这里只做渲染状态维护。"""
        for ptype, _seq, payload in self.up_parser.feed(data):
            if ptype == T_IMAGE:
                try:
                    img = ImageFrame(payload)
                except ValueError:
                    continue
                now = time.time()
                with self.state.lock:
                    self.state.image = img
                    self.state.image_seq = img.seq
                    self.state.image_t = now
                    self.state.img_n += 1
                self.last_image_t = now
                if not self.first_img_logged:
                    self.first_img_logged = True
                    self._note(f"画流正常（{img.width}x{img.height}）")
                    log(f"首帧到达：{img}")
                self.img_win.append(now)
                if len(self.img_win) > 30:
                    del self.img_win[: len(self.img_win) - 30]
                if len(self.img_win) >= 2:
                    span = self.img_win[-1] - self.img_win[0]
                    if span > 0.2:
                        with self.state.lock:
                            self.state.img_fps = (len(self.img_win) - 1) / span
            elif ptype == T_RESULT:
                try:
                    obj = json.loads(payload.decode("utf-8", "replace"))
                except ValueError:
                    continue
                with self.state.lock:
                    self.state.result = obj
                    self.state.result_t = time.time()
                    self.state.res_n += 1
            elif ptype == T_LOG:
                with self.state.lock:
                    self.state.log_n += 1

    def _keepalive_tick(self) -> None:
        now = time.time()
        if now - self.last_image_t < KEEPALIVE_IDLE_S:
            return
        with self.state.lock:
            last_cmd = self.state.last_local_cmd_t
            online = self.state.up_online
        if not online:
            return
        if now - last_cmd < KEEPALIVE_MIN_CMD_GAP_S:
            return                       # 脚本刚发过命令，别抢
        if now - self.last_keepalive_t < KEEPALIVE_COOLDOWN_S:
            return
        self._note("等画流超时，补发 stream on…")
        self.stream_on(f"停流 {now - self.last_image_t:.1f}s 无 0x05")

    # ---------------- 本机代理 ----------------
    def _broadcast(self, data: bytes, is_image: bool) -> None:
        with self.clients_lock:
            snapshot = list(self.clients)
        for c in snapshot:
            c.push(data, is_image)

    def _drop_client(self, c: LocalClient) -> None:
        with self.clients_lock:
            if c in self.clients:
                self.clients.remove(c)
                gone = True
            else:
                gone = False
            left = len(self.clients)
        with self.state.lock:
            self.state.clients = left
        if gone:
            log(f"本地客户端 #{c.id} 断开（剩 {left} 个，上行 {c.bytes_up} 字节）")

    def _accept_loop(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(self.bind)
        srv.listen(8)
        srv.settimeout(CLIENT_POLL_S)
        log(f"本机代理监听 {self.bind[0]}:{self.bind[1]}（脚本用 --tcp {self.bind[0]}:{self.bind[1]}）")
        while not self.stop.is_set():
            try:
                sock, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                if self.stop.is_set():
                    break
                continue
            c = LocalClient(sock, addr, self)
            with self.clients_lock:
                self.clients.append(c)
            with self.state.lock:
                self.state.clients = len(self.clients)
            log(f"本地客户端 #{c.id} 接入 {addr[0]}:{addr[1]}（当前 {len(self.clients)} 个）")
        try:
            srv.close()
        except OSError:
            pass

    # ---------------- 生命周期 ----------------
    def start(self) -> None:
        threading.Thread(target=self._upstream_loop, name="upstream", daemon=True).start()
        threading.Thread(target=self._accept_loop, name="accept", daemon=True).start()

    def shutdown(self) -> None:
        self.stop.set()
        # ⚠️ 不能在持 clients_lock 时调用 close()：close() → _drop_client() 会再取同一把
        #    非重入锁，直接死锁（本文件首版踩过：进程卡死不退出，还占着板子唯一连接）。
        with self.clients_lock:
            snapshot = list(self.clients)
        for c in snapshot:
            c.close()
        with self.up_lock:
            if self.up_sock:
                try:
                    self.up_sock.close()
                except OSError:
                    pass


# --------------------------------------------------------------------------- #
# 图像解码与叠加
# --------------------------------------------------------------------------- #
def _font(size: int = 14):
    from PIL import ImageFont
    for path in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf",
                 r"C:\Windows\Fonts\arial.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def decode_frame(img: ImageFrame):
    """0x05 帧 → PIL.Image（RGB）。JPEG 走 PIL，RGB565 走 numpy 向量化。"""
    from PIL import Image
    if img.is_jpeg:
        import io
        return Image.open(io.BytesIO(img.data)).convert("RGB")
    import numpy as np
    h, w = img.height, img.width
    need = w * h * 2
    buf = img.data[:need]
    if len(buf) < need:
        raise ValueError("rgb565 数据不足")
    v = np.frombuffer(buf, dtype="<u2").reshape(h, w)
    r = ((v >> 11) & 0x1F).astype("uint8") << 3
    g = ((v >> 5) & 0x3F).astype("uint8") << 2
    b = (v & 0x1F).astype("uint8") << 3
    return Image.fromarray(np.dstack((r, g, b)), "RGB")


def overlay(im, result: dict | None, show_box: bool, show_kp: bool, show_text: bool, extra: str = ""):
    """把检测框 / 5 关键点 / 状态栏画到图像上。"""
    if not (show_box or show_kp or show_text):
        return im
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    f = _font(14)
    boxes = (result or {}).get("boxes") or []
    kp = (result or {}).get("kp") or []
    name = (result or {}).get("match")
    ev = (result or {}).get("event", "-")
    sim = (result or {}).get("sim")

    if show_box:
        for i, b in enumerate(boxes):
            if not (isinstance(b, (list, tuple)) and len(b) >= 4):
                continue
            x0, y0, x1, y1 = [int(v) for v in b[:4]]
            color = (0, 255, 0) if name else (255, 160, 0)
            d.rectangle([x0, y0, x1, y1], outline=color, width=2)
            label = f"{name} {sim:.2f}" if (name and isinstance(sim, (int, float))) else str(ev)
            d.text((x0, max(0, y0 - 16)), label, fill=color, font=f)

    if show_kp and len(kp) >= 10:
        pts = [(int(kp[i]), int(kp[i + 1])) for i in range(0, 10, 2)]
        for i, p in enumerate(pts):
            d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=(255, 255, 0))
            d.text((p[0] + 4, p[1] - 14), str(i), fill=(255, 255, 0), font=f)
        d.line([pts[0], pts[1]], fill=(255, 255, 0), width=1)
        d.line([pts[1], pts[2], pts[3]], fill=(255, 255, 0), width=1)
        d.line([pts[2], pts[4]], fill=(255, 255, 0), width=1)

    if show_text:
        r = result or {}
        wmed = 0
        if boxes:
            try:
                wmed = int(boxes[0][2] - boxes[0][0])
            except (TypeError, IndexError):
                wmed = 0
        line1 = (f"{ev}  faces={r.get('faces', '-')}  box_w={wmed}px  "
                 f"sim={r.get('sim', '-')}  sim_raw={r.get('sim_raw', '-')}")
        line2 = (f"fps={r.get('fps', '-') if not isinstance(r.get('fps'), float) else round(r['fps'], 1)}  "
                 f"infer={r.get('infer_ms', '-')}ms  lib_n={r.get('lib_n', '-')}  "
                 f"src={r.get('src', '-')}  stable={r.get('stable', '-')}  {extra}")
        im2 = im.copy()
        d2 = ImageDraw.Draw(im2)
        for i, t in enumerate((line1, line2)):
            y = 4 + i * 18
            d2.rectangle([0, y - 2, min(im.width, 8 + len(t) * 8), y + 16], fill=(0, 0, 0))
            d2.text((4, y), t, fill=(255, 255, 255), font=f)
        return im2
    return im


# --------------------------------------------------------------------------- #
# 窗口
# --------------------------------------------------------------------------- #
class Viewer:
    def __init__(self, hub: Hub, zoom: int = 2) -> None:
        import tkinter as tk
        self.tk = tk
        self.hub = hub
        self.zoom = zoom
        self.show_box = True
        self.show_kp = True
        self.show_text = True
        self._photo = None
        self._last_seq = -1
        self._fps_hist: list[float] = []

        self.root = tk.Tk()
        self.root.title("JCLINK 取景中枢")
        self.canvas = tk.Canvas(self.root, width=640, height=480,
                                bg="#101010", highlightthickness=0)
        self.canvas.pack()
        self.lbl = tk.Label(self.root, anchor="w", justify="left", font=("Consolas", 10),
                            bg="#101010", fg="#d0d0d0")
        self.lbl.pack(fill="x")
        self.root.bind("<Key>", self._on_key)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(30, self._tick)

    # ---- 事件 ----
    def _on_key(self, ev) -> None:
        k = (ev.keysym or "").lower()
        if k in ("1", "2", "3"):
            self.zoom = int(k)
        elif k == "b":
            self.show_box = not self.show_box
        elif k == "k":
            self.show_kp = not self.show_kp
        elif k == "t":
            self.show_text = not self.show_text
        elif k == "s":
            self._screenshot()
        elif k in ("q", "escape"):
            self._on_close()

    def _on_close(self) -> None:
        self.hub.shutdown()
        try:
            self.root.destroy()
        except Exception:                              # noqa: BLE001
            pass

    def _screenshot(self) -> None:
        snap = self.hub.state.snap()
        img = snap["image"]
        if img is None:
            log("截图失败：还没有画面")
            return
        os.makedirs(SHOTS_DIR, exist_ok=True)
        out = os.path.join(SHOTS_DIR, time.strftime("hub_%Y%m%d_%H%M%S.png"))
        try:
            im = overlay(decode_frame(img), snap["result"], self.show_box, self.show_kp,
                         self.show_text, "SCREENSHOT")
            im.save(out)
            log(f"已截图 {out}")
        except Exception as e:                          # noqa: BLE001
            log(f"截图失败：{e}")

    # ---- 刷新 ----
    def _fps(self) -> float:
        now = time.time()
        self._fps_hist.append(now)
        self._fps_hist = [t for t in self._fps_hist if now - t < 2.0]
        return (len(self._fps_hist) - 1) / 2.0 if len(self._fps_hist) > 1 else 0.0

    def _tick(self) -> None:
        snap = self.hub.state.snap()
        img = snap["image"]
        if img is not None and snap["image_seq"] != self._last_seq:
            self._last_seq = snap["image_seq"]
            try:
                from PIL import Image, ImageTk
                im = overlay(decode_frame(img), snap["result"], self.show_box,
                             self.show_kp, self.show_text,
                             f"zoom={self.zoom}x")
                if self.zoom != 1:
                    im = im.resize((im.width * self.zoom, im.height * self.zoom), Image.NEAREST)
                self._photo = ImageTk.PhotoImage(im)
                self.canvas.config(width=im.width, height=im.height)
                self.canvas.delete("all")
                self.canvas.create_image(0, 0, anchor="nw", image=self._photo)
            except Exception as e:                      # noqa: BLE001
                log(f"渲染失败：{e}")
        fps = self._fps()
        r = snap["result"] or {}
        up = "在线" if snap["up_online"] else "断开"
        self.lbl.config(text=(
            f"上游 {self.hub.board[0]}:{self.hub.board[1]} {up}  本机 {self.hub.bind[0]}:{self.hub.bind[1]}  "
            f"客户端={snap['clients']}  重连={snap['reconnects']}\n"
            f"画面 {snap['img_n']} 帧 @{fps:4.1f}fps(本机)  上游 {snap['img_fps']:4.1f}fps  "
            f"结果 {snap['res_n']}  日志 {snap['log_n']}  RX {snap['rx_bytes'] // 1024}KB\n"
            f"状态 {snap['notes']}   event={r.get('event', '-')} sim={r.get('sim', '-')} "
            f"sim_raw={r.get('sim_raw', '-')} lib_n={r.get('lib_n', '-')}\n"
            f"按键: 1/2/3 缩放  b 框  k 关键点  t 文字  s 截图  q 退出"
        ))
        self.root.after(33, self._tick)


# --------------------------------------------------------------------------- #
# 自检（无界面）
# --------------------------------------------------------------------------- #
def selftest(board_host: str, board_port: int, bind: str, port: int,
             fps: int, fmt: str, want_images: int, timeout: float) -> int:
    """不占窗口地跑一遍：起 hub → 连本机端口当普通脚本 → 要图要结果 → 打统计。"""
    hub = Hub(board_host, board_port, bind, port, fps, fmt, None)
    hub.start()
    log("selftest：等上游连接…")
    t0 = time.time()
    while time.time() - t0 < timeout and not hub.up_online():
        time.sleep(0.2)
    if not hub.up_online():
        log(f"selftest 失败：{timeout:.0f}s 内连不上板子")
        hub.shutdown()
        return 1

    # 以"普通本地脚本"的身份连本机端口，验证代理链路
    lk = protocol.Link(tcp=f"{bind}:{port}", timeout=8)
    lk.cmd_and_wait({"cmd": "stream", "value": "on", "fps": fps, "fmt": fmt}, timeout=8)

    n_img = 0
    got_result = False
    t0 = time.time()
    last_log = t0
    while time.time() - t0 < timeout:
        item = lk.recv_any(timeout=1.0)
        if item is None:
            continue
        kind, obj = item
        if kind == "image":
            n_img += 1
            if n_img == 1:
                log(f"首帧 {obj}")
        else:
            got_result = True
        if time.time() - last_log > 2.0:
            last_log = time.time()
            log(f"selftest 进度：images={n_img} result={got_result}")
        if n_img >= want_images and got_result:
            break
    snap = hub.state.snap()
    log(f"selftest 统计：本地收到 {n_img} 图 / result={got_result} / 上游 {snap['img_n']} 图 "
        f"@{snap['img_fps']:.1f}fps / RX {snap['rx_bytes'] // 1024}KB / 重连 {snap['reconnects']}")
    lk.close()
    hub.shutdown()
    ok = n_img >= 1 and got_result
    log("selftest " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description="JCLINK 取景中枢：独占板子连接 + 本机代理 + 常驻画面")
    ap.add_argument("--board", default="192.168.33.2:3333", help="板子 host:port")
    ap.add_argument("--bind", default="127.0.0.1", help="本机监听地址（默认只绑本机）")
    ap.add_argument("--port", type=int, default=13333, help="本机监听端口")
    ap.add_argument("--fps", type=int, default=10, help="0x05 回传帧率（保活/自检用）")
    ap.add_argument("--fmt", default="jpeg", choices=("jpeg", "raw"), help="0x05 格式")
    ap.add_argument("--quality", type=int, default=None, help="JPEG 质量 1~100")
    ap.add_argument("--zoom", type=int, default=2, choices=(1, 2, 3), help="初始缩放")
    ap.add_argument("--selftest", action="store_true", help="无界面自检后退出")
    ap.add_argument("--selftest-images", type=int, default=5, help="自检要收到多少张图")
    ap.add_argument("--timeout", type=float, default=20.0, help="自检超时（秒）")
    ap.add_argument("--exit-after", type=float, default=0.0,
                    help="窗口跑满 N 秒后自动退出（0=不自动退出；便于脚本化验证）")
    args = ap.parse_args()

    bh, _, bp = args.board.partition(":")
    hub = Hub(bh, int(bp or 3333), args.bind, args.port, args.fps, args.fmt, args.quality)

    if args.selftest:
        return selftest(bh, int(bp or 3333), args.bind, args.port, args.fps, args.fmt,
                        args.selftest_images, args.timeout)

    hub.start()
    log(f"取景中枢启动：上游 {bh}:{bp or 3333} → 本机 {args.bind}:{args.port}"
        f"（脚本用 --tcp {args.bind}:{args.port}）")
    try:
        v = Viewer(hub, args.zoom)
        if args.exit_after > 0:
            v.root.after(int(args.exit_after * 1000), v._on_close)
            log(f"窗口将在 {args.exit_after:.0f}s 后自动退出（--exit-after）")
        v.root.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        hub.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
