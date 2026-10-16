# @file      lobby.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's lobby (devplan/luckyduck.md §5): each game's table as seen from the floor (a felt
# oval in a wooden rail, what is played on it, its name on a brass plate), the "coming soon" sign,
# Lucky's speech bubble and the sleeper's Z.

import math

from draw import (Picture, box, capsule, circle, ellipse, lit, mix, paint, polygon, shade, vertical)
from font import draw_text, text_width

TW, TH = 300, 210  # a table

FELT = (0x14, 0x80, 0x44)
FELT_DARK = (0x0C, 0x5A, 0x30)
WOOD = (0x8A, 0x52, 0x22)
WOOD_DARK = (0x4E, 0x2C, 0x10)
WOOD_LIGHT = (0xB8, 0x78, 0x3C)
BRASS = (0xE8, 0xBC, 0x48)
BRASS_DARK = (0x8A, 0x60, 0x18)
PAPER = (0xFF, 0xFE, 0xF8)
EDGE = (0x40, 0x40, 0x48)
RED = (0xD0, 0x14, 0x20)
BLACK = (0x18, 0x18, 0x20)
OUTLINE = (0x2E, 0x1C, 0x0C)
IVORY = (0xFA, 0xF4, 0xE4)

GAMES = [("TABLE_SOLITAIRE", "SOLITAIRE"), ("TABLE_BLACKJACK", "BLACKJACK"), ("TABLE_DICE", "LUCKY DUCK DICE"),
         ("TABLE_VIDEO", "VIDEO POKER"), ("TABLE_POKER", "POKER")]


def mini_card(pic, cx, cy, turn, suit_red, label, back=False):
    """A small card (30×42) turned by @p turn degrees about its centre."""
    w, h = 30, 42
    card = box(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 3)
    paint(pic, card.grow(1).at(turn=turn, cx=cx, cy=cy), EDGE)
    if back:
        paint(pic, card.at(turn=turn, cx=cx, cy=cy), (0x14, 0x60, 0x34))
        paint(pic, box(cx - 11, cy - 17, cx + 11, cy + 17, 2).at(turn=turn, cx=cx, cy=cy), (0x2C, 0x90, 0x50))
        return
    paint(pic, card.at(turn=turn, cx=cx, cy=cy), PAPER)
    ink = RED if suit_red else BLACK
    if label:
        sub = Picture(w, h)
        draw_text(sub, label, 3, 3, 9, 2.2, ink, spacing=0.4, squeeze=0.7 if len(label) > 1 else 1.0)
        paint(sub, (ellipse(15, 26, 6, 6) if suit_red else circle(15, 26, 6)), ink)
        a = math.radians(turn)
        for y in range(h):
            for x in range(w):
                p = sub.get(x, y)
                if p[3] < 128:
                    continue
                dx, dy = x + 0.5 - w / 2, y + 0.5 - h / 2
                tx = cx + dx * math.cos(a) - dy * math.sin(a)
                ty = cy + dx * math.sin(a) + dy * math.cos(a)
                if 0 <= int(tx) < pic.width and 0 <= int(ty) < pic.height:
                    pic.set(int(tx), int(ty), p[:3])


def chip_stack(pic, cx, cy, n, rgb):
    for k in range(n):
        y = cy - k * 4
        paint(pic, ellipse(cx, y, 15, 7).grow(1), OUTLINE)
        paint(pic, ellipse(cx, y, 15, 7), shade(rgb, 0.75))
        paint(pic, ellipse(cx, y - 2, 15, 7), rgb)
        paint(pic, ellipse(cx, y - 2, 9, 4), mix(rgb, (255, 255, 255), 0.5))


def die(pic, cx, cy, s, face, turn=0):
    body = box(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, s * 0.18)
    paint(pic, body.grow(1.2).at(turn=turn, cx=cx, cy=cy), OUTLINE)
    paint(pic, body.at(turn=turn, cx=cx, cy=cy), IVORY)
    spots = {1: [(0, 0)], 2: [(-1, -1), (1, 1)], 3: [(-1, -1), (0, 0), (1, 1)], 4: [(-1, -1), (1, -1), (-1, 1), (1, 1)],
             5: [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)], 6: [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)]}
    for u, v in spots[face]:
        paint(pic, circle(cx + u * s * 0.27, cy + v * s * 0.27, s * 0.09).at(turn=turn, cx=cx, cy=cy), BLACK)


def table(name, title):
    pic = Picture(TW, TH)
    cx, cy = TW / 2, 82
    # The shadow, the rail, the felt.
    paint(pic, ellipse(cx + 4, cy + 10, 140, 74), (0x10, 0x08, 0x04))
    paint(pic, ellipse(cx, cy, 142, 76), OUTLINE)
    paint(pic, ellipse(cx, cy, 140, 74), vertical(WOOD_LIGHT, WOOD_DARK, cy - 74, cy + 74))
    paint(pic, ellipse(cx, cy, 124, 60), OUTLINE)
    paint(pic, ellipse(cx, cy, 122, 58), lambda x, y: mix(FELT, FELT_DARK, math.hypot((x - cx) / 122, (y - cy) / 58)))
    paint(pic, ellipse(cx, cy, 112, 50) - ellipse(cx, cy, 110, 48), mix(FELT, (255, 255, 255), 0.2))
    if name == "TABLE_SOLITAIRE":
        for i, (lab, red) in enumerate((("K", False), ("Q", True), ("J", False))):
            mini_card(pic, cx - 50 + i * 20, cy + 2, -12 + i * 6, red, lab)
        mini_card(pic, cx + 40, cy - 4, 4, True, "A")
        mini_card(pic, cx + 66, cy - 2, 8, False, "A")
    elif name == "TABLE_BLACKJACK":
        mini_card(pic, cx - 24, cy, -10, False, "A")
        mini_card(pic, cx, cy - 2, 8, True, "K")
        chip_stack(pic, cx + 52, cy + 14, 4, (0xD0, 0x20, 0x30))
        chip_stack(pic, cx + 82, cy + 4, 3, (0x20, 0x90, 0x40))
        chip_stack(pic, cx - 70, cy + 12, 2, (0x20, 0x20, 0x28))
    elif name == "TABLE_DICE":
        die(pic, cx - 44, cy + 6, 30, 5, -12)
        die(pic, cx, cy - 8, 30, 5, 10)
        die(pic, cx + 44, cy + 8, 30, 5, -4)
        die(pic, cx - 18, cy + 30, 24, 3, 20)
    elif name == "TABLE_VIDEO":
        paint(pic, box(cx - 70, cy - 36, cx + 70, cy + 34, 8).grow(1.5), OUTLINE)
        paint(pic, box(cx - 70, cy - 36, cx + 70, cy + 34, 8), (0x30, 0x30, 0x60))
        paint(pic, box(cx - 62, cy - 28, cx + 62, cy + 26, 4), (0x10, 0x20, 0x70))
        for i, (lab, red) in enumerate((("10", True), ("J", True), ("Q", True), ("K", True), ("A", True))):
            mini_card(pic, cx - 48 + i * 24, cy, 0, red, lab)
    elif name == "TABLE_POKER":
        for i in range(5):
            mini_card(pic, cx - 50 + i * 22, cy - 6 + abs(i - 2) * 3, -16 + i * 8, i % 2 == 1, "AKQJ0"[i] if i < 4 else "10")
        chip_stack(pic, cx + 76, cy + 18, 5, (0x20, 0x20, 0x28))
        chip_stack(pic, cx - 82, cy + 16, 3, (0x80, 0x30, 0xA0))
        mini_card(pic, cx + 2, cy + 34, 90, False, None, back=True)
    # The brass plate with the name.
    size = 17 if len(title) < 12 else 14
    w = text_width(title, size, 2.4) + 28
    paint(pic, box(cx - w / 2, 166, cx + w / 2, 200, 6).grow(1.5), OUTLINE)
    paint(pic, box(cx - w / 2, 166, cx + w / 2, 200, 6), vertical(BRASS, BRASS_DARK, 166, 200))
    draw_text(pic, title, cx - (w - 28) / 2, 183 - size / 2, size, 2.4, (0x3A, 0x22, 0x08))
    return pic


def sign():
    """'COMING SOON' on a little wooden board on a post."""
    pic = Picture(150, 70)
    paint(pic, box(70, 40, 80, 70).grow(1.2), OUTLINE)
    paint(pic, box(70, 40, 80, 70), WOOD_DARK)
    board = box(4, 4, 146, 50, 6).at(turn=-4, cx=75, cy=27)
    paint(pic, board.grow(1.5), OUTLINE)
    paint(pic, board, vertical(WOOD_LIGHT, WOOD, 4, 50))
    sub = Picture(150, 70)
    draw_text(sub, "COMING", 75 - text_width("COMING", 13, 2.4) / 2, 9, 13, 2.4, (0xFF, 0xF4, 0xD0))
    draw_text(sub, "SOON", 75 - text_width("SOON", 13, 2.4) / 2, 28, 13, 2.4, (0xFF, 0xF4, 0xD0))
    a = math.radians(-4)
    for y in range(70):
        for x in range(150):
            p = sub.get(x, y)
            if p[3] < 128:
                continue
            dx, dy = x + 0.5 - 75, y + 0.5 - 27
            pic.set(int(75 + dx * math.cos(a) - dy * math.sin(a)), int(27 + dx * math.sin(a) + dy * math.cos(a)),
                    p[:3])
    return pic


def bubble(text="QUACK!", left=False):
    """A speech bubble, its tail at the lower left (towards a duck facing right), or the right."""
    pic = Picture(130, 64)
    tail = [(100, 40), (112, 62), (82, 44)] if left else [(30, 40), (18, 62), (48, 44)]
    shape = ellipse(65, 26, 60, 23) | polygon(tail)
    paint(pic, shape.grow(2), OUTLINE)
    paint(pic, shape, (0xFF, 0xFF, 0xFF))
    draw_text(pic, text, 65 - text_width(text, 18, 2.8) / 2, 17, 18, 2.8, (0xE0, 0x60, 0x10))
    return pic


def sleeper():
    pic = Picture(30, 30)
    draw_text(pic, "Z", 5, 4, 18, 3.0, (0xFF, 0xFF, 0xFF), outline=OUTLINE, outline_width=1.8)
    return pic


def arrow():
    """A gold arrow pointing down: whose turn it is."""
    pic = Picture(30, 24)
    tip = polygon([(3, 4), (27, 4), (15, 21)])
    paint(pic, tip.grow(1.6), OUTLINE)
    paint(pic, tip, lit(tip, (0xFF, 0xC8, 0x30), glow=0.35, rim=0.3, edge=4))
    return pic


def bulb(on):
    """A marquee bulb on the sign's dark frame (its glow drawn on the frame's colour)."""
    pic = Picture(18, 18)
    frame = (0x4A, 0x0C, 0x14)
    if on:
        paint(pic, circle(9, 9, 9), lambda x, y: mix((0xFF, 0xC8, 0x40), frame, min(1.0, ((x - 9) ** 2 + (y - 9) ** 2) ** 0.5 / 9)))
        paint(pic, circle(9, 9, 4.6), lit(circle(9, 9, 4.6), (0xFF, 0xF0, 0xB0), glow=0.3, rim=0.15, edge=2))
        paint(pic, circle(7.6, 7.4, 1.4), (0xFF, 0xFF, 0xFF))
    else:
        paint(pic, circle(9, 9, 5.4), (0x2A, 0x10, 0x08))
        paint(pic, circle(9, 9, 4.6), lit(circle(9, 9, 4.6), (0x8A, 0x5A, 0x28), glow=0.3, rim=0.3, edge=2))
        paint(pic, circle(7.6, 7.4, 1.0), (0xC8, 0xA8, 0x80))
    return pic
