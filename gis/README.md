# gis: GeoTIFF and shapefile reader

Reads the two most common GIS file formats **byte by byte, with nothing but the Python standard library**: GeoTIFF rasters (elevation grids, satellite imagery) and ESRI shapefiles (points, lines, polygons with attributes). It answers the everyday questions about them: what is in this file, what is the value at this coordinate, which polygon is this point in, what is the average elevation inside each region.

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

**Zonal statistics** (`geo/zonal.py`): a cell belongs to a polygon when its *centre* is inside. Only cells in the polygon's bounding box are tested.

## How it was checked

```
python3 -m unittest discover -s tests       # 78 tests
```

- **Hand-computed answers.** Tiny grids where the zonal result can be worked out on paper, and the sample data, whose cell counts follow from the rectangle sizes (1500 = 50 x 30 cells, the lake's no-data overlap = 10 x 12 cells, and so on).
- **Independent libraries.** `tests/test_reference.py` writes files with `tifffile` and `pyshp` (9 pixel types x 3 compressions x predictor x strips/tiles x both byte orders) and checks that this reader returns identical pixels, geometry and attributes; it also checks that those libraries can read what this repo's test writers produce, and that the statistics match NumPy. It needs `pip install numpy tifffile imagecodecs pyshp` and is skipped without them.
- **Damaged files.** Truncated and randomly corrupted files must produce a one-line `GeoError`, never a crash. Fuzzing found real problems while this was being built (a corrupt tag of the wrong type, a negative point count, an infinite cell size), all now fixed and covered by tests; `python3 tests/fuzz_tiff.py` repeats this (about 200,000 mutated files across the runs while building it, no escapes at the end).

## Not supported

- **No reprojection.** The raster and polygons must already share a coordinate system. `zonal` warns when the `.prj` looks different from the raster's EPSG or when the data don't overlap, but it can't convert between them.
- BigTIFF, JPEG-compressed TIFF, the floating-point predictor, palettes and sub-byte bit depths, and rotated rasters.
- Shapefile MultiPatch; Z and M values are skipped.
- Whole bands are loaded into memory, which is fine for files up to a few hundred megabytes and not for a continental mosaic.
- Zonal statistics treat cells as points (centre-in-polygon), as most tools do by default; they do not compute fractional coverage.
