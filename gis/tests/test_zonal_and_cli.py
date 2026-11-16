import csv
import io
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

import gis  # noqa: E402
from geo import GeoTiff, open_geotiff, open_shapefile  # noqa: E402
from geo.zonal import zonal_stats  # noqa: E402
from writers import square, write_shapefile, write_tiff  # noqa: E402

EX = os.path.join(ROOT, "examples")
DEM, REGIONS, PLACES, ROADS = (os.path.join(EX, n) for n in ("dem.tif", "regions.shp", "places.shp", "roads.shp"))


def run(*args):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = gis.main(list(args))
    return out.getvalue(), err.getvalue(), code


class TestZonalByHand(unittest.TestCase):
    """Tiny grids where the right answer can be worked out on paper.

    Every grid has 1 x 1 cells with its lower-left corner at (0, 0), so the cell
    in column c, row r (rows count down from the top) has its centre at
    (c + 0.5, H - r - 0.5).
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def grid(self, values, w, h, nodata=None):
        return GeoTiff(write_tiff(w, h, [values], fmt=3, bits=32,
                                  geo=dict(scale=(1, 1), tie=(0.0, float(h))), nodata=nodata))

    def polys(self, shapes, prj=None):
        base = os.path.join(self.tmp.name, "p")
        write_shapefile(base, 5, shapes, prj=prj)
        return open_shapefile(base)

    def test_values_picked_up_in_the_right_place(self):
        t = self.grid([float(i) for i in range(16)], 4, 4)       # 0..15, row by row from the top
        # x 1..3, y 1..3 holds centres x = 1.5, 2.5 and y = 1.5, 2.5.
        # y = 2.5 is row 1 (4 - 2.5 - 0.5 = 1) and y = 1.5 is row 2.
        rows, _ = zonal_stats(t, self.polys([[square(1, 1, 3, 3)]]))
        r = rows[0]
        self.assertEqual((r["cells"], r["valid"]), (4, 4))
        self.assertEqual((r["min"], r["max"]), (5.0, 10.0))
        self.assertAlmostEqual(r["mean"], (5 + 6 + 9 + 10) / 4)
        self.assertAlmostEqual(r["sum"], 30)

    def test_only_cell_centres_matter(self):
        t = self.grid([1.0] * 16, 4, 4)
        self.assertEqual(zonal_stats(t, self.polys([[square(0.6, 0.6, 2.4, 2.4)]]))[0][0]["cells"], 1)
        self.assertEqual(zonal_stats(t, self.polys([[square(0.4, 0.4, 2.6, 2.6)]]))[0][0]["cells"], 9)

    def test_holes_are_left_out(self):
        t = self.grid([1.0] * 100, 10, 10)
        s = self.polys([[square(0, 0, 10, 10), square(2, 2, 5, 5, hole=True)]])
        self.assertEqual(zonal_stats(t, s)[0][0]["cells"], 100 - 9)

    def test_multipart_polygon(self):
        t = self.grid([1.0] * 100, 10, 10)
        s = self.polys([[square(0, 0, 2, 2), square(5, 5, 8, 6)]])
        self.assertEqual(zonal_stats(t, s)[0][0]["cells"], 4 + 3)

    def test_nodata_is_counted_as_a_cell_but_not_a_value(self):
        t = self.grid([2.0, -1.0, 4.0, -1.0], 2, 2, nodata=-1)
        r = zonal_stats(t, self.polys([[square(0, 0, 2, 2)]]))[0][0]
        self.assertEqual((r["cells"], r["valid"]), (4, 2))
        self.assertAlmostEqual(r["mean"], 3.0)

    def test_polygon_partly_and_wholly_outside(self):
        t = self.grid([1.0] * 16, 4, 4)
        rows, _ = zonal_stats(t, self.polys([[square(2, 2, 9, 9)], [square(20, 20, 30, 30)]]))
        self.assertEqual(rows[0]["cells"], 4)             # only the part over the grid
        self.assertEqual((rows[1]["cells"], rows[1]["mean"]), (0, None))

    def test_touching_polygons_split_the_cells_between_them(self):
        t = self.grid([1.0] * 100, 10, 10)
        s = self.polys([[square(0, 0, 5, 10)], [square(5, 0, 10, 10)]])
        a, b = zonal_stats(t, s)[0]
        self.assertEqual((a["cells"], b["cells"]), (50, 50))

    def test_warnings(self):
        t = self.grid([1.0] * 16, 4, 4)
        _, w = zonal_stats(t, self.polys([[square(50, 50, 60, 60)]]))
        self.assertTrue(any("overlap" in x for x in w))
        _, w = zonal_stats(t, self.polys([[square(0, 0, 2, 2)]], prj='PROJCS["UTM"]'))
        self.assertTrue(any("projected" in x for x in w))
        _, w = zonal_stats(t, self.polys([[square(0, 0, 2, 2)]], prj='GEOGCS["WGS 84"]'))
        self.assertEqual(w, [])


class TestSampleData(unittest.TestCase):
    """The committed example files, checked against numbers worked out from how they were built."""

    def test_zonal_counts_match_the_geometry(self):
        rows, warnings = zonal_stats(open_geotiff(DEM), open_shapefile(REGIONS))
        self.assertEqual(warnings, [])
        # rectangles: width / 0.005 x height / 0.005 cells; Harborside loses its 10 x 12 hole
        self.assertEqual([r["cells"] for r in rows], [50 * 30, 50 * 40, 70 * 30, 30 * 30 - 10 * 12])
        # the 14 x 12 no-data lake overlaps Marshland (10 columns) and Harborside (4 columns)
        self.assertEqual([r["cells"] - r["valid"] for r in rows], [0, 0, 10 * 12, 4 * 12])

    def test_zonal_means_match_a_brute_force_scan(self):
        t, s = open_geotiff(DEM), open_shapefile(REGIONS)
        data = t.read_band(0)
        rows, _ = zonal_stats(t, s)
        for feat, row in zip(s.features, rows):
            vals = []
            for r in range(t.height):          # every cell, no bounding-box shortcuts
                for c in range(t.width):
                    x, y = t.georef.cell_center(c, r)
                    v = data[r * t.width + c]
                    if feat.contains(x, y) and t.is_valid(v):
                        vals.append(v)
            self.assertEqual(row["valid"], len(vals))
            self.assertAlmostEqual(row["mean"], sum(vals) / len(vals), places=9)
            self.assertEqual((row["min"], row["max"]), (min(vals), max(vals)))

    def test_dem_shape(self):
        t = open_geotiff(DEM)
        self.assertEqual((t.width, t.height, t.georef.epsg, t.nodata), (120, 90, 4326, -9999.0))
        self.assertEqual(t.tiled, True)
        self.assertEqual(t.stats()["nodata"], 14 * 12)
        self.assertIsNone(t.sample(-76.2, 36.76))     # inside the lake


class TestCli(unittest.TestCase):
    def test_info_raster(self):
        out, _, code = run("info", DEM)
        self.assertEqual(code, 0)
        for text in ("120 x 90 cells", "32-bit float", "Deflate", "tiles of 32 x 32", "EPSG          4326"):
            self.assertIn(text, out)

    def test_info_shapefile(self):
        out, _, _ = run("info", REGIONS)
        self.assertIn("4 Polygon feature(s)", out)
        self.assertIn("NAME (C16)", out)

    def test_table_and_csv(self):
        out, _, _ = run("table", REGIONS)
        self.assertIn("Harborside", out)
        out, _, _ = run("table", REGIONS, "--csv")
        rows = list(csv.reader(io.StringIO(out)))
        self.assertEqual(rows[0], ["record", "NAME", "KIND", "POP", "area"])
        self.assertEqual(rows[4][1:4], ["Harborside", "urban", "72900"])
        self.assertAlmostEqual(float(rows[4][4]), 0.0195)

    def test_table_limit_and_lines(self):
        out, _, _ = run("table", REGIONS, "--limit", "2", "--csv")
        self.assertEqual(len(out.strip().splitlines()), 3)
        out, _, _ = run("table", ROADS, "--csv")
        self.assertIn("length", out.splitlines()[0])

    def test_query_point_and_box(self):
        out, _, _ = run("query", REGIONS, "--at", "-76.20", "36.78")
        self.assertIn("Harborside", out)
        self.assertIn("1 feature(s)", out)
        out, _, _ = run("query", REGIONS, "--at", "-76.125", "36.77")      # in the lagoon hole
        self.assertIn("0 feature(s)", out)
        out, _, _ = run("query", REGIONS, "--bbox", "-76.32", "36.9", "-76.28", "36.95", "--csv")
        names = [r[1] for r in list(csv.reader(io.StringIO(out)))[1:]]
        self.assertEqual(sorted(names), ["Bayview", "Northshore"])

    def test_query_needs_a_filter(self):
        _, err, code = run("query", REGIONS)
        self.assertEqual(code, 1)
        self.assertIn("--at", err)

    def test_sample(self):
        out, _, code = run("sample", DEM, "-76.55", "36.99")
        self.assertEqual(code, 0)
        t = open_geotiff(DEM)
        self.assertAlmostEqual(float(out), t.sample(-76.55, 36.99), places=4)
        out, _, code = run("sample", DEM, "0", "0")
        self.assertEqual(code, 1)
        self.assertIn("outside", out)

    def test_stats(self):
        out, _, _ = run("stats", DEM)
        self.assertIn("count   10632", out)
        self.assertIn("nodata  168", out)
        self.assertIn("min     2.1", out)
        self.assertEqual(sum(int(line.split()[-2]) for line in out.splitlines() if "#" in line), 10632)

    def test_preview(self):
        out, _, _ = run("preview", DEM, "--width", "40")
        lines = out.splitlines()
        self.assertTrue(all(len(l) == 40 for l in lines[:10]))
        self.assertIn("?", out)                 # the no-data lake
        self.assertIn("@", out)                 # the highest cell

    def test_zonal_table_and_csv(self):
        out, _, _ = run("zonal", DEM, REGIONS, "--field", "NAME")
        self.assertIn("Northshore", out)
        out, _, _ = run("zonal", DEM, REGIONS, "--csv")
        rows = list(csv.reader(io.StringIO(out)))
        self.assertEqual(rows[0], ["record", "NAME", "KIND", "POP", "cells", "valid", "min", "max", "mean"])
        self.assertEqual([r[4] for r in rows[1:]], ["1500", "2000", "2100", "780"])

    def test_zonal_bad_field(self):
        _, err, code = run("zonal", DEM, REGIONS, "--field", "NOPE")
        self.assertEqual(code, 1)
        self.assertIn("no field 'NOPE'", err)

    def test_zonal_rejects_non_polygons(self):
        _, err, code = run("zonal", DEM, os.path.join(EX, "places.shp"))
        self.assertEqual(code, 1)
        self.assertIn("needs polygons", err)

    def test_errors_are_one_line_and_exit_1(self):
        for args in (("info", "missing.tif"), ("info", "notes.txt"), ("zonal", REGIONS, DEM)):
            out, err, code = run(*args)
            self.assertEqual(code, 1, args)
            self.assertTrue(err.startswith("gis:"), (args, err))
            self.assertNotIn("Traceback", err)

    def test_runs_as_a_script_from_another_directory(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run([sys.executable, os.path.join(ROOT, "gis.py"), "zonal", DEM, REGIONS, "--csv"],
                               capture_output=True, text=True, cwd=d)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("Harborside", r.stdout)


if __name__ == "__main__":
    unittest.main()
