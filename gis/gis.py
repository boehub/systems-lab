#!/usr/bin/env python3
"""gis: look inside GeoTIFF rasters and shapefiles, with nothing but the standard library.

    gis.py info    FILE                       what is in a .tif or .shp
    gis.py table   FILE.shp [--limit N]       the attribute table, with area / length
    gis.py query   FILE.shp --at X Y          features containing a point
    gis.py query   FILE.shp --bbox X0 Y0 X1 Y1  features touching a box
    gis.py sample  FILE.tif X Y               the raster value at a coordinate
    gis.py stats   FILE.tif [--band N]        min / max / mean, and a histogram
    gis.py preview FILE.tif [--width N]       a text picture of the raster
    gis.py zonal   FILE.tif FILE.shp          mean elevation (etc.) inside each polygon

Add --csv to table, query and zonal for output a spreadsheet can open.
"""

import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo import GeoError, open_geotiff, open_shapefile  # noqa: E402
from geo import geometry  # noqa: E402
from geo.shapefile import TYPE_NAMES, POLYGON, POLYLINE  # noqa: E402
from geo.zonal import zonal_stats  # noqa: E402

RAMP = " .:-=+*#%@"


def fmt(v, digits=4):
    if v is None:
        return ""
    if isinstance(v, float):
        return f"{v:.{digits}g}" if abs(v) < 1e6 else f"{v:.{digits}e}"
    return str(v)


def print_table(header, rows, out=None, as_csv=False):
    out = out or sys.stdout
    if as_csv:
        w = csv.writer(out, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)
        return
    cells = [[fmt(c) for c in r] for r in rows]
    widths = [max([len(h)] + [len(r[i]) for r in cells]) for i, h in enumerate(header)]
    print("  ".join(h.ljust(w) for h, w in zip(header, widths)).rstrip(), file=out)
    print("  ".join("-" * w for w in widths), file=out)
    for r in cells:
        print("  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip(), file=out)


def kind_of(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".tif", ".tiff"):
        return "tif"
    if ext == ".shp":
        return "shp"
    raise GeoError(f"{path}: expected a .tif or .shp file")


# ---- commands ---------------------------------------------------------------------

def cmd_info(a):
    if kind_of(a.file) == "tif":
        t = open_geotiff(a.file)
        print(f"{a.file}: GeoTIFF, {t.width} x {t.height} cells, {t.bands} band(s)")
        print(f"  pixel type    {t.bits}-bit {'float' if t.sample_format == 3 else 'integer'}"
              f" ({'signed' if t.sample_format == 2 else 'unsigned' if t.sample_format == 1 else 'IEEE'})")
        print(f"  compression   {({1: 'none', 5: 'LZW', 8: 'Deflate', 32946: 'Deflate'})[t.compression]}"
              f"{', predictor' if t.predictor == 2 else ''}")
        print(f"  layout        {'tiles' if t.tiled else 'strips'} of {t.block_w} x {t.block_h}")
        print(f"  no-data       {fmt(t.nodata) or 'none'}")
        g = t.georef
        if g:
            b = g.bounds()
            print(f"  cell size     {fmt(g.dx, 8)} x {fmt(g.dy, 8)}")
            print(f"  extent        x {fmt(b[0], 8)} .. {fmt(b[2], 8)}   y {fmt(b[1], 8)} .. {fmt(b[3], 8)}")
            print(f"  EPSG          {g.epsg or 'not given'}")
        else:
            print("  not georeferenced")
    else:
        s = open_shapefile(a.file)
        print(f"{a.file}: shapefile, {len(s.features)} {TYPE_NAMES[s.kind]} feature(s)")
        b = s.bbox
        print(f"  extent        x {fmt(b[0], 8)} .. {fmt(b[2], 8)}   y {fmt(b[1], 8)} .. {fmt(b[3], 8)}")
        if s.has_z_or_m:
            print("  note          Z/M values are present in the file and ignored")
        print(f"  coordinates   {s.prj[:60] + '...' if len(s.prj) > 60 else s.prj or 'no .prj file'}")
        if s.fields:
            print("  fields        " + ", ".join(f"{n} ({t}{l})" for n, t, l, _ in s.fields))


def attr_columns(s):
    return [n for n, _, _, _ in s.fields]


def feature_rows(s, feats):
    cols = attr_columns(s)
    extra = []
    if s.kind == POLYGON:
        extra = ["area"]
    elif s.kind == POLYLINE:
        extra = ["length"]
    rows = []
    for f in feats:
        row = [f.record] + [f.attrs.get(c) for c in cols]
        if s.kind == POLYGON:
            row.append(f.area)
        elif s.kind == POLYLINE:
            row.append(f.length)
        rows.append(row)
    return ["record"] + cols + extra, rows


def cmd_table(a):
    s = open_shapefile(a.file)
    feats = s.features[:a.limit] if a.limit else s.features
    header, rows = feature_rows(s, feats)
    print_table(header, rows, as_csv=a.csv)


def cmd_query(a):
    s = open_shapefile(a.file)
    if a.at:
        x, y = a.at
        feats = [f for f in s.features if f.contains(x, y)]
    elif a.bbox:
        box = tuple(a.bbox)
        feats = [f for f in s.features if f.bbox and geometry.boxes_overlap(f.bbox, box)]
    else:
        raise GeoError("give --at X Y or --bbox X0 Y0 X1 Y1")
    header, rows = feature_rows(s, feats)
    print_table(header, rows, as_csv=a.csv)
    if not a.csv:
        print(f"\n{len(feats)} feature(s)")


def cmd_sample(a):
    t = open_geotiff(a.file)
    v = t.sample(a.x, a.y, a.band - 1)
    if v is None:
        print("no value (outside the raster, or no-data)")
        return 1
    print(fmt(v, 8))


def cmd_stats(a):
    t = open_geotiff(a.file)
    s = t.stats(a.band - 1)
    for k in ("count", "nodata", "min", "max", "mean", "std"):
        print(f"{k:<7} {fmt(s[k], 7)}")
    hist = t.histogram(a.band - 1, a.bins)
    if hist:
        peak = max(c for _, _, c in hist)
        print()
        for lo, hi, c in hist:
            print(f"{fmt(lo, 5):>10} .. {fmt(hi, 5):<10} {c:>8}  {'#' * round(40 * c / peak)}")


def cmd_preview(a):
    t = open_geotiff(a.file)
    band = a.band - 1
    data = t.read_band(band)
    W, H = t.width, t.height
    cols = min(a.width, W)
    step = W / cols
    rows = max(1, round(H / step / 2))  # characters are about twice as tall as wide
    s = t.stats(band)
    if s["count"] == 0:
        print("(no valid cells)")
        return 1
    lo, hi = s["min"], s["max"]
    span = (hi - lo) or 1
    for r in range(rows):
        y0, y1 = int(r * H / rows), max(int((r + 1) * H / rows), int(r * H / rows) + 1)
        line = []
        for c in range(cols):
            x0, x1 = int(c * W / cols), max(int((c + 1) * W / cols), int(c * W / cols) + 1)
            vals = [data[y * W + x] for y in range(y0, min(y1, H)) for x in range(x0, min(x1, W))]
            vals = [v for v in vals if t.is_valid(v)]
            if not vals:
                line.append("?")
            else:
                m = sum(vals) / len(vals)
                line.append(RAMP[min(len(RAMP) - 1, int((m - lo) / span * len(RAMP)))])
        print("".join(line))
    print(f"\n{RAMP[0]!r} = {fmt(lo)}  ...  {RAMP[-1]!r} = {fmt(hi)}   ('?' = no data)")


def cmd_zonal(a):
    t = open_geotiff(a.raster)
    s = open_shapefile(a.polygons)
    if s.kind != POLYGON:
        raise GeoError(f"{a.polygons} holds {TYPE_NAMES.get(s.kind, 'unknown')} features; zonal statistics needs polygons")
    rows, warnings = zonal_stats(t, s, a.band - 1)
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    label = a.field
    if label and label not in attr_columns(s):
        raise GeoError(f"no field '{label}' in the shapefile (fields: {', '.join(attr_columns(s))})")
    header = ["record"] + ([label] if label else attr_columns(s)) + ["cells", "valid", "min", "max", "mean"]
    out = []
    for r in rows:
        names = [r["attrs"].get(label)] if label else [r["attrs"].get(c) for c in attr_columns(s)]
        out.append([r["record"]] + names + [r["cells"], r["valid"], r["min"], r["max"], r["mean"]])
    print_table(header, out, as_csv=a.csv)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Inspect GeoTIFF rasters and shapefiles.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="describe a .tif or .shp")
    p.add_argument("file")
    p.set_defaults(fn=cmd_info)

    p = sub.add_parser("table", help="print a shapefile's attribute table")
    p.add_argument("file")
    p.add_argument("--limit", type=int)
    p.add_argument("--csv", action="store_true")
    p.set_defaults(fn=cmd_table)

    p = sub.add_parser("query", help="find features by point or box")
    p.add_argument("file")
    p.add_argument("--at", nargs=2, type=float, metavar=("X", "Y"))
    p.add_argument("--bbox", nargs=4, type=float, metavar=("X0", "Y0", "X1", "Y1"))
    p.add_argument("--csv", action="store_true")
    p.set_defaults(fn=cmd_query)

    p = sub.add_parser("sample", help="raster value at a coordinate")
    p.add_argument("file")
    p.add_argument("x", type=float)
    p.add_argument("y", type=float)
    p.add_argument("--band", type=int, default=1)
    p.set_defaults(fn=cmd_sample)

    p = sub.add_parser("stats", help="raster statistics and histogram")
    p.add_argument("file")
    p.add_argument("--band", type=int, default=1)
    p.add_argument("--bins", type=int, default=8)
    p.set_defaults(fn=cmd_stats)

    p = sub.add_parser("preview", help="text picture of a raster")
    p.add_argument("file")
    p.add_argument("--band", type=int, default=1)
    p.add_argument("--width", type=int, default=64)
    p.set_defaults(fn=cmd_preview)

    p = sub.add_parser("zonal", help="raster statistics inside each polygon")
    p.add_argument("raster")
    p.add_argument("polygons")
    p.add_argument("--band", type=int, default=1)
    p.add_argument("--field", help="attribute to label rows with (default: all attributes)")
    p.add_argument("--csv", action="store_true")
    p.set_defaults(fn=cmd_zonal)

    args = ap.parse_args(argv)
    try:
        return args.fn(args) or 0
    except (GeoError, OSError) as e:
        print(f"gis: {e}", file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
