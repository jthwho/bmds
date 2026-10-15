/**
 * @file      padmapping.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/padlayout.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The standard pad (`devplan/controls.md` §4a): every pad presented
 *        as one layout, its face buttons by position.
 *
 * `L2` and `R2` are both: an axis (0 to 32767) and a button, down past a
 * quarter of the way.
 *
 * @par Thread Safety
 * A plain value.
 */
struct PadState {
                /** @brief Buttons, by bit. The names are an interface (bindings) once shipped. */
                enum Button : uint8_t {
                        South,
                        East,
                        West,
                        North,
                        L1,
                        R1,
                        L2,
                        R2,
                        L3,
                        R3,
                        Up,
                        Down,
                        Left,
                        Right,
                        Start,
                        Select,
                        Guide,
                        ButtonCount
                };

                /** @brief Axes. Sticks run −32767 (left, up) to 32767; triggers 0 to 32767. */
                enum Axis : uint8_t {
                        LeftX,
                        LeftY,
                        RightX,
                        RightY,
                        LeftTrigger,
                        RightTrigger,
                        AxisCount
                };

                uint32_t buttons = 0; ///< Bit N: `Button` N down.
                int16_t  axes[AxisCount] = {};

                /** @brief Returns true if @p button is down. */
                constexpr bool isDown(Button button) const { return (buttons >> button & 1u) != 0; }

                /** @brief Returns @p button's name (`South`, `L1`, `Start`...). */
                static const char *buttonName(Button button);

                /** @brief Returns @p axis's name (`LeftX`, ..., `L2`, `R2`). */
                static const char *axisName(Axis axis);
};

/**
 * @brief How a pad's raw controls make the standard pad.
 *
 * Written as SDL's `gamecontrollerdb.txt` writes a mapping (`a:b1,b:b2,
 * dpleft:-a3,leftx:a0,lefty:a1~,dpup:h0.1`; SDL's `a`, `b`, `x`, `y` are by
 * position: South, East, West, North), kept as a fixed table.
 *
 * @par Thread Safety
 * A plain value.
 */
class PadMapping {
        public:
                /** @brief Where a standard control comes from. */
                struct Source {
                                enum class Kind : uint8_t {
                                        None,
                                        Button,    ///< `bN`.
                                        Axis,      ///< `aN`, `aN~` inverted.
                                        AxisPlus,  ///< `+aN`: its upper half.
                                        AxisMinus, ///< `-aN`: its lower half.
                                        Hat,       ///< `hN.M`: hat N's direction bits M.
                                };
                                Kind    kind = Kind::None;
                                uint8_t index = 0;
                                uint8_t bits = 0; ///< A hat's direction bits.
                                bool    invert = false;
                };

                /** @brief Bytes `format()` may need, NUL included. */
                static constexpr size_t MaxText = 320;

                /**
                 * @brief The usual order (`devplan/controls.md` §7a) for @p layout: what a pad
                 *        without a database entry gets.
                 */
                static PadMapping standard(const PadLayout &layout);

                /**
                 * @brief Reads @p fields, a mapping's comma-separated `target:source` pairs (a
                 *        database line after its GUID and name). Fields it doesn't know
                 *        (`platform:`, SDL's newer targets) are skipped.
                 * @return `Invalid` if a known target's source doesn't parse.
                 */
                static Error parse(StringView fields, PadMapping &out);

                /**
                 * @brief Finds the line for @p vendor and @p product in @p database (SDL's line
                 *        format; lines for another platform than `BMDS` are skipped).
                 * @param name Set to the line's name when found (may be null).
                 * @return True if found and parsed.
                 */
                static bool find(StringView database, uint16_t vendor, uint16_t product, PadMapping &out,
                                 StringView *name);

                /**
                 * @brief Writes the mapping as `target:source` pairs, comma-terminated, in SDL's
                 *        order (by target name), into @p out.
                 * @return The length written (truncated to fit, NUL-terminated).
                 */
                size_t format(char *out, size_t size) const;

                /**
                 * @brief Makes @p state of @p raw: sources read, sticks given their dead zone.
                 * @hotpath
                 */
                void apply(const RawPad &raw, PadState &state) const;

                /** @brief Returns where @p button comes from (`L2` and `R2`: their axes'). */
                const Source &button(PadState::Button button) const;

                /** @brief Returns where @p axis comes from. */
                const Source &axis(PadState::Axis axis) const { return _axes[axis]; }

        private:
                Source _buttons[PadState::ButtonCount]; // L2 and R2 unused: their axes'
                Source _axes[PadState::AxisCount];
};

BMDS_NAMESPACE_END
