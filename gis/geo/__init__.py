"""geo: read GeoTIFF rasters and ESRI shapefiles using only the standard library."""

from .errors import GeoError
from .geotiff import GeoTiff, open_geotiff
from .shapefile import Feature, Shapefile, open_shapefile

__all__ = ["GeoError", "GeoTiff", "open_geotiff", "Feature", "Shapefile", "open_shapefile"]
