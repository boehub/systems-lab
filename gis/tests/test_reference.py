"""Cross-check against independent libraries (tifffile, pyshp, numpy).

The other tests read files made by this repo's own writers, which could share a
misunderstanding with the readers. These tests use files made by, and values
computed by, other people's code. They are skipped when the libraries aren't
installed (`pip install numpy tifffile imagecodecs pyshp`); CI installs them and
sets REQUIRE_REFERENCE=1 so a skip can't hide a missing check.
"""

import datetime
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

try:
    import numpy as np
    import shapefile as pyshp
    import tifffile
    HAVE = True
except ImportError:
    HAVE = False

if not HAVE and os.environ.get("REQUIRE_REFERENCE"):
    raise ImportError("REQUIRE_REFERENCE is set but numpy/tifffile/pyshp are not installed")

from geo import GeoTiff, open_geotiff, open_shapefile  # noqa: E402
from geo.shapefile import POLYGON  # noqa: E402
from writers import square, write_shapefile, write_tiff  # noqa: E402

GEOKEYS = (1, 1, 0, 3, 1024, 0, 1, 2, 2048, 0, 1, 4326, 1025, 0, 1, 1)


def geo_tags(sx, sy, x, y):
    return [(33550, "d", 3, (sx, sy, 0.0), True), (33922, "d", 6, (0.0, 0.0, 0.0, x, y, 0.0), True),
            (34735, "H", len(GEOKEYS), GEOKEYS, True)]


@unittest.skipUnless(HAVE, "reference libraries not installed")
class TestAgainstTifffile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rnd = np.random.default_rng(42)

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name="t.tif"):
        return os.path.join(self.tmp.name, name)

    def test_files_written_by_tifffile(self):
        shapes = [(37, 53), (64, 64), (5, 200)]
        dtypes = ["uint8", "int16", "uint16", "int32", "uint32", "float32", "float64", "int8", "int64"]
        for dtype in dtypes:
            for comp in (None, "zlib", "lzw"):
                can_predict = comp is not None and np.dtype(dtype).kind in "iu" and np.dtype(dtype).itemsize < 8
                for pred in ((False, True) if can_predict else (False,)):
                    for layout in ({}, {"tile": (16, 16)}, {"rowsperstrip": 7}):
                        for byteorder in ("<", ">"):
                            h, w = shapes[hash((dtype, comp)) % 3]
                            if np.dtype(dtype).kind == "f":
                                arr = (self.rnd.standard_normal((h, w)) * 1000).astype(dtype)
                            else:
                                info = np.iinfo(dtype)
                                arr = self.rnd.integers(info.min, info.max, (h, w), dtype=dtype, endpoint=True)
                            kw = dict(layout)
                            with self.subTest(dtype=dtype, comp=comp, pred=pred, layout=layout, order=byteorder):
                                tifffile.imwrite(self.path(), arr, compression=comp,
                                                 predictor=pred or None, byteorder=byteorder, **kw)
                                ours = open_geotiff(self.path())
                                self.assertEqual((ours.width, ours.height), (w, h))
                                got = np.array(ours.read_band(0)).reshape(h, w)
                                np.testing.assert_array_equal(got, arr)

    def test_multiband_planar_and_chunky(self):
        arr = self.rnd.integers(0, 255, (3, 40, 50), dtype="uint8")
        for planar in ("contig", "separate"):
            with self.subTest(planar=planar):
                data = np.moveaxis(arr, 0, -1) if planar == "contig" else arr
                tifffile.imwrite(self.path(), data, planarconfig=planar, photometric="rgb",
                                 compression="zlib", rowsperstrip=9)
                ours = open_geotiff(self.path())
                self.assertEqual(ours.bands, 3)
                for b in range(3):
                    np.testing.assert_array_equal(np.array(ours.read_band(b)).reshape(40, 50), arr[b])

    def test_georeferencing_written_by_tifffile(self):
        arr = self.rnd.random((20, 30)).astype("float32")
        tifffile.imwrite(self.path(), arr, extratags=geo_tags(0.01, 0.02, -75.5, 38.25) + [(42113, "s", 0, "-1", True)])
        t = open_geotiff(self.path())
        self.assertEqual(t.georef.epsg, 4326)
        self.assertEqual(t.nodata, -1.0)
        self.assertAlmostEqual(t.georef.x0, -75.5)
        self.assertAlmostEqual(t.georef.y0, 38.25)
        x, y = t.georef.cell_center(4, 9)
        self.assertAlmostEqual(x, -75.5 + 4.5 * 0.01)
        self.assertAlmostEqual(y, 38.25 - 9.5 * 0.02)
        self.assertAlmostEqual(t.sample(x, y), float(arr[9, 4]), places=6)

    def test_our_files_read_by_tifffile(self):
        vals = [(x * 7 + y * 3) % 250 for y in range(10) for x in range(13)]
        for kw in (dict(), dict(compression=8), dict(compression=5), dict(compression=8, predictor=2),
                   dict(tile=(4, 4), compression=5), dict(big_endian=True, rows_per_strip=3)):
            with self.subTest(**kw):
                with open(self.path(), "wb") as f:
                    f.write(write_tiff(13, 10, [vals], **kw))
                np.testing.assert_array_equal(tifffile.imread(self.path()).ravel(), vals)

    def test_stats_match_numpy(self):
        arr = self.rnd.standard_normal((50, 60)).astype("float64") * 10 + 3
        arr[5:9, 5:9] = -9999
        tifffile.imwrite(self.path(), arr, extratags=[(42113, "s", 0, "-9999", True)])
        s = open_geotiff(self.path()).stats()
        valid = arr[arr != -9999]
        self.assertEqual(s["count"], valid.size)
        self.assertAlmostEqual(s["mean"], valid.mean(), places=9)
        self.assertAlmostEqual(s["std"], valid.std(), places=9)
        self.assertEqual((s["min"], s["max"]), (valid.min(), valid.max()))


@unittest.skipUnless(HAVE, "reference libraries not installed")
class TestAgainstPyshp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = os.path.join(self.tmp.name, "t")

    def tearDown(self):
        self.tmp.cleanup()

    def test_polygons_written_by_pyshp(self):
        w = pyshp.Writer(self.base, shapeType=pyshp.POLYGON)
        w.field("NAME", "C", 20)
        w.field("N", "N", 10, 0)
        w.field("X", "N", 12, 4)
        w.field("FLAG", "L")
        w.field("WHEN", "D")
        outer = [(0, 0), (0, 10), (10, 10), (10, 0), (0, 0)]               # clockwise
        hole = [(2, 2), (4, 2), (4, 4), (2, 4), (2, 2)]                    # counter-clockwise
        w.poly([outer, hole])
        w.record("first one", 42, 3.1416, True, datetime.date(2024, 3, 9))
        w.poly([[(20, 0), (20, 4), (25, 0), (20, 0)]])
        w.record("second", -7, -0.5, False, datetime.date(1999, 12, 31))
        w.null()
        w.record(None, None, None, None, None)
        w.close()
        s = open_shapefile(self.base)
        self.assertEqual(len(s.features), 3)
        a, b, c = s.features
        self.assertEqual(a.attrs["NAME"], "first one")
        self.assertEqual(a.attrs["N"], 42)
        self.assertAlmostEqual(a.attrs["X"], 3.1416)
        self.assertIs(a.attrs["FLAG"], True)
        self.assertEqual(a.attrs["WHEN"], "2024-03-09")
        self.assertEqual(b.attrs["N"], -7)
        self.assertAlmostEqual(a.area, 100 - 4)
        self.assertAlmostEqual(b.area, 10)  # base 5, height 4
        self.assertTrue(a.contains(1, 1) and not a.contains(3, 3))
        self.assertEqual(c.kind, 0)

    def test_every_geometry_type_matches_pyshp(self):
        cases = [
            (pyshp.POINT, lambda w: w.point(1.5, -2.5)),
            (pyshp.POLYLINE, lambda w: w.line([[(0, 0), (1, 1), (2, 0)], [(5, 5), (6, 6)]])),
            (pyshp.MULTIPOINT, lambda w: w.multipoint([(1, 1), (2, 2), (3, 3)])),
            (pyshp.POLYGON, lambda w: w.poly([[(0, 0), (0, 3), (3, 3), (3, 0), (0, 0)]])),
            (pyshp.POINTZ, lambda w: w.pointz(1.5, -2.5, 7)),
            (pyshp.POLYLINEZ, lambda w: w.linez([[(0, 0, 1), (1, 1, 2), (2, 0, 3)]])),
            (pyshp.POLYGONZ, lambda w: w.polyz([[(0, 0, 1), (0, 3, 1), (3, 3, 1), (3, 0, 1), (0, 0, 1)]])),
            (pyshp.POINTM, lambda w: w.pointm(1.5, -2.5, 7)),
            (pyshp.POLYLINEM, lambda w: w.linem([[(0, 0, 1), (1, 1, 2), (2, 0, 3)]])),
        ]
        for kind, add in cases:
            with self.subTest(kind=kind):
                w = pyshp.Writer(self.base, shapeType=kind)
                w.field("ID", "N")
                add(w)
                w.record(1)
                w.close()
                ref = pyshp.Reader(self.base).shape(0)
                ours = open_shapefile(self.base).features[0]
                theirs = [[tuple(p) for p in part] for part in _parts(ref)]
                self.assertEqual(ours.parts, theirs)
                if hasattr(ref, "bbox"):  # point shapes have no box of their own
                    self.assertEqual(tuple(ours.bbox), tuple(ref.bbox))

    def test_pyshp_reads_our_files(self):
        write_shapefile(self.base, 5, [[square(0, 0, 10, 10), square(2, 2, 4, 4, hole=True)], [square(20, 0, 22, 3)]],
                        [("NAME", "C", 8, 0), ("POP", "N", 6, 0)], [["a", 5], ["b", 9]])
        r = pyshp.Reader(self.base)
        self.assertEqual(len(r), 2)
        self.assertEqual(r.record(1)[0:2], ["b", 9])
        self.assertEqual(len(r.shape(0).parts), 2)
        self.assertEqual(tuple(r.shape(1).bbox), (20.0, 0.0, 22.0, 3.0))

    def test_area_matches_pyshp_for_many_random_polygons(self):
        import random
        rnd = random.Random(8)
        w = pyshp.Writer(self.base, shapeType=pyshp.POLYGON)
        w.field("ID", "N")
        expected = []
        for i in range(60):
            x0, y0 = rnd.uniform(-50, 50), rnd.uniform(-50, 50)
            ww, hh = rnd.uniform(1, 20), rnd.uniform(1, 20)
            ring = [(x0, y0), (x0, y0 + hh), (x0 + ww, y0 + hh), (x0 + ww, y0), (x0, y0)]
            w.poly([ring])
            w.record(i)
            expected.append(ww * hh)
        w.close()
        for f, want in zip(open_shapefile(self.base).features, expected):
            self.assertAlmostEqual(f.area, want, places=9)


def _parts(shape):
    pts = list(shape.points)
    idx = list(shape.parts) + [len(pts)] if len(shape.parts) else [0, len(pts)]
    return [pts[a:b] for a, b in zip(idx, idx[1:])]


if __name__ == "__main__":
    unittest.main()
