/**
 * @file      benchfonts.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The font library's benchmarks (devplan/fonts.md §6 step 1): a face opened, its glyphs found,
 * kerned and rendered, from a font file in /rom (read where it lies).
 */

#include <cstring>
#include <bmds/bench.h>
#include <bmds/font.h>
#include <bmds/fontface.h>
#include <bmds/fontlibrary.h>
#include <bmds/glyphcache.h>
#include <bmds/memory.h>
#include <bmds/stopwatch.h>
#include <bmds/systemfonts.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr BenchParam RenderParams[] = {
                {"file", "/rom/share/fonts/IBMPlexSans-Regular.ttf", "A TrueType file in /rom"},
        };

        // Printable ASCII: what most text is.
        constexpr uint32_t First = 0x21, Last = 0x7e, Count = Last - First + 1;

        // Every printable ASCII glyph's box and coverage at @p size and @p depth: microseconds a glyph.
        Error measureRender(BenchContext &ctx, const FontFace &face, unsigned size, FontFace::Depth depth,
                            const char *metric, uint8_t *work, size_t workBytes) {
                return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        for (uint32_t c = First; c <= Last; ++c) {
                                const uint16_t glyph = face.glyphIndex(c);
                                auto [box, err] = face.box(glyph, size);
                                if (err.isError()) return err;
                                if (box.workBytes() > workBytes) return Error(Error::BufferTooSmall);
                                if (Error rerr = face.render(glyph, size, depth, box, Span<uint8_t>(work, workBytes));
                                    rerr.isError())
                                        return rerr;
                        }
                        out = double(watch.elapsedUs()) / double(Count);
                        return Error(Error::Ok);
                });
        }

        // Kerning every pair of printable ASCII glyphs (nanoseconds a pair), then characters past the
        // first 256 (Cyrillic, then symbols) found in the map (nanoseconds each).
        Error measureLookups(BenchContext &ctx, const FontFace &face, const char *kerning, const char *index) {
                uint16_t glyphs[Count];
                for (uint32_t c = First; c <= Last; ++c) glyphs[c - First] = face.glyphIndex(c);
                volatile int sink = 0;
                Error        err = ctx.measure(kerning, "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        int       total = 0;
                        for (uint16_t a : glyphs)
                                for (uint16_t b : glyphs) total += face.kerning(a, b);
                        sink = total;
                        out = double(watch.elapsedUs()) * 1000.0 / double(Count * Count);
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                return ctx.measure(index, "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        uint32_t  total = 0;
                        for (uint32_t c = 0x400; c < 0x500; ++c) total += face.glyphIndex(c);
                        for (uint32_t c = 0x2000; c < 0x2100; ++c) total += face.glyphIndex(c);
                        sink = int(total);
                        out = double(watch.elapsedUs()) * 1000.0 / 512.0;
                        return Error(Error::Ok);
                });
        }

        Error measureOpen(BenchContext &ctx, ByteSpan bytes, const char *metric) {
                return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &out) {
                        FontFace  f;
                        Stopwatch watch;
                        Error     e = f.open(bytes);
                        out = double(watch.elapsedUs());
                        return e;
                });
        }

        Error runRender(BenchContext &ctx) {
                auto [bytes, merr] = Vfs::map(ctx.param("file"), Vfs::Context());
                if (merr.isError()) return merr == Error::NotExist ? Error(Error::NotSupported) : merr;
                FontFace face;
                Error    err = measureOpen(ctx, bytes, "open_us");
                if (err.isOk()) err = face.open(bytes);
                if (err.isError()) return err;
                ctx.note("%.*s %.*s: %u glyphs, %s", int(face.family().size()), face.family().data(),
                         int(face.style().size()), face.style().data(), face.glyphCount(),
                         face.hasKerning() ? "kerned" : "no kerning");

                // The largest box drawn here: 48 px in an em, with room for the tallest marks.
                constexpr size_t WorkBytes = 96 * 96;
                auto            *work = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, WorkBytes));
                if (work == nullptr) return Error::NoMem;
                struct Case {
                                unsigned        size;
                                FontFace::Depth depth;
                                const char     *metric;
                };
                constexpr Case Cases[] = {
                        {13, FontFace::Depth::Gray4, "render_gray4_13_us"},
                        {13, FontFace::Depth::Mono, "render_mono_13_us"},
                        {16, FontFace::Depth::Gray4, "render_gray4_16_us"},
                        {24, FontFace::Depth::Gray4, "render_gray4_24_us"},
                        {48, FontFace::Depth::Gray4, "render_gray4_48_us"},
                };
                for (const Case &c : Cases) {
                        if (err.isError()) break;
                        err = measureRender(ctx, face, c.size, c.depth, c.metric, work, WorkBytes);
                }
                if (err.isOk()) err = measureLookups(ctx, face, "kerning_ns", "glyphindex_ns");

                // The same file copied into PSRAM: what reading it in place from flash costs.
                auto *copy = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes.size()));
                if (err.isOk() && copy == nullptr) err = Error::NoMem;
                if (err.isOk()) {
                        std::memcpy(copy, bytes.data(), bytes.size());
                        const ByteSpan inPsram(copy, bytes.size());
                        FontFace       psram;
                        err = measureOpen(ctx, inPsram, "open_psram_us");
                        if (err.isOk()) err = psram.open(inPsram);
                        if (err.isOk())
                                err = measureRender(ctx, psram, 13, FontFace::Depth::Gray4, "render_gray4_13_psram_us",
                                                    work, WorkBytes);
                        if (err.isOk()) err = measureLookups(ctx, psram, "kerning_psram_ns", "glyphindex_psram_ns");
                }
                Memory::free(copy);
                Memory::free(work);
                return err;
        }

        // The glyph cache over the same face: a hit, a miss (a render and an allocation), a cached
        // kerning pair; then a line laid out with the system's library.
        Error runCache(BenchContext &ctx) {
                auto [bytes, merr] = Vfs::map(ctx.param("file"), Vfs::Context());
                if (merr.isError()) return merr == Error::NotExist ? Error(Error::NotSupported) : merr;
                FontFace face;
                if (Error err = face.open(bytes); err.isError()) return err;
                uint16_t glyphs[Count];
                for (uint32_t c = First; c <= Last; ++c) glyphs[c - First] = face.glyphIndex(c);
                constexpr uint16_t Id = 0xfffe; // not a library face's
                Error              err = ctx.measure("miss_us", "us", BenchBetter::Lower, [&](double &out) {
                        GlyphCache cache(1 << 20);
                        Stopwatch  watch;
                        for (uint16_t g : glyphs) {
                                auto [glyph, e] = cache.acquire(Id, face, g, 13, FontFace::Depth::Gray4, false);
                                if (e.isError()) return e;
                                cache.release(glyph);
                        }
                        out = double(watch.elapsedUs()) / double(Count);
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                GlyphCache cache(1 << 20);
                for (uint16_t g : glyphs) {
                        auto [glyph, e] = cache.acquire(Id, face, g, 13, FontFace::Depth::Gray4, false);
                        if (e.isError()) return e;
                        cache.release(glyph);
                }
                err = ctx.measure("hit_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        for (int round = 0; round < 10; ++round)
                                for (uint16_t g : glyphs) {
                                        auto [glyph, e] = cache.acquire(Id, face, g, 13, FontFace::Depth::Gray4, false);
                                        if (e.isError()) return e;
                                        cache.release(glyph);
                                }
                        out = double(watch.elapsedUs()) * 1000.0 / double(10 * Count);
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                // A run's glyphs found under one lock, as drawing a line does.
                err = ctx.measure("hit_run_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        const GlyphCache::Glyph *found[Count];
                        Stopwatch                watch;
                        for (int round = 0; round < 10; ++round) {
                                if (cache.lookup(Id, Span<const uint16_t>(glyphs, Count), 13, FontFace::Depth::Gray4,
                                                 false, Span<const GlyphCache::Glyph *>(found, Count)) != Count)
                                        return Error(Error::NotFound);
                                for (const GlyphCache::Glyph *g : found) cache.release(g);
                        }
                        out = double(watch.elapsedUs()) * 1000.0 / double(10 * Count);
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                volatile int sink = 0;
                int16_t      kern[Count - 1];
                cache.kerning(Id, face, Span<const uint16_t>(glyphs, Count), Span<int16_t>(kern, Count - 1)); // warm
                err = ctx.measure("kerning_cached_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        // The pairs of the run just kerned: the warm table, as text drawn again has.
                        Stopwatch watch;
                        for (int round = 0; round < 10; ++round)
                                cache.kerning(Id, face, Span<const uint16_t>(glyphs, Count),
                                              Span<int16_t>(kern, Count - 1));
                        sink = kern[0];
                        out = double(watch.elapsedUs()) * 1000.0 / double(10 * (Count - 1));
                        return Error(Error::Ok);
                });
                if (err.isError()) return err;
                auto [font, ferr] = SystemFonts::library().open(StringView("IBM Plex Sans 13"));
                if (ferr.isError()) return ferr;
                static constexpr char Line[] = "The quick brown fox jumps over the lazy dog, To AVA Ta Yo.";
                (void)font.textWidth(Line); // the kerning table warm
                return ctx.measure("layout_ns", "ns", BenchBetter::Lower, [&](double &out) {
                        Stopwatch watch;
                        uint32_t  total = 0;
                        for (int round = 0; round < 10; ++round) total += font.textWidth(Line);
                        sink = int(total);
                        out = double(watch.elapsedUs()) * 1000.0 / double(10 * (sizeof(Line) - 1));
                        return Error(Error::Ok);
                });
        }

} // namespace

BMDS_BENCH_WITH(fontCache, "font.cache", runCache, BenchQuick, RenderParams,
                "The glyph cache on a TrueType face: a miss, a hit, a cached kerning pair; a line laid out (a "
                "character)")
BMDS_BENCH_WITH(fontRender, "font.render", runRender, BenchQuick, RenderParams,
                "A TrueType face opened, its ASCII glyphs rendered at 13-48 px, kerned and looked up")

BMDS_NAMESPACE_END
