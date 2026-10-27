#!/usr/bin/env python3
# make-edge-fonts.py DIR: two small TrueType fonts of our own (MIT, made here by fontTools) with
# what real fonts do and IBM Plex doesn't (devplan/fonts.md §6f), for tests/host/fonts.cpp:
#   edge.ttf    a Unicode map: "A" squares around an empty contour (Milea's), "B" two contours the
#               last a lone on-curve point (DejaVu's), "C" a lone off-curve last point (refused),
#               "D" a compound whose box is turned inside out (Gemelli's: nothing to draw),
#               "E" a 20,000-point outline (Toony's hand-traced glyphs)
#   symbol.ttf  only a Windows symbol map (Seized's): "A" at U+F041
# Run by hand after a change; the fonts are committed.
import math, os, sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent
from fontTools.ttLib.tables import _g_l_y_f

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

def square(x0, y0, x1, y1):
    p = TTGlyphPen(None)
    p.moveTo((x0, y0)); p.lineTo((x0, y1)); p.lineTo((x1, y1)); p.lineTo((x1, y0)); p.closePath()
    return p.glyph()

def simple(contours, flags):
    """A glyph from raw contours (lists of points) and each point's on-curve flag."""
    g = Glyph()
    g.numberOfContours = len(contours)
    coords, ends, n = [], [], 0
    for c in contours:
        coords += c; n += len(c); ends.append(n - 1)
    g.coordinates = _g_l_y_f.GlyphCoordinates(coords)
    g.endPtsOfContours = ends
    g.flags = bytearray(flags)
    g.program = _g_l_y_f.ttProgram.Program(); g.program.fromBytecode(b"")
    return g

def build(path, cmap_kind):
    names = [".notdef", "A", "B", "C", "D", "E", "space"]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    if cmap_kind == "unicode":
        fb.setupCharacterMap({0x41: "A", 0x42: "B", 0x43: "C", 0x44: "D", 0x45: "E", 0x20: "space"})
    else:
        fb.setupCharacterMap({0xF041: "A", 0xF020: "space"})
    sq = [(100, 0), (100, 700), (600, 700), (600, 0)]
    glyphs = {".notdef": square(50, 0, 450, 700), "space": Glyph()}
    # A: a square, an empty contour (its end the same as the one before), then two squares one above
    # the other: stb_truetype unpatched joined the two into one shape, filling the gap between them.
    a = simple([sq, [(700, 0), (700, 300), (900, 300), (900, 0)], [(700, 400), (700, 700), (900, 700), (900, 400)]],
               [1] * 12)
    a.endPtsOfContours = [3, 3, 7, 11]
    a.numberOfContours = 4
    glyphs["A"] = a
    glyphs["B"] = simple([sq, [(300, 300)]], [1] * 4 + [1])  # a lone on-curve point last
    glyphs["C"] = simple([sq, [(300, 300)]], [1] * 4 + [0])  # a lone off-curve point last
    d = Glyph(); d.numberOfContours = -1
    comp = GlyphComponent(); comp.glyphName = "space"; comp.x = 0; comp.y = 0; comp.flags = 0x4
    d.components = [comp]
    glyphs["D"] = d
    pts = [(int(400 + 300 * math.cos(2 * math.pi * i / 20000)), int(350 + 300 * math.sin(2 * math.pi * i / 20000)))
           for i in range(20000)]
    glyphs["E"] = simple([pts], [1] * 20000)
    fb.setupGlyf(glyphs)
    if "D" in fb.font["glyf"].glyphs:  # the compound's box turned inside out, as Gemelli's
        g = fb.font["glyf"]["D"]
        g.xMin, g.yMin, g.xMax, g.yMax = 32767, 32767, -32767, -32767
    fb.setupHorizontalMetrics({n: (700, 0) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "BMDS Edge" if cmap_kind == "unicode" else "BMDS Symbol", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    if cmap_kind == "symbol":
        from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
        t = CmapSubtable.newSubtable(4)
        t.platformID, t.platEncID, t.language = 3, 0, 0
        t.cmap = {0xF041: "A", 0xF020: "space"}
        fb.font["cmap"].tables = [t]
    # Keep the boxes as written (recalculating would put D's back right).
    fb.font.recalcBBoxes = False
    fb.save(path)

build(os.path.join(out, "edge.ttf"), "unicode")
build(os.path.join(out, "symbol.ttf"), "symbol")
