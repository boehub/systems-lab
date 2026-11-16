"""Plane geometry for shapefile features: area, length, point-in-polygon."""

import math


def ring_area_signed(ring):
    """Shoelace area. Positive when the ring runs counter-clockwise (y up)."""
    total = 0.0
    n = len(ring)
    for i in range(n - 1):
        x1, y1 = ring[i]
        x2, y2 = ring[i + 1]
        total += x1 * y2 - x2 * y1
    # shapefile rings are closed (first == last); close it if a writer didn't
    if n and ring[0] != ring[-1]:
        x1, y1 = ring[-1]
        x2, y2 = ring[0]
        total += x1 * y2 - x2 * y1
    return total / 2.0


def line_length(points):
    return sum(math.dist(points[i], points[i + 1]) for i in range(len(points) - 1))


def ring_contains(ring, x, y):
    """Ray casting: is (x, y) inside this one ring? A point exactly on an edge
    can come out either way, but a point on the edge shared by two neighbouring
    polygons is claimed by exactly one of them."""
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if (yi > y) != (yj > y):
            if x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
        j = i
    return inside


def split_polygons(rings):
    """Group a polygon's rings into (outer, [holes]).

    The shapefile spec says outer rings run clockwise and holes counter-clockwise,
    and a hole belongs to the outer ring that contains it.
    """
    outers, holes = [], []
    for ring in rings:
        (outers if ring_area_signed(ring) <= 0 else holes).append(ring)
    grouped = [(o, []) for o in outers]
    for h in holes:
        px, py = h[0]
        for outer, its_holes in grouped:
            if ring_contains(outer, px, py):
                its_holes.append(h)
                break
        else:
            # a hole with no outer ring around it: damaged file, treat it as an island
            grouped.append((h, []))
    return grouped


def polygon_area(rings):
    area = 0.0
    for outer, holes in split_polygons(rings):
        area += abs(ring_area_signed(outer)) - sum(abs(ring_area_signed(h)) for h in holes)
    return area


def point_in_rings(rings, x, y):
    """Inside a (multi)polygon with holes: the even-odd rule over all its rings."""
    inside = False
    for ring in rings:
        if ring_contains(ring, x, y):
            inside = not inside
    return inside


def rings_bbox(parts):
    xs = [p[0] for part in parts for p in part]
    ys = [p[1] for part in parts for p in part]
    return (min(xs), min(ys), max(xs), max(ys))


def boxes_overlap(a, b):
    return not (a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1])
