"""SQLite access for facecheck.

One connection per HTTP request (WAL + busy_timeout).  The cloud never
compares face features: it stores attendance events and byte-exact JCDF
face-bank snapshots exported by the board.
"""
import os
import sqlite3

SCHEMA = """
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    ts_iso TEXT NOT NULL,
    device TEXT NOT NULL,
    person TEXT NOT NULL,
    person_id INTEGER,
    is_unknown INTEGER NOT NULL DEFAULT 0,
    sim REAL,
    sim_raw REAL,
    stable INTEGER,
    faces INTEGER,
    src TEXT,
    link TEXT,
    fps REAL,
    infer_ms INTEGER,
    lib_n INTEGER,
    photo TEXT,
    raw_json TEXT
);
CREATE INDEX IF NOT EXISTS ix_ev_ts ON events(ts);
CREATE INDEX IF NOT EXISTS ix_ev_person ON events(person, device, ts);
CREATE INDEX IF NOT EXISTS ix_ev_device ON events(device, ts);

CREATE TABLE IF NOT EXISTS facebank_snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    ts_iso TEXT NOT NULL,
    device TEXT NOT NULL,
    size INTEGER NOT NULL,
    crc32 INTEGER NOT NULL,
    persons INTEGER NOT NULL,
    feats INTEGER NOT NULL,
    blob BLOB NOT NULL,
    source TEXT
);
CREATE INDEX IF NOT EXISTS ix_fb_ts ON facebank_snapshots(ts DESC);

CREATE TABLE IF NOT EXISTS persons (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL,
    feats INTEGER NOT NULL DEFAULT 0,
    snap_id INTEGER,
    first_seen INTEGER,
    last_seen INTEGER,
    active INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    name TEXT,
    last_seen INTEGER,
    lib_n INTEGER,
    fw TEXT
);

CREATE TABLE IF NOT EXISTS settings (
    k TEXT PRIMARY KEY,
    v TEXT
);
"""


def connect(db_path):
    con = sqlite3.connect(db_path, timeout=10.0)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA busy_timeout=5000")
    return con


def init(db_path):
    parent = os.path.dirname(os.path.abspath(db_path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    con = connect(db_path)
    try:
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("PRAGMA synchronous=NORMAL")
        con.executescript(SCHEMA)
        con.commit()
    finally:
        con.close()


def close(con):
    try:
        con.close()
    except Exception:
        pass


def recent_event(con, person, device, ts_lo, ts_hi):
    row = con.execute(
        "SELECT id FROM events WHERE person=? AND device=? AND ts>=? AND ts<=? "
        "ORDER BY ts DESC LIMIT 1",
        (person, device, ts_lo, ts_hi),
    ).fetchone()
    return row["id"] if row else None


def person_id_by_name(con, name):
    row = con.execute(
        "SELECT id FROM persons WHERE name=? AND active=1", (name,)
    ).fetchone()
    return row["id"] if row else None


def insert_event(con, e):
    cur = con.execute(
        "INSERT INTO events(ts, ts_iso, device, person, person_id, is_unknown, sim,"
        " sim_raw, stable, faces, src, link, fps, infer_ms, lib_n, photo, raw_json)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            e["ts"],
            e["ts_iso"],
            e["device"],
            e["person"],
            e.get("person_id"),
            1 if e.get("is_unknown") else 0,
            e.get("sim"),
            e.get("sim_raw"),
            e.get("stable"),
            e.get("faces"),
            e.get("src"),
            e.get("link"),
            e.get("fps"),
            e.get("infer_ms"),
            e.get("lib_n"),
            e.get("photo"),
            e.get("raw_json"),
        ),
    )
    return cur.lastrowid


def query_events(con, ts_from, ts_to, person=None, device=None, limit=200, offset=0,
                 ascending=False):
    sql = "SELECT * FROM events WHERE ts>=? AND ts<=?"
    args = [ts_from, ts_to]
    if person:
        sql += " AND person=?"
        args.append(person)
    if device:
        sql += " AND device=?"
        args.append(device)
    sql += " ORDER BY ts " + ("ASC" if ascending else "DESC") + " LIMIT ? OFFSET ?"
    args.extend([int(limit), int(offset)])
    return con.execute(sql, args).fetchall()


def event_by_id(con, event_id):
    return con.execute("SELECT * FROM events WHERE id=?", (event_id,)).fetchone()


def upsert_device(con, device, lib_n=None, fw=None, now=None):
    import time as _time

    now = int(now if now is not None else _time.time())
    row = con.execute("SELECT id FROM devices WHERE id=?", (device,)).fetchone()
    if row:
        con.execute(
            "UPDATE devices SET last_seen=?, lib_n=COALESCE(?, lib_n),"
            " fw=COALESCE(?, fw) WHERE id=?",
            (now, lib_n, fw, device),
        )
    else:
        con.execute(
            "INSERT INTO devices(id, name, last_seen, lib_n, fw) VALUES(?,?,?,?,?)",
            (device, device, now, lib_n, fw),
        )


def insert_snapshot(con, snap):
    cur = con.execute(
        "INSERT INTO facebank_snapshots(ts, ts_iso, device, size, crc32, persons,"
        " feats, blob, source) VALUES(?,?,?,?,?,?,?,?,?)",
        (
            snap["ts"], snap["ts_iso"], snap["device"], snap["size"],
            snap["crc32"], snap["persons"], snap["feats"], snap["blob"],
            snap.get("source"),
        ),
    )
    return cur.lastrowid


def latest_snapshot(con, device=None):
    if device:
        return con.execute(
            "SELECT * FROM facebank_snapshots WHERE device=? ORDER BY id DESC LIMIT 1",
            (device,),
        ).fetchone()
    return con.execute(
        "SELECT * FROM facebank_snapshots ORDER BY id DESC LIMIT 1"
    ).fetchone()


def list_snapshots(con, limit=20):
    return con.execute(
        "SELECT id, ts, ts_iso, device, size, crc32, persons, feats, source"
        " FROM facebank_snapshots ORDER BY id DESC LIMIT ?", (int(limit),)
    ).fetchall()


def prune_snapshots(con, device, keep):
    rows = con.execute(
        "SELECT id FROM facebank_snapshots WHERE device=? ORDER BY id DESC",
        (device,),
    ).fetchall()
    doomed = [r["id"] for r in rows[int(keep):]]
    for sid in doomed:
        con.execute("DELETE FROM facebank_snapshots WHERE id=?", (sid,))


def list_persons(con):
    return con.execute(
        "SELECT * FROM persons ORDER BY active DESC, name COLLATE NOCASE"
    ).fetchall()


def rebuild_persons(con, persons, snap_id, now):
    """persons: [{'name': str, 'feats': int}] parsed from the snapshot.

    Upsert by name; entries missing from the new snapshot are marked
    inactive (never deleted, so old events keep a valid person_id).
    """
    seen = set()
    for item in persons:
        name = item["name"]
        feats = int(item["feats"])
        if not name:
            continue
        seen.add(name)
        row = con.execute(
            "SELECT id FROM persons WHERE name=?", (name,)
        ).fetchone()
        if row:
            con.execute(
                "UPDATE persons SET feats=?, snap_id=?, last_seen=?, active=1 WHERE id=?",
                (feats, snap_id, now, row["id"]),
            )
        else:
            con.execute(
                "INSERT INTO persons(name, feats, snap_id, first_seen, last_seen, active)"
                " VALUES(?,?,?,?,?,1)",
                (name, feats, snap_id, now, now),
            )
    for row in con.execute("SELECT id, name FROM persons WHERE active=1").fetchall():
        if row["name"] not in seen:
            con.execute(
                "UPDATE persons SET active=0, feats=0 WHERE id=?", (row["id"],)
            )


def clear_old_photos(con, cutoff_ts):
    rows = con.execute(
        "SELECT photo FROM events WHERE photo IS NOT NULL AND ts<?", (cutoff_ts,)
    ).fetchall()
    con.execute(
        "UPDATE events SET photo=NULL WHERE photo IS NOT NULL AND ts<?", (cutoff_ts,)
    )
    return [r["photo"] for r in rows]
