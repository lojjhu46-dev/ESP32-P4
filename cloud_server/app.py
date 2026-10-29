#!/usr/bin/env python3
"""facecheck —— 极轻量人脸签到云服务（Python 3.6 标准库，无第三方依赖）。

职责（云端不做人脸比对）：
  * 接收 Qt 上位机转发的签到事件（match 边沿）+ 可选抓拍
  * 保存板端 db_export 的 JCDF 人脸库快照（字节级），供其他/重装设备下发
  * 提供 Web 后台（记录/人员/库/设备）与 CSV 导出

鉴权：X-Token / Authorization: Bearer / 登录换取的 HttpOnly 签名 cookie。
"""
import argparse
import base64
import binascii
import csv
import datetime
import hashlib
import hmac
import io
import json
import os
import re
import sqlite3
import sys
import threading
import time
import urllib.parse
import uuid
import zlib
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

import db
import facebank

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(HERE, "web")
TZ_CN = datetime.timezone(datetime.timedelta(hours=8))

DEFAULTS = {
    "host": "0.0.0.0",
    "port": 8080,
    "token": "",
    "cookie_secret": "",
    "data_dir": "/var/lib/facecheck",
    "dedup_window_s": 10,
    "snapshots_keep": 5,
    "photo_max_bytes": 2 * 1024 * 1024,
    "max_body_bytes": 8 * 1024 * 1024,
    "photo_keep_days": 90,
    "session_hours": 12,
    "max_concurrent": 8,
}


class ApiError(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code
        self.msg = msg


def iso_utc(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _strip_tz_colon(text):
    m = re.search(r"([+-]\d{2}):(\d{2})$", text)
    if m:
        return text[:m.start()] + m.group(1) + m.group(2)
    return text


def parse_ts(value):
    """Accept epoch seconds, ISO8601 with/without ms and offset (Qt ISODateWithMs)."""
    if value is None or value == "":
        return int(time.time())
    if isinstance(value, bool):
        raise ApiError(400, "bad ts")
    if isinstance(value, (int, float)):
        return int(value)
    text = str(value).strip()
    if re.match(r"^\d+(\.\d+)?$", text):
        return int(float(text))
    text = text.replace("Z", "+0000")
    text = re.sub(r"\.\d+", "", text)
    text = _strip_tz_colon(text)
    parsed = None
    for fmt in ("%Y-%m-%dT%H:%M:%S%z", "%Y-%m-%d %H:%M:%S%z",
                "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S"):
        try:
            parsed = datetime.datetime.strptime(text, fmt)
            break
        except ValueError:
            continue
    if parsed is None:
        raise ApiError(400, "bad ts")
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=TZ_CN)
    return int(parsed.timestamp())


def query_range(qs):
    now = int(time.time())
    try:
        ts_from = parse_ts(qs.get("from", [""])[0]) if qs.get("from", [""])[0] else now - 86400
        ts_to = parse_ts(qs.get("to", [""])[0]) if qs.get("to", [""])[0] else now
    except ApiError:
        raise
    if ts_from > ts_to:
        ts_from, ts_to = ts_to, ts_from
    return ts_from, ts_to


def _rss_kb():
    try:
        with open("/proc/self/status", "r") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except Exception:
        pass
    return 0


def load_config(path):
    cfg = dict(DEFAULTS)
    if path:
        with open(path, "r") as fh:
            cfg.update(json.load(fh))
    if not cfg.get("token") or cfg["token"] == "CHANGE_ME":
        raise SystemExit("config: token is empty/CHANGE_ME")
    if len(cfg.get("cookie_secret", "")) < 16:
        raise SystemExit("config: cookie_secret must be >= 16 chars")
    cfg["port"] = int(cfg["port"])
    cfg["dedup_window_s"] = int(cfg["dedup_window_s"])
    cfg["snapshots_keep"] = max(1, int(cfg["snapshots_keep"]))
    cfg["photo_max_bytes"] = int(cfg["photo_max_bytes"])
    cfg["max_body_bytes"] = int(cfg["max_body_bytes"])
    cfg["photo_keep_days"] = int(cfg["photo_keep_days"])
    cfg["session_hours"] = int(cfg["session_hours"])
    cfg["max_concurrent"] = max(1, int(cfg["max_concurrent"]))
    return cfg


class CloudServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, handler, cfg, db_path):
        HTTPServer.__init__(self, addr, handler)
        self.cfg = cfg
        self.db_path = db_path
        self.sem = threading.BoundedSemaphore(cfg["max_concurrent"])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 65
    server_version = "facecheck/1.0"

    # ---------------- logging (never print tokens/bodies) ----------------
    def log_request(self, code="-", size="-"):
        try:
            status = int(code)
        except Exception:
            status = 0
        if status >= 400:
            sys.stderr.write("%s %s -> %s\n" % (self.command, self.path.split("?")[0], code))

    def log_error(self, fmt, *args):
        msg = fmt % args
        if "timed out" in msg:
            return          # idle keep-alive connections are normal
        sys.stderr.write("http error: %s\n" % msg)

    # ---------------- transport helpers ----------------
    def _send_bytes(self, code, body, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        for key, val in (extra or []):
            self.send_header(key, val)
        if self.close_connection:
            self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD" and body:
            try:
                self.wfile.write(body)
            except OSError:
                pass

    def _json(self, code, obj, extra=None):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._send_bytes(code, body, "application/json; charset=utf-8", extra)

    def _read_body(self, limit):
        raw_len = self.headers.get("Content-Length")
        if raw_len is None:
            self.close_connection = True
            raise ApiError(411, "Content-Length required")
        try:
            length = int(raw_len)
        except ValueError:
            self.close_connection = True
            raise ApiError(400, "bad Content-Length")
        if length < 0:
            raise ApiError(400, "bad Content-Length")
        if length > limit:
            self.close_connection = True
            raise ApiError(413, "payload too large")
        chunks = []
        left = length
        while left > 0:
            chunk = self.rfile.read(min(left, 65536))
            if not chunk:
                break
            chunks.append(chunk)
            left -= len(chunk)
        return b"".join(chunks)

    def _read_json(self, limit=None):
        limit = limit or self.server.cfg["max_body_bytes"]
        raw = self._read_body(limit)
        if not raw:
            raise ApiError(400, "empty body")
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            raise ApiError(400, "bad json")
        if not isinstance(obj, dict):
            raise ApiError(400, "json object required")
        return obj

    # ---------------- auth ----------------
    def _token_ok(self, token):
        return bool(token) and hmac.compare_digest(
            str(token).encode("utf-8"), str(self.server.cfg["token"]).encode("utf-8")
        )

    def _header_token(self):
        token = self.headers.get("X-Token", "")
        if not token:
            auth = self.headers.get("Authorization", "")
            if auth.lower().startswith("bearer "):
                token = auth[7:].strip()
        return token

    def _cookie_ok(self):
        raw = self.headers.get("Cookie", "")
        if not raw:
            return False
        try:
            jar = SimpleCookie()
            jar.load(raw)
            morsel = jar.get("fc_sess")
            if not morsel:
                return False
            payload, sig = morsel.value.split(".", 1)
            exp = int(payload)
        except Exception:
            return False
        if exp < int(time.time()):
            return False
        want = hmac.new(
            self.server.cfg["cookie_secret"].encode("utf-8"),
            payload.encode("ascii"),
            hashlib.sha256,
        ).hexdigest()
        return hmac.compare_digest(sig, want)

    def _authorized(self):
        return self._token_ok(self._header_token()) or self._cookie_ok()

    def _session_cookie(self):
        cfg = self.server.cfg
        exp = int(time.time()) + int(cfg["session_hours"]) * 3600
        payload = str(exp)
        sig = hmac.new(
            cfg["cookie_secret"].encode("utf-8"), payload.encode("ascii"), hashlib.sha256
        ).hexdigest()
        return "fc_sess=%s.%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=%d" % (
            payload, sig, int(cfg["session_hours"]) * 3600
        )

    # ---------------- dispatch ----------------
    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def _dispatch(self, method):
        parts = urllib.parse.urlsplit(self.path)
        path, qs = parts.path, urllib.parse.parse_qs(parts.query)
        if not path.startswith("/api/"):
            if method != "GET":
                self.close_connection = True
                self._json(405, {"ok": False, "error": "method not allowed"})
                return
            self._serve_static(path)
            return
        if not self.server.sem.acquire(False):
            self._json(503, {"ok": False, "error": "busy"})
            return
        try:
            self._route_api(method, path, qs)
        except ApiError as exc:
            if method == "POST":
                # Body may not have been drained; do not reuse this connection.
                self.close_connection = True
            self._json(exc.code, {"ok": False, "error": exc.msg})
        except sqlite3.Error as exc:
            sys.stderr.write("sqlite error: %s\n" % exc)
            self._json(500, {"ok": False, "error": "database error"})
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write("internal error: %s: %s\n" % (type(exc).__name__, exc))
            self._json(500, {"ok": False, "error": "internal error"})
        finally:
            self.server.sem.release()

    def _route_api(self, method, path, qs):
        if path == "/api/health" and method == "GET":
            db_ok = False
            try:
                con = db.connect(self.server.db_path)
                con.execute("SELECT 1").fetchone()
                con.close()
                db_ok = True
            except Exception:
                pass
            self._json(200, {
                "ok": True, "version": "1.0", "time": int(time.time()),
                "db_ok": db_ok, "rss_kb": _rss_kb(),
                "dedup_window_s": self.server.cfg["dedup_window_s"],
            })
            return

        if path == "/api/login" and method == "POST":
            token = self._header_token()
            if not token:
                try:
                    obj = self._read_json(4096)
                    token = obj.get("token", "")
                except ApiError:
                    token = ""
            if not self._token_ok(token):
                raise ApiError(401, "invalid token")
            self._json(200, {"ok": True}, [("Set-Cookie", self._session_cookie())])
            return

        if not self._authorized():
            raise ApiError(401, "unauthorized")

        if path == "/api/logout" and method == "POST":
            self._json(200, {"ok": True},
                       [("Set-Cookie", "fc_sess=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0")])
            return
        if path == "/api/events" and method == "POST":
            self._api_event_post()
            return
        if path == "/api/events" and method == "GET":
            self._api_events_get(qs)
            return
        if path == "/api/events.csv" and method == "GET":
            self._api_events_csv(qs)
            return
        if path == "/api/persons" and method == "GET":
            self._api_persons()
            return
        if path == "/api/facebank" and method == "POST":
            self._api_facebank_post()
            return
        if path == "/api/facebank/latest" and method == "GET":
            self._api_facebank_latest(qs)
            return
        if path == "/api/facebank/meta" and method == "GET":
            self._api_facebank_meta()
            return
        if path == "/api/heartbeat" and method == "POST":
            self._api_heartbeat()
            return
        if path == "/api/devices" and method == "GET":
            self._api_devices()
            return
        m = re.match(r"^/api/photo/(\d+)$", path)
        if m and method == "GET":
            self._api_photo(int(m.group(1)))
            return
        raise ApiError(404, "not found")

    # ---------------- events ----------------
    def _api_event_post(self):
        cfg = self.server.cfg
        payload = self._read_json()
        event = str(payload.get("event", "") or "")
        if event and event != "match":
            raise ApiError(400, "only event=match is accepted")
        person = str(payload.get("person", "") or "").strip()
        if not person or len(person) > 64:
            raise ApiError(400, "person required (<=64 chars)")
        device = str(payload.get("device", "") or "").strip()[:64] or "unknown"
        ts = parse_ts(payload.get("ts"))
        now = int(time.time())
        if ts < now - 30 * 86400 or ts > now + 86400:
            raise ApiError(400, "ts out of range (check device clock)")

        photo_rel = None
        photo_abs = None
        photo_b64 = payload.get("photo_b64")
        if photo_b64:
            if not isinstance(photo_b64, str):
                raise ApiError(400, "photo_b64 must be base64 string")
            if len(photo_b64) > cfg["photo_max_bytes"] * 4 // 3 + 1024:
                raise ApiError(413, "photo too large")
            try:
                jpeg = base64.b64decode(photo_b64, validate=True)
            except (binascii.Error, ValueError):
                raise ApiError(400, "photo_b64 invalid")
            if len(jpeg) > cfg["photo_max_bytes"]:
                raise ApiError(413, "photo too large")
            if len(jpeg) < 4 or not jpeg.startswith(b"\xff\xd8\xff") \
                    or not jpeg.endswith(b"\xff\xd9"):
                raise ApiError(400, "photo is not a JPEG")

        raw = dict(payload)
        raw.pop("photo_b64", None)
        try:
            raw_json = json.dumps(raw, ensure_ascii=False, separators=(",", ":"))
        except Exception:
            raw_json = None
        if raw_json and len(raw_json) > 32768:
            raw_json = None

        con = db.connect(self.server.db_path)
        try:
            # Symmetric window on purpose: after a network outage the upper
            # computer flushes its spool out of chronological order, and a
            # look-back-only window lets a late-arriving older event slip
            # through next to an already stored newer one (observed 2026-10-29:
            # two personA check-ins 13 min apart both stored).
            dup_id = db.recent_event(
                con, person, device, ts - cfg["dedup_window_s"], ts + cfg["dedup_window_s"]
            )
            if dup_id:
                self._json(200, {"ok": True, "dup": True, "id": dup_id})
                return
            if photo_b64:
                day = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y/%m/%d")
                sub = os.path.join("photos", day)
                abs_dir = os.path.join(cfg["data_dir"], sub)
                os.makedirs(abs_dir, exist_ok=True)
                fname = "%d_%s.jpg" % (ts, uuid.uuid4().hex[:8])
                photo_rel = "/".join([sub.replace(os.sep, "/"), fname])
                photo_abs = os.path.join(cfg["data_dir"], photo_rel)
                tmp = photo_abs + ".tmp"
                with open(tmp, "wb") as fh:
                    fh.write(jpeg)
                os.replace(tmp, photo_abs)
            person_id = db.person_id_by_name(con, person)
            entry = {
                "ts": ts, "ts_iso": iso_utc(ts), "device": device, "person": person,
                "person_id": person_id, "is_unknown": person_id is None,
                "sim": payload.get("sim"), "sim_raw": payload.get("sim_raw"),
                "stable": payload.get("stable"), "faces": payload.get("faces"),
                "src": payload.get("src"), "link": payload.get("link"),
                "fps": payload.get("fps"), "infer_ms": payload.get("infer_ms"),
                "lib_n": payload.get("lib_n"), "photo": photo_rel,
                "raw_json": raw_json,
            }
            eid = db.insert_event(con, entry)
            db.upsert_device(con, device, lib_n=payload.get("lib_n"))
            con.commit()
        except Exception:
            con.rollback()
            if photo_abs and os.path.exists(photo_abs):
                try:
                    os.remove(photo_abs)
                except OSError:
                    pass
            raise
        finally:
            con.close()
        self._json(201, {"ok": True, "dup": False, "id": eid,
                         "unknown": entry["is_unknown"], "photo": photo_rel})

    def _event_rows(self, qs, default_limit, max_limit, ascending):
        ts_from, ts_to = query_range(qs)
        try:
            limit = int(qs.get("limit", [str(default_limit)])[0])
            offset = int(qs.get("offset", ["0"])[0])
        except ValueError:
            raise ApiError(400, "bad limit/offset")
        limit = max(1, min(max_limit, limit))
        offset = max(0, offset)
        con = db.connect(self.server.db_path)
        try:
            rows = db.query_events(
                con, ts_from, ts_to,
                person=(qs.get("person", [""])[0] or None),
                device=(qs.get("device", [""])[0] or None),
                limit=limit, offset=offset, ascending=ascending,
            )
            return rows, ts_from, ts_to
        finally:
            con.close()

    def _event_dict(self, row):
        return {
            "id": row["id"], "ts": row["ts"], "ts_iso": row["ts_iso"],
            "device": row["device"], "person": row["person"],
            "person_id": row["person_id"], "unknown": bool(row["is_unknown"]),
            "sim": row["sim"], "sim_raw": row["sim_raw"], "stable": row["stable"],
            "faces": row["faces"], "src": row["src"], "link": row["link"],
            "fps": row["fps"], "infer_ms": row["infer_ms"], "lib_n": row["lib_n"],
            "photo_url": ("/api/photo/%d" % row["id"]) if row["photo"] else None,
        }

    def _api_events_get(self, qs):
        rows, ts_from, ts_to = self._event_rows(qs, 200, 2000, False)
        self._json(200, {"ok": True, "from": ts_from, "to": ts_to,
                         "events": [self._event_dict(r) for r in rows]})

    def _api_events_csv(self, qs):
        rows, _ts_from, _ts_to = self._event_rows(qs, 50000, 50000, True)
        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow(["时间", "姓名", "相似度", "原始相似度", "设备", "链路", "照片"])
        for row in rows:
            local = datetime.datetime.fromtimestamp(row["ts"], TZ_CN).strftime("%Y-%m-%d %H:%M:%S")
            writer.writerow([
                local, row["person"],
                "" if row["sim"] is None else "%.4f" % float(row["sim"]),
                "" if row["sim_raw"] is None else "%.4f" % float(row["sim_raw"]),
                row["device"], row["link"] or "",
                ("/api/photo/%d" % row["id"]) if row["photo"] else "",
            ])
        body = ("\ufeff" + buf.getvalue()).encode("utf-8")
        stamp = datetime.datetime.now(TZ_CN).strftime("%Y%m%d_%H%M%S")
        self._send_bytes(200, body, "text/csv; charset=utf-8", [
            ("Content-Disposition", 'attachment; filename="facecheck_%s.csv"' % stamp),
        ])

    def _api_persons(self):
        con = db.connect(self.server.db_path)
        try:
            rows = db.list_persons(con)
            stats = {}
            for row in con.execute(
                    "SELECT person, MAX(ts) AS last_ts, COUNT(*) AS n FROM events GROUP BY person"):
                stats[row["person"]] = (row["last_ts"], row["n"])
        finally:
            con.close()
        out = []
        for row in rows:
            last_ts, n = stats.get(row["name"], (None, 0))
            out.append({
                "name": row["name"], "feats": row["feats"],
                "active": bool(row["active"]), "last_sign": last_ts,
                "sign_count": n, "snap_id": row["snap_id"],
            })
        self._json(200, {"ok": True, "persons": out})

    # ---------------- face bank ----------------
    def _api_facebank_post(self):
        cfg = self.server.cfg
        device = (self.headers.get("X-Device", "") or "").strip()[:64] or "default"
        source = (self.headers.get("X-Source", "") or "").strip()[:32] or "board"
        blob = self._read_body(4 * 1024 * 1024)
        if not blob:
            raise ApiError(400, "empty face bank")
        crc = zlib.crc32(blob) & 0xFFFFFFFF
        header_crc = self.headers.get("X-CRC32")
        if header_crc:
            try:
                if int(header_crc) != crc:
                    raise ApiError(422, "crc mismatch (header %s, actual %d)"
                                   % (header_crc, crc))
            except ValueError:
                raise ApiError(400, "bad X-CRC32")
        try:
            info = facebank.parse_jcdf(blob)
        except facebank.FacebankError as exc:
            raise ApiError(422, str(exc))

        now = int(time.time())
        con = db.connect(self.server.db_path)
        try:
            con.execute("BEGIN IMMEDIATE")
            sid = db.insert_snapshot(con, {
                "ts": now, "ts_iso": iso_utc(now), "device": device,
                "size": len(blob), "crc32": crc,
                "persons": len(info["persons"]), "feats": info["face_valid"],
                "blob": blob, "source": source,
            })
            db.rebuild_persons(con, info["persons"], sid, now)
            db.prune_snapshots(con, device, cfg["snapshots_keep"])
            # 不要用特征数覆盖 devices.lib_n：心跳里的 lib_n 是板端名单人数口径
            db.upsert_device(con, device)
            con.commit()
        except Exception:
            con.rollback()
            raise
        finally:
            con.close()
        self._json(201, {
            "ok": True, "id": sid, "size": len(blob), "crc32": crc,
            "persons": len(info["persons"]), "feats": info["face_valid"],
            "face_total": info["face_total"], "face_valid": info["face_valid"],
            "feat_len": info["feat_len"], "device": device,
        })

    def _api_facebank_latest(self, qs):
        snap_id = qs.get("id", [""])[0]
        device = (qs.get("device", [""])[0] or None)
        con = db.connect(self.server.db_path)
        try:
            if snap_id:
                try:
                    row = con.execute(
                        "SELECT * FROM facebank_snapshots WHERE id=?", (int(snap_id),)
                    ).fetchone()
                except ValueError:
                    raise ApiError(400, "bad id")
            else:
                row = db.latest_snapshot(con, device)
        finally:
            con.close()
        if not row:
            raise ApiError(404, "no face bank snapshot")
        self._send_bytes(200, row["blob"], "application/octet-stream", [
            ("X-Version", str(row["id"])), ("X-Size", str(row["size"])),
            ("X-CRC32", str(row["crc32"])), ("X-Persons", str(row["persons"])),
            ("X-Feats", str(row["feats"])),
            ("Content-Disposition", 'attachment; filename="facebank_%d.bin"' % row["id"]),
        ])

    def _api_facebank_meta(self):
        con = db.connect(self.server.db_path)
        try:
            rows = db.list_snapshots(con, 20)
        finally:
            con.close()
        self._json(200, {"ok": True, "snapshots": [dict(r) for r in rows]})

    def _api_heartbeat(self):
        payload = self._read_json(4096)
        device = str(payload.get("device", "") or "").strip()[:64] or "unknown"
        lib_n = payload.get("lib_n")
        fw = payload.get("fw")
        con = db.connect(self.server.db_path)
        try:
            db.upsert_device(con, device, lib_n=lib_n, fw=fw)
            con.commit()
        finally:
            con.close()
        self._json(200, {"ok": True, "device": device, "time": int(time.time())})

    def _api_devices(self):
        con = db.connect(self.server.db_path)
        try:
            rows = con.execute(
                "SELECT * FROM devices ORDER BY last_seen DESC").fetchall()
        finally:
            con.close()
        self._json(200, {"ok": True, "devices": [dict(r) for r in rows]})

    # ---------------- photos ----------------
    def _api_photo(self, event_id):
        con = db.connect(self.server.db_path)
        try:
            row = db.event_by_id(con, event_id)
        finally:
            con.close()
        if not row or not row["photo"]:
            raise ApiError(404, "photo not found")
        data_dir = os.path.realpath(self.server.cfg["data_dir"])
        photos_root = os.path.join(data_dir, "photos")
        full = os.path.realpath(os.path.join(data_dir, row["photo"]))
        if not (full == photos_root or full.startswith(photos_root + os.sep)):
            raise ApiError(403, "forbidden")
        try:
            with open(full, "rb") as fh:
                body = fh.read()
        except OSError:
            raise ApiError(404, "photo not found")
        self._send_bytes(200, body, "image/jpeg")

    # ---------------- static ----------------
    def _serve_static(self, path):
        if path == "/":
            path = "/index.html"
        names = {
            "/index.html": "text/html; charset=utf-8",
            "/app.js": "application/javascript; charset=utf-8",
            "/style.css": "text/css; charset=utf-8",
        }
        ctype = names.get(path)
        if not ctype:
            self._json(404, {"ok": False, "error": "not found"})
            return
        full = os.path.join(WEB_DIR, path.lstrip("/"))
        try:
            with open(full, "rb") as fh:
                body = fh.read()
        except OSError:
            self._json(404, {"ok": False, "error": "not found"})
            return
        self._send_bytes(200, body, ctype, [("Cache-Control", "no-cache")])


def janitor_loop(cfg, db_path):
    while True:
        try:
            cutoff = int(time.time()) - cfg["photo_keep_days"] * 86400
            con = db.connect(db_path)
            try:
                paths = db.clear_old_photos(con, cutoff)
                con.commit()
            finally:
                con.close()
            for rel in paths:
                full = os.path.realpath(os.path.join(cfg["data_dir"], rel))
                root = os.path.realpath(os.path.join(cfg["data_dir"], "photos"))
                if full.startswith(root + os.sep):
                    try:
                        os.remove(full)
                    except OSError:
                        pass
            for root, _dirs, files in os.walk(os.path.join(cfg["data_dir"], "photos")):
                for name in files:
                    if not name.endswith(".tmp"):
                        continue
                    full = os.path.join(root, name)
                    try:
                        if time.time() - os.path.getmtime(full) > 86400:
                            os.remove(full)
                    except OSError:
                        pass
            try:
                con = db.connect(db_path)
                con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                con.close()
            except Exception:
                pass
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write("janitor: %s\n" % exc)
        time.sleep(6 * 3600)


def main(argv=None):
    ap = argparse.ArgumentParser(description="facecheck lightweight sign-in service")
    ap.add_argument("--config", default="/etc/facecheck.json")
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--data-dir", default=None)
    args = ap.parse_args(argv)

    cfg = load_config(args.config)
    if args.host:
        cfg["host"] = args.host
    if args.port:
        cfg["port"] = args.port
    if args.data_dir:
        cfg["data_dir"] = args.data_dir
    cfg["data_dir"] = os.path.abspath(cfg["data_dir"])
    os.makedirs(cfg["data_dir"], exist_ok=True)
    photos = os.path.join(cfg["data_dir"], "photos")
    os.makedirs(photos, exist_ok=True)
    db_path = os.path.join(cfg["data_dir"], "facecheck.db")
    db.init(db_path)

    thread = threading.Thread(target=janitor_loop, args=(cfg, db_path), name="janitor")
    thread.daemon = True
    thread.start()

    srv = CloudServer((cfg["host"], cfg["port"]), Handler, cfg, db_path)
    sys.stderr.write("facecheck listening on %s:%d (data=%s)\n"
                     % (cfg["host"], cfg["port"], cfg["data_dir"]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
