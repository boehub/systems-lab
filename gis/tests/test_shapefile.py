import math
import os
import random
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from geo import GeoError, geometry, open_shapefile  # noqa: E402
from geo.shapefile import POINT, POLYGON, POLYLINE, MULTIPOINT, NULL  # noqa: E402
from writers import read_bytes, square, write_bytes, write_shapefile  # noqa: E402

FIELDS = [("NAME", "C", 12, 0), ("POP", "N", 8, 0), ("DENS", "N", 10, 3), ("OPEN", "L", 1, 0)]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = os.path.join(self.tmp.name, "t")

    def tearDown(self):
        self.tmp.cleanup()


class TestGeometryTypes(Base):
    def test_points(self):
        write_shapefile(self.base, 1, [[[(1.5, 2.5)]], [[(-3.0, 4.0)]]])
        s = open_shapefile(self.base + ".shp")
        self.assertEqual(s.kind, POINT)
        self.assertEqual([f.parts for f in s.features], [[[(1.5, 2.5)]], [[(-3.0, 4.0)]]])
        self.assertEqual(s.features[1].bbox, (-3.0, 4.0, -3.0, 4.0))
        self.assertEqual(s.bbox[:4], (-3.0, 2.5, 1.5, 4.0))

    def test_polyline_with_two_parts(self):
        parts = [[(0, 0), (3, 4)], [(10, 0), (10, 5), (13, 5)]]
        write_shapefile(self.base, 3, [parts])
        f = open_shapefile(self.base).features[0]
        self.assertEqual(f.kind, POLYLINE)
        self.assertEqual(f.parts, parts)
        self.assertAlmostEqual(f.length, 5 + 5 + 3)

    def test_multipoint(self):
        write_shapefile(self.base, 8, [[[(1, 1), (2, 2), (3, 1)]]])
        f = open_shapefile(self.base).features[0]
        self.assertEqual((f.kind, len(f.parts[0])), (MULTIPOINT, 3))

    def test_null_shape_keeps_its_row(self):
        write_shapefile(self.base, 1, [[[(1, 1)]], None, [[(2, 2)]]],
                        [("ID", "N", 3, 0)], [[1], [2], [3]])
        s = open_shapefile(self.base)
        self.assertEqual([f.kind for f in s.features], [POINT, NULL, POINT])
        self.assertEqual([f.attrs["ID"] for f in s.features], [1, 2, 3])

    def test_z_values_are_skipped_but_geometry_reads(self):
        write_shapefile(self.base, 3, [[[(0, 0), (6, 8)]]], z=True)
        s = open_shapefile(self.base)
        self.assertTrue(s.has_z_or_m)
        self.assertEqual(s.kind, POLYLINE)
        self.assertEqual(s.features[0].length, 10)


class TestPolygons(Base):
    def test_area_with_hole_and_multipart(self):
        outer = square(0, 0, 10, 10)
        hole = square(2, 2, 4, 4, hole=True)
        island = square(20, 0, 22, 3)
        write_shapefile(self.base, 5, [[outer, hole, island]])
        f = open_shapefile(self.base).features[0]
        self.assertEqual(f.kind, POLYGON)
        self.assertAlmostEqual(f.area, 100 - 4 + 6)

    def test_contains_respects_holes_and_parts(self):
        write_shapefile(self.base, 5, [[square(0, 0, 10, 10), square(2, 2, 4, 4, hole=True),
                                        square(20, 0, 22, 3)]])
        f = open_shapefile(self.base).features[0]
        self.assertTrue(f.contains(1, 1))
        self.assertFalse(f.contains(3, 3))       # in the hole
        self.assertTrue(f.contains(21, 1))       # in the second part
        self.assertFalse(f.contains(15, 1))      # between them
        self.assertFalse(f.contains(-1, 5))
        self.assertFalse(f.contains(5, 11))

    def test_neighbours_do_not_both_claim_a_shared_edge(self):
        left, right = square(0, 0, 5, 5), square(5, 0, 10, 5)
        write_shapefile(self.base, 5, [[left], [right]])
        a, b = open_shapefile(self.base).features
        for y in (0.5, 2.5, 4.9):
            self.assertEqual(a.contains(5.0, y) + b.contains(5.0, y), 1, y)

    def test_triangle_and_concave_shape(self):
        tri = [(0, 0), (0, 4), (4, 0), (0, 0)]  # clockwise
        arrow = [(0, 0), (0, 6), (3, 3), (6, 6), (6, 0), (3, 2), (0, 0)]
        write_shapefile(self.base, 5, [[tri], [arrow]])
        t, a = open_shapefile(self.base).features
        self.assertAlmostEqual(t.area, 8)
        self.assertTrue(t.contains(1, 1) and not t.contains(3, 3))
        self.assertTrue(a.contains(1, 3))
        self.assertTrue(a.contains(3, 2.5))      # the narrow waist in the middle
        self.assertFalse(a.contains(3, 1))       # the notch cut into the bottom
        self.assertFalse(a.contains(3, 4))       # the notch cut into the top

    def test_ring_orientation_signs(self):
        self.assertLess(geometry.ring_area_signed(square(0, 0, 2, 2)), 0)             # clockwise
        self.assertGreater(geometry.ring_area_signed(square(0, 0, 2, 2, hole=True)), 0)

    def test_orphan_hole_is_kept_as_a_ring(self):
        groups = geometry.split_polygons([square(0, 0, 2, 2, hole=True)])
        self.assertEqual(len(groups), 1)

    def test_contains_matches_a_brute_force_check_on_random_points(self):
        ring = [(0, 0), (0, 10), (10, 10), (10, 0), (0, 0)]
        rnd = random.Random(3)
        for _ in range(500):
            x, y = rnd.uniform(-5, 15), rnd.uniform(-5, 15)
            self.assertEqual(geometry.ring_contains(ring, x, y), 0 <= x <= 10 and 0 <= y <= 10)


class TestAttributes(Base):
    def test_types(self):
        rows = [["Alpha", 1200, 3.5, True], ["Beta", None, -0.25, False], ["Gamma", 7, 10.0, None]]
        write_shapefile(self.base, 1, [[[(i, i)]] for i in range(3)], FIELDS, rows)
        s = open_shapefile(self.base)
        self.assertEqual([f[0] for f in s.fields], ["NAME", "POP", "DENS", "OPEN"])
        self.assertEqual(s.features[0].attrs, {"NAME": "Alpha", "POP": 1200, "DENS": 3.5, "OPEN": True})
        self.assertEqual(s.features[1].attrs["POP"], None)
        self.assertEqual(s.features[1].attrs["DENS"], -0.25)
        self.assertEqual(s.features[1].attrs["OPEN"], False)
        self.assertEqual(s.features[2].attrs["OPEN"], None)
        self.assertIsInstance(s.features[0].attrs["POP"], int)
        self.assertIsInstance(s.features[0].attrs["DENS"], float)

    def test_text_encodings(self):
        write_shapefile(self.base, 1, [[[(0, 0)]]], [("N", "C", 20, 0)], [["Café Zoë"]])
        self.assertEqual(open_shapefile(self.base).features[0].attrs["N"], "Café Zoë")
        # the same text saved by older software as Windows-1252, with a .cpg saying so
        text = "Café Zoë"
        u8, w1252 = text.encode(), text.encode("cp1252")
        raw = read_bytes(self.base + ".dbf").replace(u8.ljust(20), w1252.ljust(20))
        write_bytes(self.base + ".dbf", raw)
        self.assertEqual(open_shapefile(self.base).features[0].attrs["N"], "Café Zoë")

    def test_deleted_rows_keep_alignment(self):
        write_shapefile(self.base, 1, [[[(0, 0)]], [[(1, 1)]]], [("ID", "N", 3, 0)], [[1], [2]])
        raw = bytearray(read_bytes(self.base + ".dbf"))
        header_len, = struct.unpack("<H", raw[8:10])
        raw[header_len] = 0x2A  # mark row 1 deleted
        write_bytes(self.base + ".dbf", bytes(raw))
        s = open_shapefile(self.base)
        self.assertEqual([f.attrs for f in s.features], [{}, {"ID": 2}])

    def test_prj_is_read(self):
        write_shapefile(self.base, 1, [[[(0, 0)]]], prj='GEOGCS["WGS 84"]')
        self.assertEqual(open_shapefile(self.base).prj, 'GEOGCS["WGS 84"]')

    def test_no_dbf_is_fine(self):
        write_shapefile(self.base, 1, [[[(0, 0)]]])
        self.assertEqual(open_shapefile(self.base).features[0].attrs, {})

    def test_row_count_mismatch(self):
        write_shapefile(self.base, 1, [[[(0, 0)]], [[(1, 1)]]], [("ID", "N", 3, 0)], [[1], [2]])
        write_shapefile(self.base + "2", 1, [[[(0, 0)]]], [("ID", "N", 3, 0)], [[1]])
        os.replace(self.base + "2.dbf", self.base + ".dbf")
        with self.assertRaises(GeoError):
            open_shapefile(self.base)


class TestDamaged(Base):
    def good(self):
        write_shapefile(self.base, 5, [[square(0, 0, 4, 4), square(1, 1, 2, 2, hole=True)], [square(6, 0, 8, 2)]],
                        FIELDS, [["a", 1, 1.5, True], ["b", 2, 2.5, False]])
        return read_bytes(self.base + ".shp")

    def test_wrong_file_code(self):
        data = bytearray(self.good())
        data[0:4] = struct.pack(">i", 1234)
        write_bytes(self.base + ".shp", data)
        with self.assertRaisesRegex(GeoError, "9994"):
            open_shapefile(self.base)

    def test_header_longer_than_file(self):
        data = self.good()
        write_bytes(self.base + ".shp", data[:-10])
        with self.assertRaises(GeoError):
            open_shapefile(self.base)

    def test_tiny_file(self):
        write_bytes(self.base + ".shp", b"x" * 20)
        with self.assertRaisesRegex(GeoError, "shorter"):
            open_shapefile(self.base)

    def test_unsupported_type(self):
        data = bytearray(self.good())
        data[32:36] = struct.pack("<i", 31)  # MultiPatch
        write_bytes(self.base + ".shp", data)
        with self.assertRaisesRegex(GeoError, "MultiPatch"):
            open_shapefile(self.base)

    def test_corrupt_bytes_never_crash(self):
        good = self.good()
        rnd = random.Random(5)
        for _ in range(3000):
            bad = bytearray(good)
            for _ in range(rnd.randint(1, 8)):
                bad[rnd.randrange(len(bad))] = rnd.randrange(256)
            write_bytes(self.base + ".shp", bytes(bad))
            try:
                s = open_shapefile(self.base)
                for f in s.features:
                    f.area, f.length, f.contains(1, 1)
            except GeoError:
                pass

    def test_corrupt_dbf_never_crashes(self):
        self.good()
        good = read_bytes(self.base + ".dbf")
        rnd = random.Random(6)
        for _ in range(2000):
            bad = bytearray(good)
            for _ in range(rnd.randint(1, 6)):
                bad[rnd.randrange(len(bad))] = rnd.randrange(256)
            write_bytes(self.base + ".dbf", bytes(bad))
            try:
                open_shapefile(self.base)
            except GeoError:
                pass


if __name__ == "__main__":
    unittest.main()
