"""ESRI shapefile reader: .shp geometry plus .dbf attributes.

The format is a 100-byte header followed by records. Most numbers are
little-endian, but the file code, file length and record headers are
big-endian (a quirk of the format's history), which is why the code below
switches byte order from field to field.
"""

import os
import struct
from dataclasses import dataclass, field

from . import geometry
from .errors import GeoError

NULL, POINT, POLYLINE, POLYGON, MULTIPOINT = 0, 1, 3, 5, 8
# Z and M variants carry extra arrays after the x/y data; we read x/y and ignore the rest.
_BASE_TYPE = {0: 0, 1: 1, 3: 3, 5: 5, 8: 8, 11: 1, 13: 3, 15: 5, 18: 8, 21: 1, 23: 3, 25: 5, 28: 8}
TYPE_NAMES = {NULL: "Null", POINT: "Point", POLYLINE: "PolyLine", POLYGON: "Polygon",
              MULTIPOINT: "MultiPoint"}


@dataclass
class Feature:
    record: int                 # 1-based record number
    kind: int                   # NULL, POINT, POLYLINE, POLYGON or MULTIPOINT
    parts: list = field(default_factory=list)   # lists of (x, y); one part per ring/line
    bbox: tuple = None          # (xmin, ymin, xmax, ymax)
    attrs: dict = field(default_factory=dict)

    @property
    def area(self):
        return geometry.polygon_area(self.parts) if self.kind == POLYGON else 0.0

    @property
    def length(self):
        if self.kind == POLYLINE:
            return sum(geometry.line_length(p) for p in self.parts)
        if self.kind == POLYGON:
            return sum(geometry.line_length(p) for p in self.parts)  # perimeter
        return 0.0

    def contains(self, x, y):
        if self.kind != POLYGON:
            return False
        if self.bbox and not (self.bbox[0] <= x <= self.bbox[2] and self.bbox[1] <= y <= self.bbox[3]):
            return False
        return geometry.point_in_rings(self.parts, x, y)


@dataclass
class Shapefile:
    path: str
    kind: int
    bbox: tuple
    features: list
    fields: list                # [(name, type, length, decimals)]
    prj: str = ""
    has_z_or_m: bool = False


def _read_exact(buf, pos, size, what):
    if pos + size > len(buf):
        raise GeoError(f"file ends in the middle of {what}")
    return buf[pos:pos + size]


def _parse_shp(data):
    if len(data) < 100:
        raise GeoError("not a shapefile: shorter than the 100-byte header")
    code, = struct.unpack(">i", data[0:4])
    if code != 9994:
        raise GeoError(f"not a shapefile: file code is {code}, expected 9994")
    file_words, = struct.unpack(">i", data[24:28])
    version, header_kind = struct.unpack("<2i", data[28:36])
    if version != 1000:
        raise GeoError(f"unsupported shapefile version {version}")
    if file_words * 2 > len(data):
        raise GeoError(f"header says the file is {file_words * 2} bytes but only {len(data)} are present")
    if header_kind not in _BASE_TYPE:
        raise GeoError(f"unsupported shape type {header_kind} (MultiPatch is not supported)")
    bbox = struct.unpack("<4d", data[36:68])

    features = []
    pos = 100
    end = file_words * 2
    while pos < end:
        number, words = struct.unpack(">2i", _read_exact(data, pos, 8, "a record header"))
        pos += 8
        if words < 2:
            raise GeoError(f"record {number}: its length ({words * 2} bytes) is too small")
        body = _read_exact(data, pos, words * 2, f"record {number}")
        pos += words * 2
        kind_raw, = struct.unpack("<i", body[0:4])
        if kind_raw not in _BASE_TYPE:
            raise GeoError(f"record {number}: unsupported shape type {kind_raw}")
        kind = _BASE_TYPE[kind_raw]
        feat = Feature(record=number, kind=kind)
        if kind == NULL:
            pass
        elif kind == POINT:
            x, y = struct.unpack("<2d", _read_exact(body, 4, 16, f"record {number}"))
            feat.parts = [[(x, y)]]
            feat.bbox = (x, y, x, y)
        elif kind == MULTIPOINT:
            feat.bbox = struct.unpack("<4d", _read_exact(body, 4, 32, f"record {number}"))
            n, = struct.unpack("<i", _read_exact(body, 36, 4, f"record {number}"))
            if n < 0:
                raise GeoError(f"record {number}: negative point count")
            raw = _read_exact(body, 40, 16 * n, f"record {number}")
            feat.parts = [[struct.unpack_from("<2d", raw, 16 * i) for i in range(n)]]
        else:  # polyline or polygon
            feat.bbox = struct.unpack("<4d", _read_exact(body, 4, 32, f"record {number}"))
            nparts, npoints = struct.unpack("<2i", _read_exact(body, 36, 8, f"record {number}"))
            if nparts < 0 or npoints < 0:
                raise GeoError(f"record {number}: negative part or point count")
            starts = list(struct.unpack(f"<{nparts}i", _read_exact(body, 44, 4 * nparts, f"record {number}")))
            pts_at = 44 + 4 * nparts
            raw = _read_exact(body, pts_at, 16 * npoints, f"record {number}")
            points = [struct.unpack_from("<2d", raw, 16 * i) for i in range(npoints)]
            starts.append(npoints)
            for a, b in zip(starts, starts[1:]):
                if not (0 <= a <= b <= npoints):
                    raise GeoError(f"record {number}: part offsets are out of range")
                feat.parts.append(points[a:b])
        features.append(feat)
    return header_kind, bbox, features


# ---- dBase attribute table -----------------------------------------------------

def _decode(raw, encodings):
    for enc in encodings:
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("latin-1")


def _parse_dbf(data, encodings):
    if len(data) < 32:
        raise GeoError("attribute table (.dbf) is too short")
    nrecords, header_len, record_len = struct.unpack("<IHH", data[4:12])
    fields, pos = [], 32
    while pos < header_len - 1 and data[pos] != 0x0D:
        desc = _read_exact(data, pos, 32, "the .dbf field list")
        name = _decode(desc[0:11].split(b"\0")[0], encodings)
        fields.append((name, chr(desc[11]), desc[16], desc[17]))
        pos += 32
    rows = []
    for i in range(nrecords):
        start = header_len + i * record_len
        rec = _read_exact(data, start, record_len, f"attribute row {i + 1}")
        if rec[0:1] == b"*":  # deleted: keep the slot so rows stay aligned with shapes
            rows.append(None)
            continue
        row, off = {}, 1
        for name, ftype, length, decimals in fields:
            text = _decode(rec[off:off + length], encodings).strip()
            off += length
            row[name] = _convert(text, ftype, decimals)
        rows.append(row)
    return fields, rows


def _convert(text, ftype, decimals):
    if ftype in "NF":
        if not text or set(text) <= {"*"}:
            return None
        try:
            return float(text) if (decimals or "." in text or "e" in text.lower()) else int(text)
        except ValueError:
            return None
    if ftype == "L":
        return True if text[:1] in "TtYy" and text else (False if text[:1] in "FfNn" and text else None)
    if ftype == "D":
        return f"{text[0:4]}-{text[4:6]}-{text[6:8]}" if len(text) == 8 and text.isdigit() else (text or None)
    return text


def open_shapefile(path):
    base, ext = os.path.splitext(path)
    if ext.lower() not in (".shp", ""):
        raise GeoError(f"{path}: expected a .shp file")
    shp = base + ".shp"
    with open(shp, "rb") as f:
        kind, bbox, features = _parse_shp(f.read())

    def sibling(suffix):
        for s in (suffix, suffix.upper()):
            p = base + s
            if os.path.exists(p):
                return p
        return None

    fields = []
    dbf = sibling(".dbf")
    if dbf:
        encodings = ["utf-8", "cp1252"]
        cpg = sibling(".cpg")
        if cpg:
            with open(cpg) as f:
                encodings.insert(0, f.read().strip())
        with open(dbf, "rb") as f:
            fields, rows = _parse_dbf(f.read(), encodings)
        if len(rows) != len(features):
            raise GeoError(f"the .shp has {len(features)} shapes but the .dbf has {len(rows)} rows")
        for feat, row in zip(features, rows):
            feat.attrs = row or {}
    prj = ""
    p = sibling(".prj")
    if p:
        with open(p, errors="replace") as f:
            prj = f.read().strip()
    return Shapefile(shp, _BASE_TYPE[kind], bbox, features, fields, prj, kind != _BASE_TYPE[kind])
