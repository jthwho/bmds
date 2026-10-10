/**
 * @file      graphlayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A `Graph`'s geometry, pure (`devplan/sysmon.md` §3.5): samples as
 *        bars from the bottom, the newest at the right; the plot's width is
 *        shared by `slots` columns (column k spans `k * width / slots` to the
 *        next: a few pixels wide each, all of the width used), so a full
 *        history reaches the left edge. Neighbours of the same height merge
 *        into one run, so a flat stretch is one fill.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct GraphLayout {
                /** @brief A bar, or bars side by side of one height. */
                struct Run {
                                uint16_t x = 0;      ///< From the plot's left.
                                uint16_t width = 0;  ///< Pixels.
                                uint16_t height = 0; ///< From the plot's bottom (never 0: no run then).
                };

                /** @brief Returns how many pixels of @p height show @p value of @p maximum (clamped). */
                static uint16_t barHeight(int64_t value, int64_t maximum, uint16_t height);

                /**
                 * @brief Returns the smallest 1, 2 or 5 times a power of ten that is at
                 *        least @p value (and at least 1): an automatic scale's top.
                 */
                static int64_t niceMaximum(int64_t value);

                /** @brief Returns the columns @p width holds: @p slots, but no more than one a pixel. */
                static size_t columns(uint16_t width, size_t slots) {
                        const size_t s = slots > 0 ? slots : 1;
                        return s < width ? s : width;
                }

                /** @brief Returns column @p k's left edge, of @p count across @p width. */
                static uint16_t columnX(size_t k, size_t count, uint16_t width) {
                        return uint16_t(count > 0 ? k * width / count : 0);
                }

                /**
                 * @brief Calls @p fn(const Run &) for @p samples (the oldest first) in a
                 *        plot @p width × @p height of `columns(width, slots)` columns,
                 *        the newest in the rightmost; those that don't fit (the oldest)
                 *        are left out.
                 */
                template <typename Fn>
                static void forEachRun(Span<const int32_t> samples, int64_t maximum, uint16_t width, uint16_t height,
                                       size_t slots, Fn &&fn) {
                        const size_t count = columns(width, slots);
                        const size_t n = samples.size() < count ? samples.size() : count;
                        const size_t first = samples.size() - n;
                        Run          run;
                        for (size_t i = 0; i < n; ++i) {
                                const size_t   k = count - n + i;
                                const uint16_t h = barHeight(samples[first + i], maximum, height);
                                const uint16_t x = columnX(k, count, width);
                                const uint16_t w = uint16_t(columnX(k + 1, count, width) - x);
                                if (run.width != 0 && h == run.height && x == run.x + run.width) {
                                        run.width = uint16_t(run.width + w);
                                        continue;
                                }
                                if (run.width != 0 && run.height != 0) fn(static_cast<const Run &>(run));
                                run = Run{x, w, h};
                        }
                        if (run.width != 0 && run.height != 0) fn(static_cast<const Run &>(run));
                        return;
                }
};

BMDS_NAMESPACE_END
