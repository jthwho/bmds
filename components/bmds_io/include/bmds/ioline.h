/**
 * @file      ioline.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A line request's config (devplan/io.md §12.1): Linux's GPIO v2 `gpio_v2_line_config`, checked
 * by Linux's rules and turned into each line's settings. Pure, host-tested; `GpioLines` applies
 * them to the pads.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Linux's `gpio_v2_line_config`, the same layout (the ABI's `bmds_gpio_line_config_t`
 *        is copied into it): flags for every line, then attributes that apply by mask.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoLineConfig {
                static constexpr size_t MaxAttrs = 10; ///< Linux's `GPIO_V2_LINE_NUM_ATTRS_MAX`.
                static constexpr size_t MaxLines = 64; ///< Linux's `GPIO_V2_LINES_MAX`.

                /** @brief The flags (Linux's `GPIO_V2_LINE_FLAG_...`). */
                static constexpr uint64_t Used = 1ull << 0; ///< In line info only.
                static constexpr uint64_t ActiveLow = 1ull << 1;
                static constexpr uint64_t Input = 1ull << 2;
                static constexpr uint64_t Output = 1ull << 3;
                static constexpr uint64_t EdgeRising = 1ull << 4;
                static constexpr uint64_t EdgeFalling = 1ull << 5;
                static constexpr uint64_t OpenDrain = 1ull << 6;
                static constexpr uint64_t OpenSource = 1ull << 7;
                static constexpr uint64_t PullUp = 1ull << 8;
                static constexpr uint64_t PullDown = 1ull << 9;
                static constexpr uint64_t BiasDisabled = 1ull << 10;
                static constexpr uint64_t ClockRealtime = 1ull << 11;
                static constexpr uint64_t ClockHte = 1ull << 12;

                /** @brief What an attribute is (Linux's `GPIO_V2_LINE_ATTR_ID_...`). */
                static constexpr uint32_t AttrFlags = 1;
                static constexpr uint32_t AttrOutputValues = 2;
                static constexpr uint32_t AttrDebounce = 3;

                /** @brief An attribute and the lines it is for (a bit per line of the request). */
                struct Attr {
                                uint32_t id = 0;
                                uint32_t padding = 0;
                                uint64_t value = 0; ///< Flags, output values (a bit per line), or debounce µs.
                                uint64_t mask = 0;
                };

                uint64_t flags = 0;
                uint32_t attrCount = 0;
                uint32_t padding[5] = {};
                Attr     attrs[MaxAttrs];
};

static_assert(sizeof(IoLineConfig) == 272, "Linux's gpio_v2_line_config");

/**
 * @brief One line's settings, from a config.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoLineSettings {
                enum class Direction : uint8_t {
                        AsIs, ///< Neither asked: the pad's as it is.
                        Input,
                        Output
                };
                enum class Drive : uint8_t {
                        PushPull,
                        OpenDrain,
                        OpenSource ///< Emulated: driven high, let go (an input) for low.
                };
                enum class Bias : uint8_t {
                        AsIs, ///< None asked: the pad's pulls as they are.
                        PullUp,
                        PullDown,
                        Disabled
                };
                /** @brief Logical edges (active low applied), as `IoPinSetup::Edge`'s bits. */
                enum Edge : uint8_t {
                        NoEdges = 0,
                        Rising = 1u << 0,
                        Falling = 1u << 1,
                };

                Direction direction = Direction::AsIs;
                Drive     drive = Drive::PushPull;
                Bias      bias = Bias::AsIs;
                bool      activeLow = false;
                uint8_t   edges = NoEdges;
                uint32_t  debounceUs = 0;
                bool      value = false;    ///< An output's logical value.
                bool      realtime = false; ///< Events stamped with CLOCK_REALTIME (else monotonic).

                /** @brief True if it is set up the same way (the value apart). */
                bool sameSetupAs(const IoLineSettings &o) const {
                        return direction == o.direction && drive == o.drive && bias == o.bias &&
                               activeLow == o.activeLow && edges == o.edges && debounceUs == o.debounceUs &&
                               realtime == o.realtime;
                }
};

namespace IoLine {

        /**
         * @brief Checks one line's @p flags as Linux does (`gpio_v2_line_flags_validate`).
         * @return `Ok`; `Invalid` (an unknown flag, `Used`; input and output; edges without
         *         input; a drive without output; open drain and source; a bias without a
         *         direction, or two; both clocks), `NotSupported` (the HTE clock).
         */
        Error checkFlags(uint64_t flags);

        /**
         * @brief Checks @p config for @p lineCount lines and gives each line's settings, in
         *        order, into @p out: a line's flags are the first flags attribute whose mask has
         *        it, else the config's; its output value the first output values attribute's bit,
         *        else low; its debounce the first debounce attribute's, else none.
         * @return `Ok`, or `checkFlags()`'s error for a line's flags; `Invalid` for a count, an
         *         attribute's id, padding not zero, a debounce past a second.
         */
        Error resolve(const IoLineConfig &config, size_t lineCount, IoLineSettings *out);

} // namespace IoLine

BMDS_NAMESPACE_END
