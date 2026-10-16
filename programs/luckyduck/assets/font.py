# @file      font.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's headline letters (devplan/luckyduck.md §3): our own stroke font, capitals 10 units
# tall (0 the top, 10 the baseline), each glyph strokes (polylines; a single point is a dot) and an
# advance. Drawn with round ends at any size, filled, outlined and shadowed.

from draw import arc_points, stroke, union, paint

CAP = 10.0


def A(*points):
    return list(points)


def arc(cx, cy, rx, ry, a0, a1):
    return arc_points(cx, cy, rx, ry, a0, a1)


GLYPHS = {
    "A": (6, [A((0, 10), (3, 0), (6, 10)), A((1.1, 6.6), (4.9, 6.6))]),
    "B": (6, [A((0, 5), (0, 0), (3.2, 0)) + arc(3.2, 2.5, 2.3, 2.5, -90, 90) + A((0, 5), (3.6, 5)),
              arc(3.6, 7.5, 2.4, 2.5, -90, 90) + A((0, 10), (0, 5))]),
    "C": (6.4, [arc(3.6, 5, 3.6, 5, -42, -318)]),
    "D": (6.2, [A((1.5, 10), (0, 10), (0, 0), (1.5, 0)) + arc(1.5, 5, 4.7, 5, -90, 90)]),
    "E": (5, [A((5, 0), (0, 0), (0, 10), (5, 10)), A((0, 5), (4, 5))]),
    "F": (5, [A((5, 0), (0, 0), (0, 10)), A((0, 5), (4, 5))]),
    "G": (6.8, [arc(3.6, 5, 3.6, 5, -42, -360) + A((7.2, 5.4), (4, 5.4))]),
    "H": (5.6, [A((0, 0), (0, 10)), A((5.6, 0), (5.6, 10)), A((0, 5), (5.6, 5))]),
    "I": (0, [A((0, 0), (0, 10))]),
    "J": (4.4, [A((4.4, 0), (4.4, 7.3)) + arc(2.2, 7.3, 2.2, 2.7, 0, 180)]),
    "K": (5.6, [A((0, 0), (0, 10)), A((5.4, 0), (0, 6.2)), A((1.8, 4.2), (5.8, 10))]),
    "L": (5, [A((0, 0), (0, 10), (5, 10))]),
    "M": (7.2, [A((0, 10), (0, 0), (3.6, 7), (7.2, 0), (7.2, 10))]),
    "N": (5.8, [A((0, 10), (0, 0), (5.8, 10), (5.8, 0))]),
    "O": (7.2, [arc(3.6, 5, 3.6, 5, 0, 360)]),
    "P": (6, [A((0, 10), (0, 0), (3.2, 0)) + arc(3.2, 2.75, 2.8, 2.75, -90, 90) + A((0, 5.5))]),
    "Q": (7.2, [arc(3.6, 5, 3.6, 5, 0, 360), A((4.4, 7), (7.4, 10.4))]),
    "R": (6, [A((0, 10), (0, 0), (3.2, 0)) + arc(3.2, 2.75, 2.8, 2.75, -90, 90) + A((0, 5.5)),
              A((3, 5.5), (6, 10))]),
    "S": (6, [arc(3, 2.7, 2.9, 2.7, -25, -270) + arc(3, 7.6, 3.1, 2.4, -90, 155)]),
    "T": (6, [A((0, 0), (6, 0)), A((3, 0), (3, 10))]),
    "U": (6, [A((0, 0), (0, 6.6)) + arc(3, 6.6, 3, 3.4, 180, 0) + A((6, 0))]),
    "V": (6.2, [A((0, 0), (3.1, 10), (6.2, 0))]),
    "W": (8.4, [A((0, 0), (2.1, 10), (4.2, 3), (6.3, 10), (8.4, 0))]),
    "X": (5.8, [A((0, 0), (5.8, 10)), A((5.8, 0), (0, 10))]),
    "Y": (6, [A((0, 0), (3, 5.2), (6, 0)), A((3, 5.2), (3, 10))]),
    "Z": (5.6, [A((0, 0), (5.6, 0), (0, 10), (5.6, 10))]),
    "0": (6, [arc(3, 5, 3, 5, 0, 360)]),
    "1": (4.4, [A((0.4, 2), (2.6, 0), (2.6, 10)), A((0.4, 10), (4.8, 10))]),
    "2": (6, [arc(3, 3, 3, 3, -165, 20) + A((0, 10), (6, 10))]),
    "3": (6, [arc(3, 2.6, 2.7, 2.6, -155, 90) + arc(3, 7.5, 3, 2.5, -90, 155)]),
    "4": (6, [A((4.6, 10), (4.6, 0), (0, 7), (6.2, 7))]),
    "5": (6, [A((5.6, 0), (0.8, 0), (0.4, 4.6)) + arc(3, 7, 3, 3, -125, 155)]),
    "6": (6, [arc(3, 7, 3, 3, 0, 360), arc(6.6, 7.4, 6.6, 7.4, -100, -180)]),
    "7": (6, [A((0, 0), (6, 0), (2.2, 10))]),
    "8": (6, [arc(3, 2.6, 2.6, 2.6, 0, 360), arc(3, 7.4, 3, 2.6, 0, 360)]),
    "9": (6, [arc(3, 3, 3, 3, 0, 360), arc(-0.6, 2.6, 6.6, 7.4, 0, 80)]),
    "$": (6, [arc(3, 2.9, 2.9, 2.2, -25, -270) + arc(3, 7.1, 3.1, 2.2, -90, 155), A((3, -1), (3, 11))]),
    "!": (0, [A((0, 0), (0, 6.6)), A((0, 10))]),
    "?": (5.6, [arc(2.8, 2.8, 2.8, 2.8, -170, 50) + A((2.8, 7)), A((2.8, 10))]),
    ".": (0, [A((0, 10))]),
    ",": (0.8, [A((0.6, 9.6), (-0.2, 11.6))]),
    ":": (0, [A((0, 3.6)), A((0, 10))]),
    "+": (5, [A((0, 5.2), (5, 5.2)), A((2.5, 2.7), (2.5, 7.7))]),
    "-": (3.6, [A((0, 5.6), (3.6, 5.6))]),
    "'": (0, [A((0, 0), (0, 2.8))]),
    "/": (4, [A((0, 10.5), (4, -0.5))]),
    "%": (7, [arc(1.5, 2, 1.5, 2, 0, 360), A((6.5, -0.2), (0.5, 10.2)), arc(5.5, 8, 1.5, 2, 0, 360)]),
    "x": (4.6, [A((0, 3.4), (4.6, 10)), A((4.6, 3.4), (0, 10))]),
    " ": (2.6, []),
}

CHARS = "".join(sorted(GLYPHS))

# Kerning worked out by build.py before drawing (by stroke weight): text drawn by the art itself
# takes the nearest's.
KERN = {}


def kern_for(weight):
    if not KERN:
        return None
    return KERN[min(KERN, key=lambda w: abs(w - weight))]


def glyph_shape(ch, x, y, size, weight, squeeze=1.0):
    """@p ch's strokes as one shape, its top-left at (@p x, @p y), capitals @p size pixels tall,
    strokes @p weight units wide (in the font's 10 a capital); None for a space."""
    k = size / CAP
    parts = [stroke([(x + px * k * squeeze, y + py * k) for px, py in s], weight * k / 2) for s in GLYPHS[ch][1]]
    return union(*parts) if parts else None


def advance(ch, size, weight, squeeze=1.0):
    return (GLYPHS[ch][0] * squeeze + weight) * size / CAP


def text_width(text, size, weight, spacing=1.2, squeeze=1.0):
    if not text:
        return 0.0
    return sum(advance(c, size, weight, squeeze) for c in text) + spacing * size / CAP * (len(text) - 1)


def draw_text(pic, text, x, y, size, weight, rgb, outline=None, outline_width=0.0, shadow=None, spacing=1.2,
              squeeze=1.0, kern=None):
    """Draws @p text from (@p x, @p y) (its top-left: the strokes' round ends reach a half weight
    beyond). @p rgb: a colour or a function of x and y."""
    pen = x + weight * size / CAP / 2
    shapes = []
    for i, c in enumerate(text):
        s = glyph_shape(c, pen, y, size, weight, squeeze)
        if s is not None:
            shapes.append(s)
        pen += advance(c, size, weight, squeeze) + spacing * size / CAP
        if kern is None:
            kern = kern_for(weight)
        if kern is not None and i + 1 < len(text):
            pen += kern.get((c, text[i + 1]), 0.0) * size / CAP
    # Each layer under the next, glyph by glyph (one shape of them all would cost every glyph's
    # strokes at every pixel).
    if shadow is not None:
        dx, dy, srgb = shadow
        for s in shapes:
            paint(pic, s.grow(outline_width).at(dx, dy), srgb)
    if outline is not None:
        for s in shapes:
            paint(pic, s.grow(outline_width), outline)
    for s in shapes:
        paint(pic, s, rgb)


# ---- Kerning, worked out from the shapes

def profile(ch, weight):
    """@p ch's ink, row by row (rows a quarter unit apart from -1.5 to 12): its leftmost and
    rightmost x in units from the pen (None where a row has none)."""
    shape = glyph_shape(ch, weight / 2, 0, CAP, weight)
    if shape is None:
        return None
    rows = []
    x0, _, x1, _ = shape.box
    for k in range(55):
        y = -1.5 + k * 0.25
        left = right = None
        x = x0 - 0.2
        while x <= x1 + 0.2:
            if shape(x, y) <= 0:
                if left is None:
                    left = x
                right = x
            x += 0.1
        rows.append((left, right))
    return rows


def kerning(chars, weight, spacing=1.2):
    """Pair adjustments in units (negative: closer) for @p chars at @p weight: each pair moved so
    its gap (mostly its narrowest rows, partly the white beside them) matches "HH"'s. A pair is
    only drawn closer than it would be, or a little apart where its glyphs would nearly touch."""
    prof = {c: profile(c, weight) for c in chars if c != " "}

    def gap(a, b):
        pa, pb = prof.get(a), prof.get(b)
        if pa is None or pb is None:
            return None
        offset = GLYPHS[a][0] + weight + spacing
        d = []
        for k in range(len(pa)):
            # The row and its neighbours a unit either way: a diagonal reaches across rows.
            ra = [pa[j][1] for j in range(max(0, k - 4), min(len(pa), k + 5)) if pa[j][1] is not None]
            if pb[k][0] is None or not ra:
                continue
            d.append(offset + pb[k][0] - max(ra))
        if not d:
            return None
        least = min(d)
        near = [min(v, least + 3.0) for v in d]
        return 0.6 * least + 0.4 * sum(near) / len(near)

    ref = gap("H", "H")
    out = {}
    for a in chars:
        for b in chars:
            g = gap(a, b)
            if g is None:
                continue
            k = max(-3.0, min(0.6, ref - g))
            if abs(k) >= 0.25:
                out[(a, b)] = k
    return out
