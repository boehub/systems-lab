# gis: GeoTIFF and shapefile reader, with map projections

Reads the two most common GIS file formats **byte by byte, with nothing but the Python standard library**: GeoTIFF rasters (elevation grids, satellite imagery) and ESRI shapefiles (points, lines, polygons with attributes). It answers the everyday questions about them: what is in this file, what is the value at this coordinate, which polygon is this point in, what is the average elevation inside each region. It also converts between coordinate systems (longitude/latitude, UTM, Web Mercator, state plane...) using projection maths written from the published formulas.

No GDAL, no NumPy. The point is to understand how the formats work, so the readers do their own byte-level parsing.

```
$ python3 gis.py zonal examples/dem.tif examples/regions.shp --field NAME
record  NAME        cells  valid  min   max   mean
------  ----------  -----  -----  ----  ----  -----
1       Northshore  1500   1500   9.4   48.3  23.2
2       Bayview     2000   2000   10.4  30.7  19.67
3       Marshland   2100   1980   3.5   21.6  6.431
4       Harborside  780    732    3.7   24.7  11.03
```

(All the data in `examples/` is invented: a synthetic terrain and four made-up regions. `python3 examples/make_sample.py` regenerates it.)

## Commands

```
gis.py info    FILE                         what is in a .tif or .shp
gis.py table   FILE.shp [--limit N] [--csv] attribute table, plus area (polygons) or length (lines)
gis.py query   FILE.shp --at X Y            which polygon contains this point
gis.py query   FILE.shp --bbox X0 Y0 X1 Y1  features that touch a box
gis.py sample  FILE.tif X Y                 raster value at a coordinate
gis.py stats   FILE.tif [--band N]          count / min / max / mean / std and a histogram (no-data left out)
gis.py preview FILE.tif [--width N]         a text picture of the raster
gis.py zonal   FILE.tif FILE.shp [--field NAME] [--csv]   statistics of the cells inside each polygon
gis.py project --to CRS [--from CRS] X Y ...    convert coordinates (or read "x y" lines on stdin)
gis.py reproject IN.shp OUT.shp --to CRS         write a shapefile in another coordinate system
gis.py reproject IN.tif --to CRS                 where a raster's footprint falls in another system
gis.py sample  FILE.tif X Y --crs CRS            ... with X Y written in a different system
```

A CRS is an EPSG code (`4326`, `EPSG:32618`), `utm:18N`, the path of a `.prj` file, or WKT text.

```
$ python3 gis.py project --to utm:18N -76.285 36.85
385438.8324 4079003.1063
$ python3 gis.py reproject examples/regions.shp /tmp/regions_utm.shp --to utm:18N
wrote /tmp/regions_utm.shp: 4 Polygon feature(s) in WGS 84 / UTM zone 18N
$ python3 gis.py zonal examples/dem.tif /tmp/regions_utm.shp --field NAME   # polygons are moved onto the raster's system first
note: polygons reprojected from WGS 84 / UTM zone 18N to WGS 84 to match the raster
```

`--csv` output opens straight in a spreadsheet.

```
$ python3 gis.py preview examples/dem.tif --width 60
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:::::::::::::----------:::::::::::::::::::::::::::::::::::::
:::::::::::---========---::::::::::::::::------:::::::::::::
:::::::::---=++*****++==--:::::::::::--------------:::::::::
...::::::--=+*##%%%%#*++=--:::::::::------====-------:::::::
....::::--=+*#%%@@@@%#*+==-:::::::-----===========----::::::
         ...
```

## What it reads

**GeoTIFF** (`geo/geotiff.py`): classic TIFF in either byte order; strips or tiles; uncompressed, **LZW** (decoder written from scratch), or Deflate; the horizontal predictor; 8/16/32/64-bit integers and 32/64-bit floats; any number of bands, interleaved or separate; the GeoTIFF tags for cell size, origin, EPSG code and no-data value.

**Shapefile** (`geo/shapefile.py`): `.shp` points, multipoints, polylines and polygons (the Z/M variants read as 2D), plus the `.dbf` attribute table (text, numbers, booleans, dates; deleted rows; `.cpg` encodings) and the `.prj` text.

**Geometry** (`geo/geometry.py`): shoelace area, ring orientation (outer rings run clockwise, holes counter-clockwise), grouping holes with their outer ring, ray-casting point-in-polygon with holes and multi-part polygons.

**Zonal statistics** (`geo/zonal.py`): a cell belongs to a polygon when its *centre* is inside. Only cells in the polygon's bounding box are tested. When the raster's EPSG code and the shapefile's `.prj` are both understood and differ, the polygons are reprojected into the raster's system first (their edges are cut into short pieces so they curve the way they should) and a note says so.

**Projections** (`geo/projection.py`): forward and inverse **Transverse Mercator** (UTM zones and national grids; Karney's n-series, good to well under a millimetre), **Mercator** (Web Mercator as the spherical case), **Lambert Conformal Conic** and **Albers Equal Area Conic** (Snyder's formulas), on any ellipsoid. A CRS comes from a known EPSG code (WGS84/NAD83/ETRS89 geographic, 3857, 3395, every UTM zone in WGS84, NAD83 zones 1-23 and ETRS89 zones 28-38, 5070, 2154, 32147, 2284) or from WKT1 text, so a `.prj` for any system built from those four projections works, in metres or US feet. `geo/shpwrite.py` writes shapefiles so a reprojected layer can be saved.

## How it was checked

```
python3 -m unittest discover -s tests       # 108 tests
```

- **Hand-computed answers.** Tiny grids where the zonal result can be worked out on paper, and the sample data, whose cell counts follow from the rectangle sizes (1500 = 50 x 30 cells, the lake's no-data overlap = 10 x 12 cells, and so on).
- **Independent libraries.** `tests/test_reference.py` writes files with `tifffile` and `pyshp` (9 pixel types x 3 compressions x predictor x strips/tiles x both byte orders) and checks that this reader returns identical pixels, geometry and attributes; it also checks that those libraries can read what this repo's test writers produce, and that the statistics match NumPy. It needs `pip install numpy tifffile imagecodecs pyshp` and is skipped without them.
- **Projections against PROJ.** `tests/test_projection.py` compares 3,000 random points in ten coordinate systems (forward and inverse), all 120 UTM zones, and the WKT that PROJ itself writes for nine systems, against `pyproj`: agreement is within a millimetre (nanometres for most). It also checks properties the maths must have: round trips, UTM's 0.9996 scale on the central meridian, Albers preserving area, conformal projections keeping right angles, longitude wrap-around. Fuzzing the WKT parser (3,000 mutated `.prj` strings) found a crash on a non-numeric parameter, now fixed. Needs `pip install pyproj`; skipped without it.
- **Damaged files.** Truncated and randomly corrupted files must produce a one-line `GeoError`, never a crash. Fuzzing found real problems while this was being built (a corrupt tag of the wrong type, a negative point count, an infinite cell size), all now fixed and covered by tests; `python3 tests/fuzz_tiff.py` repeats this (about 200,000 mutated files across the runs while building it, no escapes at the end).

## Not supported

- **No datum shifts.** WGS84, NAD83 and ETRS89 are treated as the same (they differ by a metre or two); datums that differ by more (NAD27, ED50) are refused rather than placed wrongly. Other projections (polyconic, stereographic, oblique Mercator...) and WKT2 are refused with a message; EPSG codes outside the list above need a `.prj` or WKT. Axis order is always x = longitude/easting.
- **Rasters are not resampled.** `reproject` on a `.tif` reports its footprint in the new system; it does not warp the pixels. Zonal statistics reproject the *polygons* to the raster instead, which gives the same answer without touching the data. When the coordinate systems are unknown, `zonal` still assumes they match and warns about obvious mismatches.
- BigTIFF, JPEG-compressed TIFF, the floating-point predictor, palettes and sub-byte bit depths, and rotated rasters.
- Shapefile MultiPatch; Z and M values are skipped.
- Whole bands are loaded into memory, which is fine for files up to a few hundred megabytes and not for a continental mosaic.
- Zonal statistics treat cells as points (centre-in-polygon), as most tools do by default; they do not compute fractional coverage.
