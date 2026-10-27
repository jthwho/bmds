/**
 * @file      glyphcache.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fontface.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The glyphs TrueType faces have rendered, kept for everyone
 *        (`devplan/fonts.md` §4c): one cache for the system, so a glyph is
 *        rendered once per face, size and depth whatever its color and whoever
 *        draws it. Kerning pairs are kept too.
 *
 * A glyph is coverage (`FontFace::Depth`), its color given as it is drawn.
 * Entries are allocated from `Psram` on a miss, which costs a render anyway,
 * and never on a hit. The least recently used go when the budget is passed,
 * or when PSRAM runs short (a `Memory::Reclaimer`). A glyph is pinned from
 * `acquire()` to `release()`, so it is never freed while drawn; the budget
 * may be passed by the glyphs pinned at once.
 *
 * A hit marks its entry used and pins it under the lock; it isn't moved. The
 * eviction sweep, from the oldest, gives a used entry a second chance (to the
 * newest end) and frees the rest. `release()` takes no lock.
 *
 * Kerning: a two-way table of 4,096 pairs (48 KB of `Psram`, made on first
 * use) in front of `FontFace::kerning()`'s table walks, read without
 * the lock (a sequence number per pair).
 *
 * @par Thread Safety
 * Every call from any task: one mutex, held for lookups, inserts, pins and
 * kerning writes, never while a glyph renders or memory is allocated.
 */
class GlyphCache final : public Memory::Reclaimer {
        public:
                /** @brief A cached glyph's coverage. */
                struct Glyph {
                                int16_t         x = 0;     ///< Left edge from the pen.
                                int16_t         y = 0;     ///< Top edge from the baseline (negative: above).
                                uint16_t        width = 0; ///< 0: no ink (a space).
                                uint16_t        height = 0;
                                uint16_t        stride = 0; ///< A row's bytes.
                                FontFace::Depth depth = FontFace::Depth::Gray4;
                                const uint8_t  *rows = nullptr; ///< `height` rows, the top first.
                };

                /** @brief What it holds and how it has done. */
                struct Stats {
                                size_t   entries = 0;
                                size_t   bytes = 0; ///< Entries and their rows.
                                size_t   budget = 0;
                                size_t   pinned = 0; ///< Entries pinned now.
                                uint32_t hits = 0;
                                uint32_t misses = 0;
                                uint32_t evictions = 0;
                                uint32_t failures = 0; ///< Misses that couldn't render (damaged, no memory).
                                uint32_t kernHits = 0;
                                uint32_t kernMisses = 0;
                };

                /** @brief The kerning table's pairs. */
                static constexpr size_t KernPairs = 4096;

                /** @brief An empty cache of @p budgetBytes (nothing allocated yet). */
                explicit GlyphCache(size_t budgetBytes);

                /** @brief Frees every entry (none may be pinned). */
                ~GlyphCache();

                GlyphCache(const GlyphCache &) = delete;
                GlyphCache &operator=(const GlyphCache &) = delete;

                /**
                 * @brief Sets the curve each pixel's 8-bit coverage goes through as a glyph
                 *        is rendered (the contrast: `font/contrast`); null for none. The
                 *        unpinned glyphs rendered with the old one are forgotten.
                 */
                void setCurve(const uint8_t (*curve)[256]);

                /** @brief Sets the budget; entries past it go now (the unpinned). */
                void setBudget(size_t budgetBytes);

                /**
                 * @brief Returns @p glyph of @p face (cache id @p faceId) at @p pixelSize
                 *        and @p depth, @p bold made up (one pixel smeared right), at pen
                 *        phase @p phase (`FontFace::Phases`), rendered on a miss; pinned until
                 *        `release()`.
                 * @return The glyph, or `FontFace::box()`'s or `render()`'s error, `NoMem`.
                 */
                Result<const Glyph *> acquire(uint16_t faceId, const FontFace &face, uint16_t glyph, unsigned pixelSize,
                                              FontFace::Depth depth, bool bold, unsigned phase = 0);

                /**
                 * @brief Looks up a run of glyphs under one lock (as `acquire()` would, same
                 *        face, size, depth and boldness; each at its phase in @p phases, or 0
                 *        if it is shorter): each found is pinned into @p out, a missing one
                 *        left null for `acquire()` to render.
                 * @return How many were found.
                 */
                size_t lookup(uint16_t faceId, Span<const uint16_t> glyphs, unsigned pixelSize, FontFace::Depth depth,
                              bool bold, Span<const Glyph *> out, Span<const uint8_t> phases = {});

                /** @brief Unpins a glyph `acquire()` or `lookup()` gave. */
                void release(const Glyph *glyph);

                /** @brief Returns the kerning of @p left and @p right in font units. */
                int kerning(uint16_t faceId, const FontFace &face, uint16_t left, uint16_t right);

                /**
                 * @brief Kerns a run: @p out[i] is the kerning between
                 *        @p glyphs[i] and @p glyphs[i + 1], in font units (@p out holds
                 *        one less than @p glyphs).
                 */
                void kerning(uint16_t faceId, const FontFace &face, Span<const uint16_t> glyphs, Span<int16_t> out);

                /** @brief Drops every unpinned glyph and every kerning pair of @p faceId. */
                void forget(uint16_t faceId);

                /** @brief Returns what it holds and its counts. */
                Stats stats() const;

                size_t reclaim(MemSpace space, size_t bytes) override;
                Usage  usage() const override;

        private:
                static constexpr size_t Buckets = 4096;

                struct Entry;
                // A pair read without the lock: a sequence number per pair, odd while it is written.
                struct KernPair {
                                std::atomic<uint32_t> sequence{0};
                                std::atomic<uint32_t> pair{0};      // left << 16 | right
                                std::atomic<uint32_t> faceValue{0}; // (faceId + 1) << 16 | value; 0: empty
                };

                static uint64_t keyOf(uint16_t faceId, uint16_t glyph, unsigned pixelSize, FontFace::Depth depth,
                                      bool bold, unsigned phase);
                Entry          *find(uint64_t key) const; // locked
                void            unlink(Entry *e);         // locked
                void            pushFront(Entry *e);      // locked
                size_t          evict(size_t target);     // locked: frees unpinned oldest until bytes <= target
                // The buckets and the kerning table: made on first use, allocated outside the lock.
                bool prepare() { return (_buckets != nullptr && _kern != nullptr) || prepareSlow(); }
                bool prepareSlow();
                bool kernRead(uint16_t faceId, uint16_t left, uint16_t right, int &value) const;
                void kernWrite(uint16_t faceId, uint16_t left, uint16_t right, int value); // locked

                mutable Mutex         _mutex;
                Entry               **_buckets = nullptr;
                KernPair             *_kern = nullptr;
                Entry                *_newest = nullptr;
                Entry                *_oldest = nullptr;
                size_t                _budget;
                uint8_t               _curve[256] = {};
                bool                  _hasCurve = false;
                size_t                _bytes = 0;
                size_t                _entries = 0;
                std::atomic<size_t>   _pinned{0};
                uint32_t              _hits = 0, _misses = 0, _evictions = 0, _failures = 0, _reclaims = 0;
                std::atomic<uint32_t> _kernHits{0}, _kernMisses{0};
};

BMDS_NAMESPACE_END
