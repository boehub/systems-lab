"""Projection maths, checked against PROJ (pyproj) to the millimetre, plus the
properties any correct projection must have (round trips, known values, units).

pyproj is the independent check; without it those tests are skipped (CI installs
it and sets REQUIRE_REFERENCE=1 so a skip can't hide a missing check).
"""

import io
import math
import os
import random
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

try:
    import pyproj
    HAVE = True
except ImportError:
    HAVE = False

if not HAVE and os.environ.get("REQUIRE_REFERENCE"):
    raise ImportError("REQUIRE_REFERENCE is set but pyproj is not installed")

import gis  # noqa: E402
from geo import open_geotiff, open_shapefile  # noqa: E402
from geo import projection as P  # noqa: E402
from geo.shpwrite import write_shapefile  # noqa: E402
from geo.zonal import zonal_stats  # noqa: E402
from writers import square, write_shapefile as test_writer, write_tiff  # noqa: E402

EX = os.path.join(ROOT, "examples")

# (source EPSG, target EPSG, lon range, lat range): where each system is meant to be used
REGIONS = [
    (4326, 32618, (-80, -72), (0, 60)),
    (4326, 32755, (144, 150), (-60, -1)),
    (4326, 3857, (-180, 180), (-85, 85)),
    (4326, 3395, (-180, 180), (-80, 80)),
    (4269, 32147, (-80, -77), (35.5, 38.5)),
    (4269, 2284, (-80, -77), (35.5, 38.5)),
    (4269, 5070, (-125, -67), (25, 49)),
    (4326, 2154, (-5, 9), (42, 51)),
    (4269, 26910, (-126, -120), (30, 60)),
    (4326, 25832, (6, 12), (40, 70)),
]


@unittest.skipUnless(HAVE, "pyproj is not installed")
class AgainstProj(unittest.TestCase):
    def test_forward_and_inverse_match_proj(self):
        rnd = random.Random(7)
        for a, b, (x0, x1), (y0, y1) in REGIONS:
            fwd = pyproj.Transformer.from_crs(a, b, always_xy=True)
            back = pyproj.Transformer.from_crs(b, a, always_xy=True)
            dst = P.from_epsg(b)
            for _ in range(300):
                lon, lat = rnd.uniform(x0, x1), rnd.uniform(y0, y1)
                X, Y = fwd.transform(lon, lat)
                x, y = dst.from_lonlat(lon, lat)
                self.assertAlmostEqual(x, X, delta=1e-3, msg=f"EPSG:{b} x at {lon},{lat}")
                self.assertAlmostEqual(y, Y, delta=1e-3, msg=f"EPSG:{b} y at {lon},{lat}")
                lo, la = dst.to_lonlat(X, Y)
                lo2, la2 = back.transform(X, Y)
                # 1e-8 degrees is about a millimetre
                self.assertAlmostEqual(lo, lo2, delta=1e-8, msg=f"EPSG:{b} inverse lon")
                self.assertAlmostEqual(la, la2, delta=1e-8, msg=f"EPSG:{b} inverse lat")

    def test_every_utm_zone_both_hemispheres(self):
        rnd = random.Random(3)
        for zone in range(1, 61):
            for south in (False, True):
                code = (32700 if south else 32600) + zone
                lon0 = -183 + 6 * zone
                lat = rnd.uniform(-80, -1) if south else rnd.uniform(1, 84)
                lon = lon0 + rnd.uniform(-3, 3)
                lon = (lon + 180) % 360 - 180
                X, Y = pyproj.Transformer.from_crs(4326, code, always_xy=True).transform(lon, lat)
                x, y = P.from_epsg(code).from_lonlat(lon, lat)
                self.assertAlmostEqual(x, X, delta=1e-3, msg=f"zone {zone} south={south}")
                self.assertAlmostEqual(y, Y, delta=1e-3, msg=f"zone {zone} south={south}")

    def test_wkt_written_by_proj_is_understood(self):
        for code in (32618, 32147, 5070, 2154, 3857, 3395, 26915, 4326, 4269):
            crs = pyproj.CRS.from_epsg(code)
            ref = P.from_epsg(code)
            for flavour in ("WKT1_ESRI", "WKT1_GDAL"):
                got = P.from_wkt(crs.to_wkt(version=flavour))
                a, b = got.from_lonlat(-77.1, 38.6), ref.from_lonlat(-77.1, 38.6)
                self.assertAlmostEqual(a[0], b[0], delta=1e-3, msg=f"{code} {flavour}")
                self.assertAlmostEqual(a[1], b[1], delta=1e-3, msg=f"{code} {flavour}")

    def test_wkt_unit_and_parameters_of_state_plane_feet(self):
        crs = P.from_wkt(pyproj.CRS.from_epsg(2284).to_wkt(version="WKT1_ESRI"))
        self.assertAlmostEqual(crs.unit, 0.3048006096012192, places=12)
        X, Y = pyproj.Transformer.from_crs(4269, 2284, always_xy=True).transform(-77.5, 37.5)
        x, y = crs.from_lonlat(-77.5, 37.5)
        self.assertAlmostEqual(x, X, delta=1e-3)
        self.assertAlmostEqual(y, Y, delta=1e-3)


class KnownValues(unittest.TestCase):
    def test_equator_central_meridian_is_the_false_origin(self):
        for zone, lon0 in ((18, -75), (31, 3)):
            x, y = P.from_epsg(32600 + zone).from_lonlat(lon0, 0)
            self.assertAlmostEqual(x, 500000, places=6)
            self.assertAlmostEqual(y, 0, places=6)
        x, y = P.from_epsg(32718).from_lonlat(-75, 0)
        self.assertAlmostEqual(y, 10000000, places=6)

    def test_web_mercator_known_points(self):
        crs = P.from_epsg(3857)
        x, y = crs.from_lonlat(180, 0)
        self.assertAlmostEqual(x, 20037508.342789244, places=6)
        x, y = crs.from_lonlat(-77.0365, 38.8977)   # Washington, DC
        self.assertAlmostEqual(x, -8575663.9525, delta=1e-3)   # x is just lon * R * pi/180
        self.assertAlmostEqual(y, 4707028.5508, delta=1e-3)

    def test_utm_scale_factor_on_the_central_meridian(self):
        # at the equator one degree of latitude is a(1-e^2) * pi/180 = 110574.39 m,
        # and UTM shrinks distances on its central meridian by 0.9996
        _, y = P.from_epsg(32618).from_lonlat(-75, 1)
        self.assertAlmostEqual(y, 0.9996 * 110574.39, delta=0.5)

    def test_round_trip_everywhere_a_system_is_defined(self):
        rnd = random.Random(11)
        for a, b, (x0, x1), (y0, y1) in REGIONS:
            crs = P.from_epsg(b)
            for _ in range(500):
                lon, lat = rnd.uniform(x0, x1), rnd.uniform(y0, y1)
                lo, la = crs.to_lonlat(*crs.from_lonlat(lon, lat))
                self.assertAlmostEqual(lo, lon, delta=1e-9, msg=f"EPSG:{b}")
                self.assertAlmostEqual(la, lat, delta=1e-9, msg=f"EPSG:{b}")

    def test_albers_preserves_area(self):
        # equal-area: a 1 degree x 1 degree cell has the same true area as the projected one
        crs = P.from_epsg(5070)
        a, f = crs.a, crs.f
        e2 = crs.e2

        def true_area(lat0, lat1, dlon):
            def q(p):
                s, e = math.sin(math.radians(p)), math.sqrt(e2)
                return s / (1 - e2 * s * s) - math.log((1 - e * s) / (1 + e * s)) / (2 * e)
            return a * a * (1 - e2) * math.radians(dlon) * (q(lat1) - q(lat0)) / 2

        for lat in (26.0, 35.0, 44.0):
            lon = -100.0
            pts = [(lon, lat), (lon + 0.01, lat), (lon + 0.01, lat + 0.01), (lon, lat + 0.01)]
            xy = [crs.from_lonlat(*p) for p in pts]
            shoelace = abs(sum(xy[i][0] * xy[(i + 1) % 4][1] - xy[(i + 1) % 4][0] * xy[i][1] for i in range(4))) / 2
            ref = abs(true_area(lat, lat + 0.01, 0.01))
            self.assertAlmostEqual(shoelace / ref, 1.0, delta=2e-4)

    def test_conformal_projections_keep_angles(self):
        # a tiny square stays a square (equal sides, right angle) under conformal projections
        for code, lon, lat in ((32618, -76.0, 40.0), (3857, 10.0, 50.0), (2154, 2.0, 47.0), (32147, -78.0, 37.0)):
            crs = P.from_epsg(code)
            h = 1e-5
            p = crs.from_lonlat(lon, lat)
            e = crs.from_lonlat(lon + h, lat)
            n = crs.from_lonlat(lon, lat + h)
            ve, vn = (e[0] - p[0], e[1] - p[1]), (n[0] - p[0], n[1] - p[1])
            dot = (ve[0] * vn[0] + ve[1] * vn[1]) / (math.hypot(*ve) * math.hypot(*vn))
            self.assertAlmostEqual(dot, 0.0, delta=1e-6, msg=f"EPSG:{code}")

    def test_longitude_wraps(self):
        crs = P.from_epsg(32601)   # central meridian -177
        a = crs.from_lonlat(-178, 10)
        b = crs.from_lonlat(182, 10)
        self.assertAlmostEqual(a[0], b[0], places=6)
        self.assertAlmostEqual(a[1], b[1], places=6)
        self.assertAlmostEqual(crs.to_lonlat(500000, 0)[0], -177, places=9)

    def test_polar_and_bad_input(self):
        with self.assertRaises(P.ProjectionError):
            P.from_epsg(3857).from_lonlat(0, 90)
        with self.assertRaises(P.ProjectionError):
            P.from_epsg(32618).from_lonlat(0, 91)
        with self.assertRaises(P.ProjectionError):
            P.from_epsg(99999)
        with self.assertRaises(P.ProjectionError):
            P.parse_crs("utm:61N")
        with self.assertRaises(P.ProjectionError):
            P.parse_crs("no such thing")


class ParsingAndText(unittest.TestCase):
    def test_spec_strings(self):
        self.assertEqual(P.parse_crs(4326), P.from_epsg(4326))
        self.assertEqual(P.parse_crs("EPSG:32618"), P.from_epsg(32618))
        self.assertEqual(P.parse_crs("utm:18N"), P.from_epsg(32618))
        self.assertEqual(P.parse_crs("utm 33s"), P.from_epsg(32733))
        self.assertEqual(P.parse_crs("WGS84"), P.from_epsg(4326))
        self.assertEqual(P.parse_crs(os.path.join(EX, "regions.prj")), P.from_epsg(4326))

    def test_own_wkt_round_trips(self):
        for code in (4326, 4269, 32618, 32755, 3857, 3395, 5070, 2154, 32147, 2284):
            crs = P.from_epsg(code)
            again = P.from_wkt(crs.to_wkt())
            self.assertEqual(again, crs, f"EPSG:{code}")
            a, b = crs.from_lonlat(-77.3, 38.1) if not crs.is_geographic else (0, 0), again.from_lonlat(-77.3, 38.1) if not crs.is_geographic else (0, 0)
            self.assertEqual(a, b)

    def test_refusals_say_why(self):
        nad27 = ('GEOGCS["GCS_North_American_1927",DATUM["D_North_American_1927",'
                 'SPHEROID["Clarke_1866",6378206.4,294.9786982]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]]')
        with self.assertRaisesRegex(P.ProjectionError, "datum"):
            P.from_wkt(nad27)
        with self.assertRaisesRegex(P.ProjectionError, "WKT2"):
            P.from_wkt('PROJCRS["x"]')
        poly = ('PROJCS["x",GEOGCS["g",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137,298.257223563]],PRIMEM["Greenwich",0],'
                'UNIT["Degree",0.0174532925199433]],PROJECTION["Polyconic"],UNIT["Meter",1]]')
        with self.assertRaisesRegex(P.ProjectionError, "not supported"):
            P.from_wkt(poly)
        for bad in ("", "GEOGCS[", 'GEOGCS["x"', "nonsense", "PROJCS[]]"):
            with self.assertRaises(P.ProjectionError):
                P.from_wkt(bad)

    def test_wkt_fuzz_never_crashes(self):
        rnd = random.Random(5)
        base = P.from_epsg(32147).to_wkt()
        for _ in range(3000):
            s = list(base)
            for _ in range(rnd.randint(1, 6)):
                i = rnd.randrange(len(s))
                op = rnd.random()
                if op < 0.4:
                    del s[i]
                elif op < 0.8:
                    s[i] = rnd.choice('[]",.0123456789-eE ABC')
                else:
                    s.insert(i, rnd.choice('[],"'))
            try:
                P.from_wkt("".join(s))
            except P.ProjectionError:
                pass


class Features(unittest.TestCase):
    def test_densify_and_transform_bounds(self):
        pts = P.densify([(0, 0), (10, 0)], 3)
        self.assertEqual(len(pts), 5)
        self.assertEqual(pts[0], (0, 0))
        self.assertEqual(pts[-1], (10, 0))
        self.assertEqual(P.densify([(1, 1)], 3), [(1, 1)])
        tr = P.Transformer(4326, 32618)
        b = P.transform_bounds((-77, 38, -76, 39), tr)
        self.assertLess(b[0], b[2])
        self.assertLess(b[1], b[3])

    def test_identity_transformer_does_nothing(self):
        tr = P.Transformer(4326, "EPSG:4326")
        self.assertTrue(tr.identity)
        self.assertEqual(tr(1.5, 2.5), (1.5, 2.5))

    def test_edge_curvature_is_followed(self):
        # a long east-west edge at 60N is a straight line in lon/lat but a curve in Lambert-93
        crs = P.from_epsg(3857)
        tr = P.Transformer(4326, crs)
        coarse = tr.points([(-60.0, 60.0), (60.0, 60.0)])
        fine = tr.points(P.densify([(-60.0, 60.0), (60.0, 60.0)], 10))
        # Mercator keeps parallels straight, so the dense points stay on the same y
        self.assertTrue(all(abs(p[1] - coarse[0][1]) < 1e-6 for p in fine))
        lcc = P.Transformer(4326, P.from_epsg(2154))
        fine = lcc.points(P.densify([(-4.0, 50.0), (8.0, 50.0)], 1))
        mid = fine[len(fine) // 2]
        chord_mid = tuple((a + b) / 2 for a, b in zip(fine[0], fine[-1]))
        self.assertGreater(math.hypot(mid[0] - chord_mid[0], mid[1] - chord_mid[1]), 1000)

    def test_shapefile_writer_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("regions", "places", "roads"):
                s = open_shapefile(os.path.join(EX, name + ".shp"))
                out = write_shapefile(os.path.join(d, name), s.kind, s.features, s.fields, s.prj)
                t = open_shapefile(out)
                self.assertEqual(t.bbox, s.bbox)
                self.assertEqual(t.fields, s.fields)
                self.assertEqual(t.prj, s.prj)
                for a, b in zip(s.features, t.features):
                    self.assertEqual((a.kind, a.parts, a.attrs), (b.kind, b.parts, b.attrs))

    def test_writer_output_is_readable_by_pyshp(self):
        try:
            import shapefile as pyshp
        except ImportError:
            if os.environ.get("REQUIRE_REFERENCE"):
                raise
            self.skipTest("pyshp is not installed")
        with tempfile.TemporaryDirectory() as d:
            s = open_shapefile(os.path.join(EX, "regions.shp"))
            out = write_shapefile(os.path.join(d, "r"), s.kind, s.features, s.fields, s.prj)
            r = pyshp.Reader(out)
            self.assertEqual(len(r), len(s.features))
            for sh, f in zip(r.shapes(), s.features):
                self.assertEqual([tuple(p) for p in sh.points], [p for part in f.parts for p in part])
            self.assertEqual([rec.as_dict()["NAME"] for rec in r.records()], [f.attrs["NAME"] for f in s.features])


def run(*args, stdin=None):
    out, err = io.StringIO(), io.StringIO()
    old = sys.stdin
    if stdin is not None:
        sys.stdin = io.StringIO(stdin)
    try:
        with redirect_stdout(out), redirect_stderr(err):
            code = gis.main(list(args))
    finally:
        sys.stdin = old
    return out.getvalue(), err.getvalue(), code


class Zonal(unittest.TestCase):
    def test_zonal_gives_the_same_answer_in_another_coordinate_system(self):
        dem = os.path.join(EX, "dem.tif")
        regions = os.path.join(EX, "regions.shp")
        t, s = open_geotiff(dem), open_shapefile(regions)
        base, _ = zonal_stats(t, s)
        with tempfile.TemporaryDirectory() as d:
            for crs in ("utm:18N", "3857", "2154"):
                out = os.path.join(d, "proj.shp")
                o, e, code = run("reproject", regions, out, "--to", crs)
                self.assertEqual(code, 0, e)
                moved = open_shapefile(out)
                self.assertNotEqual(moved.bbox, s.bbox)
                rows, notes = zonal_stats(t, moved)
                self.assertTrue(any("reprojected" in n for n in notes), notes)
                self.assertEqual(len(rows), len(base))
                for a, b in zip(rows, base):
                    # the polygon edges are curved slightly differently after the round trip, so
                    # a boundary cell may flip: allow a couple of cells
                    self.assertLessEqual(abs(a["cells"] - b["cells"]), max(2, b["cells"] // 100), crs)
                    if b["mean"] is not None:
                        self.assertAlmostEqual(a["mean"], b["mean"], delta=max(1.0, abs(b["mean"]) * 0.02), msg=crs)

    def test_known_systems_that_match_are_left_alone(self):
        t, s = open_geotiff(os.path.join(EX, "dem.tif")), open_shapefile(os.path.join(EX, "regions.shp"))
        rows, notes = zonal_stats(t, s)
        self.assertEqual(notes, [])

    def test_unknown_system_falls_back_to_the_old_warnings(self):
        with tempfile.TemporaryDirectory() as d:
            t = open_geotiff(os.path.join(EX, "dem.tif"))
            b = t.georef.bounds()
            mid = ((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
            weird = ('PROJCS["x",GEOGCS["g",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137,298.257223563]],'
                     'PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199433]],PROJECTION["Polyconic"],UNIT["Meter",1]]')
            p = test_writer(os.path.join(d, "w"), 5, [[square(mid[0] - .1, mid[1] - .1, mid[0] + .1, mid[1] + .1)]],
                            [("N", "N", 4, 0)], [[1]], prj=weird)
            rows, notes = zonal_stats(t, open_shapefile(p))
            self.assertTrue(any("projected" in n for n in notes), notes)


class Cli(unittest.TestCase):
    def test_project_pairs_and_stdin(self):
        out, err, code = run("project", "--to", "utm:18N", "-76.285", "36.85")
        self.assertEqual(code, 0, err)
        x, y = map(float, out.split())
        self.assertAlmostEqual(x, 385438.8324, delta=1e-3)
        self.assertAlmostEqual(y, 4079003.1063, delta=1e-3)
        out, _, code = run("project", "--to", "3857", stdin="0 0\n# a comment\n\n180,0\n")
        self.assertEqual(out.split("\n")[:2], ["0 0", "20037508.3428 0"])
        out, _, _ = run("project", "--from", "32618", "--to", "4326", "385438.8324212996", "4079003.10627818")
        lon, lat = map(float, out.split())
        self.assertAlmostEqual(lon, -76.285, delta=1e-8)
        self.assertAlmostEqual(lat, 36.85, delta=1e-8)

    def test_project_errors(self):
        for args, stdin, text in (
            (("project", "--to", "3857", "1"), None, "pairs"),
            (("project", "--to", "3857", "a", "b"), None, "numbers"),
            (("project", "--to", "99999", "1", "2"), None, "EPSG:99999"),
            (("project", "--to", "3857"), "1 2\nxyz\n", "line 2"),
            (("project", "--to", "3857", "0", "95"), None, "latitude"),
        ):
            _, err, code = run(*args, stdin=stdin)
            self.assertEqual(code, 1)
            self.assertIn(text, err)

    def test_reproject_shapefile_and_info(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "r.shp")
            text, err, code = run("reproject", os.path.join(EX, "places.shp"), out, "--to", "5070")
            self.assertEqual(code, 0, err)
            self.assertIn("wrote", text)
            info, _, _ = run("info", out)
            self.assertIn("Albers", info)
            back = os.path.join(d, "b.shp")
            run("reproject", out, back, "--to", "4326")
            a, b = open_shapefile(os.path.join(EX, "places.shp")), open_shapefile(back)
            for fa, fb in zip(a.features, b.features):
                self.assertAlmostEqual(fa.parts[0][0][0], fb.parts[0][0][0], delta=1e-9)
                self.assertAlmostEqual(fa.parts[0][0][1], fb.parts[0][0][1], delta=1e-9)
                self.assertEqual(fa.attrs, fb.attrs)

    def test_reproject_needs_to_know_the_source(self):
        with tempfile.TemporaryDirectory() as d:
            s = open_shapefile(os.path.join(EX, "places.shp"))
            p = test_writer(os.path.join(d, "n"), s.kind, [f.parts for f in s.features])
            _, err, code = run("reproject", p, os.path.join(d, "o.shp"), "--to", "3857")
            self.assertEqual(code, 1)
            self.assertIn("--from", err)
            o, err, code = run("reproject", p, os.path.join(d, "o.shp"), "--to", "3857", "--from", "4326")
            self.assertEqual(code, 0, err)
            _, err, code = run("reproject", p, "--to", "3857", "--from", "4326")
            self.assertEqual(code, 1)

    def test_raster_footprint(self):
        out, err, code = run("reproject", os.path.join(EX, "dem.tif"), "--to", "utm:18N")
        self.assertEqual(code, 0, err)
        self.assertIn("bounding box", out)
        self.assertIn("not resampled", out)

    def test_sample_with_another_coordinate_system(self):
        dem = os.path.join(EX, "dem.tif")
        lon, lat = -76.3012, 36.9013
        direct, _, _ = run("sample", dem, str(lon), str(lat))
        x, y = P.Transformer(4326, "utm:18N")(lon, lat)
        via, err, code = run("sample", dem, repr(x), repr(y), "--crs", "utm:18N")
        self.assertEqual(code, 0, err)
        self.assertEqual(direct, via)


if __name__ == "__main__":
    unittest.main()
