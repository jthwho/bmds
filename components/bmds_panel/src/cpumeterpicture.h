/**
 * @file      cpumeterpicture.h
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
#include <bmds/stat.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's CPU meter's 16 × 16 picture, pure (`devplan/sysmon.md`
 *        §3.5, step 5): a 2-pixel frame (a grey pixel and a black one), inside
 *        it the last `Samples` samples as bars from the bottom, each a share of
 *        a whole core, rounded to the nearest pixel (an idle core shows none).
 *
 * `Form::Cores` puts core 0 in the top half and core 1 in the bottom (bars
 * up to `BandBars` pixels, the band's top row left as a gap); `Form::Combined`
 * draws the cores' mean over the whole height. An alarm (the unseen problems'
 * worst health) colours both of the frame's pixels; CPU time not
 * counted leaves out the bars and halves the field's alpha.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct CpuMeterPicture {
                /** @brief The picture's side, in pixels. */
                static constexpr uint16_t Side = 16;

                /** @brief The frame's width. */
                static constexpr uint16_t Frame = 2;

                /** @brief The samples shown: the inside of the frame. */
                static constexpr size_t Samples = Side - 2 * Frame;

                /** @brief A core's band in `Form::Cores`, and its tallest bar. */
                static constexpr uint16_t Band = Samples / 2;
                static constexpr uint16_t BandBars = Band - 1;

                /** @brief The colours (0xAARRGGBB). */
                static constexpr uint32_t FieldColor = 0xff000000;
                static constexpr uint32_t BarColor = 0xff00d000;
                static constexpr uint32_t FrameColor = 0xff808080;
                static constexpr uint32_t WarnColor = 0xffffb000;
                static constexpr uint32_t CritColor = 0xffff2020;

                /** @brief How the cores are drawn (`panel/tray/cpu-graph`). */
                enum class Form : uint8_t {
                        Cores,    ///< A band each (the default).
                        Combined, ///< Their mean, the whole height.
                };

                /** @brief What it shows. */
                struct Input {
                                Span<const int32_t> cores[2]; ///< Each core's samples, the oldest first.
                                size_t              coreCount = 0;
                                int64_t      full = 1000000; ///< A sample of a whole core (busy all the interval).
                                Stat::Health alarm = Stat::Health::Normal;
                                bool         counted = true; ///< CPU time is counted.
                                Form         form = Form::Cores;
                };

                /** @brief Draws @p in into @p out (rows top first). */
                static void draw(const Input &in, uint32_t (&out)[Side * Side]);
};

BMDS_NAMESPACE_END
