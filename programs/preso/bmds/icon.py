#!/usr/bin/env python3
# @file      icon.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Preso's icons, built into the program (devplan/preso.md §8b, gui.md §4j): a slide with a
# title band and a bar chart, on an easel; our own pixel art. Python's standard library only.
#
#     icon.py DIR       icon-16.png and icon-32.png

import os
import struct
import sys
import zlib

# 16 x 16: '.' clear, k outline, b the title band, w the slide, r g y the bars, t the easel.
SLIDE = [
    "................",
    ".kkkkkkkkkkkkkk.",
    ".kbbbbbbbbbbbbk.",
    ".kbbbbbbbbbbbbk.",
    ".kwwwwwwwwwwwwk.",
    ".kwwwwwwwwwyywk.",
    ".kwwwwwwggwyywk.",
    ".kwwwrrwggwyywk.",
    ".kwwwrrwggwyywk.",
    ".kwwwwwwwwwwwwk.",
    ".kkkkkkkkkkkkkk.",
    ".......tt.......",
    "......t..t......",
    ".....t.tt.t.....",
    "....t..tt..t....",
    "...t...tt...t...",
]

PALETTE = {
    "k": (0x20, 0x24, 0x30),
    "b": (0x2A, 0x5C, 0xC8),
    "w": (0xF4, 0xF4, 0xEE),
    "r": (0xE0, 0x40, 0x30),
    "g": (0x30, 0xA8, 0x48),
    "y": (0xF0, 0xB8, 0x20),
    "t": (0x8A, 0x5A, 0x2C),
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


def icon(path, side):
    scale = side // 16
    px = bytearray(side * side * 4)
    for y in range(side):
        for x in range(side):
            c = SLIDE[y // scale][x // scale]
            if c == ".":
                continue
            i = (y * side + x) * 4
            px[i : i + 4] = bytes(PALETTE[c] + (255,))
    png(path, side, side, px)


def main():
    if len(SLIDE) != 16 or any(len(row) != 16 for row in SLIDE):
        sys.exit("icon.py: the slide isn't 16 x 16")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for side in (16, 32):
        icon(os.path.join(out, "icon-%d.png" % side), side)


if __name__ == "__main__":
    main()
