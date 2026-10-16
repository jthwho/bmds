# @file      table.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's table pieces (devplan/luckyduck.md §16): chips (seen from above at an angle, to
# stack; and flat, larger, for the rack), the dealing shoe and the discard tray.

import math

from draw import (Picture, box, circle, ellipse, lit, mix, paint, polygon, shade, stroke, vertical)
from font import draw_text, text_width

OUTLINE = (0x2A, 0x1A, 0x0C)
WHITE = (0xFF, 0xFF, 0xFF)

# Each denomination: its value's label, its colour, its spots' colour, its label's colour.
CHIPS = [
    ("1", (0xEE, 0xEE, 0xEA), (0x30, 0x58, 0xC0), (0x22, 0x3A, 0x80)),
    ("5", (0xD0, 0x22, 0x2C), (0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF)),
    ("25", (0x1E, 0x8C, 0x40), (0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF)),
    ("100", (0x24, 0x24, 0x2A), (0xFF, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF)),
    ("500", (0x80, 0x34, 0xA8), (0xFF, 0xE0, 0x60), (0xFF, 0xFF, 0xFF)),
    ("1K", (0xF0, 0xB0, 0x20), (0x6A, 0x3A, 0x10), (0x5A, 0x30, 0x08)),
]
CHIP_W, CHIP_H, CHIP_THICK = 48, 30, 6


def chip(index):
    """A chip seen from above at an angle: its top an ellipse, its edge below it, striped."""
    label, rgb, spot, ink = CHIPS[index]
    pic = Picture(CHIP_W, CHIP_H)
    cx, rx, ry = 24, 22.5, 11.0
    top_y = 12
    edge = ellipse(cx, top_y + CHIP_THICK, rx, ry) | box(cx - rx, top_y, cx + rx, top_y + CHIP_THICK)
    paint(pic, edge.grow(1), OUTLINE)
    paint(pic, edge, shade(rgb, 0.72))
    for k in range(6):  # the edge's stripes
        x = cx - rx + 4 + k * 7.6
        paint(pic, box(x, top_y + 3, x + 3, top_y + CHIP_THICK + 9) & edge, shade(spot, 0.85))
    face = ellipse(cx, top_y, rx, ry)
    paint(pic, face, lit(face, rgb, glow=0.18, rim=0.2, edge=4))
    for k in range(8):  # the rim's spots
        a = k * math.pi / 4 + math.pi / 8
        sx, sy = cx + math.cos(a) * (rx - 3.2), top_y + math.sin(a) * (ry - 1.6)
        paint(pic, ellipse(sx, sy, 2.6, 1.4).at(turn=math.degrees(a) * 0.4, cx=sx, cy=sy), spot)
    paint(pic, ellipse(cx, top_y, rx - 7, ry - 3.6) - ellipse(cx, top_y, rx - 8, ry - 4.2), shade(rgb, 0.7))
    size = 7.5
    w = text_width(label, size, 2.4, spacing=0.6)
    draw_text(pic, label, cx - w / 2, top_y - size / 2, size, 2.4, ink, spacing=0.6)
    return pic


def big_chip(index):
    """A chip seen flat, for the rack: larger, its value plain."""
    label, rgb, spot, ink = CHIPS[index]
    pic = Picture(56, 56)
    c = circle(28, 28, 26)
    paint(pic, c.grow(1.2), OUTLINE)
    paint(pic, c, lit(c, rgb, glow=0.22, rim=0.25, edge=6))
    for k in range(8):
        a = k * math.pi / 4
        paint(pic, box(-3.5, -6, 3.5, 6, 1).at(28 + math.cos(a) * 21.5, 28 + math.sin(a) * 21.5,
                                                 math.degrees(a) + 90), spot)
    paint(pic, circle(28, 28, 15.5) - circle(28, 28, 14.2), shade(rgb, 0.65))
    size = 12 if len(label) < 3 else 9.5
    w = text_width(label, size, 2.6, spacing=0.6)
    draw_text(pic, label, 28 - w / 2, 28 - size / 2, size, 2.6, ink, spacing=0.6)
    return pic


def shoe():
    """The dealing shoe: a smoked box seen from the side, the decks' edges stacked inside it,
    sloping down to its mouth, where the next card's back shows."""
    pic = Picture(140, 96)
    body = polygon([(4, 18), (104, 30), (134, 52), (134, 90), (4, 90)])
    paint(pic, body.grow(1.6), OUTLINE)
    paint(pic, body, lit(body, (0x30, 0x24, 0x1E), glow=0.3, rim=0.3, edge=8))
    # The window in its side: the cards' edges, cream and grey in turn, sloping with the shoe.
    window = polygon([(12, 28), (100, 39), (100, 80), (12, 80)])
    paint(pic, window, (0x18, 0x12, 0x10))
    for k in range(17):
        y0 = 30 + k * 3
        paint(pic, (polygon([(12, y0), (100, y0 + 11), (100, y0 + 12.4), (12, y0 + 1.4)]) & window),
              (0xF4, 0xEE, 0xDC) if k % 2 == 0 else (0xB8, 0xB0, 0xA0))
    paint(pic, window.grow(1.2) - window, (0x14, 0x0E, 0x0A))
    # The mouth: a card's back leaning out.
    card = polygon([(106, 46), (130, 58), (128, 88), (106, 80)])
    paint(pic, card.grow(1), OUTLINE)
    paint(pic, card, (0x14, 0x60, 0x34))
    paint(pic, polygon([(109, 51), (126, 60), (124.5, 84), (109, 78)]), (0x2C, 0x90, 0x50))
    paint(pic, box(4, 84, 134, 90), (0xB0, 0x80, 0x28))
    return pic


def tray():
    """The discard tray: a shallow box."""
    pic = Picture(116, 70)
    body = box(4, 6, 112, 64, 6)
    paint(pic, body.grow(1.5), OUTLINE)
    paint(pic, body, lit(body, (0x3A, 0x2A, 0x22), glow=0.3, rim=0.3, edge=6))
    paint(pic, box(12, 14, 104, 56, 4), (0x22, 0x18, 0x12))
    return pic


def dealer_button():
    """Poker's dealer button: a white puck seen at the chips' angle, thicker, a D on its face."""
    pic = Picture(44, 30)
    cx, rx, ry, top_y, thick = 22, 20, 10.0, 11, 7
    edge = ellipse(cx, top_y + thick, rx, ry) | box(cx - rx, top_y, cx + rx, top_y + thick)
    paint(pic, edge.grow(1), OUTLINE)
    paint(pic, edge, (0xB8, 0xB4, 0xA8))
    face = ellipse(cx, top_y, rx, ry)
    paint(pic, face, lit(face, (0xF8, 0xF6, 0xF0), glow=0.2, rim=0.2, edge=4))
    paint(pic, ellipse(cx, top_y, rx - 3, ry - 1.6) - ellipse(cx, top_y, rx - 4.2, ry - 2.2), (0xC8, 0xC0, 0xB0))
    size = 9.5
    w = text_width("D", size, 2.8, spacing=0.6)
    draw_text(pic, "D", cx - w / 2, top_y - size / 2, size, 2.8, (0x20, 0x20, 0x24), spacing=0.6)
    return pic
