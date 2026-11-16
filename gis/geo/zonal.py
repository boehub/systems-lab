"""Zonal statistics: summarise the raster cells that fall inside each polygon."""

import math

from . import geometry, projection
from .shapefile import POLYGON


def _crs_warnings(tif, shp):
    warnings = []
    if tif.georef is None:
        return warnings
    epsg = tif.georef.epsg
    prj = shp.prj.lstrip().upper()
    if epsg and prj:
        geographic_raster = epsg in (4326, 4269, 4267, 4258) or 4000 <= epsg < 5000
        if geographic_raster and prj.startswith("PROJCS"):
            warnings.append("the raster uses geographic coordinates (degrees) but the shapefile's .prj is projected")
        elif not geographic_raster and prj.startswith("GEOGCS"):
            warnings.append("the raster is projected but the shapefile's .prj uses geographic coordinates (degrees)")
    rb, sb = tif.georef.bounds(), shp.bbox
    if not geometry.boxes_overlap(rb, sb):
        warnings.append("the polygons don't overlap the raster at all (different coordinate systems?)")
    return warnings


def _crs_of(tif, shp):
    """(raster CRS, polygon CRS), each None when it is missing or not understood."""
    raster = polys = None
    if tif.georef is not None and tif.georef.epsg:
        try:
            raster = projection.from_epsg(tif.georef.epsg)
        except projection.ProjectionError:
            pass
    if shp.prj:
        try:
            polys = projection.from_wkt(shp.prj)
        except projection.ProjectionError:
            pass
    return raster, polys


def zonal_stats(tif, shp, band=0, reproject=True):
    """One result per polygon: how many cells have their centre inside it, and
    the min / max / mean / sum of their values (no-data cells are left out).

    When both coordinate systems are known and differ, the polygons are moved
    into the raster's system first (their edges are subdivided so they curve the
    way they should) and a note says so; `area` stays in the polygons' own
    units. When either is unknown the two are assumed to match, and warnings
    point out the obvious mismatches. Returns (rows, warnings).
    """
    if tif.georef is None:
        raise ValueError("the raster has no georeferencing")
    raster_crs, poly_crs = _crs_of(tif, shp)
    notes = []
    features = shp.features
    if reproject and raster_crs and poly_crs:
        if raster_crs != poly_crs:
            tr = projection.Transformer(poly_crs, raster_crs)
            # edges are cut into pieces no longer than 1/100 of the polygon layer's width
            sx0, sy0, sx1, sy1 = shp.bbox
            step = max(sx1 - sx0, sy1 - sy0) / 100.0
            moved = []
            for f in features:
                if f.kind == POLYGON and f.parts:
                    nf = projection.transform_feature(f, tr, step)
                    moved.append((f, nf))
            features = moved
            notes.append(f"polygons reprojected from {poly_crs.name or poly_crs.kind} to "
                         f"{raster_crs.name or raster_crs.kind} to match the raster")
        else:
            features = [(f, f) for f in features]
        warnings = notes
    else:
        features = [(f, f) for f in features]
        warnings = _crs_warnings(tif, shp)
    return _zonal(tif, features, band), warnings


def _zonal(tif, features, band):
    g, data = tif.georef, tif.read_band(band)
    rows = []
    for orig, feat in features:
        if feat.kind != POLYGON or feat.bbox is None:
            continue
        xmin, ymin, xmax, ymax = feat.bbox
        # the range of cells whose centres could fall inside the polygon's bounding box
        c0 = max(0, math.ceil((xmin - g.x0) / g.dx - 0.5))
        c1 = min(g.width - 1, math.floor((xmax - g.x0) / g.dx - 0.5))
        r0 = max(0, math.ceil((g.y0 - ymax) / g.dy - 0.5))
        r1 = min(g.height - 1, math.floor((g.y0 - ymin) / g.dy - 0.5))
        cells = valid = 0
        total = 0.0
        lo = hi = None
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                x, y = g.cell_center(c, r)
                if not feat.contains(x, y):
                    continue
                cells += 1
                v = data[r * g.width + c]
                if not tif.is_valid(v):
                    continue
                valid += 1
                total += v
                lo = v if lo is None or v < lo else lo
                hi = v if hi is None or v > hi else hi
        rows.append({
            "record": orig.record, "attrs": orig.attrs, "area": orig.area,
            "cells": cells, "valid": valid, "min": lo, "max": hi,
            "mean": total / valid if valid else None, "sum": total if valid else None,
        })
    return rows
