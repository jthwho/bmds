# @file      draw.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's drawing (devplan/luckyduck.md §3): pictures, and shapes as signed distances (negative
# inside, in pixels), so an edge's coverage is one evaluation (0.5 - distance) and an outline is
# the shape grown. Python's standard library only.

import math
import struct
import zlib


class Picture:
    """RGBA pixels, clear to start."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.px = [[0, 0, 0, 0] for _ in range(width * height)]

    def get(self, x, y):
        return self.px[y * self.width + x]

    def set(self, x, y, rgb, a=255):
        if 0 <= x < self.width and 0 <= y < self.height:
            self.px[y * self.width + x] = [rgb[0], rgb[1], rgb[2], a]

    def blend(self, x, y, rgb, cover):
        """Paints @p rgb over the pixel by @p cover (0-1); a clear pixel takes the colour and the cover."""
        if cover <= 0.0:
            return
        p = self.px[y * self.width + x]
        if cover >= 1.0 or p[3] == 0:
            p[0], p[1], p[2] = rgb[0], rgb[1], rgb[2]
            p[3] = 255 if cover >= 1.0 else max(p[3], int(255 * cover))
            return
        a = p[3] / 255.0
        out_a = cover + a * (1 - cover)
        for i in range(3):
            p[i] = int(round((rgb[i] * cover + p[i] * a * (1 - cover)) / out_a))
        p[3] = int(round(255 * out_a))

    def paste(self, other, ox, oy):
        for y in range(other.height):
            row = (oy + y) * self.width + ox
            for x in range(other.width):
                self.px[row + x] = list(other.px[y * other.width + x])

    def over(self, other, ox, oy):
        """Draws @p other's opaque pixels over this one's (half covered and up)."""
        for y in range(other.height):
            for x in range(other.width):
                p = other.px[y * other.width + x]
                if p[3] >= 128 and 0 <= ox + x < self.width and 0 <= oy + y < self.height:
                    self.px[(oy + y) * self.width + ox + x] = list(p)

    def png(self, path):
        raw = bytearray()
        for y in range(self.height):
            raw.append(0)
            for x in range(self.width):
                raw += bytes(self.px[y * self.width + x])

        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
            f.write(chunk(b"IEND", b""))


# ---- Shapes: a signed distance and the box it can be inside of

class S:
    __slots__ = ("f", "box")

    def __init__(self, f, box):
        self.f = f
        self.box = box

    def __call__(self, x, y):
        return self.f(x, y)

    def grow(self, o):
        f = self.f
        x0, y0, x1, y1 = self.box
        return S(lambda x, y: f(x, y) - o, (x0 - o, y0 - o, x1 + o, y1 + o))

    def __or__(self, other):
        a, b = self.f, other.f
        return S(lambda x, y: min(a(x, y), b(x, y)), union_box(self.box, other.box))

    def __and__(self, other):
        a, b = self.f, other.f
        return S(lambda x, y: max(a(x, y), b(x, y)), self.box)

    def __sub__(self, other):
        a, b = self.f, other.f
        return S(lambda x, y: max(a(x, y), -b(x, y)), self.box)

    def at(self, dx=0.0, dy=0.0, turn=0.0, scale=1.0, cx=0.0, cy=0.0, mirror=False):
        """Moved: mirrored across x = @p cx if asked, scaled and turned by @p turn degrees
        (clockwise, y down) about (@p cx, @p cy), then moved by (@p dx, @p dy)."""
        f = self.f
        c, n = math.cos(math.radians(turn)), math.sin(math.radians(turn))

        def local(x, y):
            x, y = x - dx - cx, y - dy - cy
            u, v = (x * c + y * n) / scale, (-x * n + y * c) / scale
            return (cx - u if mirror else cx + u), cy + v

        def g(x, y):
            u, v = local(x, y)
            return f(u, v) * scale

        x0, y0, x1, y1 = self.box
        pts = []
        for px, py in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
            u = (cx - (px - cx) if mirror else px) - cx
            v = py - cy
            u, v = u * scale, v * scale
            pts.append((cx + u * c - v * n + dx, cy + u * n + v * c + dy))
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return S(g, (min(xs), min(ys), max(xs), max(ys)))


def union_box(a, b):
    return (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))


def union(*shapes):
    out = shapes[0]
    for s in shapes[1:]:
        out = out | s
    return out


def circle(cx, cy, r):
    return S(lambda x, y: math.hypot(x - cx, y - cy) - r, (cx - r, cy - r, cx + r, cy + r))


def ellipse(cx, cy, rx, ry):
    """An ellipse's distance, near enough at its edge (Quilez's: the radial one over its gradient)."""

    def f(x, y):
        u, v = (x - cx) / rx, (y - cy) / ry
        k0 = math.hypot(u, v)
        if k0 == 0:
            return -min(rx, ry)
        k1 = math.hypot(u / rx, v / ry)
        return k0 * (k0 - 1.0) / k1

    return S(f, (cx - rx, cy - ry, cx + rx, cy + ry))


def box(x0, y0, x1, y1, r=0.0):
    """A rectangle, its corners rounded by @p r."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2

    def f(x, y):
        qx = abs(x - cx) - (hw - r)
        qy = abs(y - cy) - (hh - r)
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

    return S(f, (x0, y0, x1, y1))


def _segment_distance(x, y, ax, ay, bx, by):
    px, py, ex, ey = x - ax, y - ay, bx - ax, by - ay
    d = ex * ex + ey * ey
    t = 0.0 if d == 0 else max(0.0, min(1.0, (px * ex + py * ey) / d))
    return math.hypot(px - ex * t, py - ey * t)


def capsule(ax, ay, bx, by, r):
    """A line from (ax, ay) to (bx, by), @p r either side, its ends round."""
    return S(lambda x, y: _segment_distance(x, y, ax, ay, bx, by) - r,
             (min(ax, bx) - r, min(ay, by) - r, max(ax, bx) + r, max(ay, by) + r))


def stroke(points, r):
    """A polyline @p r either side, its joints and ends round."""
    segs = list(zip(points, points[1:])) or [(points[0], points[0])]

    def f(x, y):
        return min(_segment_distance(x, y, a[0], a[1], b[0], b[1]) for a, b in segs) - r

    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return S(f, (min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r))


def polygon(points):
    """A polygon's exact distance (its edges', signed by the even-odd rule)."""
    n = len(points)

    def f(x, y):
        d = float("inf")
        inside = False
        j = n - 1
        for i in range(n):
            xi, yi = points[i]
            xj, yj = points[j]
            d = min(d, _segment_distance(x, y, xj, yj, xi, yi))
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
            j = i
        return -d if inside else d

    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return S(f, (min(xs), min(ys), max(xs), max(ys)))


def arc_points(cx, cy, rx, ry, a0, a1, steps=None):
    """Points along an ellipse from @p a0 to @p a1 degrees (0 right, 90 down)."""
    if steps is None:
        steps = max(4, int(abs(a1 - a0) / 10))
    out = []
    for k in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * k / steps)
        out.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return out


# ---- Painting

def paint(pic, shape, rgb, clip=None):
    """Paints @p shape in @p rgb (a colour, or a function of x and y giving one), its edge
    antialiased; @p clip: a box to stay in."""
    x0, y0, x1, y1 = shape.box
    if clip is not None:
        x0, y0, x1, y1 = max(x0, clip[0]), max(y0, clip[1]), min(x1, clip[2]), min(y1, clip[3])
    f = shape.f
    colour = rgb if callable(rgb) else None
    for py in range(max(0, int(math.floor(y0 - 1))), min(pic.height, int(math.ceil(y1 + 1)))):
        for px in range(max(0, int(math.floor(x0 - 1))), min(pic.width, int(math.ceil(x1 + 1)))):
            d = f(px + 0.5, py + 0.5)
            if d >= 0.5:
                continue
            cover = 1.0 if d <= -0.5 else 0.5 - d
            pic.blend(px, py, colour(px + 0.5, py + 0.5) if colour else rgb, cover)


def outlined(pic, shape, rgb, line, width):
    """@p shape with an outline of @p width in @p line around it."""
    paint(pic, shape.grow(width), line)
    paint(pic, shape, rgb)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def vertical(top, bottom, y0, y1):
    """A colour going from @p top at @p y0 to @p bottom at @p y1."""
    return lambda x, y: mix(top, bottom, (y - y0) / (y1 - y0))


def shade(rgb, k):
    return tuple(max(0, min(255, int(round(c * k)))) for c in rgb)


def hexrgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def lit(shape, base, light=(-0.55, -0.83), glow=0.22, rim=0.32, edge=5.0, centre=None, reach=None):
    """A colour for @p shape: @p base lit from @p light's way (a unit vector towards the light,
    y down): brighter towards it by @p glow, darker on the far side's edge by @p rim (within
    @p edge pixels of the edge), so a flat shape reads as round."""
    x0, y0, x1, y1 = shape.box
    cx, cy = centre if centre is not None else ((x0 + x1) / 2, (y0 + y1) / 2)
    r = reach if reach is not None else max(1.0, max(x1 - x0, y1 - y0) / 2)
    f = shape.f
    lx, ly = light

    def colour(x, y):
        u, v = (x - cx) / r, (y - cy) / r
        toward = max(-1.0, min(1.0, u * lx + v * ly))
        k = 1.0 + glow * toward
        d = f(x, y)
        near = max(0.0, min(1.0, 1.0 + d / edge))
        k -= rim * near * (0.5 - 0.5 * toward)
        return shade(base, k)

    return colour
