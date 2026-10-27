/**
 * @file      fakefonts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font system calls (slots 127-131, devplan/fonts.md §4e) for the host's fake system call
 * table (oss.cpp): the real FontLibrary (unscii and the IBM Plex files), glyph cache and
 * TextDraw behind handles of their own, as the kernel's fontcalls.cpp has them, so libbmdsgame's
 * text runs on the host as it does on the board.
 */

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <bmds/bitmapfont.h>
#include <bmds/errno.h>
#include <bmds/font.h>
#include <bmds/fontlibrary.h>
#include <bmds/glyphcache.h>
#include <bmds/memory.h>
#include <bmds/sys.h>
#include <bmds/textdraw.h>
#include "fakefonts.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::Font;
using bmds::FontLibrary;
using bmds::FontName;
using bmds::Result;
using bmds::Span;
using bmds::StringView;

namespace {

        // The host's font folder, read whole and kept (as if mapped).
        class Files final : public bmds::FontFiles {
                public:
                        Error list(StringView dir, Visit visit, void *arg) override {
                                std::error_code          ec;
                                std::vector<std::string> names;
                                for (const auto &e :
                                     std::filesystem::directory_iterator(std::string(dir.data(), dir.size()), ec))
                                        names.push_back(e.path().string());
                                if (ec) return Error::NotExist;
                                for (const std::string &n : names) visit(arg, StringView(n.data(), n.size()));
                                return Error::Ok;
                        }
                        Result<ByteSpan> map(StringView path) override {
                                std::ifstream in(std::string(path.data(), path.size()), std::ios::binary);
                                _kept.emplace_back(std::istreambuf_iterator<char>(in),
                                                   std::istreambuf_iterator<char>());
                                return bmds::makeResult(ByteSpan(_kept.back().data(), _kept.back().size()));
                        }
                        Result<Span<uint8_t>> load(StringView) override {
                                return bmds::makeError<Span<uint8_t>>(Error::NotSupported);
                        }

                private:
                        std::vector<std::vector<uint8_t>> _kept;
        };

        constexpr int32_t FirstFd = 100, MaxFds = 32;

        struct State {
                        bmds::GlyphCache cache{256 * 1024};
                        Files            files;
                        FontLibrary      library{cache, &files};
                        Font             fds[MaxFds];
                        State() {
                                (void)library.addBitmap(bmds::Fonts::unscii16, "unscii", 13);
                                (void)library.scan(BMDS_FONTS_DIR);
                        }
        };

        State &state() {
                static State s;
                return s;
        }

        const Font *fontOf(int32_t fd) {
                if (fd < FirstFd || fd >= FirstFd + MaxFds || !state().fds[fd - FirstFd].isValid()) return nullptr;
                return &state().fds[fd - FirstFd];
        }

        StringView lineOf(const char *text, uint32_t length) {
                if (text == nullptr) return StringView();
                const void *nl = std::memchr(text, '\n', length);
                return StringView(text, nl != nullptr ? size_t(static_cast<const char *>(nl) - text) : length);
        }

        int32_t hostFontOpen(const bmds_font_spec_t *spec) {
                if (spec == nullptr || spec->name == nullptr) return -BMDS_EINVAL;
                auto [font, err] = state().library.open(StringView(spec->name));
                if (err.isError()) return -BMDS_EINVAL;
                for (int32_t i = 0; i < MaxFds; ++i)
                        if (!state().fds[i].isValid()) {
                                state().fds[i] = font;
                                return FirstFd + i;
                        }
                return -BMDS_EMFILE;
        }

        int32_t hostFontInfo(int32_t fd, bmds_font_info_t *info) {
                const Font *f = fontOf(fd);
                if (f == nullptr) return -BMDS_EBADF;
                info->pixel_size = f->pixelSize();
                info->ascent = f->metrics().ascent;
                info->descent = f->metrics().descent;
                info->line_height = f->metrics().lineHeight;
                info->max_advance = f->metrics().maxAdvance;
                info->flags = f->isBitmap() ? BMDS_FONT_BITMAP : 0u;
                (void)f->name().format(info->name, sizeof(info->name));
                return 0;
        }

        int32_t hostTextMeasure(int32_t fd, const char *text, uint32_t length, uint32_t maxWidth,
                                bmds_text_extent_t *e) {
                const Font *f = fontOf(fd);
                if (f == nullptr) return -BMDS_EBADF;
                const StringView line = lineOf(text, length);
                const size_t     fits = maxWidth == 0 ? line.size() : f->fitting(line, maxWidth);
                e->width = f->textWidth(line.left(fits));
                e->bytes = uint32_t(fits);
                e->height = uint32_t(f->metrics().lineHeight);
                return 0;
        }

        int32_t hostFontClose(int32_t fd) {
                if (fontOf(fd) == nullptr) return -BMDS_EBADF;
                state().fds[fd - FirstFd] = Font();
                return 0;
        }

        int32_t hostTextDraw(const bmds_text_draw_t *d) {
                const Font *f = fontOf(d->font);
                if (f == nullptr) return -BMDS_EBADF;
                bmds::TextTarget t;
                t.pixels = static_cast<uint8_t *>(d->pixels);
                t.format = bmds::TextBlend::Format(d->format);
                t.width = int(d->width);
                t.height = int(d->height);
                t.stride = d->stride != 0 ? d->stride : d->width * bmds::TextBlend::bytesPerPixel(t.format);
                if (d->clip_width != 0 || d->clip_height != 0) {
                        t.clipLeft = d->clip_x;
                        t.clipTop = d->clip_y;
                        t.clipRight = d->clip_x + int(d->clip_width);
                        t.clipBottom = d->clip_y + int(d->clip_height);
                }
                bmds::TextPaint paint;
                paint.color = d->color;
                paint.background = d->background;
                paint.opaque = (d->flags & BMDS_TEXT_OPAQUE) != 0;
                paint.smooth = (d->flags & BMDS_TEXT_SHARP) == 0;
                auto [w, err] = bmds::TextDraw::line(t, *f, lineOf(d->text, d->length), d->x, d->y, paint, d->max_width,
                                                     bmds::Simd::Kernel::Scalar);
                return err.isError() ? -BMDS_EIO : int32_t(w);
        }

} // namespace

void FakeFonts::install(bmds_sys_fn_t *slots) {
        slots[BMDS_SYS_FONT_OPEN] = reinterpret_cast<bmds_sys_fn_t>(hostFontOpen);
        slots[BMDS_SYS_FONT_INFO] = reinterpret_cast<bmds_sys_fn_t>(hostFontInfo);
        slots[BMDS_SYS_TEXT_MEASURE] = reinterpret_cast<bmds_sys_fn_t>(hostTextMeasure);
        slots[BMDS_SYS_TEXT_DRAW] = reinterpret_cast<bmds_sys_fn_t>(hostTextDraw);
        slots[BMDS_SYS_FONT_CLOSE] = reinterpret_cast<bmds_sys_fn_t>(hostFontClose);
        return;
}
