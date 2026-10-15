#!/usr/bin/env python3
# @file      assets.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Solitaire's assets (devplan/games.md §21), written at build time into the application folder:
# cards.png (our own card art: the 52 faces in rows of 13 by suit, then the back and the empty
# places) and the card sounds as WAVs; and the program's icons (built into it: devplan/gui.md
# §4j). Python's standard library only.
#
#     assets.py OUTDIR
#     assets.py --icons DIR     icon-16.png and icon-32.png

import math
import os
import random
import struct
import sys
import wave
import zlib

W, H = 71, 96  # a card
RADIUS = 5.0
SS = 4  # supersampling, each way

PAPER = (0xFF, 0xFF, 0xFC)
EDGE = (0x40, 0x40, 0x48)
RED = (0xD0, 0x14, 0x20)
BLACK = (0x14, 0x14, 0x18)
GOLD = (0xE8, 0xB4, 0x20)
GOLD_DARK = (0x9A, 0x6C, 0x10)
BLUE = (0x26, 0x46, 0xA8)
BLUE_DARK = (0x16, 0x26, 0x68)
CRIMSON = (0xC0, 0x1C, 0x28)
CRIMSON_DARK = (0x78, 0x0C, 0x14)
SKIN = (0xF6, 0xD2, 0xA6)
CREAM = (0xFF, 0xF4, 0xD8)
HAIR = (0x6A, 0x40, 0x18)
BLONDE = (0xF0, 0xC8, 0x48)
FELT_LINE = (0x58, 0xA8, 0x70)

CLUBS, DIAMONDS, HEARTS, SPADES = range(4)


# ---- Pictures

class Picture:
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
        if cover <= 0 or not (0 <= x < self.width and 0 <= y < self.height):
            return
        p = self.px[y * self.width + x]
        if p[3] == 0:
            self.px[y * self.width + x] = [rgb[0], rgb[1], rgb[2], int(255 * min(1.0, cover))]
            return
        k = min(1.0, cover)
        a = p[3] / 255.0
        out_a = k + a * (1 - k)
        for i in range(3):
            p[i] = int(round((rgb[i] * k + p[i] * a * (1 - k)) / out_a))
        p[3] = int(round(255 * out_a))

    def paste(self, other, ox, oy):
        for y in range(other.height):
            for x in range(other.width):
                self.px[(oy + y) * self.width + ox + x] = list(other.get(x, y))

    def png(self, path):
        raw = bytearray()
        for y in range(self.height):
            raw.append(0)
            for x in range(self.width):
                raw += bytes(self.get(x, y))

        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
            f.write(chunk(b"IEND", b""))


def shape(pic, inside, box, rgb, turned=False):
    """Paints the points where inside(x, y) holds (in the picture's coordinates, a pixel's centre
    at +0.5) over the pixels of box = (x0, y0, x1, y1), supersampled; turned: by half a turn about
    the picture's centre."""
    x0, y0, x1, y1 = box
    if turned:
        x0, y0, x1, y1 = pic.width - x1, pic.height - y1, pic.width - x0, pic.height - y0
    for py in range(max(0, int(math.floor(y0))), min(pic.height, int(math.ceil(y1)))):
        for px in range(max(0, int(math.floor(x0))), min(pic.width, int(math.ceil(x1)))):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if turned:
                        x, y = pic.width - x, pic.height - y
                    if inside(x, y):
                        hits += 1
            if hits:
                pic.blend(px, py, rgb, hits / (SS * SS))


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x < x1 and y0 <= y < y1


def polygon(points):
    def inside(x, y):
        hit = False
        j = len(points) - 1
        for i in range(len(points)):
            xi, yi = points[i]
            xj, yj = points[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    return inside


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(a, b):
    return lambda x, y: a(x, y) and not b(x, y)


def rounded(x0, y0, x1, y1, r):
    """A rounded rectangle's signed distance (negative inside)."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2

    def dist(x, y):
        qx = abs(x - cx) - (hw - r)
        qy = abs(y - cy) - (hh - r)
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r

    return dist


# ---- Suits: shapes in a unit box (-1..1 each way, y down), placed by pip()

def unit_heart(u, v):
    if (u + 0.5) ** 2 + (v + 0.38) ** 2 <= 0.53 ** 2 or (u - 0.5) ** 2 + (v + 0.38) ** 2 <= 0.53 ** 2:
        return True
    return -0.42 <= v <= 1.0 and abs(u) <= 1.02 * (1.0 - v) / 1.4


def unit_diamond(u, v):
    return abs(u) / 0.78 + abs(v) <= 1.0


def unit_stem(u, v):
    return 0.25 <= v <= 1.0 and abs(u) <= 0.07 + 0.3 * ((v - 0.25) / 0.75) ** 2


def unit_spade(u, v):
    # Two lobes below, sides curving up to the point, and a stem.
    if (u + 0.47) ** 2 + (v - 0.16) ** 2 <= 0.47 ** 2 or (u - 0.47) ** 2 + (v - 0.16) ** 2 <= 0.47 ** 2:
        return True
    if -1.0 <= v <= 0.16 and abs(u) <= 0.94 * ((v + 1.0) / 1.16) ** 0.7:
        return True
    return unit_stem(u, v)


def unit_club(u, v):
    r = 0.4
    if (u ** 2 + (v + 0.5) ** 2 <= r * r or (u + 0.46) ** 2 + (v - 0.12) ** 2 <= r * r
            or (u - 0.46) ** 2 + (v - 0.12) ** 2 <= r * r or u * u + (v + 0.05) ** 2 <= 0.25 ** 2):
        return True
    return unit_stem(u, v)


UNIT = {CLUBS: unit_club, DIAMONDS: unit_diamond, HEARTS: unit_heart, SPADES: unit_spade}


def ink(suit):
    return RED if suit in (DIAMONDS, HEARTS) else BLACK


def pip(pic, suit, cx, cy, w, h, turned=False, rgb=None):
    f = UNIT[suit]

    def inside(x, y):
        return f((x - cx) / (w / 2), (y - cy) / (h / 2))

    shape(pic, inside, (cx - w / 2 - 1, cy - h / 2 - 1, cx + w / 2 + 1, cy + h / 2 + 1), rgb or ink(suit), turned)


# ---- The corners' ranks: a bitmap font of our own, 9 rows

GLYPHS = {
    "A": ["..##..", ".####.", "##..##", "##..##", "##..##", "######", "##..##", "##..##", "##..##"],
    "2": [".####.", "##..##", "....##", "...##.", "..##..", ".##...", "##....", "##....", "######"],
    "3": [".####.", "##..##", "....##", "..###.", "....##", "....##", "....##", "##..##", ".####."],
    "4": ["...##.", "..###.", ".####.", "##.##.", "##.##.", "######", "...##.", "...##.", "...##."],
    "5": ["######", "##....", "##....", "#####.", "....##", "....##", "....##", "##..##", ".####."],
    "6": [".####.", "##..##", "##....", "#####.", "##..##", "##..##", "##..##", "##..##", ".####."],
    "7": ["######", "....##", "....##", "...##.", "...##.", "..##..", "..##..", "..##..", "..##.."],
    "8": [".####.", "##..##", "##..##", ".####.", "##..##", "##..##", "##..##", "##..##", ".####."],
    "9": [".####.", "##..##", "##..##", "##..##", ".#####", "....##", "....##", "##..##", ".####."],
    "1": [".##", "###", ".##", ".##", ".##", ".##", ".##", ".##", ".##"],
    "0": [".####.", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##..##", ".####."],
    "J": ["..####", "....##", "....##", "....##", "....##", "....##", "##..##", "##..##", ".####."],
    "Q": [".####.", "##..##", "##..##", "##..##", "##..##", "##..##", "##.###", "##..##", ".###.#"],
    "K": ["##..##", "##.##.", "####..", "###...", "####..", "##.##.", "##.##.", "##..##", "##..##"],
}
RANKS = ["A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"]


def text_width(text):
    return sum(len(GLYPHS[c][0]) for c in text) + len(text) - 1


def text(pic, x, y, s, rgb):
    for c in s:
        g = GLYPHS[c]
        for row, line in enumerate(g):
            for col, ch in enumerate(line):
                if ch == "#":
                    pic.set(x + col, y + row, rgb)
        x += len(g[0]) + 1


# ---- Cards

def blank(face=PAPER):
    """A card: paper inside a dark edge, rounded, clear outside the corners."""
    pic = Picture(W, H)
    d = rounded(0, 0, W, H, RADIUS)
    for y in range(H):
        for x in range(W):
            dist = d(x + 0.5, y + 0.5)
            if dist <= 0:
                pic.set(x, y, EDGE if dist > -1.0 else face)
    return pic


def turn_copy(pic, x0, y0, x1, y1):
    """Copies the region turned half way round into its place opposite (the corner index)."""
    for y in range(y0, y1):
        for x in range(x0, x1):
            pic.px[(H - 1 - y) * W + (W - 1 - x)] = list(pic.get(x, y))


def corner(pic, suit, rank):
    label = RANKS[rank]
    w = text_width(label)
    text(pic, 8 - w // 2, 5, label, ink(suit))
    pip(pic, suit, 8.0, 20.0, 7.0, 8.0)
    turn_copy(pic, 0, 0, 15, 26)


L, C, R = 22.0, 35.5, 49.0
TOP, SPAN = 18.5, 59.0
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


def number_card(suit, rank):
    pic = blank()
    corner(pic, suit, rank)
    if rank == 0:
        if suit == SPADES:  # the Ace of spades, larger and ringed
            shape(pic, circle(35.5, 48, 23), (11, 24, 61, 73), GOLD)
            shape(pic, circle(35.5, 48, 21.5), (11, 24, 61, 73), PAPER)
            pip(pic, suit, 35.5, 47, 30, 34)
        else:
            pip(pic, suit, 35.5, 48, 26, 28)
        return pic
    for x, f in LAYOUTS[rank + 1]:
        y = TOP + f * SPAN
        if y > 48.0:  # the lower half's pips turned
            pip(pic, suit, W - x, H - y, 13, 14, turned=True)
        else:
            pip(pic, suit, x, y, 13, 14)
    return pic


# The courts: a figure in the frame's upper half, turned for the lower (double-headed).
FX0, FY0, FX1, FY1 = 13, 10, 58, 86  # the frame, symmetric about the card's centre


def court_card(suit, rank):
    pic = blank()
    corner(pic, suit, rank)
    red = suit in (DIAMONDS, HEARTS)
    robe, robe_dark = (CRIMSON, CRIMSON_DARK) if red else (BLUE, BLUE_DARK)
    trim, trim_dark = (BLUE, BLUE_DARK) if red else (CRIMSON, CRIMSON_DARK)
    half = Picture(W, H)
    for y in range(FY0, 48):
        for x in range(FX0, FX1):
            half.set(x, y, CREAM)
    mid = 48.0
    cx = 35.5

    def paint(f, box, rgb):
        shape(half, f, box, rgb)

    # Shoulders and robe, a collar of gold, the trim down the front.
    paint(polygon([(FX0 + 1, mid), (17, 37), (cx - 6, 33), (cx + 6, 33), (54, 37), (FX1 - 1, mid)]),
          (FX0, 32, FX1, mid), robe)
    paint(polygon([(cx - 4, mid), (cx - 2, 34), (cx + 2, 34), (cx + 4, mid)]), (cx - 6, 33, cx + 6, mid), trim)
    paint(polygon([(cx - 9, 33.5), (cx + 9, 33.5), (cx + 6, 37), (cx, 39.5), (cx - 6, 37)]),
          (cx - 10, 32, cx + 10, 40), GOLD)
    for k in range(3):  # the robe's pattern
        paint(circle(20 + k * 3.2, 42 + (k % 2) * 2.5, 1.0), (17, 39, 30, 47), GOLD)
        paint(circle(51 - k * 3.2, 42 + (k % 2) * 2.5, 1.0), (41, 39, 54, 47), GOLD)
    # The head.
    if rank == 11:  # the Queen's hair falls to the shoulders
        paint(ellipse(cx, 27, 9.5, 10), (cx - 11, 16, cx + 11, 38), BLONDE)
    paint(ellipse(cx, 25.5, 6.5, 7.5), (cx - 8, 17, cx + 8, 34), SKIN)
    if rank == 12:  # the King's beard
        paint(minus(ellipse(cx, 28.5, 6.8, 6.2), rect(cx - 8, 20, cx + 8, 27.5)), (cx - 8, 26, cx + 8, 35), HAIR)
        paint(ellipse(cx, 29.5, 2.2, 0.9), (cx - 3, 28, cx + 3, 31), SKIN)
    elif rank == 10:  # the Jack's short hair
        paint(minus(ellipse(cx, 22, 7, 5), rect(cx - 5.2, 21, cx + 5.2, 30)), (cx - 8, 16, cx + 8, 28), HAIR)
    for ex in (cx - 2.6, cx + 2.6):
        half.set(int(ex), 24, BLACK)
    for mx in range(int(cx - 1), int(cx + 2)):
        half.set(mx, 29 if rank != 12 else 30, CRIMSON_DARK)
    # The headwear.
    if rank == 12:  # a crown: a band and three points with jewels
        paint(polygon([(cx - 7.5, 19.5), (cx - 8, 12), (cx - 4.5, 15.5), (cx, 10.5), (cx + 4.5, 15.5), (cx + 8, 12),
                       (cx + 7.5, 19.5)]), (cx - 9, 10, cx + 9, 20), GOLD)
        paint(rect(cx - 7.5, 17.5, cx + 7.5, 19.5), (cx - 8, 17, cx + 8, 20), GOLD_DARK)
        for jx in (cx - 4, cx, cx + 4):
            paint(circle(jx, 16 if jx != cx else 14.5, 1.1), (jx - 2, 12, jx + 2, 18), trim)
    elif rank == 11:  # a tiara
        paint(polygon([(cx - 6.5, 19), (cx - 5, 15.5), (cx - 2.5, 17), (cx, 13), (cx + 2.5, 17), (cx + 5, 15.5),
                       (cx + 6.5, 19)]), (cx - 7, 12, cx + 7, 20), GOLD)
        paint(circle(cx, 16, 1.0), (cx - 2, 14, cx + 2, 18), trim)
    else:  # a cap with a feather
        paint(ellipse(cx, 18.5, 8.5, 4.5), (cx - 10, 13, cx + 10, 23), trim)
        paint(rect(cx - 8, 19, cx + 8, 21), (cx - 9, 18, cx + 9, 22), trim_dark)
        paint(polygon([(cx + 5, 17), (cx + 14, 9), (cx + 15, 11), (cx + 7, 19)]), (cx + 4, 8, cx + 16, 20), GOLD)
    # What each holds: the King a sword, the Queen a flower, the Jack a halberd.
    if rank == 12:
        paint(rect(49.5, 13, 51.5, 41), (49, 12, 52, 42), (0xB8, 0xC0, 0xCC))
        paint(rect(46.5, 37, 54.5, 39), (46, 36, 55, 40), GOLD)
    elif rank == 11:
        paint(rect(20.5, 25, 21.5, 40), (20, 24, 22, 41), (0x30, 0x90, 0x40))
        for a in range(5):
            t = a * 2 * math.pi / 5
            paint(circle(21 + 2.2 * math.cos(t), 23 + 2.2 * math.sin(t), 1.6), (17, 19, 25, 27), trim)
        paint(circle(21, 23, 1.2), (19, 21, 23, 25), GOLD)
    else:
        paint(rect(50, 12, 51.5, 46), (49, 11, 52, 47), HAIR)
        paint(polygon([(51.5, 14), (56, 12), (56, 20), (51.5, 18)]), (51, 11, 57, 21), (0xB8, 0xC0, 0xCC))
    # The suit, small, in the frame's corner.
    pip(half, suit, FX0 + 5.5, FY0 + 6.0, 7, 8)
    # Into the card: the half, then turned below; the frame and the line between.
    for y in range(FY0, 48):
        for x in range(FX0, FX1):
            p = half.get(x, y)
            pic.px[y * W + x] = list(p)
            pic.px[(H - 1 - y) * W + (W - 1 - x)] = list(p)
    for x in range(FX0, FX1):
        pic.set(x, FY0, robe_dark)
        pic.set(x, FY1 - 1, robe_dark)
        pic.set(x, 47, trim_dark)
        pic.set(x, 48, trim_dark)
    for y in range(FY0, FY1):
        pic.set(FX0, y, robe_dark)
        pic.set(FX1 - 1, y, robe_dark)
    return pic


def back():
    pic = blank()
    lattice_a, lattice_b = (0x1E, 0x3C, 0x8C), (0x4C, 0x70, 0xC8)
    x0, y0, x1, y1 = 5, 5, W - 5, H - 5
    for y in range(y0, y1):
        for x in range(x0, x1):
            on = (x + y) % 7 == 0 or (x - y) % 7 == 0
            pic.set(x, y, lattice_b if on else lattice_a)
    for x in range(x0, x1):
        pic.set(x, y0, BLUE_DARK)
        pic.set(x, y1 - 1, BLUE_DARK)
    for y in range(y0, y1):
        pic.set(x0, y, BLUE_DARK)
        pic.set(x1 - 1, y, BLUE_DARK)
    # A medallion in the middle.
    shape(pic, ellipse(35.5, 48, 13, 16), (21, 31, 50, 65), PAPER)
    shape(pic, ellipse(35.5, 48, 11, 14), (21, 31, 50, 65), BLUE_DARK)
    pip(pic, SPADES, 35.5, 48, 13, 15, rgb=GOLD)
    return pic


def place(mark=None):
    """An empty place on the felt: a rounded outline, clear inside; mark: 'A', 'again' or 'done'."""
    pic = Picture(W, H)
    d = rounded(0, 0, W, H, RADIUS)
    for y in range(H):
        for x in range(W):
            dist = d(x + 0.5, y + 0.5)
            if -2.0 < dist <= 0:
                pic.set(x, y, FELT_LINE)
    if mark == "A":
        for row, line in enumerate(GLYPHS["A"]):
            for col, ch in enumerate(line):
                if ch == "#":
                    for k in range(9):  # three times the size
                        pic.set(26 + col * 3 + k % 3, 35 + row * 3 + k // 3, FELT_LINE)
    elif mark == "again":
        ring = minus(circle(35.5, 48, 15), circle(35.5, 48, 11))
        shape(pic, minus(ring, polygon([(35.5, 48), (60, 25), (60, 34)])), (18, 31, 54, 66), FELT_LINE)
        shape(pic, polygon([(44, 31), (54, 33), (49, 42)]), (42, 29, 56, 44), FELT_LINE)
    elif mark == "done":
        for k in range(-12, 13):
            for t in range(-1, 2):
                pic.set(int(35.5 + k) + t, 48 + k, FELT_LINE)
                pic.set(int(35.5 - k) + t, 48 + k, FELT_LINE)
    return pic


def cards(path):
    sheet = Picture(W * 13, H * 5)
    for suit in range(4):
        for rank in range(13):
            pic = court_card(suit, rank) if rank >= 10 else number_card(suit, rank)
            sheet.paste(pic, rank * W, suit * H)
    for i, pic in enumerate([back(), place(), place("A"), place("again"), place("done")]):
        sheet.paste(pic, i * W, 4 * H)
    sheet.png(path)


# ---- The icons: two cards fanned, the Ace of spades over a heart

def icon(path, side):
    pic = Picture(side, side)
    s = side / 32.0

    def card_at(x0, y0, w, h, lean):
        """A card's outline and paper, leaning by @p lean (radians) about its centre."""
        cx, cy = x0 + w / 2, y0 + h / 2
        c, n = math.cos(lean), math.sin(lean)
        d = rounded(x0, y0, x0 + w, y0 + h, 2.5 * s)

        def local(x, y):
            dx, dy = x - cx, y - cy
            return cx + dx * c + dy * n, cy - dx * n + dy * c

        box = (0, 0, side, side)
        shape(pic, lambda x, y: d(*local(x, y)) <= 0, box, EDGE)
        shape(pic, lambda x, y: d(*local(x, y)) <= -max(1.0, s), box, PAPER)
        return local

    small = side <= 16  # upright: leaning blurs at this size
    local = card_at(2 * s, 6 * s, 17 * s, 23 * s, 0 if small else 0.32)
    unit = UNIT[HEARTS]

    def heart(x, y):
        lx, ly = local(x, y)
        return unit((lx - 8.0 * s) / (4.5 * s), (ly - 13.0 * s) / (5 * s))

    shape(pic, heart, (0, 0, side, side), RED)
    local = card_at(12 * s, 4 * s, 17 * s, 23 * s, 0 if small else -0.12)
    spade = UNIT[SPADES]

    def ace(x, y):
        lx, ly = local(x, y)
        return spade((lx - 20.5 * s) / (6.5 * s), (ly - 15.5 * s) / (7.5 * s))

    shape(pic, ace, (0, 0, side, side), BLACK)
    pic.png(path)


# ---- Sounds

RATE = 22050


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 32767)))) for v in samples))


def snap(seconds, level, decay, bright, seed, rise=0.0005):
    """A card's noise: filtered noise with a sharp attack, @p bright the filter (0-1, higher brighter)."""
    rnd = random.Random(seed)
    out = []
    low = 0.0
    band = 0.0
    for i in range(int(RATE * seconds)):
        t = i / RATE
        n = rnd.random() * 2 - 1
        low += bright * (n - low)
        band += 0.5 * (low - band)
        env = min(1.0, t / rise) * math.exp(-t * decay)
        out.append((low - 0.6 * band) * env * level)
    return out


def mix_at(out, samples, at):
    start = int(at * RATE)
    if len(out) < start + len(samples):
        out.extend([0.0] * (start + len(samples) - len(out)))
    for i, v in enumerate(samples):
        out[start + i] += v


def tone(seconds, freq, level, decay):
    out = []
    for i in range(int(RATE * seconds)):
        t = i / RATE
        v = math.sin(2 * math.pi * freq * t) + 0.3 * math.sin(4 * math.pi * freq * t)
        out.append(v * level * math.exp(-t * decay) * min(1.0, t / 0.003))
    return out


def hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def sounds(out):
    write_wav(os.path.join(out, "place.wav"), snap(0.06, 0.9, 70, 0.55, 1))
    write_wav(os.path.join(out, "flip.wav"), snap(0.07, 0.6, 45, 0.85, 2, rise=0.01))
    deal = []
    rnd = random.Random(3)
    for k in range(18):  # a riffle
        mix_at(deal, snap(0.03, 0.5 + 0.3 * rnd.random(), 120, 0.7, 10 + k), 0.03 * k + 0.008 * rnd.random())
    write_wav(os.path.join(out, "deal.wav"), deal)
    win = []
    for k, n in enumerate((72, 76, 79, 84, 79, 84, 88)):
        mix_at(win, tone(0.6, hz(n), 0.22, 5), 0.11 * k)
    mix_at(win, tone(1.2, hz(60), 0.2, 2.5), 0.66)
    mix_at(win, tone(1.2, hz(67), 0.15, 2.5), 0.66)
    write_wav(os.path.join(out, "win.wav"), win)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--icons":
        os.makedirs(sys.argv[2], exist_ok=True)
        for side in (16, 32):
            icon(os.path.join(sys.argv[2], "icon-%d.png" % side), side)
        return
    if len(sys.argv) != 2:
        sys.exit("usage: assets.py OUTDIR | --icons DIR")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cards(os.path.join(out, "cards.png"))
    sounds(out)


if __name__ == "__main__":
    main()
