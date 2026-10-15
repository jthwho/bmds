#!/usr/bin/env python3
# @file      assets.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Blocks' assets (devplan/games.md §17), written at build time into the application folder:
# tiles.png (the cells), the sound effects as WAVs, and music.mid (Korobeiniki, a public-domain
# folk song, in our own arrangement); and the program's icons (built into it: devplan/gui.md §4j).
# Python's standard library only.
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

TILE = 16

# The pieces' colours (I, O, T, S, Z, J, L), as the guideline's.
COLOURS = [0x00C8E8, 0xF0D000, 0xA040E0, 0x40D040, 0xE83838, 0x3060F0, 0xF08C20]


def png(path, width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4 : (y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        f.write(chunk(b"IEND", b""))


def shade(rgb, k):
    out = []
    for shift in (16, 8, 0):
        c = (rgb >> shift) & 0xFF
        c = c + (255 - c) * k if k > 0 else c * (1 + k)
        out.append(max(0, min(255, int(c))))
    return out


def tiles(path):
    """16 tiles in a row: 0 empty, 1-7 the pieces, 8 the flash, 9-15 the pieces' ghosts."""
    width = TILE * 16
    px = bytearray(width * TILE * 4)

    def put(t, x, y, rgb, a=255):
        i = (y * width + t * TILE + x) * 4
        px[i : i + 4] = bytes(rgb + [a])

    for y in range(TILE):
        for x in range(TILE):
            edge = x == 0 or y == 0 or x == TILE - 1 or y == TILE - 1
            put(0, x, y, shade(0x1C2436, 0) if edge else shade(0x111827, 0))
            for p, c in enumerate(COLOURS):
                if edge:
                    rgb = shade(c, -0.55)
                elif x == 1 or y == 1:
                    rgb = shade(c, 0.45)  # lit from the top left
                elif x == TILE - 2 or y == TILE - 2:
                    rgb = shade(c, -0.3)
                else:
                    # A soft gradient down the face.
                    rgb = shade(c, 0.12 - 0.2 * y / TILE)
                put(1 + p, x, y, rgb)
                # The ghost: an outline two pixels wide, clear inside.
                ring = x < 2 or y < 2 or x >= TILE - 2 or y >= TILE - 2
                put(9 + p, x, y, shade(c, -0.15) if ring else [0, 0, 0], 255 if ring else 0)
            put(8, x, y, shade(0xFFFFFF, 0) if not edge else shade(0xC8D0E0, 0))
    png(path, width, TILE, px)


# The icon: a well of bevelled cells (T, I, J), on a dark tile. '.' empty.
ICON_CELLS = [
    ". T . .",
    "T T T I",
    "J . . I",
    "J J J I",
]
ICON_COLOURS = {"T": COLOURS[2], "I": COLOURS[0], "J": COLOURS[5]}


def icon(path, side):
    margin = 0 if side <= 16 else 2  # the tile's border shows round the cells when there is room
    cell = (side - 2 * margin) // 4
    px = bytearray(side * side * 4)

    def put(x, y, rgb):
        i = (y * side + x) * 4
        px[i : i + 4] = bytes(rgb + [255])

    for y in range(side):
        for x in range(side):
            edge = x == 0 or y == 0 or x == side - 1 or y == side - 1
            put(x, y, shade(0x3a4560, 0) if edge else shade(0x111827, 0))
    for row, line in enumerate(ICON_CELLS):
        for col, name in enumerate(line.split()):
            if name == ".":
                continue
            c = ICON_COLOURS[name]
            bevel = 1 if side <= 16 else 2
            for y in range(cell):
                for x in range(cell):
                    if x < bevel or y < bevel:
                        rgb = shade(c, 0.45)  # lit from the top left
                    elif x >= cell - bevel or y >= cell - bevel:
                        rgb = shade(c, -0.45)
                    else:
                        rgb = shade(c, 0.1 - 0.25 * y / cell)
                    put(margin + col * cell + x, margin + row * cell + y, rgb)
    png(path, side, side, px)


RATE = 22050


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s * 32767)))) for s in samples))


def tone(seconds, freq, level=0.5, decay=10.0, wave_kind="square", sweep=None, noise=0.0):
    out = []
    phase = 0.0
    n = int(RATE * seconds)
    rnd = random.Random(7)
    for i in range(n):
        t = i / RATE
        f = freq if sweep is None else freq + (sweep - freq) * i / n
        phase += f / RATE
        ph = phase % 1.0
        if wave_kind == "square":
            v = 1.0 if ph < 0.5 else -1.0
        elif wave_kind == "triangle":
            v = 4 * ph - 1 if ph < 0.5 else 3 - 4 * ph
        else:
            v = math.sin(2 * math.pi * ph)
        v = v * (1 - noise) + (rnd.random() * 2 - 1) * noise
        env = math.exp(-t * decay) * min(1.0, i / (RATE * 0.002))  # 2 ms attack: no click
        out.append(v * env * level)
    return out


def notes(seq, step, level=0.35, decay=6.0, kind="square"):
    out = [0.0] * int(RATE * step * (len(seq) + 3))
    for k, freq in enumerate(seq):
        if freq is None:
            continue
        start = int(RATE * step * k)
        for i, v in enumerate(tone(step * 3, freq, level, decay, kind)):
            if start + i < len(out):
                out[start + i] += v
    return out


def hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def sounds(out):
    write_wav(os.path.join(out, "move.wav"), tone(0.018, 1400, 0.18, 60, "square"))
    write_wav(os.path.join(out, "turn.wav"), tone(0.04, 700, 0.22, 30, "triangle", sweep=1200))
    write_wav(os.path.join(out, "lock.wav"), tone(0.06, 190, 0.4, 40, "sine", noise=0.25))
    write_wav(os.path.join(out, "drop.wav"), tone(0.11, 160, 0.55, 25, "sine", sweep=50, noise=0.35))
    write_wav(os.path.join(out, "hold.wav"), tone(0.08, 400, 0.25, 20, "triangle", sweep=950))
    write_wav(os.path.join(out, "clear.wav"), notes([hz(n) for n in (72, 76, 79)], 0.055))
    write_wav(os.path.join(out, "four.wav"), notes([hz(n) for n in (72, 76, 79, 84, 88, 91, 96)], 0.05, 0.3, 4))
    write_wav(os.path.join(out, "level.wav"), notes([hz(n) for n in (67, 72, 76, 79, 84)], 0.07, 0.3, 5, "triangle"))
    write_wav(os.path.join(out, "over.wav"), notes([hz(n) for n in (76, 72, 69, 64, 60, 57)], 0.14, 0.35, 4, "triangle"))


# ---- Korobeiniki (Russian folk song, 1861: public domain), our arrangement: lead, bass, drums.

EIGHTH = 48  # ticks; 96 to a quarter

# (note, eighths); None rests.
PART_A = [
    (76, 2), (71, 1), (72, 1), (74, 2), (72, 1), (71, 1),
    (69, 2), (69, 1), (72, 1), (76, 2), (74, 1), (72, 1),
    (71, 3), (72, 1), (74, 2), (76, 2),
    (72, 2), (69, 2), (69, 4),
    (None, 1), (74, 2), (77, 1), (81, 2), (79, 1), (77, 1),
    (76, 3), (72, 1), (76, 2), (74, 1), (72, 1),
    (71, 2), (71, 1), (72, 1), (74, 2), (76, 2),
    (72, 2), (69, 2), (69, 4),
]
PART_B = [
    (76, 4), (72, 4), (74, 4), (71, 4), (72, 4), (69, 4), (68, 4), (71, 4),
    (76, 4), (72, 4), (74, 4), (71, 4), (72, 2), (76, 2), (81, 4), (80, 8),
]
# A bar's bass root (E2 40, A2 45, D2 38, C2 36).
BASS_A = [40, 45, 40, 45, 38, 36, 40, 45]
BASS_B = [45, 40, 45, 40, 45, 40, 45, 40]


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.insert(0, (v & 0x7F) | 0x80)
        v >>= 7
    return bytes(out)


def music(path):
    events = []  # (tick, order, bytes): note offs before ons at the same tick

    def note(ch, t, n, length, vel):
        events.append((t, 1, bytes([0x90 | ch, n, vel])))
        events.append((t + length, 0, bytes([0x80 | ch, n, 0])))

    events += [
        (0, 0, bytes([0xC0, 80])),  # lead: square
        (0, 0, bytes([0xC1, 33])),  # bass: fingered
        (0, 0, bytes([0xB0, 7, 92])),
        (0, 0, bytes([0xB1, 7, 100])),
        (0, 0, bytes([0xB9, 7, 70])),
        (0, 0, bytes([0xB0, 91, 40])),  # some reverb on the lead
    ]
    t = 0
    bars = []
    for part, bass in ((PART_A, BASS_A), (PART_A, BASS_A), (PART_B, BASS_B)):
        start = t
        for n, eighths in part:
            if n is not None:
                note(0, t, n, eighths * EIGHTH - 8, 96)
            t += eighths * EIGHTH
        bars += [(start + i * 8 * EIGHTH, root) for i, root in enumerate(bass)]
    for bar, root in bars:
        for e in range(8):
            at = bar + e * EIGHTH
            note(1, at, root + (12 if e % 2 else 0), EIGHTH - 6, 90 if e % 2 == 0 else 72)
            note(9, at, 42, EIGHTH // 2, 50)  # closed hat
            if e in (0, 4):
                note(9, at, 36, EIGHTH, 88)  # kick
            if e in (2, 6):
                note(9, at, 38, EIGHTH, 70)  # snare
    events.sort(key=lambda e: (e[0], e[1]))
    track = b"\x00\xff\x51\x03" + struct.pack(">I", 400000)[1:]  # 150 quarters a minute
    last = 0
    for tick, _, data in events:
        track += vlq(tick - last) + data
        last = tick
    track += b"\x00\xff\x2f\x00"
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 2 * EIGHTH))
        f.write(b"MTrk" + struct.pack(">I", len(track)) + track)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--icons":
        os.makedirs(sys.argv[2], exist_ok=True)
        for side in (16, 32):
            icon(os.path.join(sys.argv[2], "icon-%d.png" % side), side)
        return
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    tiles(os.path.join(out, "tiles.png"))
    sounds(out)
    music(os.path.join(out, "music.mid"))


if __name__ == "__main__":
    main()
