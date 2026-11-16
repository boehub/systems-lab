import math
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from geo import GeoError, GeoTiff  # noqa: E402
from geo.geotiff import lzw_decode  # noqa: E402
from writers import lzw_encode, write_tiff  # noqa: E402

W, H = 13, 10  # deliberately not a multiple of the strip/tile sizes used below
VALUES = [(x * 7 + y * 3) % 250 for y in range(H) for x in range(W)]


def read(data, band=0):
    return list(GeoTiff(data).read_band(band))


class TestLayouts(unittest.TestCase):
    def test_every_layout_reads_back_the_same_pixels(self):
        for big in (False, True):
            for comp in (1, 5, 8, 32946):
                for pred in (1, 2):
                    for layout in (dict(), dict(rows_per_strip=3), dict(tile=(4, 4)), dict(tile=(16, 16))):
                        with self.subTest(big=big, comp=comp, pred=pred, layout=layout):
                            data = write_tiff(W, H, [VALUES], big_endian=big, compression=comp,
                                              predictor=pred, **layout)
                            self.assertEqual(read(data), VALUES)

    def test_pixel_types(self):
        cases = [
            (1, 8, [0, 1, 255, 128]), (2, 8, [-128, -1, 0, 127]),
            (1, 16, [0, 1, 65535, 300]), (2, 16, [-32768, -1, 0, 32767]),
            (1, 32, [0, 4294967295, 7, 70000]), (2, 32, [-2**31, -1, 2**31 - 1, 5]),
            (3, 32, [0.0, -1.5, 3.25, 1e10]), (3, 64, [0.0, -1.5, 3.141592653589793, 1e300]),
            (1, 64, [0, 2**64 - 1, 7, 1 << 40]), (2, 64, [-2**63, -1, 2**63 - 1, 9]),
        ]
        for fmt, bits, vals in cases:
            for big in (False, True):
                for pred in ((1, 2) if fmt != 3 else (1,)):
                    with self.subTest(fmt=fmt, bits=bits, big=big, pred=pred):
                        data = write_tiff(2, 2, [vals], fmt=fmt, bits=bits, big_endian=big,
                                          compression=8, predictor=pred)
                        self.assertEqual(read(data), vals)

    def test_several_bands_interleaved_and_separate(self):
        b0 = VALUES
        b1 = [(v * 3) % 251 for v in VALUES]
        b2 = [255 - v for v in VALUES]
        for planar in (1, 2):
            for layout in (dict(rows_per_strip=4), dict(tile=(8, 8))):
                with self.subTest(planar=planar, layout=layout):
                    data = write_tiff(W, H, [b0, b1, b2], planar=planar, compression=8, **layout)
                    t = GeoTiff(data)
                    self.assertEqual(t.bands, 3)
                    self.assertEqual([list(t.read_band(i)) for i in range(3)], [b0, b1, b2])

    def test_bad_band(self):
        t = GeoTiff(write_tiff(W, H, [VALUES]))
        with self.assertRaises(GeoError):
            t.read_band(1)

    def test_predictor_wraps_at_the_sample_size(self):
        vals = [250, 3, 5, 255, 0, 1, 128, 127]  # differences cross the 8-bit boundary
        data = write_tiff(8, 1, [vals], compression=8, predictor=2)
        self.assertEqual(read(data), vals)


class TestLzw(unittest.TestCase):
    def test_roundtrip(self):
        samples = [b"", b"a", b"abababababab", bytes(range(256)) * 5, b"\0" * 10000,
                   bytes((i * i) % 251 for i in range(20000))]
        for s in samples:
            with self.subTest(len=len(s)):
                self.assertEqual(lzw_decode(lzw_encode(s)), s)

    def test_known_vector_from_the_tiff_spec(self):
        # The TIFF 6.0 spec's own example: the bytes 7,7,7,8,8,7,7,6,6 compress to these codes.
        bits = "".join(f"{c:09b}" for c in (256, 7, 258, 8, 8, 258, 6, 6, 257))
        bits += "0" * (-len(bits) % 8)
        data = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
        self.assertEqual(lzw_decode(data), bytes([7, 7, 7, 8, 8, 7, 7, 6, 6]))

    def test_damaged(self):
        with self.assertRaises(GeoError):
            lzw_decode(bytes([0xFF] * 16))


class TestGeoreferencing(unittest.TestCase):
    def make(self, **geo):
        g = dict(scale=(0.5, 0.25), tie=(-10.0, 40.0), epsg=4326)
        g.update(geo)
        return GeoTiff(write_tiff(W, H, [VALUES], geo=g, nodata=-9999))

    def test_extent_and_crs(self):
        t = self.make()
        self.assertEqual(t.georef.epsg, 4326)
        self.assertEqual(t.georef.bounds(), (-10.0, 40.0 - 0.25 * H, -10.0 + 0.5 * W, 40.0))

    def test_projected_epsg(self):
        self.assertEqual(self.make(epsg=32618).georef.epsg, 32618)

    def test_cell_lookup(self):
        t = self.make()
        g = t.georef
        self.assertEqual(g.cell_at(-9.9, 39.9), (0, 0))      # just inside the top-left cell
        self.assertEqual(g.cell_at(-10.0 + 0.5 * W - 0.01, 40.0 - 0.25 * H + 0.01), (W - 1, H - 1))
        self.assertIsNone(g.cell_at(-10.1, 39.9))            # west of the grid
        self.assertIsNone(g.cell_at(-9.9, 40.1))             # north of the grid
        self.assertIsNone(g.cell_at(-10.0 + 0.5 * W, 39.9))  # the east edge is outside
        self.assertEqual(g.cell_center(0, 0), (-9.75, 39.875))

    def test_sample_returns_the_cell_value(self):
        t = self.make()
        x, y = t.georef.cell_center(5, 7)
        self.assertEqual(t.sample(x, y), VALUES[7 * W + 5])
        self.assertIsNone(t.sample(100, 100))

    def test_point_raster_type_shifts_by_half_a_cell(self):
        t = self.make(point=True)
        self.assertEqual(t.georef.x0, -10.0 - 0.25)
        self.assertEqual(t.georef.y0, 40.0 + 0.125)

    def test_not_georeferenced(self):
        t = GeoTiff(write_tiff(W, H, [VALUES]))
        self.assertIsNone(t.georef)
        with self.assertRaises(GeoError):
            t.sample(0, 0)


class TestStats(unittest.TestCase):
    def test_nodata_and_nan_are_ignored(self):
        vals = [1.0, 2.0, 3.0, -9999.0, float("nan"), 6.0]
        t = GeoTiff(write_tiff(3, 2, [vals], fmt=3, bits=32, nodata=-9999))
        s = t.stats()
        self.assertEqual((s["count"], s["nodata"], s["min"], s["max"]), (4, 2, 1.0, 6.0))
        self.assertAlmostEqual(s["mean"], 3.0)
        self.assertAlmostEqual(s["std"], math.sqrt(((2) ** 2 + 1 + 0 + 9) / 4))
        self.assertEqual(sum(c for _, _, c in t.histogram(bins=3)), 4)

    def test_all_nodata(self):
        t = GeoTiff(write_tiff(2, 1, [[-1, -1]], fmt=2, nodata=-1))
        self.assertEqual(t.stats()["count"], 0)
        self.assertEqual(t.histogram(), [])


class TestDamagedFiles(unittest.TestCase):
    def assertRejected(self, data, text=None):
        with self.assertRaises(GeoError) as cm:
            t = GeoTiff(data)
            t.read_band(0)
        if text:
            self.assertIn(text, str(cm.exception))

    def test_not_a_tiff(self):
        self.assertRejected(b"", "too short")
        self.assertRejected(b"XX*\0\0\0\0\0", "byte-order")
        self.assertRejected(b"II\x07\0\0\0\0\0", "magic")
        self.assertRejected(b"II\x2b\0\x08\0\0\0\x08\0\0\0\0\0\0\0", "BigTIFF")

    def test_truncated_everywhere(self):
        good = write_tiff(W, H, [VALUES], compression=8, rows_per_strip=3, geo=dict(scale=(1, 1), tie=(0, 0)))
        for n in range(0, len(good) - 1, 7):  # every cut must raise GeoError, never crash
            try:
                GeoTiff(good[:n]).read_band(0)
            except GeoError:
                pass

    def test_corrupt_bytes_never_crash(self):
        import random
        rnd = random.Random(1)
        good = write_tiff(W, H, [VALUES], compression=5, rows_per_strip=3, geo=dict(scale=(1, 1), tie=(0, 0)))
        for _ in range(300):
            bad = bytearray(good)
            for _ in range(rnd.randint(1, 6)):
                bad[rnd.randrange(len(bad))] = rnd.randrange(256)
            try:
                t = GeoTiff(bytes(bad))
                if t.width * t.height < 100000:
                    t.read_band(0)
            except GeoError:
                pass

    def test_unsupported_compression(self):
        data = bytearray(write_tiff(W, H, [VALUES]))
        t = GeoTiff(bytes(data))
        # patch the compression tag value (259) to 7 (JPEG)
        ifd, = struct.unpack("<I", data[4:8])
        n, = struct.unpack("<H", data[ifd:ifd + 2])
        for i in range(n):
            at = ifd + 2 + 12 * i
            if struct.unpack("<H", data[at:at + 2])[0] == 259:
                data[at + 8:at + 10] = struct.pack("<H", 7)
        self.assertRejected(bytes(data), "compression")
        self.assertEqual(t.compression, 1)


if __name__ == "__main__":
    unittest.main()
