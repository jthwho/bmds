# stb_truetype (vendored)

Sean Barrett's TrueType rasterizer, <https://github.com/nothings/stb>, used by
`components/bmds_fonts` (`devplan/fonts.md`). License: at the end of `stb_truetype.h` (public
domain, or MIT, as one chooses).

- **Version:** 1.26, commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`. `stb_truetype.h` as
  released: sha256 `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`.
- **Changes:** four BMDS patches, in this order:
  - `patches/bmds-first-row.diff`: the rasterizer clamps an edge that ends above the bitmap to its
    top row whatever the bitmap's offset (upstream: only when it isn't 0). A damaged glyph whose
    points lie outside its box tripped the assert after it (found by the fuzzer); the first row
    is the only one that takes such edges, and they cover nothing, so good glyphs draw the same.
  - `patches/bmds-empty-contours.diff`: an empty contour (its end the same as the one before) is
    skipped, as FreeType does. Unpatched, the contour count stopped matching there and every
    contour after it was joined into one shape (stray filled areas). Found testing other fonts
    (Milea's `A`); `tests/host/fonts/edge.ttf` has one.
  - `patches/bmds-symbol-cmap.diff`: a font with only a Windows symbol map opens (stb_truetype
    wanted a Unicode one, though `FontFace` reads the map itself).
  - `patches/bmds-compound-range.diff`: a compound part's transformed coordinates are clamped to
    a vertex's 16 bits; a scale or offset could put them past it, and the conversion was undefined
    (found by the fuzzer, kept in `tests/host/fonts/fuzz/`).
- **Used for** outlines and rasterizing only. It checks nothing in a file, so `FontFace`
  (`bmds_fonts/src/fontface.cpp`) checks the tables at open and each glyph before stb_truetype
  reads it, and reads the character map, the names and the kerning itself. stb_truetype's own
  kerning takes the first GPOS lookup that covers the left glyph, not the sum of the `kern`
  feature's lookups: IBM Plex Sans has 410 wrong pairs that way in Latin and Cyrillic alone.
- **Built with** (`bmds_fonts/src/stbtruetype.c`): single-precision math (`floorf`, `sqrtf`, ...:
  the P4's FPU has no doubles), its memory from `FontFace`'s allocator (`Internal`, else `Psram`),
  its asserts on in the host tests and the fuzzer only (`BMDS_STBTT_ASSERTS`), at `-O2`.

To update: copy `stb_truetype.h` from the new release, apply the patches, record it here, and run the host tests
(`tests/host/fonts.cpp`) and the fuzzer (`tests/fuzz`, `fontface_fuzz`).
