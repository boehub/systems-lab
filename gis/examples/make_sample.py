#!/usr/bin/env python3
"""Regenerate the sample data in this folder (all of it is invented).

    python3 examples/make_sample.py

dem.tif       120 x 90 elevation grid (float32, tiled, Deflate), WGS 84, with a no-data "lake"
regions.shp   four polygons (one with a hole) and attributes
places.shp    five points
roads.shp     two polylines
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests"))

from writers import square, write_shapefile, write_tiff  # noqa: E402

PRJ = ('GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],'
       'PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]]')

W, H = 120, 90
X0, Y0, CELL = -76.60, 37.10, 0.005
NODATA = -9999.0


def elevation(col, row):
    hill = 38 * math.exp(-((col - 35) ** 2 + (row - 28) ** 2) / 180)
    ridge = 22 * math.exp(-((col - 88) ** 2 + (row - 40) ** 2) / 420)
    slope = 12 * (1 - row / H)                 # higher toward the north
    return round(2 + slope + hill + ridge, 1)


def dem():
    vals = []
    for row in range(H):
        for col in range(W):
            lake = 70 <= col < 84 and 62 <= row < 74
            vals.append(NODATA if lake else elevation(col, row))
    data = write_tiff(W, H, [vals], fmt=3, bits=32, compression=8, tile=(32, 32),
                      geo=dict(scale=(CELL, CELL), tie=(X0, Y0), epsg=4326), nodata=NODATA)
    with open(os.path.join(HERE, "dem.tif"), "wb") as f:
        f.write(data)


def vectors():
    regions = [
        [square(-76.55, 36.90, -76.30, 37.05)],
        [square(-76.30, 36.85, -76.05, 37.05)],
        [square(-76.55, 36.70, -76.20, 36.85)],
        [square(-76.20, 36.70, -76.05, 36.85), square(-76.15, 36.74, -76.10, 36.80, hole=True)],
    ]
    write_shapefile(
        os.path.join(HERE, "regions"), 5, regions,
        [("NAME", "C", 16, 0), ("KIND", "C", 10, 0), ("POP", "N", 8, 0)],
        [["Northshore", "suburb", 48200], ["Bayview", "suburb", 31500],
         ["Marshland", "rural", 6100], ["Harborside", "urban", 72900]],
        prj=PRJ)
    places = [("Old Mill", -76.45, 36.98), ("Fort Hill", -76.37, 37.02), ("Bay Pier", -76.12, 36.95),
              ("Salt Creek", -76.40, 36.76), ("Lagoon Point", -76.125, 36.77)]
    write_shapefile(os.path.join(HERE, "places"), 1, [[[(x, y)]] for _, x, y in places],
                    [("NAME", "C", 16, 0)], [[n] for n, _, _ in places], prj=PRJ)
    roads = [[[(-76.55, 36.95), (-76.40, 36.93), (-76.20, 36.97), (-76.05, 36.92)]],
             [[(-76.30, 37.05), (-76.30, 36.85), (-76.28, 36.70)]]]
    write_shapefile(os.path.join(HERE, "roads"), 3, roads,
                    [("NAME", "C", 16, 0), ("LANES", "N", 2, 0)], [["Shore Road", 4], ["Ridge Avenue", 2]], prj=PRJ)


if __name__ == "__main__":
    dem()
    vectors()
    print("wrote dem.tif, regions.*, places.*, roads.*")
