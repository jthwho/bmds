# @file      cards.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's cards (devplan/luckyduck.md §3): 100×140, pips as printed cards, the courts
# double-headed ducks (the King a mallard with a crown, the Queen a white duck with a tiara, the
# Jack a yellow one in a feathered cap), the Ace of spades with a duck in it, two backs with Lucky's
# emblem, a jester Joker, and the places on the felt.

import math

from draw import (Picture, arc_points, box, capsule, circle, ellipse, lit, mix, paint, polygon, shade, stroke, union)
from ducks import STYLES, Painter, draw_duck, draw_head, pose
from font import draw_text, text_width

W, H = 100, 140
RADIUS = 7.0

PAPER = (0xFF, 0xFE, 0xF8)
EDGE = (0x40, 0x40, 0x48)
RED = (0xD0, 0x14, 0x20)
BLACK = (0x18, 0x18, 0x20)
GOLD = (0xE8, 0xB4, 0x20)
GOLD_DARK = (0x9A, 0x6C, 0x10)
BLUE = (0x26, 0x46, 0xA8)
BLUE_DARK = (0x16, 0x26, 0x68)
CRIMSON = (0xC0, 0x1C, 0x28)
CRIMSON_DARK = (0x78, 0x0C, 0x14)
CREAM = (0xFF, 0xF4, 0xD8)
FELT_LINE = (0x6C, 0xC0, 0x84)
GREEN_DARK = (0x0E, 0x50, 0x2A)
GREEN_LIGHT = (0x3C, 0x9C, 0x5C)
STEEL = (0xB8, 0xC0, 0xCC)
WOOD = (0x6A, 0x40, 0x18)
OUTLINE_C = (0x3A, 0x24, 0x10)

CLUBS, DIAMONDS, HEARTS, SPADES = range(4)
RANKS = ["A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"]


# ---- Suits in a unit box (-1..1, y down), placed by suit()

def _spade_body():
    side = []
    for k in range(13):
        v = -1.0 + 1.16 * k / 12
        side.append((0.94 * ((v + 1.0) / 1.16) ** 0.7, v))
    pts = [(-u, v) for u, v in reversed(side)] + side
    return union(polygon(pts), circle(-0.47, 0.16, 0.47), circle(0.47, 0.16, 0.47))


def _stem():
    return polygon([(-0.07, 0.25), (0.07, 0.25), (0.36, 1.0), (-0.36, 1.0)])


UNIT = {
    HEARTS: union(circle(-0.5, -0.38, 0.53), circle(0.5, -0.38, 0.53),
                  polygon([(-1.02, -0.28), (1.02, -0.28), (0, 1.0)])),
    DIAMONDS: polygon([(0, -1), (0.78, 0), (0, 1), (-0.78, 0)]),
    SPADES: union(_spade_body(), _stem()),
    CLUBS: union(circle(0, -0.5, 0.4), circle(-0.46, 0.12, 0.4), circle(0.46, 0.12, 0.4), circle(0, -0.05, 0.25),
                 _stem()),
}


def ink(suit):
    return RED if suit in (DIAMONDS, HEARTS) else BLACK


def suit_shape(suit, cx, cy, h, turned=False):
    """Suit @p suit @p h pixels tall centred on (cx, cy); @p turned: upside down."""
    return UNIT[suit].at(cx, cy, 180 if turned else 0, h / 2)


def pip(pic, suit, cx, cy, h, turned=False, rgb=None, flat=False):
    """A suit's mark, lit a little (flat: one colour, for small or printed marks)."""
    shape = suit_shape(suit, cx, cy, h, turned)
    colour = rgb or ink(suit)
    paint(pic, shape, colour if flat or h < 12 else lit(shape, colour, glow=0.18, rim=0.22, edge=h / 6))


# ---- Pictures turned half way (the lower halves of a card)

def turned_copy(pic, x0, y0, x1, y1):
    """Copies the region turned half way round into its place opposite."""
    for y in range(y0, y1):
        for x in range(x0, x1):
            pic.px[(H - 1 - y) * W + (W - 1 - x)] = list(pic.get(x, y))


def blank(face=PAPER):
    pic = Picture(W, H)
    paint(pic, box(0, 0, W, H, RADIUS), EDGE)
    paint(pic, box(1.2, 1.2, W - 1.2, H - 1.2, RADIUS - 1.2), face)
    return pic


def corner(pic, suit, rank):
    """The rank and the suit in the top-left corner, and turned in the bottom-right."""
    label = RANKS[rank]
    squeeze = 0.62 if len(label) > 1 else 1.0
    size, weight = 15.0, 2.3
    w = text_width(label, size, weight, spacing=0.6, squeeze=squeeze)
    draw_text(pic, label, 12.5 - w / 2, 7, size, weight, ink(suit), spacing=0.6, squeeze=squeeze)
    pip(pic, suit, 12.5, 33, 13)
    turned_copy(pic, 0, 0, 24, 42)


# Number cards' pips: columns and rows (0 the top row, 1 the bottom), as printed.
L, C, R = 31.0, 50.0, 69.0
TOP, SPAN = 25.0, 90.0
LAYOUTS = {
    2: [(C, 0), (C, 1)],
    3: [(C, 0), (C, 0.5), (C, 1)],
    4: [(L, 0), (R, 0), (L, 1), (R, 1)],
    5: [(L, 0), (R, 0), (C, 0.5), (L, 1), (R, 1)],
    6: [(L, 0), (R, 0), (L, 0.5), (R, 0.5), (L, 1), (R, 1)],
    7: [(L, 0), (R, 0), (C, 0.25), (L, 0.5), (R, 0.5), (L, 1), (R, 1)],
    8: [(L, 0), (R, 0), (C, 0.25), (L, 0.5), (R, 0.5), (C, 0.75), (L, 1), (R, 1)],
    9: [(L, 0), (R, 0), (L, 1 / 3), (R, 1 / 3), (C, 0.5), (L, 2 / 3), (R, 2 / 3), (L, 1), (R, 1)],
    10: [(L, 0), (R, 0), (C, 1 / 6), (L, 1 / 3), (R, 1 / 3), (L, 2 / 3), (R, 2 / 3), (C, 5 / 6), (L, 1), (R, 1)],
}


def duck_silhouette(cx, cy, r):
    """A duck's head facing left, @p r its head's radius: the emblem's shape."""
    head = circle(86, 46, 21) | ellipse(80, 64, 12, 14)
    beak = polygon([(100, 43.5), (119, 46.5), (123.5, 50.5), (119.5, 54.5), (100, 55)])
    return (head | beak).at(cx - 86, cy - 46, 0, r / 21.0, 86, 46, mirror=True)


def number_card(suit, rank):
    pic = blank()
    corner(pic, suit, rank)
    if rank == 0:
        if suit == SPADES:  # the Ace of spades: ringed in gold, a duck in its spade
            paint(pic, circle(50, 70, 36), GOLD)
            paint(pic, circle(50, 70, 33), PAPER)
            for k in range(16):
                a = k * math.pi / 8
                paint(pic, circle(50 + 34.5 * math.cos(a), 70 + 34.5 * math.sin(a), 1.4), GOLD_DARK)
            pip(pic, suit, 50, 68, 50)
            paint(pic, duck_silhouette(49, 62, 6.5), GOLD)
        else:
            pip(pic, suit, 50, 70, 40)
        return pic
    for x, f in LAYOUTS[rank + 1]:
        y = TOP + f * SPAN
        pip(pic, suit, x, y, 19, turned=y > 70.0)
    return pic


# The courts: a duck in the frame's upper half, turned for the lower (double-headed).
FX0, FY0, FX1, FY1 = 18, 14, 82, 126


ERMINE = (0xFC, 0xFA, 0xF2)
ERMINE_SPOT = (0x22, 0x1C, 0x18)


def court_half(suit, rank):
    """The frame's upper half: a patterned ground, the robe, the duck's head, what it wears and
    holds, the suit in a medallion on its chest."""
    half = Picture(W, H)
    red = suit in (DIAMONDS, HEARTS)
    robe, robe_dark = (CRIMSON, CRIMSON_DARK) if red else (BLUE, BLUE_DARK)
    trim, trim_dark = (BLUE, BLUE_DARK) if red else (CRIMSON, CRIMSON_DARK)
    clip = (FX0, FY0, FX1, 70)
    # The ground: cream with a faint lattice.
    paint(half, box(FX0, FY0, FX1, 70), CREAM)
    for k in range(-8, 12):
        paint(half, stroke([(FX0 + k * 8, FY0), (FX0 + k * 8 + 56, 70)], 0.35), mix(CREAM, GOLD, 0.35), clip)
        paint(half, stroke([(FX0 + k * 8 + 56, FY0), (FX0 + k * 8, 70)], 0.35), mix(CREAM, GOLD, 0.35), clip)
    style = {12: "mallard", 11: "pekin", 10: "duckling"}[rank]
    # The robe over the shoulders, its pattern, its trim.
    cape = polygon([(FX0 - 1, 71), (20, 58), (33, 51), (50, 49), (67, 51), (80, 58), (FX1 + 1, 71)])
    paint(half, cape.grow(1.2), robe_dark, clip)
    paint(half, cape, lit(cape, robe, glow=0.25, rim=0.3, edge=6), clip)
    for k in range(-6, 8):  # a gold diamond pattern on it
        paint(half, stroke([(20 + k * 7, 72), (34 + k * 7, 50)], 0.4) & cape, mix(robe, GOLD, 0.55), clip)
        paint(half, stroke([(34 + k * 7, 72), (20 + k * 7, 50)], 0.4) & cape, mix(robe, GOLD, 0.55), clip)
    if rank == 10:  # the Jack's tunic: stripes down the front
        for x in (40, 46, 54, 60):
            paint(half, box(x, 52, x + 3, 71) & cape, trim, clip)
    else:  # ermine down the front and round the collar
        band = box(42, 50, 58, 71) | ellipse(50, 51, 17, 5)
        paint(half, band.grow(0.8), OUTLINE_C, clip)
        paint(half, band, ERMINE, clip)
        for x, y in ((45, 56), (53, 56), (49, 61), (45, 66), (53, 66), (37, 51), (63, 51), (49, 52)):
            paint(half, polygon([(x, y - 1.6), (x + 1, y + 1.2), (x - 1, y + 1.2)]), ERMINE_SPOT, clip)
    # The medallion with the suit, on a chain.
    paint(half, stroke(arc_points(50, 50, 13, 9, 20, 160), 0.6), GOLD_DARK, clip)
    paint(half, circle(50, 62, 5.6), GOLD_DARK, clip)
    paint(half, circle(50, 62, 4.8), lit(circle(50, 62, 4.8), GOLD, glow=0.35, rim=0.3, edge=2), clip)
    pip(half, suit, 50, 62, 5.6, flat=True)
    # The head: facing left, its centre near (47, 40).
    p = Painter(half, 0, 0, 128 * 0.6, mirror=True, outline=1.9)
    p.aim(86, 46, 47, 40)
    draw_head(p, STYLES[style], pose(), neck=True)
    # What each wears on its head (local units, as the head's).
    if rank == 12:  # a crown: a band, five points with pearls, jewels
        crown = polygon([(68, 31), (66, 12), (73, 20), (79, 7), (86, 18), (93, 7), (99, 20), (106, 12), (104, 31)])
        p.part(crown, GOLD, 1.5, glow=0.35, rim=0.3)
        p.part(box(67, 26, 105, 32, 1.5), GOLD_DARK, 1.2, glow=0.3)
        for jx, col in ((74, (0xD0, 0x20, 0x30)), (86, (0x20, 0x60, 0xD0)), (98, (0x20, 0xA0, 0x50))):
            p.part(ellipse(jx, 29, 2.4, 2), col, 0.8, glow=0.4)
        for px_, py_ in ((66, 12), (79, 7), (93, 7), (106, 12)):
            p.part(circle(px_, py_, 2.2), (0xFF, 0xFC, 0xF0), 0.8, glow=0.3)
    elif rank == 11:  # a tiara with a pink jewel
        tiara = polygon([(72, 28), (75, 19), (80, 23), (86, 12), (92, 23), (97, 19), (100, 28)])
        p.part(tiara, GOLD, 1.3, glow=0.35)
        p.part(ellipse(86, 21, 2.6, 3), (0xE8, 0x50, 0x98), 0.8, glow=0.4)
        for jx in (77, 95):
            p.part(circle(jx, 24, 1.5), (0xFF, 0xFC, 0xF0), 0.6)
    else:  # a cap with a long feather
        p.part(polygon([(72, 23), (50, 3), (54, 1), (77, 18)]), GOLD, 1.3, glow=0.3)
        for k in range(4):
            p.fill(stroke([(58 - k * 2.5 + 8, 10 + k * 3), (60 - k * 2.5 + 8, 7 + k * 3)], 0.5), GOLD_DARK)
        p.part(ellipse(85, 26, 18, 8.5), trim, 1.5, glow=0.3)
        p.part(box(67, 26, 103, 31, 2), trim_dark, 1.2)
    # What each holds: the King a sceptre, the Queen a rose, the Jack a halberd.
    if rank == 12:
        rod = box(73.5, 22, 76.5, 66)
        paint(half, rod.grow(0.8), OUTLINE_C, clip)
        paint(half, rod, lit(rod, GOLD, glow=0.4, rim=0.3, edge=1.5), clip)
        paint(half, circle(75, 20, 4.4), OUTLINE_C, clip)
        paint(half, circle(75, 20, 3.6), lit(circle(75, 20, 3.6), GOLD, glow=0.4, rim=0.35, edge=2), clip)
        paint(half, stroke([(75, 13.5), (75, 17)], 0.7) | stroke([(73.2, 15), (76.8, 15)], 0.7), GOLD_DARK, clip)
    elif rank == 11:
        paint(half, capsule(26, 34, 27, 64, 0.9), (0x2C, 0x88, 0x3C), clip)
        paint(half, ellipse(29.5, 46, 3, 1.4).at(turn=-30, cx=29.5, cy=46), (0x2C, 0x88, 0x3C), clip)
        for r_, col in ((4.6, OUTLINE_C), (4.0, trim), (2.6, mix(trim, (255, 255, 255), 0.3)), (1.2, GOLD)):
            paint(half, circle(26, 31, r_), col, clip)
    else:
        shaft = box(74, 14, 76.5, 70)
        paint(half, shaft.grow(0.7), OUTLINE_C, clip)
        paint(half, shaft, WOOD, clip)
        blade = polygon([(76.5, 15), (83, 12), (84, 22), (83, 30), (76.5, 26)])
        paint(half, blade.grow(0.7), OUTLINE_C, clip)
        paint(half, blade, lit(blade, STEEL, glow=0.4, rim=0.3, edge=2), clip)
        paint(half, polygon([(75.2, 6), (77, 14), (73.5, 14)]), STEEL, clip)
    pip(half, suit, FX0 + 7, FY0 + 8, 10, flat=True)
    return half, robe_dark, trim_dark


def court_card(suit, rank):
    pic = blank()
    corner(pic, suit, rank)
    half, robe_dark, trim_dark = court_half(suit, rank)
    for y in range(FY0, 70):
        for x in range(FX0, FX1):
            p = half.get(x, y)
            pic.px[y * W + x] = list(p)
            pic.px[(H - 1 - y) * W + (W - 1 - x)] = list(p)
    # The frame: a double line; between the halves, a gold band with the suit's colour in dots.
    paint(pic, box(FX0 - 1.5, FY0 - 1.5, FX1 + 1.5, FY1 + 1.5) - box(FX0, FY0, FX1, FY1), robe_dark)
    paint(pic, box(FX0 + 1.5, FY0 + 1.5, FX1 - 1.5, FY1 - 1.5) - box(FX0 + 2.2, FY0 + 2.2, FX1 - 2.2, FY1 - 2.2),
          GOLD_DARK)
    paint(pic, box(FX0, 68, FX1, 72), GOLD)
    paint(pic, box(FX0, 67.4, FX1, 68) | box(FX0, 72, FX1, 72.6), GOLD_DARK)
    for x in range(FX0 + 4, FX1 - 2, 6):
        paint(pic, circle(x + 0.5, 70, 1.1), trim_dark)
    return pic


def clover(pic, cx, cy, r, rgb):
    """A four-leaf clover (for luck), @p r its leaves' reach."""
    leaves = union(*[circle(cx + math.cos(a) * r * 0.5, cy + math.sin(a) * r * 0.5, r * 0.42)
                     for a in (math.pi / 4, 3 * math.pi / 4, 5 * math.pi / 4, 7 * math.pi / 4)])
    paint(pic, leaves | capsule(cx, cy, cx + r * 0.5, cy + r * 1.1, r * 0.1), rgb)


def back(dark, light, edge):
    """A back: a gold-studded lattice inside a double border, clovers in the corners, Lucky in a
    gold cartouche in the middle."""
    pic = blank()
    x0, y0, x1, y1 = 6, 6, W - 6, H - 6
    for y in range(y0, y1):
        for x in range(x0, x1):
            u, v = (x + y) % 10, (x - y) % 10
            pic.set(x, y, light if u in (0, 1) or v in (0, 1) else dark)
    for k in range(0, 40):  # gold studs where the lattice crosses
        for j in range(0, 30):
            sx, sy = 6 + j * 5, 6 + k * 5
            if (sx + sy) % 10 == 0 and x0 < sx < x1 and y0 < sy < y1:
                paint(pic, circle(sx + 0.5, sy + 0.5, 0.9), GOLD)
    paint(pic, box(x0, y0, x1, y1, 4) - box(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 3), edge)
    paint(pic, box(x0 + 4, y0 + 4, x1 - 4, y1 - 4, 3) - box(x0 + 5, y0 + 5, x1 - 5, y1 - 5, 2), GOLD)
    for cx, cy in ((x0 + 13, y0 + 13), (x1 - 13, y0 + 13), (x0 + 13, y1 - 15), (x1 - 13, y1 - 15)):
        paint(pic, circle(cx, cy + 1, 8), edge)
        clover(pic, cx, cy, 7, GOLD)
    paint(pic, ellipse(50, 70, 29, 36), edge)
    paint(pic, ellipse(50, 70, 27, 34), lit(ellipse(50, 70, 27, 34), GOLD, glow=0.35, rim=0.3, edge=4))
    paint(pic, ellipse(50, 70, 23.5, 30.5), GOLD_DARK)
    paint(pic, ellipse(50, 70, 22.5, 29.5), lit(ellipse(50, 70, 22.5, 29.5), dark, glow=0.4, rim=0.2, edge=6))
    p = Painter(pic, 0, 0, 128 * 0.52, outline=2.6)
    p.aim(86, 48, 50, 69)
    draw_head(p, STYLES["lucky"], pose(), neck=True)
    return pic


def joker():
    pic = blank()
    draw_duck(pic, 4, 26, 92, "lucky", pose(eyes="happy", beak=16, wing=40))
    # The jester's hat over the visor: three points with bells.
    k = 92 / 128.0

    def at(x, y):
        return 4 + x * k, 26 + y * k

    hat = [at(68, 36), at(52, 6), at(78, 24), at(88, 0), at(96, 24), at(120, 8), at(104, 34)]
    paint(pic, polygon(hat).grow(1.4), EDGE)
    paint(pic, polygon(hat[:4] + [at(86, 34)]), RED)
    paint(pic, polygon(hat[3:] + [at(86, 34)]), BLUE)
    for x, y in ((52, 6), (88, 0), (120, 8)):
        cx, cy = at(x, y)
        paint(pic, circle(cx, cy, 3.4), EDGE)
        paint(pic, circle(cx, cy, 2.4), GOLD)
    for i, ch in enumerate("JOKER"):
        draw_text(pic, ch, 7, 8 + i * 11, 9, 2.0, RED)
    return pic


def place(mark=None):
    """An empty place on the felt: a rounded outline, clear inside; @p mark: 'A', 'again' or 'done'."""
    pic = Picture(W, H)
    paint(pic, box(0, 0, W, H, RADIUS) - box(2.5, 2.5, W - 2.5, H - 2.5, RADIUS - 2.5), FELT_LINE)
    if mark == "A":
        draw_text(pic, "A", 50 - text_width("A", 40, 3.2) / 2, 50, 40, 3.2, FELT_LINE)
    elif mark == "again":
        ring = circle(50, 70, 22) - circle(50, 70, 16)
        paint(pic, ring - polygon([(50, 70), (90, 32), (90, 46)]), FELT_LINE)
        paint(pic, polygon([(63, 44), (78, 47), (70, 60)]), FELT_LINE)
    elif mark == "done":
        paint(pic, capsule(32, 52, 68, 88, 3) | capsule(68, 52, 32, 88, 3), FELT_LINE)
    return pic


# The sheet: the faces in rows of 13 by suit, then the backs, the places and the Joker.
EXTRAS = ["CARD_BACK", "CARD_BACK_RED", "CARD_EMPTY", "CARD_HOME", "CARD_AGAIN", "CARD_DONE", "CARD_JOKER"]


def tile(index):
    """Card @p index (suit × 13 + rank, then the extras) drawn."""
    if index < 52:
        suit, rank = divmod(index, 13)
        return court_card(suit, rank) if rank >= 10 else number_card(suit, rank)
    name = EXTRAS[index - 52]
    if name == "CARD_BACK":
        return back(GREEN_DARK, GREEN_LIGHT, (0x08, 0x38, 0x1C))
    if name == "CARD_BACK_RED":
        return back((0x80, 0x10, 0x18), (0xC0, 0x40, 0x48), (0x50, 0x08, 0x10))
    if name == "CARD_EMPTY":
        return place()
    if name == "CARD_HOME":
        return place("A")
    if name == "CARD_AGAIN":
        return place("again")
    if name == "CARD_DONE":
        return place("done")
    return joker()
