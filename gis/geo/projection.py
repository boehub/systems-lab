"""Map projections, from the formulas up: longitude/latitude <-> projected coordinates.

A coordinate reference system (CRS) here is an ellipsoid plus, for a projected
one, a projection with its parameters. Four projections cover most of what
shows up in GIS files:

    tmerc  Transverse Mercator      UTM zones, many national grids
    merc   Mercator                 Web Mercator (EPSG:3857), World Mercator (3395)
    lcc    Lambert Conformal Conic  most US state plane zones, Lambert-93 (France)
    aea    Albers Equal Area Conic  the US "contiguous" equal-area grid (EPSG:5070)

Transformations go through longitude/latitude: source -> lon/lat -> target.
Two things are deliberately simple:

* No datum shifts. WGS84, NAD83 and ETRS89 differ by a metre or two, which is
  ignored; datums that differ by tens or hundreds of metres (NAD27, ED50...)
  are refused rather than silently placed wrongly.
* Axis order is always x = longitude / easting, y = latitude / northing, the
  way shapefiles and GeoTIFFs store it (EPSG:4326 officially says lat, lon).

The maths follows Karney (2011) for Transverse Mercator (an n-series accurate
to well under a millimetre within thousands of km of the central meridian) and
Snyder, "Map Projections: A Working Manual" (1987) for the others. The tests
compare against PROJ (through pyproj) to the millimetre.
"""

import math
import re

from .errors import GeoError


class ProjectionError(GeoError):
    pass


# ---- ellipsoids ---------------------------------------------------------------

WGS84 = (6378137.0, 1 / 298.257223563)
GRS80 = (6378137.0, 1 / 298.257222101)
SPHERE = (6378137.0, 0.0)          # what Web Mercator treats the Earth as

_FOOT_US = 0.3048006096012192      # metres per US survey foot


# ---- the CRS object -----------------------------------------------------------

class CRS:
    """A coordinate reference system. Build one with from_epsg(), from_wkt() or parse_crs()."""

    def __init__(self, kind, ellipsoid, name="", epsg=None, lon0=0.0, lat0=0.0, k0=1.0,
                 lat1=None, lat2=None, fe=0.0, fn=0.0, unit=1.0):
        # fe / fn are in metres (like PROJ); `unit` is metres per coordinate unit
        # (1 for metres, 0.3048006096 for US feet). Geographic CRSs use degrees.
        self.kind = kind
        self.a, self.f = ellipsoid
        self.name, self.epsg = name, epsg
        self.lon0, self.lat0, self.k0 = lon0, lat0, k0
        self.lat1 = lat0 if lat1 is None else lat1
        self.lat2 = self.lat1 if lat2 is None else lat2
        self.fe, self.fn, self.unit = fe, fn, unit
        self.e2 = self.f * (2 - self.f)
        self.e = math.sqrt(self.e2)
        if kind != "geographic":
            getattr(self, "_setup_" + kind)()

    @property
    def is_geographic(self):
        return self.kind == "geographic"

    def key(self):
        """What makes two CRSs the same transformation."""
        r = lambda v: None if v is None else round(v, 9)
        return (self.kind, r(self.a), r(self.f * 1e6), r(self.lon0), r(self.lat0), r(self.k0),
                r(self.lat1), r(self.lat2), r(self.fe), r(self.fn), r(self.unit))

    def __eq__(self, other):
        return isinstance(other, CRS) and self.key() == other.key()

    def __hash__(self):
        return hash(self.key())

    def __repr__(self):
        return f"CRS({self.name or self.kind!r}{', EPSG:%d' % self.epsg if self.epsg else ''})"

    # -- public conversions ------------------------------------------------------

    def to_lonlat(self, x, y):
        """Coordinates in this CRS's units -> (lon, lat) in degrees."""
        if self.kind == "geographic":
            return x, y
        lon, lat = self._inverse((x * self.unit - self.fe), (y * self.unit - self.fn))
        lon = math.degrees(lon + self.lon0)
        lon = (lon + 180.0) % 360.0 - 180.0 if not -180.0 <= lon <= 180.0 else lon
        return lon, math.degrees(lat)

    def from_lonlat(self, lon, lat):
        """(lon, lat) in degrees -> coordinates in this CRS's units."""
        if not (-90.0 <= lat <= 90.0):
            raise ProjectionError(f"latitude {lat} is outside -90..90")
        if self.kind == "geographic":
            return lon, lat
        dlam = lon - math.degrees(self.lon0)
        while dlam > 180.0:       # wrap into -180..180, keeping +180 as +180
            dlam -= 360.0
        while dlam < -180.0:
            dlam += 360.0
        dlam = math.radians(dlam)
        x, y = self._forward(dlam, math.radians(lat))
        return (x + self.fe) / self.unit, (y + self.fn) / self.unit

    # -- Transverse Mercator (Karney's n-series) ------------------------------------

    def _setup_tmerc(self):
        n = self.f / (2 - self.f)
        n2, n3, n4, n5, n6 = n * n, n ** 3, n ** 4, n ** 5, n ** 6
        self._A = self.a / (1 + n) * (1 + n2 / 4 + n4 / 64 + n6 / 256)
        self._alpha = (
            n / 2 - 2 * n2 / 3 + 5 * n3 / 16 + 41 * n4 / 180 - 127 * n5 / 288 + 7891 * n6 / 37800,
            13 * n2 / 48 - 3 * n3 / 5 + 557 * n4 / 1440 + 281 * n5 / 630 - 1983433 * n6 / 1935360,
            61 * n3 / 240 - 103 * n4 / 140 + 15061 * n5 / 26880 + 167603 * n6 / 181440,
            49561 * n4 / 161280 - 179 * n5 / 168 + 6601661 * n6 / 7257600,
            34729 * n5 / 80640 - 3418889 * n6 / 1995840,
            212378941 * n6 / 319334400,
        )
        self._beta = (
            n / 2 - 2 * n2 / 3 + 37 * n3 / 96 - n4 / 360 - 81 * n5 / 512 + 96199 * n6 / 604800,
            n2 / 48 + n3 / 15 - 437 * n4 / 1440 + 46 * n5 / 105 - 1118711 * n6 / 3870720,
            17 * n3 / 480 - 37 * n4 / 840 - 209 * n5 / 4480 + 5569 * n6 / 90720,
            4397 * n4 / 161280 - 11 * n5 / 504 - 830251 * n6 / 7257600,
            4583 * n5 / 161280 - 108847 * n6 / 3991680,
            20648693 * n6 / 638668800,
        )
        xi0p = math.atan(self._taup(math.tan(math.radians(self.lat0))))
        self._xi0 = xi0p + sum(a * math.sin(2 * (j + 1) * xi0p) for j, a in enumerate(self._alpha))

    def _taup(self, tau):
        """tan of the conformal latitude, from tan of the geodetic latitude."""
        e = self.e
        h = math.hypot(1.0, tau)
        sigma = math.sinh(e * math.atanh(e * tau / h))
        return tau * math.hypot(1.0, sigma) - sigma * h

    def _tau(self, taup):
        """The inverse of _taup (Newton's method; 3-4 steps reach double precision)."""
        e2 = self.e2
        tau = taup / (1 - e2) if e2 else taup
        for _ in range(12):
            h = math.hypot(1.0, tau)
            sigma = math.sinh(self.e * math.atanh(self.e * tau / h))
            taupa = tau * math.hypot(1.0, sigma) - sigma * h
            d = (taup - taupa) / math.hypot(1.0, taupa) * (1 + (1 - e2) * tau * tau) / ((1 - e2) * h)
            tau += d
            if abs(d) <= 1e-15 * (1 + abs(tau)):
                break
        return tau

    def _forward_tmerc(self, lam, phi):
        taup = self._taup(math.tan(phi)) if abs(phi) < math.pi / 2 else math.copysign(1e300, phi)
        c = math.cos(lam)
        xi_p = math.atan2(taup, c)
        eta_p = math.asinh(math.sin(lam) / math.hypot(taup, c))
        xi, eta = xi_p, eta_p
        for j, al in enumerate(self._alpha, 1):
            xi += al * math.sin(2 * j * xi_p) * math.cosh(2 * j * eta_p)
            eta += al * math.cos(2 * j * xi_p) * math.sinh(2 * j * eta_p)
        k = self.k0 * self._A
        return k * eta, k * (xi - self._xi0)

    def _inverse_tmerc(self, x, y):
        k = self.k0 * self._A
        xi, eta = y / k + self._xi0, x / k
        xi_p, eta_p = xi, eta
        for j, b in enumerate(self._beta, 1):
            xi_p -= b * math.sin(2 * j * xi) * math.cosh(2 * j * eta)
            eta_p -= b * math.cos(2 * j * xi) * math.sinh(2 * j * eta)
        taup = math.sin(xi_p) / math.hypot(math.sinh(eta_p), math.cos(xi_p))
        lam = math.atan2(math.sinh(eta_p), math.cos(xi_p))
        return lam, math.atan(self._tau(taup))

    # -- Mercator -------------------------------------------------------------------

    def _setup_merc(self):
        # lat1 is the latitude of true scale (Mercator 2SP); k0 is the 1SP scale factor
        s = math.sin(math.radians(self.lat1))
        k = self.k0 if self.lat1 == self.lat0 else math.cos(math.radians(self.lat1)) / math.sqrt(1 - self.e2 * s * s)
        self._mk = self.a * k

    def _forward_merc(self, lam, phi):
        if abs(phi) >= math.pi / 2 - 1e-12:
            raise ProjectionError("the poles have no Mercator coordinates")
        s = math.sin(phi)
        y = math.atanh(s) - (self.e * math.atanh(self.e * s) if self.e else 0.0)
        return self._mk * lam, self._mk * y

    def _inverse_merc(self, x, y):
        taup = math.sinh(y / self._mk)
        return x / self._mk, math.atan(self._tau(taup))

    # -- Lambert Conformal Conic ----------------------------------------------------

    def _m(self, phi):
        s = math.sin(phi)
        return math.cos(phi) / math.sqrt(1 - self.e2 * s * s)

    def _t(self, phi):
        s = math.sin(phi)
        return math.tan(math.pi / 4 - phi / 2) / ((1 - self.e * s) / (1 + self.e * s)) ** (self.e / 2)

    def _setup_lcc(self):
        p1, p2, p0 = (math.radians(v) for v in (self.lat1, self.lat2, self.lat0))
        if abs(p1 + p2) < 1e-12:
            raise ProjectionError("a Lambert conic with standard parallels symmetric about the equator is undefined")
        m1, t1 = self._m(p1), self._t(p1)
        if abs(p1 - p2) < 1e-12:
            self._n = math.sin(p1)
        else:
            self._n = (math.log(m1) - math.log(self._m(p2))) / (math.log(t1) - math.log(self._t(p2)))
        self._F = m1 / (self._n * t1 ** self._n)
        self._rho0 = self.a * self.k0 * self._F * self._t(p0) ** self._n

    def _forward_lcc(self, lam, phi):
        if abs(phi) >= math.pi / 2 - 1e-12:
            if phi * self._n < 0:
                raise ProjectionError("this pole has no Lambert conic coordinates")
            rho = 0.0
        else:
            rho = self.a * self.k0 * self._F * self._t(phi) ** self._n
        th = self._n * lam
        return rho * math.sin(th), self._rho0 - rho * math.cos(th)

    def _inverse_lcc(self, x, y):
        n = self._n
        sgn = 1.0 if n > 0 else -1.0
        dy = self._rho0 - y
        rho = sgn * math.hypot(x, dy)
        th = math.atan2(sgn * x, sgn * dy)
        if rho == 0.0:
            return th / n, sgn * math.pi / 2
        t = (rho / (self.a * self.k0 * self._F)) ** (1.0 / n)
        phi = math.pi / 2 - 2 * math.atan(t)
        for _ in range(30):
            s = math.sin(phi)
            new = math.pi / 2 - 2 * math.atan(t * ((1 - self.e * s) / (1 + self.e * s)) ** (self.e / 2))
            done = abs(new - phi) < 1e-15
            phi = new
            if done:
                break
        return th / n, phi

    # -- Albers Equal Area Conic ----------------------------------------------------

    def _q(self, phi):
        s = math.sin(phi)
        if self.e == 0.0:
            return 2 * s
        es = self.e * s
        return (1 - self.e2) * (s / (1 - es * es) - math.log((1 - es) / (1 + es)) / (2 * self.e))

    def _setup_aea(self):
        p1, p2, p0 = (math.radians(v) for v in (self.lat1, self.lat2, self.lat0))
        m1, m2 = self._m(p1), self._m(p2)
        q1, q2, q0 = self._q(p1), self._q(p2), self._q(p0)
        self._n = (m1 * m1 - m2 * m2) / (q2 - q1) if abs(p1 - p2) > 1e-12 else math.sin(p1)
        if self._n == 0:
            raise ProjectionError("an Albers conic with its parallels on the equator is undefined")
        self._C = m1 * m1 + self._n * q1
        self._rho0 = self.a * math.sqrt(max(self._C - self._n * q0, 0.0)) / self._n
        self._qp = self._q(math.pi / 2)

    def _forward_aea(self, lam, phi):
        rho = self.a * math.sqrt(max(self._C - self._n * self._q(phi), 0.0)) / self._n
        th = self._n * lam
        return rho * math.sin(th), self._rho0 - rho * math.cos(th)

    def _inverse_aea(self, x, y):
        n = self._n
        sgn = 1.0 if n > 0 else -1.0
        dy = self._rho0 - y
        rho = sgn * math.hypot(x, dy)
        th = math.atan2(sgn * x, sgn * dy)
        q = (self._C - (rho * n / self.a) ** 2) / n
        if abs(abs(q) - abs(self._qp)) < 1e-12:
            return th / n, math.copysign(math.pi / 2, q)
        if self.e == 0.0:
            return th / n, math.asin(max(-1.0, min(1.0, q / 2)))
        phi = math.asin(max(-1.0, min(1.0, q / 2)))
        for _ in range(30):
            s = math.sin(phi)
            es = self.e * s
            d = ((1 - es * es) ** 2 / (2 * math.cos(phi))
                 * (q / (1 - self.e2) - s / (1 - es * es) + math.log((1 - es) / (1 + es)) / (2 * self.e)))
            phi += d
            if abs(d) < 1e-15:
                break
        return th / n, phi

    def _forward(self, lam, phi):
        return getattr(self, "_forward_" + self.kind)(lam, phi)

    def _inverse(self, x, y):
        lam, phi = getattr(self, "_inverse_" + self.kind)(x, y)
        return lam, phi

    # -- text form --------------------------------------------------------------------

    def to_wkt(self):
        """ESRI-style WKT1, readable again by from_wkt() and by other GIS software."""
        inv_f = 1 / self.f if self.f else 0
        geog_name = "GCS_Unknown"
        geog = (f'GEOGCS["{geog_name}",DATUM["D_Unknown",SPHEROID["Unknown",{self.a!r},{inv_f!r}]],'
                f'PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]]')
        if self.kind == "geographic":
            return geog.replace(geog_name, self.name or geog_name, 1)
        proj = {"tmerc": "Transverse_Mercator", "merc": "Mercator", "lcc": "Lambert_Conformal_Conic",
                "aea": "Albers"}[self.kind]
        u = self.unit
        P = lambda n, v: f'PARAMETER["{n}",{v!r}]'
        params = [P("False_Easting", self.fe / u), P("False_Northing", self.fn / u)]
        if self.kind == "tmerc":
            params += [P("Central_Meridian", math.degrees(self.lon0)), P("Scale_Factor", self.k0),
                       P("Latitude_Of_Origin", self.lat0)]
        elif self.kind == "merc":
            params += [P("Central_Meridian", math.degrees(self.lon0)), P("Standard_Parallel_1", self.lat1)]
        else:
            params += [P("Central_Meridian", math.degrees(self.lon0)), P("Standard_Parallel_1", self.lat1),
                       P("Standard_Parallel_2", self.lat2), P("Latitude_Of_Origin", self.lat0)]
            if self.k0 != 1.0:
                params.append(P("Scale_Factor", self.k0))
        unit = "Meter" if u == 1.0 else ("Foot_US" if abs(u - _FOOT_US) < 1e-12 else "Unit")
        return (f'PROJCS["{self.name or "Unknown"}",{geog},PROJECTION["{proj}"],' + ",".join(params)
                + f',UNIT["{unit}",{u!r}]]')


# NB: lon0 is stored in radians, lat0/lat1/lat2 in degrees; the constructor below
# takes degrees for both and converts, so callers never think about it.
def _make(kind, ellipsoid, *, lon0=0.0, **kw):
    return CRS(kind, ellipsoid, lon0=math.radians(lon0), **kw)


def geographic(ellipsoid=WGS84, name="", epsg=None):
    return _make("geographic", ellipsoid, name=name, epsg=epsg)


def utm(zone, south=False, ellipsoid=WGS84, name="", epsg=None):
    if not 1 <= zone <= 60:
        raise ProjectionError(f"UTM zone {zone} does not exist (1..60)")
    return _make("tmerc", ellipsoid, lon0=-183.0 + 6.0 * zone, k0=0.9996, fe=500000.0,
                 fn=10000000.0 if south else 0.0, name=name or f"UTM_Zone_{zone}{'S' if south else 'N'}", epsg=epsg)


# ---- EPSG codes we know -------------------------------------------------------------

_LCC_VA_SOUTH = dict(lon0=-78.5, lat0=36 + 1 / 3, lat1=37 + 58 / 60, lat2=36 + 46 / 60)


def from_epsg(code):
    """The CRS for an EPSG code, or ProjectionError if this module does not know it."""
    code = int(code)
    if code == 4326:
        return geographic(WGS84, "WGS 84", code)
    if code == 4269:
        return geographic(GRS80, "NAD83", code)
    if code == 4258:
        return geographic(GRS80, "ETRS89", code)
    if code == 3857:
        return _make("merc", SPHERE, name="WGS 84 / Pseudo-Mercator", epsg=code)
    if code == 3395:
        return _make("merc", WGS84, name="WGS 84 / World Mercator", epsg=code)
    if 32601 <= code <= 32660 or 32701 <= code <= 32760:
        zone, south = code % 100, code >= 32700
        return utm(zone, south, WGS84, f"WGS 84 / UTM zone {zone}{'S' if south else 'N'}", code)
    if 26901 <= code <= 26923:
        return utm(code - 26900, False, GRS80, f"NAD83 / UTM zone {code - 26900}N", code)
    if 25828 <= code <= 25838:
        return utm(code - 25800, False, GRS80, f"ETRS89 / UTM zone {code - 25800}N", code)
    if code == 5070:
        return _make("aea", GRS80, lon0=-96.0, lat0=23.0, lat1=29.5, lat2=45.5,
                     name="NAD83 / Conus Albers", epsg=code)
    if code == 2154:
        return _make("lcc", GRS80, lon0=3.0, lat0=46.5, lat1=49.0, lat2=44.0, fe=700000.0, fn=6600000.0,
                     name="RGF93 v1 / Lambert-93", epsg=code)
    if code == 32147:
        return _make("lcc", GRS80, fe=3500000.0, fn=1000000.0, name="NAD83 / Virginia South",
                     epsg=code, **_LCC_VA_SOUTH)
    if code == 2284:
        return _make("lcc", GRS80, fe=11482916.666666666 * _FOOT_US, fn=3280833.3333333335 * _FOOT_US,
                     unit=_FOOT_US, name="NAD83 / Virginia South (ftUS)", epsg=code, **_LCC_VA_SOUTH)
    raise ProjectionError(
        f"EPSG:{code} is not one this program knows (WGS84/NAD83/ETRS89 geographic, 3857, 3395, UTM zones, "
        f"5070, 2154, 32147, 2284); give a .prj file or WKT text instead")


# ---- reading WKT (the text inside a .prj file) -----------------------------------------

_TOKEN = re.compile(r'\s*(?:("(?:[^"]|"")*")|([\[\](){},])|([^\s\[\](){},"]+))')


def _parse_wkt_tree(text):
    toks = []
    pos = 0
    text = text.strip()
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise ProjectionError("the projection text is not valid WKT")
        pos = m.end()
        if m.group(1) is not None:
            toks.append(("s", m.group(1)[1:-1].replace('""', '"')))
        elif m.group(2) is not None:
            toks.append((m.group(2), None))
        else:
            toks.append(("w", m.group(3)))
    i = 0

    def node():
        nonlocal i
        if i >= len(toks) or toks[i][0] != "w":
            raise ProjectionError("the projection text is not valid WKT")
        name = toks[i][1]
        i += 1
        if i >= len(toks) or toks[i][0] not in "[(":
            raise ProjectionError("the projection text is not valid WKT")
        i += 1
        args = []
        while True:
            if i >= len(toks):
                raise ProjectionError("the projection text ends in the middle of a bracket")
            kind, val = toks[i]
            if kind in "])":
                i += 1
                return (name.upper(), args)
            if kind == ",":
                i += 1
            elif kind == "s":
                args.append(val)
                i += 1
            elif kind == "w" and i + 1 < len(toks) and toks[i + 1][0] in "[(":
                args.append(node())
            elif kind == "w":
                try:
                    args.append(float(val))
                except ValueError:
                    args.append(val)
                i += 1
            else:
                raise ProjectionError("the projection text is not valid WKT")

    root = node()
    if i != len(toks):
        raise ProjectionError("unexpected text after the end of the WKT")
    return root


def _children(node, name):
    return [a for a in node[1] if isinstance(a, tuple) and a[0] == name]


def _norm(s):
    return re.sub(r"[^a-z0-9]", "", str(s).lower())


_PROJ_KIND = {
    "transversemercator": "tmerc", "gausskruger": "tmerc",
    "mercator": "merc", "mercator1sp": "merc", "mercator2sp": "merc",
    "mercatorauxiliarysphere": "merc_sphere", "popularvisualisationpseudomercator": "merc_sphere",
    "lambertconformalconic": "lcc", "lambertconformalconic2sp": "lcc", "lambertconformalconic1sp": "lcc",
    "albers": "aea", "albersconicequalarea": "aea", "albersequalareaconic": "aea",
}
_PARAM = {
    "falseeasting": "fe", "falsenorthing": "fn", "centralmeridian": "lon0", "longitudeoforigin": "lon0",
    "longitudeofcenter": "lon0", "latitudeoforigin": "lat0", "latitudeofcenter": "lat0",
    "scalefactor": "k0", "standardparallel1": "lat1", "standardparallel2": "lat2",
}


def from_wkt(text):
    """A CRS from WKT1 text (ESRI .prj files and the common GDAL/OGC form).

    Understands geographic (degree) systems and the four projections above.
    WKT2 ("PROJCRS[...]") and other projections are refused with a clear message.
    """
    try:
        return _from_wkt(text)
    except (ValueError, IndexError, TypeError, AttributeError, ZeroDivisionError, OverflowError, KeyError):
        raise ProjectionError("the projection text is malformed (a number or bracket is wrong)") from None


def _from_wkt(text):
    root = _parse_wkt_tree(text)
    kind = root[0]
    if kind in ("PROJCRS", "GEOGCRS", "BASEGEOGCRS"):
        raise ProjectionError("WKT2 text (PROJCRS/GEOGCRS) is not supported; use a WKT1 .prj")
    if kind not in ("GEOGCS", "PROJCS"):
        raise ProjectionError(f"expected GEOGCS or PROJCS, found {kind}")
    name = root[1][0] if root[1] and isinstance(root[1][0], str) else ""
    geogcs = root if kind == "GEOGCS" else (_children(root, "GEOGCS") or [None])[0]
    if geogcs is None:
        raise ProjectionError("the projected system has no GEOGCS inside it")
    spheroid = None
    for d in _children(geogcs, "DATUM"):
        s = _children(d, "SPHEROID")
        if s:
            spheroid = s[0]
    if spheroid is None or len(spheroid[1]) < 3:
        raise ProjectionError("no SPHEROID in the projection text")
    a, invf = float(spheroid[1][1]), float(spheroid[1][2])
    ell = (a, 1 / invf if invf else 0.0)
    datum_name = _norm(([d for d in _children(geogcs, "DATUM")] or [("", [""])])[0][1][0])
    if any(bad in datum_name for bad in ("nad27", "northamerican1927", "ed50", "europeandatum1950", "tokyo")):
        raise ProjectionError("datum shifts are not implemented, so this older datum cannot be converted exactly")
    epsg = None
    auth = _children(root, "AUTHORITY")
    if auth and len(auth[-1][1]) >= 2 and str(auth[-1][1][0]).upper() == "EPSG":
        try:
            epsg = int(float(auth[-1][1][1]))
        except ValueError:
            pass

    if kind == "GEOGCS":
        units = _children(root, "UNIT")
        if units and abs(float(units[0][1][1]) - 0.0174532925199433) > 1e-9:
            raise ProjectionError("only angles in degrees are supported")
        return geographic(ell, name, epsg)

    proj = _children(root, "PROJECTION")
    if not proj:
        raise ProjectionError("the PROJCS has no PROJECTION")
    pname = _norm(proj[0][1][0])
    if pname not in _PROJ_KIND:
        raise ProjectionError(f"projection '{proj[0][1][0]}' is not supported (Transverse Mercator, Mercator, "
                              f"Lambert Conformal Conic and Albers are)")
    pk = _PROJ_KIND[pname]
    p = {"fe": 0.0, "fn": 0.0, "lon0": 0.0, "lat0": 0.0, "k0": 1.0, "lat1": None, "lat2": None}
    for par in _children(root, "PARAMETER"):
        key = _PARAM.get(_norm(par[1][0]))
        if key:
            p[key] = float(par[1][1])
    units = [u for u in _children(root, "UNIT")]
    unit = float(units[0][1][1]) if units else 1.0
    if not 0.01 < unit < 100:
        raise ProjectionError(f"odd linear unit factor {unit}")
    # GDAL spells Web Mercator as an ellipsoidal Mercator plus a PROJ4 "+a=R +b=R" hint
    for ext in _children(root, "EXTENSION"):
        text = " ".join(str(v) for v in ext[1])
        ma, mb = re.search(r"\+a=([\d.]+)", text), re.search(r"\+b=([\d.]+)", text)
        if ma and mb and ma.group(1) == mb.group(1) and pk == "merc":
            ell = (float(ma.group(1)), 0.0)
    if pk == "merc_sphere":
        ell, pk = (a, 0.0), "merc"
    if pk in ("lcc", "aea") and p["lat1"] is None:
        p["lat1"] = p["lat0"]
    if pk == "merc" and p["lat1"] is None:
        p["lat1"] = p["lat0"] = 0.0
    if pk == "merc" and pname == "mercator2sp":
        p["lat0"] = 0.0
    crs = _make(pk, ell, lon0=p["lon0"], lat0=p["lat0"], k0=p["k0"], lat1=p["lat1"], lat2=p["lat2"],
                fe=p["fe"] * unit, fn=p["fn"] * unit, unit=unit, name=name, epsg=epsg)
    return crs


def parse_crs(spec):
    """Turn what a person typed into a CRS: 4326, 'EPSG:32618', 'utm:18N', 'wgs84',
    the path of a .prj file, or WKT text."""
    if isinstance(spec, CRS):
        return spec
    s = str(spec).strip()
    low = s.lower()
    m = re.fullmatch(r"(?:epsg:)?(\d{4,6})", low)
    if m:
        return from_epsg(int(m.group(1)))
    m = re.fullmatch(r"utm:?\s*(\d{1,2})\s*([ns])", low)
    if m:
        zone, south = int(m.group(1)), m.group(2) == "s"
        if not 1 <= zone <= 60:
            raise ProjectionError(f"UTM zone {zone} does not exist (1..60)")
        return from_epsg((32700 if south else 32600) + zone)
    if low in ("wgs84", "lonlat", "lon/lat", "longlat", "geographic"):
        return from_epsg(4326)
    if low in ("webmercator", "web-mercator", "mercator"):
        return from_epsg(3857)
    if s[:6].upper() in ("GEOGCS", "PROJCS") or s[:7].upper() in ("PROJCRS", "GEOGCRS"):
        return from_wkt(s)
    try:
        with open(s, errors="replace") as f:
            return from_wkt(f.read())
    except OSError:
        raise ProjectionError(f"don't know what '{s}' is: expected an EPSG code, utm:18N, a .prj file or WKT text") from None


# ---- moving points and shapes between systems --------------------------------------------

class Transformer:
    """Converts coordinates from one CRS to another (through longitude/latitude)."""

    def __init__(self, src, dst):
        self.src, self.dst = parse_crs(src), parse_crs(dst)
        self.identity = self.src == self.dst

    def __call__(self, x, y):
        if self.identity:
            return x, y
        lon, lat = self.src.to_lonlat(x, y)
        return self.dst.from_lonlat(lon, lat)

    def points(self, pts):
        return [self(x, y) for x, y in pts]


def densify(pts, max_step):
    """Insert points along straight segments so none is longer than max_step, so a
    straight edge in one projection can curve properly in another."""
    if max_step <= 0 or len(pts) < 2:
        return list(pts)
    out = [pts[0]]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        steps = min(256, max(1, math.ceil(math.hypot(x1 - x0, y1 - y0) / max_step)))
        for k in range(1, steps):
            out.append((x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps))
        out.append((x1, y1))
    return out


def transform_feature(feat, tr, max_step=0.0):
    """A copy of a shapefile feature with every vertex moved by Transformer `tr`.
    With max_step > 0, line and polygon edges are first cut into pieces no longer
    than that (in the source units)."""
    from .shapefile import Feature, POINT, MULTIPOINT
    parts = []
    try:
        for part in feat.parts:
            if feat.kind not in (POINT, MULTIPOINT):
                part = densify(part, max_step)
            parts.append(tr.points(part))
    except ProjectionError as e:
        raise ProjectionError(f"record {feat.record}: {e}") from None
    xs = [p[0] for part in parts for p in part]
    ys = [p[1] for part in parts for p in part]
    bbox = (min(xs), min(ys), max(xs), max(ys)) if xs else None
    return Feature(feat.record, feat.kind, parts, bbox, dict(feat.attrs))


def transform_bounds(bounds, tr, samples=21):
    """The bounding box, in the target system, of a source-system box (its whole
    outline is sampled, because a box's edges curve when projected)."""
    x0, y0, x1, y1 = bounds
    pts = []
    for i in range(samples):
        t = i / (samples - 1)
        pts += [(x0 + (x1 - x0) * t, y0), (x0 + (x1 - x0) * t, y1), (x0, y0 + (y1 - y0) * t), (x1, y0 + (y1 - y0) * t)]
    out = tr.points(pts)
    xs, ys = [p[0] for p in out], [p[1] for p in out]
    return min(xs), min(ys), max(xs), max(ys)
