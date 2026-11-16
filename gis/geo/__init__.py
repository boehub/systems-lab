"""geo: read GeoTIFF rasters and ESRI shapefiles using only the standard library."""

from .errors import GeoError
from .geotiff import GeoTiff, open_geotiff
from .shapefile import Feature, Shapefile, open_shapefile
from .projection import CRS, ProjectionError, Transformer, parse_crs

__all__ = ["GeoError", "GeoTiff", "open_geotiff", "Feature", "Shapefile", "open_shapefile", "CRS", "ProjectionError", "Transformer", "parse_crs"]
