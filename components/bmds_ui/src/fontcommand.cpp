/**
 * @file      fontcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `font` command (devplan/fonts.md §4h): the system's faces, what a name opens, the glyph
 * cache's figures, a line drawn to a PNG, and scanning the folders again. Here rather than in
 * bmds_fonts for the PNG writer.
 */

#include <cstdio>
#include <cstring>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/png.h>
#include <bmds/shell.h>
#include <bmds/systemfonts.h>
#include <bmds/textdraw.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int fail(CommandContext &ctx, const char *what, Error err) {
                ctx.eprintf("font: %s: %s\n", what, err.desc());
                return Shell::ExitFailure;
        }

        int list(CommandContext &ctx) {
                FontLibrary &lib = SystemFonts::library();
                for (size_t i = 0; i < lib.faceCount(); ++i) {
                        const FontLibrary::FaceInfo f = lib.face(i);
                        char                        size[16] = "";
                        if (f.bitmapSize != 0) std::snprintf(size, sizeof(size), " %upx", unsigned(f.bitmapSize));
                        ctx.printf("%2u  %.*s %.*s%s  weight %u%s%s  %u glyphs%s%s%s  %.*s\n", unsigned(i),
                                   int(f.family.size()), f.family.data(), int(f.style.size()), f.style.data(), size,
                                   unsigned(f.weight), f.italic ? ", italic" : "", f.fixedPitch ? ", fixed" : "",
                                   unsigned(f.glyphs), f.loaded ? ", loaded" : "", f.present ? "" : ", gone",
                                   f.users != 0 ? ", in use" : "", int(f.path.size()), f.path.data());
                }
                return Shell::ExitSuccess;
        }

        void describe(CommandContext &ctx, const char *label, const Font &font) {
                char                        name[FontName::MaxText];
                const size_t                n = font.name().format(name, sizeof(name));
                const FontLibrary::FaceInfo f = font.library()->face(font.faceIndex());
                const Font::Metrics        &m = font.metrics();
                ctx.printf("%s%.*s: face %u (%.*s), %u px%s%s, ascent %d, descent %d, line %d, widest %d\n", label,
                           int(n), name, unsigned(font.faceIndex()), int(f.path.size() != 0 ? f.path.size() : 8),
                           f.path.size() != 0 ? f.path.data() : "built in", font.pixelSize(),
                           font.isBitmap() && font.scale() > 1 ? " (scaled)" : "",
                           font.isMadeUpBold() ? ", bold made up" : "", m.ascent, m.descent, m.lineHeight,
                           m.maxAdvance);
                return;
        }

        int info(CommandContext &ctx, Span<char *const> args) {
                FontLibrary &lib = SystemFonts::library();
                if (args.size() == 2) { // the roles
                        for (FontName::Role role : {FontName::Role::Ui, FontName::Role::Mono, FontName::Role::Title,
                                                    FontName::Role::Terminal}) {
                                FontName want;
                                want.role = role;
                                auto [font, err] = lib.open(want);
                                char label[24];
                                std::snprintf(label, sizeof(label), "%-9s ", FontName::roleName(role));
                                if (err.isError())
                                        ctx.printf("%s%s\n", label, err.desc());
                                else
                                        describe(ctx, label, font);
                        }
                        return Shell::ExitSuccess;
                }
                if (args.size() != 3) return ctx.usage();
                auto [font, err] = lib.open(StringView(args[2]));
                if (err.isError()) return fail(ctx, args[2], err);
                describe(ctx, "", font);
                return Shell::ExitSuccess;
        }

        int stats(CommandContext &ctx) {
                const GlyphCache::Stats s = SystemFonts::cache().stats();
                ctx.printf("glyphs %lu (%lu pinned), %lu of %lu KB; hits %lu, misses %lu, evicted %lu, failed %lu\n"
                           "kerning pairs: hits %lu, misses %lu; smoothing %s\n",
                           (unsigned long)s.entries, (unsigned long)s.pinned, (unsigned long)(s.bytes / 1024),
                           (unsigned long)(s.budget / 1024), (unsigned long)s.hits, (unsigned long)s.misses,
                           (unsigned long)s.evictions, (unsigned long)s.failures, (unsigned long)s.kernHits,
                           (unsigned long)s.kernMisses, SystemFonts::smoothing() ? "on" : "off");
                return Shell::ExitSuccess;
        }

        Error toFile(void *arg, ByteSpan data) {
                auto *file = static_cast<FileHandle *>(arg);
                while (!data.isEmpty()) {
                        auto [n, err] = file->write(data, WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = ByteSpan(data.data() + n, data.size() - n);
                }
                return Error::Ok;
        }

        int draw(CommandContext &ctx, Span<char *const> args) {
                bool sharp = false;
                if (args.size() == 6 && std::strcmp(args[5], "sharp") == 0)
                        sharp = true;
                else if (args.size() == 6 && std::strcmp(args[5], "smooth") == 0)
                        sharp = false;
                else if (args.size() != 5)
                        return ctx.usage();
                else
                        sharp = !SystemFonts::smoothing();
                auto [font, err] = SystemFonts::library().open(StringView(args[2]));
                if (err.isError()) return fail(ctx, args[2], err);
                const StringView text(args[3]);
                const int        margin = 8;
                const int        width = int(font.textWidth(text)) + 2 * margin + font.metrics().maxAdvance;
                const int        height = font.metrics().lineHeight + 2 * margin;
                if (width > 4096 || height > 1024) return fail(ctx, args[3], Error::OutOfRange);
                const size_t bytes = size_t(width) * height * 3;
                auto        *pixels = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
                if (pixels == nullptr) return fail(ctx, "picture", Error::NoMem);
                std::memset(pixels, 0xff, bytes);
                // Drawn as everything draws text: TextDraw, the kernels, the system's glyph cache.
                TextTarget target;
                target.pixels = pixels;
                target.format = TextBlend::Format::Bgr24;
                target.width = width;
                target.height = height;
                target.stride = size_t(width) * 3;
                TextPaint paint;
                paint.smooth = !sharp;
                err = TextDraw::line(target, font, text, margin, margin, paint, 0, Simd::best()).error();
                if (err.isOk()) {
                        auto [file, oerr] =
                                Vfs::open(StringView(args[4]), Vfs::Write | Vfs::Create | Vfs::Truncate, ctx.vfs());
                        err = oerr;
                        if (err.isOk())
                                err = Png::write(pixels, uint16_t(width), uint16_t(height), size_t(width) * 3,
                                                 Png::DefaultLevel, toFile, &*file, nullptr);
                }
                Memory::free(pixels);
                if (err.isError()) return fail(ctx, args[4], err);
                ctx.printf("font: %s, %dx%d, %s\n", args[4], width, height, sharp ? "sharp" : "smooth");
                return Shell::ExitSuccess;
        }

        int runFont(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() < 2) return ctx.usage();
                const StringView what(args[1]);
                if (what == "list" && args.size() == 2) return list(ctx);
                if (what == "info") return info(ctx, args);
                if (what == "stats" && args.size() == 2) return stats(ctx);
                if (what == "draw") return draw(ctx, args);
                if (what == "rescan" && args.size() == 2) {
                        const Error err = SystemFonts::rescan();
                        if (err.isError()) return fail(ctx, "rescan", err);
                        ctx.printf("font: %u faces\n", unsigned(SystemFonts::library().faceCount()));
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(font, runFont, "list | info [NAME] | stats | draw NAME TEXT FILE.png [sharp|smooth] | rescan",
                   "The system's fonts: the faces, what a name opens (the roles without one), the glyph cache, a "
                   "line drawn to a PNG, the font folders scanned again")

BMDS_NAMESPACE_END
