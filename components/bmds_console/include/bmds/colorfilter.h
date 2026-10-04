/**
 * @file      colorfilter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/color.h>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/vtparser.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Rewrites SGR color sequences in terminal output for a terminal of
 *        lesser color depth.
 *
 * Text and every other sequence pass through byte for byte; each SGR
 * sequence is replaced by the shortest one that produces the same style at
 * the target depth (24-bit colors become their nearest 256- or 16-color
 * match; at `None`, colors go and attributes stay). A sequence too long to
 * hold (a long OSC title) passes through unchanged.
 *
 * A console uses one only while its depth is below `TrueColor`, so output
 * at full depth is never parsed. About 200 bytes of state.
 *
 * @par Thread Safety
 * Not thread-safe: the console serializes its writes.
 */
class ColorFilter final : private VtHandler {
        public:
                /** @brief Receives the filtered output. */
                using Sink = void (*)(void *context, ByteSpan data);

                /** @brief Most raw sequence bytes held while deciding (longer ones pass through). */
                static constexpr size_t MaxHeld = 96;

                explicit ColorFilter(ColorDepth depth) : _parser(*this), _depth(depth) {}

                ColorFilter(const ColorFilter &) = delete;
                ColorFilter &operator=(const ColorFilter &) = delete;

                /** @brief Changes the target depth. */
                void setDepth(ColorDepth depth) {
                        _depth = depth;
                        return;
                }

                /** @brief Returns the target depth. */
                ColorDepth depth() const { return _depth; }

                /**
                 * @brief Filters @p data, passing the result to @p sink in pieces.
                 *
                 * A sequence split across calls is held until it completes.
                 */
                void process(ByteSpan data, Sink sink, void *context);

        private:
                void print(uint32_t) override {}
                void execute(uint8_t) override {}
                void csiDispatch(const VtParams &params, StringView intermediates, uint8_t final) override;
                void escDispatch(StringView, uint8_t) override {}
                void oscDispatch(StringView) override {}

                VtParser   _parser;
                ColorDepth _depth;
                TextStyle  _in;                  // the style the program has asked for
                TextStyle  _out;                 // the style sent on
                bool       _sgr = false;         // the held sequence was an SGR
                bool       _passThrough = false; // the held sequence overflowed: pass the rest on
                uint8_t    _held[MaxHeld];
                size_t     _heldLength = 0;
};

BMDS_NAMESPACE_END
