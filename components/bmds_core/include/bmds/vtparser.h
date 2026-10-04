/**
 * @file      vtparser.h
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
#include <bmds/stringview.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

/** @brief Parameters of a control sequence (`ESC [ 1 ; 38 : 2 : ... m`). */
struct VtParams {
                static constexpr size_t MaxParams = 16;

                uint16_t values[MaxParams] = {}; ///< Missing values are 0.
                uint16_t subparams = 0;          ///< Bit i: values[i] followed a `:` (a sub-parameter).
                uint8_t  count = 0;              ///< Values present (0: none written).
                uint8_t  privateMarker = 0;      ///< `<`, `=`, `>` or `?` right after `[`, or 0.

                /** @brief Returns value @p i, or @p fallback if it is missing or 0. */
                uint16_t get(size_t i, uint16_t fallback) const {
                        return i < count && values[i] != 0 ? values[i] : fallback;
                }

                /** @brief Returns true if value @p i is a sub-parameter. */
                bool isSub(size_t i) const { return i < MaxParams && (subparams & (1u << i)) != 0; }
};

/** @brief Receives what a `VtParser` recognizes. */
class VtHandler {
        public:
                /** @brief A printable character. */
                virtual void print(uint32_t codepoint) = 0;

                /** @brief A C0 control (BEL, BS, HT, LF, CR...). */
                virtual void execute(uint8_t control) = 0;

                /** @brief A complete control sequence: `ESC [ params intermediates final`. */
                virtual void csiDispatch(const VtParams &params, StringView intermediates, uint8_t final) = 0;

                /** @brief An escape sequence: `ESC intermediates final`. */
                virtual void escDispatch(StringView intermediates, uint8_t final) = 0;

                /** @brief An operating system command, `ESC ] data BEL` (truncated to `VtParser::MaxOsc`). */
                virtual void oscDispatch(StringView data) = 0;

        protected:
                ~VtHandler() = default;
};

/**
 * @brief Parses a terminal output stream: text, controls, escape sequences.
 *
 * The ECMA-48 / DEC state machine (Paul Williams' model): ground, escape,
 * CSI entry/param/intermediate/ignore, OSC string, and DCS/SOS/PM/APC
 * strings (consumed and ignored). Text is decoded as UTF-8. CAN and SUB
 * abort a sequence; ESC starts a new one anywhere. Malformed input never
 * stalls it, and its state is fixed-size (no allocation).
 *
 * @par Thread Safety
 * Not thread-safe; one parser per stream.
 */
class VtParser {
        public:
                /** @brief Longest OSC string kept. */
                static constexpr size_t MaxOsc = 64;

                /** @brief Most intermediate bytes kept. */
                static constexpr size_t MaxIntermediates = 2;

                explicit VtParser(VtHandler &handler) : _handler(handler) {}

                /** @brief Processes one byte. */
                void feed(uint8_t b);

                /** @brief Processes bytes. */
                void feed(StringView text) {
                        for (char c : text) feed(static_cast<uint8_t>(c));
                        return;
                }

                /** @brief Returns true between sequences and characters (nothing pending). */
                bool isGround() const { return _state == State::Ground && !_utf8.isPending(); }

                /** @brief Returns to the ground state, dropping anything partial. */
                void reset();

        private:
                enum class State : uint8_t {
                        Ground,
                        Escape,
                        EscapeIntermediate,
                        CsiEntry,
                        CsiParam,
                        CsiIntermediate,
                        CsiIgnore,
                        OscString,
                        IgnoredString, // DCS, SOS, PM, APC: until ST
                };

                void clear();
                void collect(uint8_t b);
                void param(uint8_t b);
                void dispatchCsi(uint8_t final);

                VtHandler  &_handler;
                Utf8Decoder _utf8;
                VtParams    _params;
                bool        _paramStarted = false;
                char        _intermediates[MaxIntermediates] = {};
                uint8_t     _intermediateCount = 0;
                char        _osc[MaxOsc] = {};
                uint8_t     _oscLength = 0;
                State       _state = State::Ground;
};

/**
 * @brief SGR (`ESC [ ... m`): applying it to a style, and producing it.
 */
namespace Sgr {

        /** @brief Longest sequence `transition()` produces. */
        inline constexpr size_t MaxSequence = 64;

        /**
         * @brief Applies SGR parameters to @p style: attributes, the 8/16 colors,
         *        `38;5;n`, `38;2;r;g;b` (and their `:` forms), defaults, reset.
         */
        void apply(const VtParams &params, TextStyle &style);

        /**
         * @brief Writes the shortest SGR sequence that turns @p from into @p to on a
         *        terminal of @p depth (colors are downgraded to it).
         * @return The length; 0 if nothing changes.
         */
        size_t transition(const TextStyle &from, const TextStyle &to, ColorDepth depth, char (&out)[MaxSequence]);

} // namespace Sgr

BMDS_NAMESPACE_END
