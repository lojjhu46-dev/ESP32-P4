"""JCDF face-bank envelope parsing (byte-exact, no feature comparison).

Envelope layout (little endian), produced by the board's db_export:
    magic  "JCDF" (4 B)
    ver    u32 = 1
    face_len u32          # /storage/face.db size
    names_len u32         # /storage/names.json size
    names  JSON bytes     # [{"id": 9, "name": "personA"}, ...]
    face.db raw bytes     # 6 B header + N * (u16 id + feat_len * float32)

face.db header:
    u16 num_feats_total | u16 num_feats_valid | u16 feat_len (512)
    then 2050 B per slot: u16 id (0 = hole) + 512 * float32 (L2 normalized)
"""
import json
import struct
import zlib

MAGIC = b"JCDF"
VERSION = 1
ENVELOPE_HEADER = 16


class FacebankError(ValueError):
    pass


def crc32(buf):
    return zlib.crc32(buf) & 0xFFFFFFFF


def parse_face_db(blob):
    if len(blob) < 6:
        raise FacebankError("face.db too short")
    total, valid_header, feat_len = struct.unpack_from("<HHH", blob, 0)
    if feat_len < 1 or feat_len > 4096:
        raise FacebankError("bad feat_len %d" % feat_len)
    rec_size = 2 + feat_len * 4
    want = 6 + total * rec_size
    if len(blob) != want:
        raise FacebankError(
            "face.db size mismatch: got %d want %d (total=%d feat_len=%d)"
            % (len(blob), want, total, feat_len)
        )
    ids = []
    for i in range(total):
        (fid,) = struct.unpack_from("<H", blob, 6 + i * rec_size)
        ids.append(fid)
    valid = sum(1 for fid in ids if fid)
    return {
        "total": total,
        "valid_header": valid_header,
        "valid": valid,
        "feat_len": feat_len,
        "ids": ids,
    }


def parse_jcdf(buf):
    if len(buf) < ENVELOPE_HEADER:
        raise FacebankError("envelope too short")
    magic, ver, face_len, names_len = struct.unpack_from("<4sIII", buf, 0)
    if magic != MAGIC:
        raise FacebankError("bad magic %r" % magic)
    if ver != VERSION:
        raise FacebankError("unsupported version %d" % ver)
    if names_len > len(buf) - ENVELOPE_HEADER:
        raise FacebankError("names_len out of range")
    want = ENVELOPE_HEADER + names_len + face_len
    if len(buf) != want:
        raise FacebankError(
            "envelope size mismatch: got %d want %d" % (len(buf), want)
        )
    names_raw = buf[ENVELOPE_HEADER:ENVELOPE_HEADER + names_len]
    face_db = buf[ENVELOPE_HEADER + names_len:]
    try:
        names_obj = json.loads(names_raw.decode("utf-8"))
    except Exception as exc:
        raise FacebankError("names.json parse failed: %s" % exc)
    if not isinstance(names_obj, list):
        raise FacebankError("names.json is not a list")

    names = []
    id_to_name = {}
    for item in names_obj:
        if not isinstance(item, dict):
            continue
        try:
            fid = int(item.get("id"))
            name = str(item.get("name", "")).strip()
        except Exception:
            continue
        if fid <= 0 or not name:
            continue
        names.append({"id": fid, "name": name})
        id_to_name[fid] = name

    info = parse_face_db(face_db)
    counts = {}
    for fid in info["ids"]:
        name = id_to_name.get(fid)
        if name:
            counts[name] = counts.get(name, 0) + 1
    # Names whose feature slots were all deleted still belong to the roster
    # (0 feats) so the web page can show them.
    for item in names:
        counts.setdefault(item["name"], 0)
    persons = [{"name": k, "feats": v} for k, v in sorted(counts.items())]

    return {
        "version": ver,
        "face_len": face_len,
        "names_len": names_len,
        "names": names,
        "persons": persons,
        "face_total": info["total"],
        "face_valid": info["valid"],
        "feat_len": info["feat_len"],
        "face_db": face_db,
        "crc32": crc32(buf),
        "size": len(buf),
    }
