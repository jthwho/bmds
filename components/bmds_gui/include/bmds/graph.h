/**
 * @file      graph.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/graphlayout.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A graph of samples over time (`devplan/sysmon.md` §3.5): one or
 *        more series drawn as bars from the bottom, the newest at the right,
 *        each sample a column (`GraphLayout`), on a dark field with lines at
 *        the quarters; an optional warning and critical level drawn across in
 *        amber and red; a caption at the top left; a border in amber or red
 *        while its health says so. The painter has no lines: everything is
 *        fills, runs of equal height merged.
 *
 * Series are drawn in order, a later one in front. The scale is fixed
 * (`setMaximum()`) or follows the largest sample shown (rounded up to 1, 2 or
 * 5 times a power of ten, never below the levels).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Graph : public Widget {
        public:
                /** @brief Most series. */
                static constexpr size_t MaxSeries = 3;

                /** @brief Most samples kept per series. */
                static constexpr size_t Capacity = 240;

                /** @brief Longest caption, in bytes. */
                static constexpr size_t MaxCaption = 47;

                /** @brief No level drawn. */
                static constexpr int64_t NoLevel = INT64_MIN;

                /** @brief How it stands (its border). */
                enum class Health : uint8_t {
                        Normal,
                        Warning,
                        Critical,
                };

                explicit Graph(Widget *parent);

                /**
                 * @brief Sets series @p series' samples, the oldest first (the newest
                 *        `Capacity` kept), and redraws.
                 */
                void setSamples(size_t series, Span<const int32_t> samples);

                /** @brief Sets how many series there are (1 by default, `MaxSeries` at most). */
                void setSeriesCount(size_t count);

                /** @brief Sets series @p series' colour (0xRRGGBB). */
                void setColor(size_t series, uint32_t rgb);

                /** @brief Sets the scale's top (0: automatic). */
                void setMaximum(int64_t maximum);

                /** @brief Sets the samples a full width shows (each column its share; 120 by default). */
                void setSlots(size_t slots);

                /** @brief Sets the levels drawn across (`NoLevel`: none). */
                void setLevels(int64_t warn, int64_t crit);

                /** @brief Sets the caption (cut at `MaxCaption` bytes on a character). */
                void setCaption(StringView caption);

                /** @brief Sets its health (its border). */
                void setHealth(Health health);

                /** @brief Sets the height it asks a layout for (64 by default). */
                void setPreferredHeight(uint16_t height) { _height = height; }

                /** @brief Returns the scale's top as last drawn. */
                int64_t scale() const;

                GuiSize sizeHint() const override { return GuiSize{160, _height}; }
                void    paint(Painter &painter) override;

        private:
                int32_t  _samples[MaxSeries][Capacity] = {};
                uint16_t _counts[MaxSeries] = {};
                uint32_t _colors[MaxSeries] = {0x00e000, 0x00a0ff, 0xffffff};
                int64_t  _maximum = 0;
                int64_t  _warn = NoLevel;
                int64_t  _crit = NoLevel;
                char     _caption[MaxCaption + 1] = {};
                uint16_t _slots = 120;
                uint16_t _height = 64;
                uint8_t  _series = 1;
                Health   _health = Health::Normal;
};

BMDS_NAMESPACE_END
