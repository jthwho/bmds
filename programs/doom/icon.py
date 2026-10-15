#!/usr/bin/env python3
# @file      icon.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Doom's icons, built into the program (devplan/gui.md §4j): a horned skull over flames, our own
# pixel art (nothing of id's). Python's standard library only.
#
#     icon.py DIR       icon-16.png and icon-32.png

import os
import struct
import sys
import zlib

# 16 x 16: '.' the background, k outline, w bone, g bone in shade, e the eyes' glow.
SKULL = [
    "................",
    ".g............g.",
    ".gw..........wg.",
    "..gw.kkkkkk.wg..",
    "..gwkwwwwwwkwg..",
    "...kwwwwwwwwk...",
    "..kwwwwwwwwwwk..",
    "..kwkkkwwkkkwk..",
    "..kwkeekkeekwk..",
    "..kwgkkwwkkgwk..",
    "..kgwwwkkwwwgk..",
    "...kgwwkkwwgk...",
    "...kwkwkwkwkk...",
    "....kgkgkgkk....",
    ".....kkkkkk.....",
    "................",
]

PALETTE = {
    "k": (0x1A, 0x06, 0x02),
    "w": (0xEC, 0xE2, 0xC8),
    "g": (0xA8, 0x96, 0x7A),
    "e": (0xFF, 0x40, 0x10),
}


def png(path, width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4 : (y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def fire(x, y, side):
    """The background: dark red at the top to orange flames at the bottom, licking up in tongues."""
    t = y / (side - 1)
    tongue = 0.12 * ((x * 5 // side) % 2)  # alternate columns reach higher
    t = min(1.0, max(0.0, t + tongue - 0.06))
    if t < 0.55:
        return mix((0x24, 0x02, 0x02), (0x9A, 0x14, 0x06), t / 0.55)
    return mix((0x9A, 0x14, 0x06), (0xFF, 0xA0, 0x20), (t - 0.55) / 0.45)


def icon(path, side):
    scale = side // 16
    px = bytearray(side * side * 4)
    for y in range(side):
        for x in range(side):
            edge = x == 0 or y == 0 or x == side - 1 or y == side - 1
            rgb = (0x10, 0x02, 0x00) if edge else fire(x, y, side)
            c = SKULL[y // scale][x // scale]
            if c != ".":
                rgb = PALETTE[c]
            i = (y * side + x) * 4
            px[i : i + 4] = bytes(rgb + (255,))
    png(path, side, side, px)


def main():
    if len(SKULL) != 16 or any(len(row) != 16 for row in SKULL):
        sys.exit("icon.py: the skull isn't 16 x 16")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for side in (16, 32):
        icon(os.path.join(out, "icon-%d.png" % side), side)


if __name__ == "__main__":
    main()
