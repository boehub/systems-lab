"""Tiny TIFF and shapefile *writers*, used to build test files.

They exist so the tests can make files with exactly the features they want
(big-endian, tiles, LZW, predictor, planar bands, Z values...). They are
cross-checked against independent libraries in test_reference.py.
"""

import os
import struct
import zlib

def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


_STRUCT = {(1, 8): "B", (2, 8): "b", (1, 16): "H", (2, 16): "h", (1, 32): "I", (2, 32): "i",
           (3, 32): "f", (3, 64): "d", (1, 64): "Q", (2, 64): "q"}


def lzw_encode(data):
    """TIFF LZW (MSB-first codes, early change), the inverse of geotiff.lzw_decode."""
    out = bytearray()
    acc = nbits = 0
    size = 9

    def emit(code):
        nonlocal acc, nbits
        acc = (acc << size) | code
        nbits += size
        while nbits >= 8:
            out.append((acc >> (nbits - 8)) & 0xFF)
            nbits -= 8
        acc &= (1 << nbits) - 1

    table, nxt, w = {}, 258, b""
    emit(256)
    for byte in data:
        c = bytes([byte])
        wc = w + c
        if len(wc) == 1 or wc in table:
            w = wc
            continue
        emit(table[w] if len(w) > 1 else w[0])
        table[wc] = nxt
        nxt += 1
        if nxt >= (1 << size) and size < 12:
            size += 1
        if nxt >= 4094:
            emit(256)
            table, nxt, size = {}, 258, 9
        w = c
    if w:
        emit(table[w] if len(w) > 1 else w[0])
    emit(257)
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def write_tiff(width, height, bands, fmt=1, bits=8, big_endian=False, compression=1,
               predictor=1, rows_per_strip=None, tile=None, planar=1, geo=None, nodata=None):
    """Build a TIFF. `bands` is a list of flat row-major value lists, one per band.

    geo: dict with scale=(sx, sy), tie=(x, y) for the top-left corner, optional
    epsg, optional point=True (tie point is a cell centre).
    """
    e = ">" if big_endian else "<"
    code = _STRUCT[(fmt, bits)]
    nb = len(bands)
    mod = 1 << bits

    def pack(vals):
        return struct.pack(e + code * len(vals), *vals)

    def block(plane_bands, x0, y0, w, h, full_w, full_h):
        """Pixels of a block as rows of interleaved samples (zero-padded past the edge)."""
        rows = []
        for r in range(h):
            row = []
            for c in range(w):
                x, y = x0 + c, y0 + r
                for b in plane_bands:
                    row.append(b[y * width + x] if x < width and y < height else 0)
            rows.append(row)
        if predictor == 2:
            n = len(plane_bands)
            for row in rows:
                for i in range(len(row) - 1, n - 1, -1):
                    d = (row[i] - row[i - n]) % mod
                    row[i] = d - mod if code.islower() and d >= mod // 2 else d
        raw = b"".join(pack(r) for r in rows)
        if compression == 5:
            return lzw_encode(raw)
        if compression in (8, 32946):
            return zlib.compress(raw)
        return raw

    blocks = []
    if tile:
        tw, th = tile
        groups = [list(range(nb))] if planar == 1 else [[i] for i in range(nb)]
        for g in groups:
            for ty in range(-(-height // th)):
                for tx in range(-(-width // tw)):
                    blocks.append(block([bands[i] for i in g], tx * tw, ty * th, tw, th, width, height))
    else:
        rps = rows_per_strip or height
        groups = [list(range(nb))] if planar == 1 else [[i] for i in range(nb)]
        for g in groups:
            for y0 in range(0, height, rps):
                blocks.append(block([bands[i] for i in g], 0, y0, width, min(rps, height - y0), width, height))

    buf = bytearray(b"MM\0*" if big_endian else b"II*\0")
    buf += b"\0\0\0\0"
    offsets, counts = [], []
    for b in blocks:
        offsets.append(len(buf))
        counts.append(len(b))
        buf += b
        if len(buf) % 2:
            buf += b"\0"

    entries = {  # tag: (type, values)
        256: (4, [width]), 257: (4, [height]), 258: (3, [bits] * nb), 259: (3, [compression]),
        262: (3, [1]), 277: (3, [nb]), 284: (3, [planar]), 339: (3, [fmt] * nb),
    }
    if predictor != 1:
        entries[317] = (3, [predictor])
    if tile:
        entries[322], entries[323] = (4, [tile[0]]), (4, [tile[1]])
        entries[324], entries[325] = (4, offsets), (4, counts)
    else:
        entries[278] = (4, [rows_per_strip or height])
        entries[273], entries[279] = (4, offsets), (4, counts)
    if geo:
        entries[33550] = (12, [geo["scale"][0], geo["scale"][1], 0.0])
        x, y = geo["tie"]
        entries[33922] = (12, [0.0, 0.0, 0.0, x, y, 0.0])
        epsg = geo.get("epsg", 4326)
        directory = [1, 1, 0, 4,
                     1024, 0, 1, 2 if 4000 <= epsg < 5000 else 1,
                     1025, 0, 1, 2 if geo.get("point") else 1,
                     2048 if 4000 <= epsg < 5000 else 3072, 0, 1, epsg,
                     1026, 0, 1, 0]
        entries[34735] = (3, directory)
    if nodata is not None:
        text = (f"{nodata}" + "\0").encode()
        entries[42113] = (2, text)

    sizes = {2: 1, 3: 2, 4: 4, 12: 8}
    codes = {3: "H", 4: "I", 12: "d"}
    ifd = bytearray()
    ifd += struct.pack(e + "H", len(entries))
    tail = bytearray()
    ifd_at = len(buf)
    ifd_at += ifd_at % 2
    tail_at = ifd_at + 2 + 12 * len(entries) + 4
    for tag in sorted(entries):
        ftype, vals = entries[tag]
        if ftype == 2:
            raw, n = vals, len(vals)
        else:
            raw, n = struct.pack(e + codes[ftype] * len(vals), *vals), len(vals)
        if len(raw) <= 4:
            ifd += struct.pack(e + "HHI", tag, ftype, n) + raw.ljust(4, b"\0")
        else:
            ifd += struct.pack(e + "HHII", tag, ftype, n, tail_at + len(tail))
            tail += raw
            if len(tail) % 2:
                tail += b"\0"
    ifd += b"\0\0\0\0"
    if len(buf) % 2:
        buf += b"\0"
    buf += ifd + tail
    buf[4:8] = struct.pack(e + "I", ifd_at)
    return bytes(buf)


# ---- shapefiles -------------------------------------------------------------------

def _record(number, content):
    return struct.pack(">2i", number, len(content) // 2) + content


def _bbox(parts):
    xs = [p[0] for part in parts for p in part]
    ys = [p[1] for part in parts for p in part]
    return min(xs), min(ys), max(xs), max(ys)


def write_shapefile(base, kind, shapes, fields=(), rows=(), prj=None, z=False):
    """Write base.shp/.shx/.dbf. `shapes` is a list of parts-lists (each a list of
    (x, y)), or None for a null shape. kind: 1 point, 3 polyline, 5 polygon, 8 multipoint.
    fields: [(name, type, length, decimals)]; rows: list of value lists.
    z=True writes the Z variant of the type (with dummy Z values)."""
    file_kind = kind + 10 if z else kind
    contents = []
    for parts in shapes:
        if parts is None:
            contents.append(struct.pack("<i", 0))
            continue
        if kind == 1:
            x, y = parts[0][0]
            c = struct.pack("<i2d", file_kind, x, y) + (struct.pack("<2d", 0.0, 0.0) if z else b"")
        elif kind == 8:
            pts = parts[0]
            c = struct.pack("<i4di", file_kind, *_bbox(parts), len(pts))
            c += b"".join(struct.pack("<2d", *p) for p in pts)
            if z:
                c += struct.pack("<2d", 0.0, 0.0) + struct.pack(f"<{len(pts)}d", *([0.0] * len(pts)))
        else:
            pts = [p for part in parts for p in part]
            starts, at = [], 0
            for part in parts:
                starts.append(at)
                at += len(part)
            c = struct.pack("<i4d2i", file_kind, *_bbox(parts), len(parts), len(pts))
            c += struct.pack(f"<{len(parts)}i", *starts)
            c += b"".join(struct.pack("<2d", *p) for p in pts)
            if z:
                c += struct.pack("<2d", 0.0, 0.0) + struct.pack(f"<{len(pts)}d", *([0.0] * len(pts)))
        contents.append(c)

    shp_body = b"".join(_record(i + 1, c) for i, c in enumerate(contents))
    real = [p for parts in shapes if parts for p in [parts]]
    box = _bbox([pt for parts in real for pt in [part for part in parts]]) if real else (0, 0, 0, 0)
    head = struct.pack(">i5ii", 9994, 0, 0, 0, 0, 0, (100 + len(shp_body)) // 2)
    head += struct.pack("<2i4d4d", 1000, file_kind, *box, 0.0, 0.0, 0.0, 0.0)
    with open(base + ".shp", "wb") as f:
        f.write(head + shp_body)

    shx = struct.pack(">i5ii", 9994, 0, 0, 0, 0, 0, (100 + 8 * len(contents)) // 2)
    shx += struct.pack("<2i4d4d", 1000, file_kind, *box, 0.0, 0.0, 0.0, 0.0)
    off = 50
    for c in contents:
        shx += struct.pack(">2i", off, len(c) // 2)
        off += 4 + len(c) // 2
    with open(base + ".shx", "wb") as f:
        f.write(shx)

    if fields:
        rec_len = 1 + sum(f[2] for f in fields)
        hdr_len = 32 + 32 * len(fields) + 1
        dbf = struct.pack("<4xIHH20x", len(rows), hdr_len, rec_len)
        dbf = b"\x03\x7e\x0b\x0f" + dbf[4:]
        for name, ftype, length, dec in fields:
            dbf += name.encode().ljust(11, b"\0") + ftype.encode() + b"\0" * 4 + bytes([length, dec]) + b"\0" * 14
        dbf += b"\r"
        for row in rows:
            dbf += b" "
            for (name, ftype, length, dec), v in zip(fields, row):
                if v is None:
                    text = ""
                elif ftype in "NF":
                    text = f"{v:.{dec}f}"
                elif ftype == "L":
                    text = "T" if v else "F"
                else:
                    text = str(v)
                raw = text.encode("utf-8")[:length]
                dbf += raw.rjust(length) if ftype in "NF" else raw.ljust(length)
        dbf += b"\x1a"
        with open(base + ".dbf", "wb") as f:
            f.write(dbf)
    if prj:
        with open(base + ".prj", "w") as f:
            f.write(prj)
    return base + ".shp"


def square(x0, y0, x1, y1, hole=False):
    """A closed rectangle ring: clockwise for an outer ring, counter-clockwise for a hole."""
    cw = [(x0, y0), (x0, y1), (x1, y1), (x1, y0), (x0, y0)]
    return list(reversed(cw)) if hole else cw
