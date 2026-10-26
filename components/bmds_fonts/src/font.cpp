/**
 * @file      font.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/bitmapfont.h>
#include <bmds/font.h>
#include <bmds/fontface.h>
#include <bmds/glyphcache.h>
#include <bmds/utf8.h>

#include "fontrecord.h"

BMDS_NAMESPACE_BEGIN

Font::Font(const Font &other)
    : _library(other._library), _metrics(other._metrics), _unitsTo64(other._unitsTo64), _face(other._face),
      _size(other._size), _scale(other._scale), _madeUpBold(other._madeUpBold) {
        retain();
}

Font::Font(Font &&other)
    : _library(other._library), _metrics(other._metrics), _unitsTo64(other._unitsTo64), _face(other._face),
      _size(other._size), _scale(other._scale), _madeUpBold(other._madeUpBold) {
        other._library = nullptr;
}

Font &Font::operator=(const Font &other) {
        if (this == &other) return *this;
        other.retain();
        drop();
        _library = other._library;
        _metrics = other._metrics;
        _unitsTo64 = other._unitsTo64;
        _face = other._face;
        _size = other._size;
        _scale = other._scale;
        _madeUpBold = other._madeUpBold;
        return *this;
}

Font &Font::operator=(Font &&other) {
        if (this == &other) return *this;
        drop();
        _library = other._library;
        _metrics = other._metrics;
        _unitsTo64 = other._unitsTo64;
        _face = other._face;
        _size = other._size;
        _scale = other._scale;
        _madeUpBold = other._madeUpBold;
        other._library = nullptr;
        return *this;
}

Font::~Font() {
        drop();
}

void Font::retain() const {
        if (_library != nullptr) _library->retain(_face);
        return;
}

void Font::drop() {
        if (_library != nullptr) _library->drop(_face);
        _library = nullptr;
        return;
}

bool Font::isBitmap() const {
        return _library != nullptr && _library->record(_face)->bitmap != nullptr;
}

const BitmapFont *Font::bitmap() const {
        return _library != nullptr ? _library->record(_face)->bitmap : nullptr;
}

const FontFace *Font::face() const {
        return _library != nullptr ? _library->record(_face)->face : nullptr;
}

FontName Font::name() const {
        if (_library == nullptr) return FontName();
        const FontLibrary::Record &r = *_library->record(_face);
        uint8_t                    style = r.weight >= 600 || _madeUpBold ? FontName::Bold : 0;
        if (r.italic) style |= FontName::Italic;
        return FontName::make(StringView(r.family), _size, style);
}

int64_t Font::layout(StringView utf8, Visit visit, void *arg) const {
        if (_library == nullptr) return 0;
        const FontLibrary &lib = *_library;
        int64_t            pen = 0;
        int                prevFace = -1;
        uint16_t           prevGlyph = 0;
        for (size_t i = 0; i < utf8.size();) {
                Placed p;
                p.offset = static_cast<uint32_t>(i);
                const uint32_t c = Utf8::decode(utf8, i);
                // This face, then the fallback faces, then unscii, then this face's missing glyph.
                const FontLibrary::Record *r = lib.record(_face);
                p.faceIndex = _face;
                bool found = false;
                if (r->bitmap != nullptr) {
                        found = r->bitmap->hasGlyph(c);
                } else if (r->face != nullptr) {
                        p.glyph = r->face->glyphIndex(c);
                        found = p.glyph != 0;
                }
                for (int16_t f : lib.fallbacks()) {
                        if (found) break;
                        const FontLibrary::Record *fr = lib.record(uint16_t(f));
                        if (fr->face == nullptr || uint16_t(f) == _face) continue;
                        p.glyph = fr->face->glyphIndex(c);
                        if (p.glyph != 0) {
                                p.faceIndex = uint16_t(f);
                                found = true;
                        }
                }
                if (!found && lib.unscii() != 0xffff && lib.unscii() != _face &&
                    lib.record(lib.unscii())->bitmap->hasGlyph(c)) {
                        p.faceIndex = lib.unscii();
                        found = true;
                }
                if (!found) { // the missing glyph: TrueType's glyph 0, a bitmap font's replacement
                        p.faceIndex = _face;
                        p.glyph = 0;
                }
                const FontLibrary::Record *used = lib.record(p.faceIndex);
                if (used->bitmap != nullptr) {
                        p.bitmap = used->bitmap;
                        p.glyph = c;
                        const unsigned height = used->bitmap->height();
                        const unsigned scale = p.faceIndex == _face ? _scale : (_size + height / 2) / height;
                        p.scale = static_cast<uint16_t>(scale > 0 ? scale : 1);
                        p.ascent = static_cast<uint16_t>(used->bitmapAscent * p.scale);
                        p.advance = int32_t(used->bitmap->glyph(c).width) * p.scale * 64;
                        prevFace = -1;
                } else {
                        const FontFace &face = *used->face;
                        p.face = &face;
                        // Font units to 1/64 pixels in 16.16 fixed point: this face's from the font, a
                        // fallback face's worked out.
                        const int64_t scale =
                                p.faceIndex == _face ? _unitsTo64 : (int64_t(_size) << 22) / face.unitsPerEm();
                        if (prevFace == p.faceIndex)
                                pen += (int64_t(lib.cache().kerning(p.faceIndex, face, prevGlyph, uint16_t(p.glyph))) *
                                                scale +
                                        32768) >>
                                       16;
                        p.advance = int32_t((int64_t(face.advance(uint16_t(p.glyph))) * scale + 32768) >> 16);
                        prevFace = p.faceIndex;
                        prevGlyph = uint16_t(p.glyph);
                }
                if (_madeUpBold) p.advance += 64;
                p.pen = pen;
                p.x = int32_t((pen + 32) >> 6);
                if (visit != nullptr && !visit(arg, p)) return pen;
                pen += p.advance;
        }
        return pen;
}

uint32_t Font::textWidth(StringView utf8) const {
        const int64_t advance = layout(utf8, nullptr, nullptr);
        return advance <= 0 ? 0 : uint32_t((advance + 63) >> 6);
}

size_t Font::fitting(StringView utf8, uint32_t maxWidth) const {
        struct Fit {
                        int64_t limit;
                        size_t  bytes;
        } fit{int64_t(maxWidth) * 64, utf8.size()};
        (void)layout(
                utf8,
                [](void *arg, const Placed &g) {
                        auto *f = static_cast<Fit *>(arg);
                        if (g.pen + g.advance > f->limit) {
                                f->bytes = g.offset;
                                return false;
                        }
                        return true;
                },
                &fit);
        return fit.bytes;
}

BMDS_NAMESPACE_END
