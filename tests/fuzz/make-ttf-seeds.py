#!/usr/bin/env python3
# make-ttf-seeds.py DIR: the font library's fuzzing seeds (devplan/fonts.md, fontface_fuzz), small
# TrueType files cut from IBM Plex by fontTools: a few letters with their GPOS kerning, a compound
# glyph (Á), a format 12 character map, and one with a legacy kern table instead of GPOS, each
# renamed "BMDS Seed" (the license reserves the name "Plex" for the unmodified fonts).
import os, sys
from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'thirdparty', 'plex', 'fonts')
out = sys.argv[1] if len(sys.argv) > 1 else sys.exit('usage: make-ttf-seeds.py DIR')
os.makedirs(out, exist_ok=True)

def cut(name, text, dest, keep_kern=True, cmap12=False, kern_table=False):
    options = subset.Options()
    options.layout_features = ['kern'] if keep_kern else []
    options.hinting = False
    options.name_IDs = [1, 2, 16, 17]
    options.drop_tables += ['DSIG', 'meta', 'gasp']
    font = TTFont(os.path.join(root, name))
    s = subset.Subsetter(options)
    s.populate(text=text)
    s.subset(font)
    if cmap12:
        from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
        t = CmapSubtable.newSubtable(12)
        t.platformID, t.platEncID, t.language = 3, 10, 0
        t.cmap = dict(font.getBestCmap())
        font['cmap'].tables.append(t)
    if kern_table and 'GPOS' in font:
        cm = font.getBestCmap()
        kt = KernTable_format_0()
        kt.version, kt.coverage, kt.format = 0, 1, 0
        kt.kernTable = {(cm[ord('A')], cm[ord('V')]): -40, (cm[ord('T')], cm[ord('o')]): -60}
        font['kern'] = newTable('kern')
        font['kern'].version = 0
        font['kern'].kernTables = [kt]
        del font['GPOS']
    # Renamed: the license keeps "Plex" for the unmodified fonts, and a find may be committed.
    for record in font['name'].names:
        if record.nameID in (1, 16):
            record.string = 'BMDS Seed'
    font.save(os.path.join(out, dest))

cut('IBMPlexSans-Regular.ttf', 'AVTo.aÁ', 'sans.ttf')
cut('IBMPlexSans-Bold.ttf', 'AVTo.aЖ', 'bold-cmap12.ttf', cmap12=True)
cut('IBMPlexSerif-Regular.ttf', 'AVTo', 'serif-kern.ttf', kern_table=True)
cut('IBMPlexMono-Regular.ttf', 'Aa0', 'mono.ttf', keep_kern=False)
