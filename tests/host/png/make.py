#!/usr/bin/env python3
# Writes the PNG decoder's test images (tests/host/gui.cpp): each NAME.png beside
# NAME.argb, the 0xAARRGGBB pixels (little-endian words) it must decode to, and
# bad-*.png that must be refused. Deterministic; Python's standard library only.
# Two real icons (rendered by tools/render-icons) are copied with their pixels
# from ImageMagick's decoding, when `convert` is there.
#
#   python3 tests/host/png/make.py
import os, random, shutil, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def filtered(rows, bpp):
    out = b""
    prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        f = y % 5  # every filter, in turn
        enc = bytearray()
        for x in range(len(row)):
            a = row[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            pred = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]
            enc.append((row[x] - pred) & 0xff)
        out += bytes([f]) + bytes(enc)
        prev = row
    return out


def compress(raw, how):
    if how == "stored":
        return zlib.compress(raw, 0)
    if how == "fixed":
        c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_FIXED)
        return c.compress(raw) + c.flush()
    return zlib.compress(raw, 9)


def png(width, height, depth, ctype, rows, bpp, extra=b"", how="dynamic", split=0, interlace=0):
    ihdr = struct.pack(">IIBBBBB", width, height, depth, ctype, 0, 0, interlace)
    data = compress(filtered(rows, bpp), how)
    idats = b""
    if split:
        for i in range(0, len(data), split):
            idats += chunk(b"IDAT", data[i:i + split])
    else:
        idats = chunk(b"IDAT", data)
    text = chunk(b"tEXt", b"Comment\0an ancillary chunk, skipped")
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + text + extra + idats + chunk(b"IEND", b"")


def write(name, data, argb=None):
    with open(os.path.join(HERE, name + ".png"), "wb") as f:
        f.write(data)
    if argb is not None:
        with open(os.path.join(HERE, name + ".argb"), "wb") as f:
            f.write(struct.pack("<%dI" % len(argb), *argb))


def main():
    rnd = random.Random(7)
    w, h = 29, 23  # odd sizes: packed rows that don't end on a byte
    # 8-bit kinds: a few colours in runs, so that the data compresses (fixed and dynamic blocks).
    colours = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(5)]
    px = []
    for _ in range(h):
        row = []
        for x in range(w):
            row.append(row[-1] if row and rnd.random() < 0.6 else rnd.choice(colours))
        px.append(row)
    for how in ("stored", "fixed", "dynamic"):
        rows = [bytes(v for p in r for v in p) for r in px]
        write("rgba-" + how, png(w, h, 8, 6, rows, 4, how=how),
              [p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2] for r in px for p in r])
    rows = [bytes(v for p in r for v in p[:3]) for r in px]
    key = px[2][3][:3]
    write("rgb-key", png(w, h, 8, 2, rows, 3, extra=chunk(b"tRNS", struct.pack(">HHH", *key)), split=17),
          [(0 if p[:3] == key else 0xff000000) | p[0] << 16 | p[1] << 8 | p[2] for r in px for p in r])
    rows = [bytes(p[0] for p in r) for r in px]
    write("grey", png(w, h, 8, 0, rows, 1), [0xff000000 | p[0] * 0x010101 for r in px for p in r])
    rows = [bytes(v for p in r for v in (p[0], p[3])) for r in px]
    write("grey-alpha", png(w, h, 8, 4, rows, 2), [p[3] << 24 | p[0] * 0x010101 for r in px for p in r])
    # Palettes of 1, 2, 4 and 8 bits, with alpha for some entries.
    for depth in (1, 2, 4, 8):
        n = 1 << depth
        pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(n)]
        alpha = [rnd.randrange(256) for _ in range(max(1, n // 2))]
        idx = [[rnd.randrange(n) for _ in range(w)] for _ in range(h)]
        if depth == 8:  # runs, for a dynamic block
            idx = [[(x // 5 + y) % n for x in range(w)] for y in range(h)]
        rows = []
        for r in idx:
            bits = 0
            count = 0
            out = bytearray()
            for i in r:
                bits = bits << depth | i
                count += depth
                if count == 8:
                    out.append(bits)
                    bits = count = 0
            if count:
                out.append(bits << (8 - count))
            rows.append(bytes(out))
        extra = chunk(b"PLTE", bytes(v for p in pal for v in p)) + chunk(b"tRNS", bytes(alpha))
        argb = [(alpha[i] if i < len(alpha) else 255) << 24 | pal[i][0] << 16 | pal[i][1] << 8 | pal[i][2]
                for r in idx for i in r]
        write("palette-%d" % depth, png(w, h, depth, 3, rows, 1, extra=extra), argb)
    # Refused.
    rows = [bytes(v for p in r for v in p) for r in px]
    good = png(w, h, 8, 6, rows, 4)
    bad = bytearray(good)
    bad[50] ^= 1  # inside the tEXt chunk (after the 33 bytes of signature and IHDR): its CRC fails
    write("bad-crc", bytes(bad))
    write("bad-truncated", good[:len(good) - 20])
    write("bad-interlaced", png(w, h, 8, 6, rows, 4, interlace=1))
    write("bad-16bit", png(w, h, 16, 6, [r + r for r in rows], 8))
    # Real icons.
    icons = os.path.join(ROOT, "thirdparty", "flat-color-icons", "png")
    if shutil.which("convert"):
        for size, name in (("16x16", "folder"), ("32x32", "command_line")):
            src = os.path.join(icons, size, name + ".png")
            dst = "icon-%s-%s" % (name, size)
            shutil.copy(src, os.path.join(HERE, dst + ".png"))
            rgba = subprocess.run(["convert", src, "-depth", "8", "rgba:-"], check=True,
                                  capture_output=True).stdout
            argb = [rgba[i + 3] << 24 | rgba[i] << 16 | rgba[i + 1] << 8 | rgba[i + 2] for i in range(0, len(rgba), 4)]
            write(dst, open(src, "rb").read(), argb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
