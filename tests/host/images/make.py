#!/usr/bin/env python3
# Writes the image library's test pictures (tests/host/images.cpp, devplan/images.md §3b): each
# NAME.png or NAME.bmp beside NAME.argb, the 0xAARRGGBB pixels (little-endian words) it must decode
# to; JPEGs (Pillow's libjpeg) with NAME.rgb, libjpeg's own decoding (R, G, B bytes), which ours
# must come near. Deterministic.
#
#   python3 tests/host/images/make.py
import io, os, random, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "png"))
from make import chunk, compress, paeth  # noqa: E402  (the PNG decoder's own writer)


def filter_rows(rows, bpp):
    out = b""
    prev = bytes(len(rows[0])) if rows else b""
    for y, row in enumerate(rows):
        f = y % 5
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


def pack(samples, depth):
    """A row of samples of `depth` bits into bytes, the leftmost in the high bits."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out = bytearray()
    bits = count = 0
    for s in samples:
        bits = bits << depth | s
        count += depth
        if count == 8:
            out.append(bits)
            bits = count = 0
    if count:
        out.append(bits << (8 - count))
    return bytes(out)


ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def png(w, h, depth, ctype, pixels, chans, extra=b"", interlace=0):
    """`pixels`: rows of per-pixel sample tuples."""
    bpp = max(1, chans * depth // 8)
    raw = b""
    if interlace:
        for x0, y0, dx, dy in ADAM7:
            sub = [[pixels[y][x] for x in range(x0, w, dx)] for y in range(y0, h, dy)]
            sub = [r for r in sub if r]
            if not sub:
                continue
            raw += filter_rows([pack([s for p in r for s in p], depth) for r in sub], bpp)
    else:
        raw = filter_rows([pack([s for p in r for s in p], depth) for r in pixels], bpp)
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + extra + chunk(b"IDAT", compress(raw, "dynamic")) +
            chunk(b"IEND", b""))


def write(name, ext, data, argb=None):
    with open(os.path.join(HERE, name + "." + ext), "wb") as f:
        f.write(data)
    if argb is not None:
        with open(os.path.join(HERE, name + ".argb"), "wb") as f:
            f.write(struct.pack("<%dI" % len(argb), *argb))


def eight(v, depth):
    return v >> 8 if depth == 16 else v if depth == 8 else v * 255 // ((1 << depth) - 1)


def pngs(rnd):
    w, h = 21, 13
    # 16-bit RGB with a colour key, 16-bit RGBA.
    px = [[(rnd.randrange(65536), rnd.randrange(65536), rnd.randrange(65536)) for _ in range(w)] for _ in range(h)]
    key = px[1][2]
    write("png-rgb16-key", "png", png(w, h, 16, 2, px, 3, extra=chunk(b"tRNS", struct.pack(">HHH", *key))),
          [(0 if p == key else 0xff000000) | eight(p[0], 16) << 16 | eight(p[1], 16) << 8 | eight(p[2], 16)
           for r in px for p in r])
    px = [[tuple(rnd.randrange(65536) for _ in range(4)) for _ in range(w)] for _ in range(h)]
    write("png-rgba16", "png", png(w, h, 16, 6, px, 4),
          [eight(p[3], 16) << 24 | eight(p[0], 16) << 16 | eight(p[1], 16) << 8 | eight(p[2], 16)
           for r in px for p in r])
    # Greyscale of 1, 2, 4 and 16 bits.
    for depth in (1, 2, 4, 16):
        px = [[(rnd.randrange(1 << depth),) for _ in range(w)] for _ in range(h)]
        write("png-grey%d" % depth, "png", png(w, h, depth, 0, px, 1),
              [0xff000000 | eight(p[0], depth) * 0x010101 for r in px for p in r])
    # Interlaced: RGBA 8-bit, a 2-bit palette, grey 4-bit; odd sizes so that some passes are short.
    for (iw, ih) in ((21, 13), (3, 2), (1, 1)):
        px = [[tuple(rnd.randrange(256) for _ in range(4)) for _ in range(iw)] for _ in range(ih)]
        write("png-adam7-rgba-%dx%d" % (iw, ih), "png", png(iw, ih, 8, 6, px, 4, interlace=1),
              [p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2] for r in px for p in r])
    pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(4)]
    px = [[(rnd.randrange(4),) for _ in range(w)] for _ in range(h)]
    write("png-adam7-palette2", "png",
          png(w, h, 2, 3, px, 1, extra=chunk(b"PLTE", bytes(v for p in pal for v in p)), interlace=1),
          [0xff000000 | pal[p[0]][0] << 16 | pal[p[0]][1] << 8 | pal[p[0]][2] for r in px for p in r])
    px = [[(rnd.randrange(16),) for _ in range(w)] for _ in range(h)]
    write("png-adam7-grey4", "png", png(w, h, 4, 0, px, 1, interlace=1),
          [0xff000000 | eight(p[0], 4) * 0x010101 for r in px for p in r])


def bmp_file(w, h, bpp, rows, palette=None, compression=0, masks=None, dib=40, top_down=False, data=None):
    """`rows`: the stored rows' bytes, in file order (bottom-up unless top_down)."""
    if data is None:
        stride = (w * bpp + 31) // 32 * 4
        data = b"".join(r + bytes(stride - len(r)) for r in rows)
    pal = b""
    if palette is not None:
        pal = b"".join(bytes((b, g, r, 0)) for (r, g, b) in palette) if dib != 12 else \
            b"".join(bytes((b, g, r)) for (r, g, b) in palette)
    extra = b""
    if dib == 12:
        head = struct.pack("<IHHHH", 12, w, h, 1, bpp)
    else:
        head = struct.pack("<IiiHHIIiiII", dib, w, -h if top_down else h, 1, bpp, compression, len(data), 2835, 2835,
                           len(palette) if palette else 0, 0)
        if dib > 40:
            m = list(masks or (0, 0, 0, 0)) + [0] * 4
            head += struct.pack("<IIII", *m[:4])
            head += bytes(dib - len(head))
        elif masks is not None:
            extra = struct.pack("<%dI" % len(masks), *masks)
    off = 14 + len(head) + len(extra) + len(pal)
    return b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off) + head + extra + pal + data


def bmps(rnd):
    w, h = 13, 7
    cols = [[(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(w)] for _ in range(h)]
    flat = lambda img: [0xff000000 | c[0] << 16 | c[1] << 8 | c[2] for r in img for c in r]  # noqa: E731
    # 24-bit, bottom-up and top-down.
    rows = [bytes(v for c in r for v in (c[2], c[1], c[0])) for r in cols]
    write("bmp-24", "bmp", bmp_file(w, h, 24, rows[::-1]), flat(cols))
    write("bmp-24-topdown", "bmp", bmp_file(w, h, 24, rows, top_down=True), flat(cols))
    # 32-bit B, G, R, unused; 32-bit with an alpha mask (V5 header).
    rows = [bytes(v for c in r for v in (c[2], c[1], c[0], 0x55)) for r in cols]
    write("bmp-32", "bmp", bmp_file(w, h, 32, rows[::-1]), flat(cols))
    alpha = [[rnd.randrange(256) for _ in range(w)] for _ in range(h)]
    rows = [bytes(v for c, a in zip(r, ar) for v in (c[2], c[1], c[0], a)) for r, ar in zip(cols, alpha)]
    write("bmp-32-alpha", "bmp",
          bmp_file(w, h, 32, rows[::-1], compression=3, masks=(0xff0000, 0xff00, 0xff, 0xff000000), dib=124),
          [a << 24 | c[0] << 16 | c[1] << 8 | c[2] for r, ar in zip(cols, alpha) for c, a in zip(r, ar)])
    # 16-bit: 5-5-5 by default, 5-6-5 by bit fields after a 40-byte header.
    v555 = [[(c[0] >> 3, c[1] >> 3, c[2] >> 3) for c in r] for r in cols]
    rows = [b"".join(struct.pack("<H", r5 << 10 | g5 << 5 | b5) for (r5, g5, b5) in r) for r in v555]
    write("bmp-16-555", "bmp", bmp_file(w, h, 16, rows[::-1]),
          [0xff000000 | eight(r5, 5) << 16 | eight(g5, 5) << 8 | eight(b5, 5) for r in v555 for (r5, g5, b5) in r])
    v565 = [[(c[0] >> 3, c[1] >> 2, c[2] >> 3) for c in r] for r in cols]
    rows = [b"".join(struct.pack("<H", r5 << 11 | g6 << 5 | b5) for (r5, g6, b5) in r) for r in v565]
    write("bmp-16-565", "bmp", bmp_file(w, h, 16, rows[::-1], compression=3, masks=(0xf800, 0x07e0, 0x001f)),
          [0xff000000 | eight(r5, 5) << 16 | eight(g6, 6) << 8 | eight(b5, 5) for r in v565 for (r5, g6, b5) in r])
    # Palettes of 1, 4 and 8 bits; an OS/2 header.
    for bpp in (1, 4, 8):
        n = 1 << bpp
        pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(n)]
        idx = [[rnd.randrange(n) for _ in range(w)] for _ in range(h)]
        rows = [pack(r, bpp) for r in idx]
        argb = [0xff000000 | pal[i][0] << 16 | pal[i][1] << 8 | pal[i][2] for r in idx for i in r]
        write("bmp-%d" % bpp, "bmp", bmp_file(w, h, bpp, rows[::-1], palette=pal), argb)
        if bpp == 8:
            write("bmp-8-os2", "bmp", bmp_file(w, h, bpp, rows[::-1], palette=pal, dib=12), argb)
    # RLE8 and RLE4: runs, literals, a move (skipped pixels are entry 0), end of line and bitmap.
    pal = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(16)]
    idx = [[0] * w for _ in range(h)]
    data = bytearray()
    # File row 0 (the bottom): a run of 5 of colour 3, 4 literal pixels, a run to the end.
    data += bytes((5, 3)) + bytes((0, 4, 7, 8, 9, 10)) + bytes((w - 9, 2)) + bytes((0, 0))
    bottom = [3] * 5 + [7, 8, 9, 10] + [2] * (w - 9)
    # File row 1: a move 4 across and 1 down, then a run of 3 of colour 5 on row 2, end.
    data += bytes((0, 2, 4, 1)) + bytes((3, 5)) + bytes((0, 0))
    row2 = [0] * 4 + [5] * 3 + [0] * (w - 7)
    data += bytes((0, 1))
    idx[h - 1] = bottom
    idx[h - 3] = row2
    write("bmp-rle8", "bmp", bmp_file(w, h, 8, None, palette=pal, compression=1, data=bytes(data)),
          [0xff000000 | pal[i][0] << 16 | pal[i][1] << 8 | pal[i][2] for r in idx for i in r])
    # RLE4: a run of 7 alternating colours 1 and 2, 3 literal pixels (4, 5, 6), end of bitmap.
    data = bytes((7, 0x12)) + bytes((0, 3, 0x45, 0x60)) + bytes((0, 1))
    idx = [[0] * w for _ in range(h)]
    idx[h - 1] = [1, 2, 1, 2, 1, 2, 1, 4, 5, 6] + [0] * (w - 10)
    write("bmp-rle4", "bmp", bmp_file(w, h, 4, None, palette=pal, compression=2, data=data),
          [0xff000000 | pal[i][0] << 16 | pal[i][1] << 8 | pal[i][2] for r in idx for i in r])


def jpegs(rnd):
    from PIL import Image
    # Gradients at an odd size (not a multiple of the MCU).
    w, h = 61, 37
    img = Image.new("RGB", (w, h))
    # Smooth colour: libjpeg smooths chroma when upsampling, TJpgDec repeats it; sharp chroma would
    # measure that, not the decoder.
    img.putdata([((x * 255) // w, (y * 255) // h, ((x + y) * 255) // (w + h)) for y in range(h) for x in range(w)])

    def save(name, im, **kw):
        buf = io.BytesIO()
        im.save(buf, "JPEG", **kw)
        data = buf.getvalue()
        with open(os.path.join(HERE, name + ".jpg"), "wb") as f:
            f.write(data)
        dec = Image.open(io.BytesIO(data)).convert("RGB")
        with open(os.path.join(HERE, name + ".rgb"), "wb") as f:
            f.write(dec.tobytes())

    save("jpeg-444", img, quality=90, subsampling=0)
    save("jpeg-422", img, quality=90, subsampling=1)
    save("jpeg-420", img, quality=90, subsampling=2)
    save("jpeg-grey", img.convert("L"), quality=90)
    save("jpeg-progressive", img, quality=90, progressive=True)
    save("jpeg-restart", img, quality=90, subsampling=2, restart_marker_rows=1)
    exif = Image.Exif()
    exif[0x0112] = 6
    save("jpeg-exif6", img, quality=90, exif=exif.tobytes())


def main():
    rnd = random.Random(11)
    pngs(rnd)
    bmps(rnd)
    jpegs(rnd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
