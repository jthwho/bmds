#!/usr/bin/env python3
# @file      build.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's assets (devplan/luckyduck.md §3), drawn at build time: cards.png (the faces in rows
# of 13 by suit, then the backs, the places and the Joker), sprites.png (the ducks, the lobby's
# tables and signs), font.png (the headline letters in three styles), the sounds as WAVs; and for
# the program art.h and art.c (where each sprite and glyph is) and its icons. Pictures are drawn in
# parallel. Python's standard library only.
#
#     build.py --out APPDIR --code DIR

import argparse
import math
import os
import sys
from multiprocessing import Pool

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cards  # noqa: E402
import dice  # noqa: E402
import ducks  # noqa: E402
import lobby  # noqa: E402
import sounds  # noqa: E402
import table  # noqa: E402
from draw import Picture, mix, vertical  # noqa: E402
import font  # noqa: E402
from font import CHARS, advance, draw_text  # noqa: E402

# ---- The headline letters: (name, capital height, stroke weight, fill, outline, its width, shadow)

GOLD_TOP, GOLD_BOTTOM = (0xFF, 0xEC, 0x6E), (0xF5, 0x96, 0x14)
OUTLINE = (0x46, 0x23, 0x0A)
FONTS = [
    ("BIG", 40, 2.2, "gold", OUTLINE, 2.6, 3),
    ("MED", 24, 2.3, (0xFF, 0xFF, 0xFF), (0x28, 0x18, 0x0C), 2.0, 2),
    ("SMALL", 15, 2.4, (0xFF, 0xFF, 0xFF), (0x28, 0x18, 0x0C), 1.5, 0),
    ("HINT", 15, 2.4, (0xFF, 0xD0, 0x40), (0x28, 0x18, 0x0C), 1.5, 0),  # the small one in gold
]
SPACING = 1.2
SHADOW = (0x14, 0x10, 0x0C)


# Each style's spacing beyond the plain one, in units: its outline on both sides would otherwise
# eat the gap (the small style's letters touched); the big one's shadow and size hide it.
EXTRA_SPACING = {"BIG": 0.0, "MED": 1.0, "SMALL": 2.0, "HINT": 2.0}


def spacing_of(f):
    """A style's letter spacing in units."""
    return SPACING + EXTRA_SPACING[f[0]]


def glyph_job(args):
    font, ch = args
    name, size, weight, fill, outline, width, shadow = font
    pad_left = width + 1
    pad_top = width + 1 + size * 0.16
    spacing = spacing_of(font)
    adv = advance(ch, size, weight) + spacing * size / 10.0
    w = int(math.ceil(adv + 2 * pad_left + shadow))
    h = int(math.ceil(size * 1.2 + pad_top + width + 1 + shadow))
    pic = Picture(max(1, w), h)
    rgb = vertical(GOLD_TOP, GOLD_BOTTOM, pad_top, pad_top + size) if fill == "gold" else fill
    draw_text(pic, ch, pad_left, pad_top, size, weight, rgb, outline=outline, outline_width=width,
              shadow=(shadow, shadow, SHADOW) if shadow else None, spacing=spacing)
    return pic, int(round(pad_left)), int(round(pad_top)), int(round(adv))


# ---- The sprites, by name

def sprite_job(name):
    if name in ducks.LUCKY_POSES or name in ducks.CUP_POSES:
        return ducks.lucky(name)
    if name in ducks.DUCKLING_POSES:
        return ducks.duckling(name)
    for tname, title in lobby.GAMES:
        if name == tname:
            return lobby.table(tname, title)
    if name == "SIGN_SOON":
        return lobby.sign()
    if name == "BUBBLE_QUACK":
        return lobby.bubble()
    if name == "BUBBLE_QUACK_LEFT":
        return lobby.bubble(left=True)
    if name in ("BULB_ON", "BULB_OFF"):
        return lobby.bulb(name == "BULB_ON")
    if name == "SHADOW":
        return ducks.ground_shadow()
    if name.startswith("BOT_"):
        return ducks.bot(name)
    if name.startswith("CHIP_BIG_"):
        return table.big_chip(int(name[9:]))
    if name.startswith("CHIP_"):
        return table.chip(int(name[5:]))
    if name == "ARROW":
        return lobby.arrow()
    if name == "SHOE":
        return table.shoe()
    if name == "TRAY":
        return table.tray()
    if name.startswith("DIE_SMALL_"):
        return dice.face(int(name[10:]), dice.SMALL)
    if name.startswith("DIE_TUMBLE_"):
        return dice.tumble(int(name[11:]))
    if name == "DIE_SHADOW":
        return dice.shadow()
    if name.startswith("DIE_"):
        return dice.face(int(name[4:]))
    if name.startswith("FEATHER_"):
        return dice.feather(int(name[8:]))
    if name == "SLEEP_Z":
        return lobby.sleeper()
    raise ValueError(name)


SPRITES = list(ducks.LUCKY_POSES) + list(ducks.DUCKLING_POSES) + [t for t, _ in lobby.GAMES] + [
    "SIGN_SOON", "BUBBLE_QUACK", "BUBBLE_QUACK_LEFT", "SLEEP_Z", "BULB_ON", "BULB_OFF", "SHADOW"] + [
    "BOT_%s_%s" % (kind.upper(), p) for kind, _ in ducks.BOTS for p in ducks.BOT_POSES] + [
    "CHIP_%d" % i for i in range(len(table.CHIPS))] + ["CHIP_BIG_%d" % i for i in range(len(table.CHIPS))] + [
    "SHOE", "TRAY", "ARROW"] + list(ducks.CUP_POSES) + ["DIE_%d" % v for v in range(1, 7)] + [
    "DIE_SMALL_%d" % v for v in range(1, 7)] + ["DIE_TUMBLE_%d" % f for f in range(dice.TUMBLE_FRAMES)] + [
    "DIE_SHADOW", "FEATHER_0", "FEATHER_1"]


def kern_job(job):
    key, weight, spacing = job
    return key, font.kerning(CHARS, weight, spacing)


def card_job(index):
    return cards.tile(index)


def icon_job(side):
    """The program's icon: Lucky's head over a card."""
    pic = Picture(side, side)
    s = side / 32.0
    from draw import box, paint
    paint(pic, box(15 * s, 2 * s, 31 * s, 24 * s, 2.5 * s).grow(max(0.8, s)), cards.EDGE)
    paint(pic, box(15 * s, 2 * s, 31 * s, 24 * s, 2.5 * s), cards.PAPER)
    cards.pip(pic, cards.HEARTS, 26 * s, 9 * s, 8 * s)
    p = ducks.Painter(pic, 0, 0, side * 1.25, outline=2.6 if side > 16 else 4.0)
    p.aim(88, 48, 12 * s, 19 * s)
    ducks.draw_head(p, ducks.STYLES["lucky"], ducks.pose(), neck=True)
    return pic


# ---- Packing

def pack(pictures, width):
    """Shelves of @p pictures (tallest first) @p width wide: their places, and the height."""
    order = sorted(range(len(pictures)), key=lambda i: -pictures[i].height)
    places = [None] * len(pictures)
    x = y = shelf = 0
    for i in order:
        p = pictures[i]
        if x + p.width > width:
            x, y, shelf = 0, y + shelf, 0
        places[i] = (x, y)
        x += p.width
        shelf = max(shelf, p.height)
    return places, y + shelf


def sheet(pictures, places, width, height):
    out = Picture(width, height)
    for p, (x, y) in zip(pictures, places):
        out.paste(p, x, y)
    return out


def write_code(code_dir, sprite_places, sprites, glyphs):
    h = ["/* art.h: where Lucky Duck's pictures are, written by assets/build.py (devplan/luckyduck.md",
         "   §3): don't edit. */", "", "#ifndef LUCKYDUCK_ART_H", "#define LUCKYDUCK_ART_H", "",
         "#include <stdint.h>", "",
         "/* cards.png: the faces (suit × 13 + rank, rank 0 the Ace) in rows of 13, then these. */",
         "#define ART_CARD_W %d" % cards.W, "#define ART_CARD_H %d" % cards.H, "#define ART_CARD_COLUMNS 13",
         "enum {"]
    for i, name in enumerate(cards.EXTRAS):
        h.append("        ART_%s%s," % (name, " = 52" if i == 0 else ""))
    h += ["        ART_CARD_TILES", "};", "",
          "/** @brief A picture's place in its sheet. */",
          "typedef struct {", "                uint16_t x, y, w, h;", "} art_tile_t;", "",
          "/* sprites.png */", "enum {"]
    for name in SPRITES:
        h.append("        ART_%s," % name)
    h += ["        ART_SPRITES", "};", "extern const art_tile_t art_sprites[ART_SPRITES];", "",
          "/* font.png: the headline letters, by ASCII from 32 (w 0: none). A glyph's picture starts",
          "   `left` before the pen and `top` above the capitals' top; the pen moves `advance`. */",
          "typedef struct {", "                uint16_t x, y;", "                uint8_t  w, h, left, top, advance;",
          "} art_glyph_t;", "", "enum {"]
    for f in FONTS:
        h.append("        ART_FONT_%s," % f[0])
    h += ["        ART_FONTS", "};", "", "typedef struct {",
          "                uint8_t            cap;  /* a capital's height */",
          "                uint8_t            tail; /* the spacing after the last letter's ink */",
          "                const art_glyph_t *glyph; /* 95 of them */", "} art_font_t;",
          "extern const art_font_t art_fonts[ART_FONTS];", "",
          "/* Each font's kerning: pairs (by ASCII, sorted) and the pixels between them (negative closer). */",
          "typedef struct {", "                uint8_t a, b;", "                int8_t  dx;", "} art_kern_t;",
          "extern const art_kern_t *const art_kern[ART_FONTS];", "extern const uint16_t art_kern_count[ART_FONTS];",
          "", "#endif", ""]
    with open(os.path.join(code_dir, "art.h"), "w") as f:
        f.write("\n".join(h))
    c = ["/* art.c: written by assets/build.py (devplan/luckyduck.md §3): don't edit. */", "",
         '#include "art.h"', "", "const art_tile_t art_sprites[ART_SPRITES] = {"]
    for name, pic, (x, y) in zip(SPRITES, sprites, sprite_places):
        c.append("        {%d, %d, %d, %d}, /* %s */" % (x, y, pic.width, pic.height, name))
    c.append("};")
    for f in FONTS:
        c += ["", "static const art_glyph_t glyphs_%s[95] = {" % f[0].lower()]
        for code in range(32, 127):
            g = glyphs.get((f[0], chr(code)))
            if g is None:
                c.append("        {0, 0, 0, 0, 0, 0, 0},")
            else:
                (x, y), pic, left, top, adv = g
                c.append("        {%d, %d, %d, %d, %d, %d, %d}, /* %r */" % (x, y, pic.width, pic.height, left, top,
                                                                         adv, chr(code)))
        c.append("};")
    c += ["", "const art_font_t art_fonts[ART_FONTS] = {"]
    for f in FONTS:
        c.append("        {%d, %d, glyphs_%s}," % (f[1], int(round(spacing_of(f) * f[1] / 10.0)), f[0].lower()))
    c += ["};", ""]
    with open(os.path.join(code_dir, "art.c"), "w") as f:
        f.write("\n".join(c))


STYLE_KERN = {}


def write_kerning(code_dir):
    """Each font's pairs in pixels (those that round to a pixel or more), appended to art.c."""
    c = []
    counts = []
    for f in FONTS:
        name, size, weight = f[0], f[1], f[2]
        pairs = []
        for (a, b), k in sorted(STYLE_KERN[name].items()):
            px = int(round(k * size / font.CAP))
            if px != 0 and a != " " and b != " ":
                pairs.append((a, b, px))
        counts.append(len(pairs))
        c += ["", "static const art_kern_t kern_%s[] = {" % name.lower()]
        c += ["        {%d, %d, %d}," % (ord(a), ord(b), px) for a, b, px in pairs] or ["        {0, 0, 0},"]
        c += ["};"]
    c += ["", "const art_kern_t *const art_kern[ART_FONTS] = {%s};" % ", ".join("kern_" + f[0].lower() for f in FONTS),
          "const uint16_t art_kern_count[ART_FONTS] = {%s};" % ", ".join("%d" % n for n in counts), ""]
    with open(os.path.join(code_dir, "art.c"), "a") as out:
        out.write("\n".join(c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="the application folder: the sheets and sounds")
    ap.add_argument("--code", required=True, help="art.h, art.c and the icons")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.code, exist_ok=True)
    glyph_jobs = [(f, ch) for f in FONTS for ch in CHARS]
    jobs = min(8, os.cpu_count() or 1)
    # Kerning first: the workers drawing text (forked after) inherit it.
    # The art's own text (plain spacing) by weight; each headline style with its own spacing.
    kjobs = [(w, w, SPACING) for w in (2.0, 2.2, 2.4, 2.8, 3.2)] + [(f[0], f[2], spacing_of(f)) for f in FONTS]
    with Pool(jobs) as pool:
        done = dict(pool.map(kern_job, kjobs))
    font.KERN.update({k: v for k, v in done.items() if not isinstance(k, str)})
    STYLE_KERN.update({k: v for k, v in done.items() if isinstance(k, str)})
    with Pool(jobs) as pool:
        card_res = pool.map_async(card_job, range(52 + len(cards.EXTRAS)), chunksize=1)
        sprite_res = pool.map_async(sprite_job, SPRITES, chunksize=1)
        glyph_res = pool.map_async(glyph_job, glyph_jobs, chunksize=4)
        icon_res = pool.map_async(icon_job, (16, 32))
        sounds.write_all(args.out)
        card_pics = card_res.get()
        sprite_pics = sprite_res.get()
        glyph_out = glyph_res.get()
        icons = icon_res.get()
    # Cards on their grid.
    rows = 4 + (len(cards.EXTRAS) + 12) // 13
    grid = Picture(13 * cards.W, rows * cards.H)
    for i, pic in enumerate(card_pics):
        grid.paste(pic, (i % 13) * cards.W, (i // 13) * cards.H)
    grid.png(os.path.join(args.out, "cards.png"))
    places, height = pack(sprite_pics, 1024)
    sheet(sprite_pics, places, 1024, height).png(os.path.join(args.out, "sprites.png"))
    gpics = [g[0] for g in glyph_out]
    gplaces, gheight = pack(gpics, 1024)
    sheet(gpics, gplaces, 1024, gheight).png(os.path.join(args.out, "font.png"))
    glyphs = {}
    for (style, ch), place, (pic, left, top, adv) in zip(glyph_jobs, gplaces, glyph_out):
        glyphs[(style[0], ch)] = (place, pic, left, top, adv)
    write_code(args.code, places, sprite_pics, glyphs)
    write_kerning(args.code)
    for side, pic in zip((16, 32), icons):
        pic.png(os.path.join(args.code, "icon-%d.png" % side))


if __name__ == "__main__":
    main()
