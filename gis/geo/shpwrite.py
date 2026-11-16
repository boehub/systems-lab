"""Write an ESRI shapefile (.shp, .shx, .dbf, .prj): the inverse of shapefile.open_shapefile."""

import struct

from .errors import GeoError
from .shapefile import NULL, POINT, POLYLINE, POLYGON, MULTIPOINT


def _bbox(parts):
    xs = [p[0] for part in parts for p in part]
    ys = [p[1] for part in parts for p in part]
    return (min(xs), min(ys), max(xs), max(ys)) if xs else (0.0, 0.0, 0.0, 0.0)


def _shape(kind, parts):
    if not parts or not any(parts):
        return struct.pack("<i", NULL)
    if kind == POINT:
        return struct.pack("<i2d", POINT, *parts[0][0])
    pts = [p for part in parts for p in part]
    if kind == MULTIPOINT:
        return struct.pack("<i4di", MULTIPOINT, *_bbox(parts), len(pts)) + b"".join(struct.pack("<2d", *p) for p in pts)
    starts, at = [], 0
    for part in parts:
        starts.append(at)
        at += len(part)
    return (struct.pack("<i4d2i", kind, *_bbox(parts), len(parts), len(pts))
            + struct.pack(f"<{len(parts)}i", *starts) + b"".join(struct.pack("<2d", *p) for p in pts))


def _dbf(fields, rows):
    rec_len = 1 + sum(f[2] for f in fields)
    out = struct.pack("<4B I H H 20x", 3, 126, 11, 15, len(rows), 32 + 32 * len(fields) + 1, rec_len)
    for name, ftype, length, dec in fields:
        out += name.encode("utf-8")[:10].ljust(11, b"\0") + ftype.encode() + b"\0" * 4 + bytes([length, dec]) + b"\0" * 14
    out += b"\r"
    for row in rows:
        out += b" "
        for (name, ftype, length, dec), v in zip(fields, row):
            if v is None:
                text = ""
            elif ftype in "NF":
                text = f"{v:.{dec}f}" if dec else str(int(v)) if float(v).is_integer() else f"{v:.6f}"
            elif ftype == "L":
                text = "T" if v else "F"
            elif ftype == "D":
                text = str(v).replace("-", "")
            else:
                text = str(v)
            raw = text.encode("utf-8")[:length]
            out += raw.rjust(length) if ftype in "NF" else raw.ljust(length)
    return out + b"\x1a"


def write_shapefile(path, kind, features, fields=(), prj=""):
    """Write `features` (shapefile.Feature objects) as path.shp/.shx/.dbf (and .prj when
    given). `fields` is the [(name, type, length, decimals)] list the reader gives."""
    if kind not in (POINT, POLYLINE, POLYGON, MULTIPOINT):
        raise GeoError(f"cannot write shape type {kind}")
    base = path[:-4] if path.lower().endswith(".shp") else path
    shapes = [_shape(kind, f.parts if f.kind != NULL else None) for f in features]
    boxes = [_bbox(f.parts) for f in features if f.kind != NULL and any(f.parts)]
    total = (min(b[0] for b in boxes), min(b[1] for b in boxes),
             max(b[2] for b in boxes), max(b[3] for b in boxes)) if boxes else (0.0, 0.0, 0.0, 0.0)

    def header(words):
        return struct.pack(">7i", 9994, 0, 0, 0, 0, 0, words) + struct.pack("<2i8d", 1000, kind, *total, 0.0, 0.0, 0.0, 0.0)

    body = b"".join(struct.pack(">2i", i + 1, len(s) // 2) + s for i, s in enumerate(shapes))
    with open(base + ".shp", "wb") as f:
        f.write(header((100 + len(body)) // 2) + body)
    index, off = b"", 50
    for s in shapes:
        index += struct.pack(">2i", off, len(s) // 2)
        off += 4 + len(s) // 2
    with open(base + ".shx", "wb") as f:
        f.write(header((100 + len(index)) // 2) + index)
    if fields:
        rows = [[ft.attrs.get(name) for name, _, _, _ in fields] for ft in features]
        with open(base + ".dbf", "wb") as f:
            f.write(_dbf(list(fields), rows))
        with open(base + ".cpg", "w") as f:
            f.write("UTF-8")
    if prj:
        with open(base + ".prj", "w") as f:
            f.write(prj)
    return base + ".shp"
