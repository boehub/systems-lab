"""GeoTIFF reader: TIFF structure, strip/tile decoding, and georeferencing.

A TIFF file is a header pointing at an "image file directory" (IFD): a table of
tagged values (width, height, where the pixel data is, how it is compressed...).
GeoTIFF adds three more tags that say where the pixel grid sits on the earth.

Supported: classic TIFF (not BigTIFF), either byte order, strips or tiles,
uncompressed / LZW / Deflate, horizontal-differencing predictor, 8/16/32/64-bit
integers and 32/64-bit floats, any number of bands. Not supported: JPEG and
other compression, floating-point predictor, palettes and bit depths below 8.
"""

import array
import math
import struct
import sys
import zlib
from dataclasses import dataclass

from .errors import GeoError

# TIFF tag numbers
IMAGE_WIDTH, IMAGE_LENGTH, BITS_PER_SAMPLE, COMPRESSION = 256, 257, 258, 259
STRIP_OFFSETS, SAMPLES_PER_PIXEL, ROWS_PER_STRIP, STRIP_BYTE_COUNTS = 273, 277, 278, 279
PLANAR_CONFIG, PREDICTOR = 284, 317
TILE_WIDTH, TILE_LENGTH, TILE_OFFSETS, TILE_BYTE_COUNTS = 322, 323, 324, 325
SAMPLE_FORMAT = 339
MODEL_PIXEL_SCALE, MODEL_TIEPOINT, MODEL_TRANSFORMATION = 33550, 33922, 34264
GEO_KEY_DIRECTORY, GDAL_NODATA = 34735, 42113

# field type -> (struct code, size in bytes)
_FIELD_TYPES = {1: ("B", 1), 2: ("c", 1), 3: ("H", 2), 4: ("I", 4), 5: ("II", 8), 6: ("b", 1),
                7: ("B", 1), 8: ("h", 2), 9: ("i", 4), 10: ("ii", 8), 11: ("f", 4), 12: ("d", 8)}

# (sample format, bits) -> array typecode
_DTYPES = {(1, 8): "B", (2, 8): "b", (1, 16): "H", (2, 16): "h", (1, 32): "I", (2, 32): "i",
           (1, 64): "Q", (2, 64): "q", (3, 32): "f", (3, 64): "d"}


def lzw_decode(data):
    """TIFF's flavour of LZW: codes are read most-significant-bit first, 9 to 12
    bits wide, and the width grows one code early ("early change")."""
    out = bytearray()
    table = []
    size = 9
    nxt = 258
    prev = None
    acc = nbits = 0

    def reset():
        nonlocal table, size, nxt, prev
        table = [bytes([i]) for i in range(256)] + [b"", b""]
        size, nxt, prev = 9, 258, None

    reset()
    for byte in data:
        acc = (acc << 8) | byte
        nbits += 8
        while nbits >= size:
            code = (acc >> (nbits - size)) & ((1 << size) - 1)
            nbits -= size
            acc &= (1 << nbits) - 1
            if code == 257:  # end of information
                return bytes(out)
            if code == 256:  # clear table
                reset()
                continue
            if code < nxt:
                entry = table[code]
            elif code == nxt and prev is not None:
                entry = prev + prev[:1]
            else:
                raise GeoError("damaged LZW data")
            out += entry
            if prev is not None:
                table.append(prev + entry[:1])
                nxt += 1
                if nxt + 1 >= (1 << size) and size < 12:
                    size += 1
            prev = entry
    return bytes(out)


def _undo_predictor(rows, width, spp, typecode):
    """Horizontal differencing: each sample was stored as the difference from the
    sample to its left. Add them back up, wrapping at the sample size."""
    size = array.array(typecode).itemsize * 8
    mod = 1 << size
    half = mod >> 1
    signed = typecode.islower()
    for r, row in enumerate(rows):
        vals = [v % mod for v in row]  # work unsigned, whatever the real type
        for i in range(spp, width * spp):
            vals[i] = (vals[i] + vals[i - spp]) % mod
        if signed:
            vals = [v - mod if v >= half else v for v in vals]
        rows[r] = array.array(typecode, vals)


@dataclass
class Georef:
    """Where the pixel grid sits: the top-left corner and the cell size."""
    x0: float
    y0: float
    dx: float            # cell width (positive: columns run east)
    dy: float            # cell height (positive: rows run south)
    width: int
    height: int
    epsg: int = None
    is_point: bool = False

    def bounds(self):
        return (self.x0, self.y0 - self.dy * self.height, self.x0 + self.dx * self.width, self.y0)

    def cell_center(self, col, row):
        return (self.x0 + (col + 0.5) * self.dx, self.y0 - (row + 0.5) * self.dy)

    def cell_at(self, x, y):
        """(col, row) of the cell containing the point, or None if it is off the grid."""
        try:
            col = math.floor((x - self.x0) / self.dx)
            row = math.floor((self.y0 - y) / self.dy)
        except (OverflowError, ValueError):  # a corrupt file's absurd numbers
            return None
        if 0 <= col < self.width and 0 <= row < self.height:
            return col, row
        return None


class GeoTiff:
    def __init__(self, data, path="<memory>"):
        self.path = path
        self._data = data
        self._cache = {}
        self._parse()

    # ---- TIFF structure ----------------------------------------------------

    def _parse(self):
        d = self._data
        if len(d) < 8:
            raise GeoError("not a TIFF: file is too short")
        if d[0:2] == b"II":
            self._e = "<"
        elif d[0:2] == b"MM":
            self._e = ">"
        else:
            raise GeoError("not a TIFF: bad byte-order mark")
        magic, ifd = struct.unpack(self._e + "HI", d[2:8])
        if magic == 43:
            raise GeoError("BigTIFF is not supported")
        if magic != 42:
            raise GeoError("not a TIFF: bad magic number")
        self.tags = self._read_ifd(ifd)

        t = self.tags
        self.width = self._one(IMAGE_WIDTH)
        self.height = self._one(IMAGE_LENGTH)
        self.bands = self._one(SAMPLES_PER_PIXEL, 1)
        if self.width < 1 or self.height < 1 or self.bands < 1:
            raise GeoError("the image has no pixels")
        bits = self._all(BITS_PER_SAMPLE, [1])
        if len(set(bits)) != 1:
            raise GeoError("bands with different bit depths are not supported")
        self.bits = bits[0]
        fmt = self._all(SAMPLE_FORMAT, [1])[0]
        if (fmt, self.bits) not in _DTYPES:
            raise GeoError(f"unsupported pixel type (format {fmt}, {self.bits} bits)")
        self.sample_format = fmt
        self.typecode = _DTYPES[(fmt, self.bits)]
        if array.array(self.typecode).itemsize * 8 != self.bits:
            raise GeoError("this platform's array type doesn't match the file's pixel size")
        if self.width * self.height * self.bands * (self.bits // 8) > 1 << 31:
            raise GeoError("image is larger than this reader will load (2 GB)")
        self.compression = self._one(COMPRESSION, 1)
        if self.compression not in (1, 5, 8, 32946):
            raise GeoError(f"unsupported compression {self.compression} (supported: none, LZW, Deflate)")
        self.predictor = self._one(PREDICTOR, 1)
        if self.predictor not in (1, 2):
            raise GeoError(f"unsupported predictor {self.predictor}")
        if self.predictor == 2 and fmt == 3:
            raise GeoError("horizontal predictor on floating-point data is not valid")
        self.planar = self._one(PLANAR_CONFIG, 1)
        self.tiled = TILE_WIDTH in t
        if self.tiled:
            self.block_w, self.block_h = self._one(TILE_WIDTH), self._one(TILE_LENGTH)
            self.offsets, self.counts = self._all(TILE_OFFSETS), self._all(TILE_BYTE_COUNTS)
        else:
            self.block_w = self.width
            self.block_h = min(self._one(ROWS_PER_STRIP, self.height), self.height)
            self.offsets, self.counts = self._all(STRIP_OFFSETS), self._all(STRIP_BYTE_COUNTS)
        if self.block_w < 1 or self.block_h < 1:
            raise GeoError("strip or tile size is zero")
        if len(self.offsets) != len(self.counts):
            raise GeoError("strip/tile offsets and byte counts don't match")
        self.nodata = None
        if isinstance(t.get(GDAL_NODATA), str):
            try:
                self.nodata = float(t[GDAL_NODATA].strip("\0 \n"))
            except ValueError:
                pass
        self.georef = self._read_georef()

    def _read_ifd(self, offset):
        d, e = self._data, self._e
        if offset + 2 > len(d):
            raise GeoError("TIFF directory is outside the file")
        count, = struct.unpack(e + "H", d[offset:offset + 2])
        tags = {}
        for i in range(count):
            at = offset + 2 + 12 * i
            if at + 12 > len(d):
                raise GeoError("TIFF directory is cut off")
            tag, ftype, n = struct.unpack(e + "HHI", d[at:at + 8])
            if ftype not in _FIELD_TYPES:
                continue  # a type we don't know: skip the tag rather than fail
            code, size = _FIELD_TYPES[ftype]
            total = size * n
            where = at + 8 if total <= 4 else struct.unpack(e + "I", d[at + 8:at + 12])[0]
            if where + total > len(d):
                raise GeoError(f"tag {tag}: value is outside the file")
            raw = d[where:where + total]
            if ftype == 2:
                tags[tag] = raw.decode("latin-1")
            elif ftype in (5, 10):
                v = struct.unpack(e + code[0] * (2 * n), raw)
                tags[tag] = [v[j] / v[j + 1] if v[j + 1] else 0.0 for j in range(0, 2 * n, 2)]
            else:
                tags[tag] = list(struct.unpack(e + code * n, raw))
        return tags

    def _all(self, tag, default=None):
        v = self.tags.get(tag)
        if v is None:
            if default is None:
                raise GeoError(f"missing required TIFF tag {tag}")
            return default
        v = v if isinstance(v, list) else [v]
        if not v or not all(type(x) is int for x in v):
            raise GeoError(f"TIFF tag {tag} should hold whole numbers")
        return v

    def _one(self, tag, default=None):
        return self._all(tag, None if default is None else [default])[0]

    # ---- georeferencing ----------------------------------------------------

    def _read_georef(self):
        t = self.tags

        def numbers(tag):
            v = t.get(tag)
            ok = isinstance(v, list) and v and all(isinstance(x, (int, float)) for x in v)
            if ok and not all(math.isfinite(x) for x in v):
                raise GeoError("the georeferencing contains an infinite or undefined number")
            return v if ok else None

        keys = {}
        kd = numbers(GEO_KEY_DIRECTORY)
        if kd and len(kd) >= 4 and all(type(x) is int for x in kd):
            for i in range(kd[3]):
                base = 4 + 4 * i
                if base + 4 <= len(kd) and kd[base + 1] == 0:  # value stored inline
                    keys[kd[base]] = kd[base + 3]
        epsg = None
        for key in (3072, 2048):  # projected CRS, else geographic CRS
            if keys.get(key) not in (None, 0, 32767):
                epsg = keys[key]
                break
        is_point = keys.get(1025) == 2

        scale, tie, matrix = numbers(MODEL_PIXEL_SCALE), numbers(MODEL_TIEPOINT), numbers(MODEL_TRANSFORMATION)
        if scale and tie and len(scale) >= 2 and len(tie) >= 6:
            if scale[0] == 0 or scale[1] == 0:
                raise GeoError("the georeferencing has a cell size of zero")
            i, j, _, x, y, _ = tie[:6]
            x0, y0 = x - i * scale[0], y + j * scale[1]
            if is_point:  # the tie point is a cell centre, not its corner
                x0, y0 = x0 - scale[0] / 2, y0 + scale[1] / 2
            return Georef(x0, y0, scale[0], scale[1], self.width, self.height, epsg, is_point)
        if matrix and len(matrix) >= 16:
            if matrix[1] != 0 or matrix[4] != 0:
                raise GeoError("rotated or sheared rasters are not supported")
            if matrix[0] == 0 or matrix[5] == 0:
                raise GeoError("the georeferencing has a cell size of zero")
            return Georef(matrix[3], matrix[7], matrix[0], -matrix[5], self.width, self.height, epsg, is_point)
        return None

    # ---- pixels ------------------------------------------------------------

    def _decode_block(self, index, w, h, spp):
        start, count = self.offsets[index], self.counts[index]
        if start + count > len(self._data):
            raise GeoError("pixel data is outside the file")
        raw = self._data[start:start + count]
        if self.compression == 5:
            raw = lzw_decode(raw)
        elif self.compression in (8, 32946):
            try:
                raw = zlib.decompress(raw)
            except zlib.error as e:
                raise GeoError(f"damaged Deflate data: {e}")
        need = w * h * spp * (self.bits // 8)
        if len(raw) < need:
            raise GeoError(f"block {index} decoded to {len(raw)} bytes, expected {need}")
        a = array.array(self.typecode)
        a.frombytes(raw[:need])
        if (self._e == "<") != (sys.byteorder == "little"):
            a.byteswap()
        if self.predictor == 2:
            rows = [a[r * w * spp:(r + 1) * w * spp] for r in range(h)]
            _undo_predictor(rows, w, spp, self.typecode)
            a = array.array(self.typecode)
            for row in rows:
                a.extend(row)
        return a

    def read_band(self, band=0):
        """All pixels of one band, row by row, as an array.array of width*height values."""
        if not 0 <= band < self.bands:
            raise GeoError(f"band {band + 1} doesn't exist (the file has {self.bands})")
        if band in self._cache:
            return self._cache[band]
        W, H = self.width, self.height
        bw, bh = self.block_w, self.block_h
        across = -(-W // bw)
        down = -(-H // bh)
        per_plane = across * down
        chunky = self.planar == 1
        spp = self.bands if chunky else 1
        plane = 0 if chunky else band
        out = array.array(self.typecode, bytes(W * H * (self.bits // 8)))
        for by in range(down):
            for bx in range(across):
                index = plane * per_plane + by * across + bx
                if index >= len(self.offsets):
                    raise GeoError("fewer strips/tiles than the image needs")
                # strips are cropped to the image; tiles are always stored full size
                if self.tiled:
                    w, h = bw, bh
                else:
                    w, h = bw, min(bh, H - by * bh)
                block = self._decode_block(index, w, h, spp)
                for r in range(h):
                    y = by * bh + r
                    if y >= H:
                        break
                    x0 = bx * bw
                    n = min(w, W - x0)
                    src = block[r * w * spp:(r + 1) * w * spp]
                    row = src[band::spp] if chunky else src
                    out[y * W + x0:y * W + x0 + n] = row[:n]
        self._cache[band] = out
        return out

    def value(self, col, row, band=0):
        return self.read_band(band)[row * self.width + col]

    def is_valid(self, v):
        if self.nodata is not None and v == self.nodata:
            return False
        return not (isinstance(v, float) and math.isnan(v))

    def sample(self, x, y, band=0):
        """Value at a map coordinate, or None when off the grid or no-data."""
        if not self.georef:
            raise GeoError("this TIFF has no georeferencing")
        cell = self.georef.cell_at(x, y)
        if cell is None:
            return None
        v = self.value(cell[0], cell[1], band)
        return v if self.is_valid(v) else None

    def stats(self, band=0):
        data = self.read_band(band)
        n = 0
        total = 0.0
        lo, hi = None, None
        for v in data:
            if not self.is_valid(v):
                continue
            n += 1
            total += v
            if lo is None or v < lo:
                lo = v
            if hi is None or v > hi:
                hi = v
        if n == 0:
            return {"count": 0, "min": None, "max": None, "mean": None, "std": None, "nodata": len(data)}
        mean = total / n
        var = sum((v - mean) ** 2 for v in data if self.is_valid(v)) / n
        return {"count": n, "min": lo, "max": hi, "mean": mean, "std": math.sqrt(var),
                "nodata": len(data) - n}

    def histogram(self, band=0, bins=10):
        s = self.stats(band)
        if s["count"] == 0:
            return []
        lo, hi = s["min"], s["max"]
        counts = [0] * bins
        span = (hi - lo) or 1
        for v in self.read_band(band):
            if self.is_valid(v):
                counts[min(bins - 1, int((v - lo) / span * bins))] += 1
        return [(lo + span * i / bins, lo + span * (i + 1) / bins, c) for i, c in enumerate(counts)]


def open_geotiff(path):
    with open(path, "rb") as f:
        return GeoTiff(f.read(), path)
