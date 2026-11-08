#!/usr/bin/env python3
# @file      icon.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# BMDS Demo #1's icons, built into the program (devplan/gui.md §4j): copper bars in a rainbow
# behind a white "1", outlined, on black. Python's standard library only.
#
#     icon.py DIR       icon-16.png and icon-32.png

import colorsys
import os
import struct
import sys
import zlib

# 16 x 16: '.' the bars show through, k the outline, w the figure.
ONE = [
    "................",
    "................",
    "......kkkk......",
    ".....kwwwwk.....",
    "....kwwwwwk.....",
    "...kwwkwwwk.....",
    "....kk.kwwk.....",
    ".......kwwk.....",
    ".......kwwk.....",
    ".......kwwk.....",
    ".......kwwk.....",
    ".....kkkwwkkk...",
    ".....kwwwwwwk...",
    ".....kkkkkkkk...",
    "................",
    "................",
]


def png(path, width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4 : (y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


def bars(y, side):
    """Copper bars: each a hue, bright in its middle row, dark at its edges, round the rainbow."""
    per = side // 4  # four bars
    bar, into = divmod(y, per)
    centre = (per - 1) / 2
    light = 1.0 - abs(into - centre) / (centre + 1)
    r, g, b = colorsys.hsv_to_rgb((bar / 4 + 0.95) % 1.0, 0.85, 0.25 + 0.75 * light)
    return (int(r * 255), int(g * 255), int(b * 255))


def icon(path, side):
    scale = side // 16
    px = bytearray(side * side * 4)
    for y in range(side):
        for x in range(side):
            edge = x == 0 or y == 0 or x == side - 1 or y == side - 1
            rgb = (0, 0, 0) if edge else bars(y, side)
            c = ONE[y // scale][x // scale]
            if c == "k":
                rgb = (0, 0, 0)
            elif c == "w":
                rgb = (0xFF, 0xFF, 0xFF)
            i = (y * side + x) * 4
            px[i : i + 4] = bytes(rgb + (255,))
    png(path, side, side, px)


def main():
    if len(ONE) != 16 or any(len(row) != 16 for row in ONE):
        sys.exit("icon.py: the figure isn't 16 x 16")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for side in (16, 32):
        icon(os.path.join(out, "icon-%d.png" % side), side)


if __name__ == "__main__":
    main()
