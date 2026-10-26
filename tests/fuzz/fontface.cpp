/**
 * @file      fontface.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for TrueType faces (devplan/fonts.md, bmds_fonts): open, the names, the
 * character map, the kerning, and glyphs boxed and rendered sharp and smooth at small and large
 * sizes; nothing may fault, read outside the file, overflow or leak, and no box may be larger than
 * the face allows. stb_truetype's own asserts are on.
 */

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <bmds/fontface.h>

using bmds::ByteSpan;
using bmds::FontFace;
using bmds::Span;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        // A copy of exactly the input's size, so that reading past it is caught.
        std::vector<uint8_t> font(data, data + size);
        FontFace             face;
        if (face.open(ByteSpan(font.data(), font.size())).isError()) return 0;
        volatile size_t sink = face.family().size() + face.style().size();
        uint16_t        glyphs[48];
        size_t          count = 0;
        for (uint32_t c : {0u, 0x20u, 0x41u, 0x56u, 0x61u, 0x6fu, 0x54u, 0x2eu, 0xc1u, 0x416u, 0x20acu, 0x1f600u})
                glyphs[count++] = face.glyphIndex(c);
        for (uint32_t g = 0; g < 32 && g < face.glyphCount(); ++g) glyphs[count++] = uint16_t(g);
        for (size_t i = 0; i < count; ++i)
                for (size_t j = 0; j < count; j += 7) sink = sink + size_t(face.kerning(glyphs[i], glyphs[j]));
        std::vector<uint8_t> work;
        for (size_t i = 0; i < count; ++i) {
                sink = sink + size_t(face.advance(glyphs[i]));
                for (unsigned px : {7u, 13u, 40u}) {
                        auto [box, err] = face.box(glyphs[i], px);
                        if (err.isError()) continue;
                        if (box.width > 4 * FontFace::MaxPixelSize || box.height > 4 * FontFace::MaxPixelSize)
                                std::abort();
                        work.assign(box.workBytes(), 0);
                        for (FontFace::Depth depth : {FontFace::Depth::Gray4, FontFace::Depth::Mono})
                                (void)face.render(glyphs[i], px, depth, box, Span<uint8_t>(work.data(), work.size()));
                }
        }
        (void)sink;
        return 0;
}
