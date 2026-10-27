/**
 * @file      fontcalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SDK's fonts (devplan/fonts.md §4e): font_open, font_info, text_measure, text_draw,
 * font_list and font_close (slots 127-132), the system's font library and glyph cache in front.
 * A program's fonts are a table of its own (Process::fontAdd), not fds.
 */

#include <cstdint>
#include <cstring>
#include <bmds/bitmapfont.h>
#include <bmds/errno.h>
#include <bmds/fontface.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include <bmds/systemfonts.h>
#include <bmds/textdraw.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::Font;
using bmds::FontLibrary;
using bmds::FontName;
using bmds::KernelCall;
using bmds::Process;
using bmds::StringView;
using bmds::detail::copyIn;
using bmds::detail::copyOut;
using bmds::detail::copyText;
namespace TextBlend = bmds::TextBlend;

namespace {

        static_assert(BMDS_FONT_NAME_MAX == FontName::MaxText);
        static_assert(BMDS_FONT_BOLD == FontName::Bold && BMDS_FONT_ITALIC == FontName::Italic);
        static_assert(BMDS_FONT_ROLE_UI == uint32_t(FontName::Role::Ui) &&
                      BMDS_FONT_ROLE_MONO == uint32_t(FontName::Role::Mono) &&
                      BMDS_FONT_ROLE_TITLE == uint32_t(FontName::Role::Title) &&
                      BMDS_FONT_ROLE_TERMINAL == uint32_t(FontName::Role::Terminal));
        static_assert(BMDS_PIXEL_BGR24 == uint32_t(TextBlend::Format::Bgr24) &&
                      BMDS_PIXEL_RGB24 == uint32_t(TextBlend::Format::Rgb24) &&
                      BMDS_PIXEL_BGRA32 == uint32_t(TextBlend::Format::Bgra32) &&
                      BMDS_PIXEL_RGB565 == uint32_t(TextBlend::Format::Rgb565) &&
                      BMDS_PIXEL_GRAY8 == uint32_t(TextBlend::Format::Gray8) &&
                      BMDS_PIXEL_L8 == uint32_t(TextBlend::Format::Index8));

        int32_t errnoOf(Error err) {
                if (err == Error::CorruptData || err == Error::IoError) return -BMDS_EIO;
                if (err == Error::NotExist) return -BMDS_ENOENT;
                return Process::errnoFor(err);
        }

        // The font behind handle @p id (a copy: usable even if another thread closes it meanwhile).
        int32_t fontOf(KernelCall &call, int32_t id, Font &font) {
                return call->fontOf(id, font) ? 0 : -BMDS_EBADF;
        }

        // @p length bytes of @p text up to a newline.
        int32_t lineOf(const char *text, uint32_t length, StringView &line) {
                if (length == 0) {
                        line = StringView();
                        return 0;
                }
                if (text == nullptr) return -BMDS_EFAULT;
                const void *newline = std::memchr(text, '\n', length);
                line = StringView(text,
                                  newline != nullptr ? size_t(static_cast<const char *>(newline) - text) : length);
                return 0;
        }

        uint32_t styleOf(const FontLibrary::FaceInfo &f) {
                return (f.weight >= 600 ? BMDS_FONT_BOLD : 0u) | (f.italic ? BMDS_FONT_ITALIC : 0u);
        }

} // namespace

extern "C" {

int32_t bmds_k_font_open(const bmds_font_spec_t *spec) {
        KernelCall       call;
        bmds_font_spec_t s;
        int32_t          r = copyIn(spec, s);
        if (r < 0) return r;
        if (s.flags != 0 || (s.style & ~(BMDS_FONT_BOLD | BMDS_FONT_ITALIC)) != 0 || s.role > BMDS_FONT_ROLE_TERMINAL)
                return -BMDS_EINVAL;
        if (s.pixel_size != 0 && (s.pixel_size < FontName::MinSize || s.pixel_size > FontName::MaxSize))
                return -BMDS_ERANGE;
        FontName name;
        if (s.name != nullptr) {
                const size_t n = strnlen(s.name, BMDS_FONT_NAME_MAX);
                if (n == BMDS_FONT_NAME_MAX) return -BMDS_EINVAL;
                auto [parsed, err] = FontName::parse(StringView(s.name, n));
                if (err == Error::OutOfRange) return -BMDS_ERANGE;
                if (err.isError()) return -BMDS_EINVAL;
                name = parsed;
        } else if (s.family != nullptr) {
                const size_t n = strnlen(s.family, FontName::MaxFamily + 1);
                if (n == 0 || n > FontName::MaxFamily) return -BMDS_EINVAL;
                name = FontName::make(StringView(s.family, n), uint16_t(s.pixel_size), uint8_t(s.style));
        } else {
                if (s.role == BMDS_FONT_ROLE_NONE) return -BMDS_EINVAL;
                name.role = FontName::Role(s.role);
                name.size = uint16_t(s.pixel_size);
                name.style = uint8_t(s.style);
                name.hasStyle = s.style != 0;
        }
        auto [font, err] = bmds::SystemFonts::library().open(name);
        if (err.isError()) return errnoOf(err);
        return call->fontAdd(font);
}

int32_t bmds_k_font_info(int32_t id, bmds_font_info_t *info) {
        KernelCall  call;
        Font        held;
        int32_t     r = fontOf(call, id, held);
        const Font *font = &held;
        if (r < 0) return r;
        bmds_font_info_t i;
        r = copyIn(info, i);
        if (r < 0) return r;
        i = bmds_font_info_t{};
        const Font::Metrics        &m = font->metrics();
        const FontLibrary::FaceInfo f = font->library()->face(font->faceIndex());
        i.pixel_size = font->pixelSize();
        i.ascent = m.ascent;
        i.descent = m.descent;
        i.line_height = m.lineHeight;
        i.max_advance = m.maxAdvance;
        i.flags = (font->isBitmap() ? BMDS_FONT_BITMAP : 0u) | (f.fixedPitch ? BMDS_FONT_FIXED : 0u) |
                  (font->isMadeUpBold() ? BMDS_FONT_MADE_UP_BOLD : 0u);
        i.style = styleOf(f);
        i.face = font->faceIndex();
        const size_t n = font->name().format(i.name, sizeof(i.name));
        (void)n;
        return copyOut(i, info);
}

int32_t bmds_k_text_measure(int32_t id, const char *text, uint32_t length, uint32_t max_width,
                            bmds_text_extent_t *extent) {
        KernelCall  call;
        Font        held;
        int32_t     r = fontOf(call, id, held);
        const Font *font = &held;
        if (r < 0) return r;
        bmds_text_extent_t e;
        r = copyIn(extent, e);
        if (r < 0) return r;
        StringView line;
        r = lineOf(text, length, line);
        if (r < 0) return r;
        const size_t fits = max_width == 0 ? line.size() : font->fitting(line, max_width);
        e = bmds_text_extent_t{};
        e.width = font->textWidth(line.left(fits));
        e.bytes = uint32_t(fits);
        e.height = uint32_t(font->metrics().lineHeight);
        return copyOut(e, extent);
}

int32_t bmds_k_text_draw(const bmds_text_draw_t *draw) {
        KernelCall       call;
        bmds_text_draw_t d;
        int32_t          r = copyIn(draw, d);
        if (r < 0) return r;
        Font held;
        r = fontOf(call, d.font, held);
        if (r < 0) return r;
        const Font        *font = &held;
        constexpr uint32_t Flags = BMDS_TEXT_OPAQUE | BMDS_TEXT_SMOOTH | BMDS_TEXT_SHARP;
        if (d.format > BMDS_PIXEL_L8 || (d.flags & ~Flags) != 0 ||
            (d.flags & (BMDS_TEXT_SMOOTH | BMDS_TEXT_SHARP)) == (BMDS_TEXT_SMOOTH | BMDS_TEXT_SHARP) ||
            d.width > 0x7fff || d.height > 0x7fff)
                return -BMDS_EINVAL;
        StringView line;
        r = lineOf(d.text, d.length, line);
        if (r < 0) return r;
        if (d.width == 0 || d.height == 0 || line.isEmpty()) return 0;
        if (d.pixels == nullptr) return -BMDS_EFAULT;
        const auto     format = TextBlend::Format(d.format);
        const uint32_t row = d.width * TextBlend::bytesPerPixel(format);
        if (d.stride != 0 && d.stride < row) return -BMDS_EINVAL;
        bmds::TextTarget t;
        t.pixels = static_cast<uint8_t *>(d.pixels);
        t.format = format;
        t.width = int(d.width);
        t.height = int(d.height);
        t.stride = d.stride != 0 ? d.stride : row;
        if (d.clip_width != 0 || d.clip_height != 0) {
                t.clipLeft = d.clip_x;
                t.clipTop = d.clip_y;
                t.clipRight = int(int64_t(d.clip_x) + d.clip_width > INT32_MAX ? INT32_MAX
                                                                               : d.clip_x + int64_t(d.clip_width));
                t.clipBottom = int(int64_t(d.clip_y) + d.clip_height > INT32_MAX ? INT32_MAX
                                                                                 : d.clip_y + int64_t(d.clip_height));
        }
        bmds::TextPaint paint;
        paint.color = d.color;
        paint.background = d.background;
        paint.opaque = (d.flags & BMDS_TEXT_OPAQUE) != 0;
        paint.smooth = (d.flags & BMDS_TEXT_SMOOTH) != 0  ? true
                       : (d.flags & BMDS_TEXT_SHARP) != 0 ? false
                                                          : bmds::SystemFonts::smoothing();
        auto [width, err] = bmds::TextDraw::line(t, *font, line, d.x, d.y, paint, d.max_width, bmds::Simd::best());
        if (err.isError()) return errnoOf(err);
        return int32_t(width);
}

int32_t bmds_k_font_list(uint32_t index, bmds_font_face_t *face) {
        KernelCall       call;
        bmds_font_face_t out;
        int32_t          r = copyIn(face, out);
        if (r < 0) return r;
        FontLibrary &lib = bmds::SystemFonts::library();
        if (index >= lib.faceCount()) return -BMDS_ENOENT;
        const FontLibrary::FaceInfo f = lib.face(index);
        out = bmds_font_face_t{};
        out.style = styleOf(f);
        out.weight = f.weight;
        out.flags = (f.bitmapSize != 0 ? BMDS_FONT_BITMAP : 0u) | (f.fixedPitch ? BMDS_FONT_FIXED : 0u);
        out.glyphs = f.glyphs;
        out.bitmap_size = f.bitmapSize;
        copyText(f.family, out.family, sizeof(out.family));
        copyText(f.style, out.style_name, sizeof(out.style_name));
        copyText(f.path, out.path, sizeof(out.path));
        return copyOut(out, face);
}

int32_t bmds_k_font_close(int32_t id) {
        KernelCall call;
        return call->fontClose(id);
}

} // extern "C"
