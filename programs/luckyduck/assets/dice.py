# @file      dice.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck Dice's pieces (devplan/luckyduck.md §18): the dice seen from above (each face, at two
# sizes), the dice tumbling (a rounded cube drawn turned, its faces lit by their angle to the
# light), a die's shadow, and the feathers that fall when a Lucky Duck is rolled.

import math

from draw import Picture, box, circle, ellipse, lit, mix, paint, polygon, shade
from cards import duck_silhouette

OUTLINE = (0x2A, 0x1C, 0x10)
IVORY = (0xFA, 0xF4, 0xE2)
PIP = (0x22, 0x24, 0x3C)
RED = (0xC8, 0x1C, 0x2C)
GOLD = (0xFF, 0xD0, 0x40)

# Where each face's pips are, from -1 to 1 across the face.
PIPS = {1: [(0, 0)], 2: [(-1, -1), (1, 1)], 3: [(-1, -1), (0, 0), (1, 1)], 4: [(-1, -1), (1, -1), (-1, 1), (1, 1)],
        5: [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)], 6: [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)]}

FLAT = 56  # a die at rest: its picture's side
SMALL = 26  # the scorecard's
TUMBLE = 80  # a die in the air: room for its corners however it is turned
TUMBLE_FRAMES = 8


def face(value, side=FLAT):
    """A die from straight above, @p value up: an ivory rounded square with a bevel, its pips
    drilled (each darker on its lit side); the one a red pip with a gold duck in it."""
    pic = Picture(side, side)
    k = side / FLAT
    m = 4 * k
    body = box(m, m, side - m, side - m, 10 * k)
    paint(pic, body.grow(1.4 * k), OUTLINE)
    paint(pic, body, lit(body, shade(IVORY, 0.9), glow=0.12, rim=0.35, edge=7 * k))
    top = box(m + 4 * k, m + 4 * k, side - m - 4 * k, side - m - 4 * k, 7 * k)
    paint(pic, top, lit(top, IVORY, glow=0.1, rim=0.08, edge=6 * k))
    c = side / 2
    step = 12.5 * k
    if value == 1:
        r = 9.5 * k
        paint(pic, circle(c, c, r + 0.8 * k), shade(RED, 0.6))
        paint(pic, circle(c + 0.6 * k, c + 0.6 * k, r), RED)
        if side >= FLAT:
            paint(pic, duck_silhouette(c - 0.5 * k, c - 1.5 * k, 4.6 * k), GOLD)
        return pic
    r = 4.6 * k
    for u, v in PIPS[value]:
        x, y = c + u * step, c + v * step
        paint(pic, circle(x, y, r), shade(PIP, 0.7))
        paint(pic, circle(x + 0.7 * k, y + 0.7 * k, r - 0.6 * k), PIP)
    return pic


# ---- Tumbling: a rounded cube, turned

# Each face: its value, its normal, and its across and down directions.
FACES = [
    (1, (0, 0, 1), (1, 0, 0), (0, 1, 0)),
    (6, (0, 0, -1), (-1, 0, 0), (0, 1, 0)),
    (3, (1, 0, 0), (0, 0, -1), (0, 1, 0)),
    (4, (-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    (2, (0, 1, 0), (1, 0, 0), (0, 0, -1)),
    (5, (0, -1, 0), (1, 0, 0), (0, 0, 1)),
]
LIGHT = (-0.45, -0.6, 0.66)


def rotate(p, ax, ay, az):
    """@p p turned about x, then y, then z (degrees)."""
    x, y, z = p
    a, b, c = math.radians(ax), math.radians(ay), math.radians(az)
    y, z = y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a)
    x, z = x * math.cos(b) + z * math.sin(b), -x * math.sin(b) + z * math.cos(b)
    x, y = x * math.cos(c) - y * math.sin(c), x * math.sin(c) + y * math.cos(c)
    return x, y, z


def rounded_square(r, steps=4):
    """A rounded square's outline from -1 to 1, its corners' radius @p r."""
    pts = []
    for cx, cy, a0 in ((1 - r, 1 - r, 0), (-1 + r, 1 - r, 90), (-1 + r, -1 + r, 180), (1 - r, -1 + r, 270)):
        for s in range(steps + 1):
            a = math.radians(a0 + 90 * s / steps)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def tumble(frame):
    """A die in the air: the cube turned a step further about its across axis each frame (a
    quarter turn over the frames, after which it looks the same), tilted to show its sides."""
    pic = Picture(TUMBLE, TUMBLE)
    h = 22.0
    c = TUMBLE / 2
    angles = (frame * 90.0 / TUMBLE_FRAMES, 24.0, 8.0)
    lx, ly, lz = LIGHT
    ll = math.sqrt(lx * lx + ly * ly + lz * lz)
    seen = []
    for value, n, u, v in FACES:
        nn = rotate(n, *angles)
        if nn[2] <= 0.02:
            continue
        seen.append((value, n, u, v, nn))

    def on_face(n, u, v, a, b, inset=1.0):
        p = tuple(h * (n[i] + inset * (a * u[i] + b * v[i])) for i in range(3))
        x, y, _ = rotate(p, *angles)
        return c + x, c + y

    # The silhouette: every face seen, whole, outlined; then the edges' colour under the faces.
    whole = None
    for value, n, u, v, nn in seen:
        quad = polygon([on_face(n, u, v, a, b) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
        whole = quad if whole is None else whole | quad
    paint(pic, whole.grow(1.4), OUTLINE)
    paint(pic, whole, shade(IVORY, 0.7))
    for value, n, u, v, nn in seen:
        k = 0.74 + 0.3 * max(0.0, (nn[0] * lx + nn[1] * ly + nn[2] * lz) / ll)
        outline = polygon([on_face(n, u, v, a, b, 0.86) for a, b in rounded_square(0.28)])
        paint(pic, outline, shade(IVORY, k))
        if nn[2] < 0.25:
            continue  # nearly edge on: its pips would be slivers
        for pu, pv in PIPS[value]:
            r = 0.3 if value == 1 else 0.19
            ring = [on_face(n, u, v, pu * 0.5 + r * math.cos(t), pv * 0.5 + r * math.sin(t))
                    for t in (i * math.pi / 10 for i in range(20))]
            paint(pic, polygon(ring), shade(RED if value == 1 else PIP, 0.75 + 0.25 * k))
    return pic


def shadow():
    """Under a die (a shade's shape: only its pixels count)."""
    pic = Picture(FLAT, FLAT)
    paint(pic, box(5, 5, FLAT - 5, FLAT - 5, 13), (0, 0, 0))
    return pic


def feather(kind):
    """A feather falling: a curved vane about its quill, white or gold."""
    pic = Picture(28, 14)
    rgb = (0xFF, 0xFF, 0xFA) if kind == 0 else (0xFF, 0xD8, 0x50)
    vane = ellipse(15, 7, 11.5, 5).at(turn=-6, cx=15, cy=7) - ellipse(22, 2, 5, 3)
    paint(pic, vane.grow(1.1), OUTLINE)
    paint(pic, vane, lit(vane, rgb, glow=0.2, rim=0.3, edge=3))
    paint(pic, polygon([(2, 8.4), (24, 6.2), (24, 7.2), (2, 9.2)]), shade(rgb, 0.7))
    for k in range(4):
        x = 9 + k * 4
        paint(pic, polygon([(x, 7.4), (x + 3, 3.4), (x + 3.6, 3.8), (x + 0.8, 7.6)]), mix(rgb, OUTLINE, 0.25))
    return pic
