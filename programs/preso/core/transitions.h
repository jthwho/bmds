/**
 * @file      transitions.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Going from one slide to the next (devplan/preso.md §6): `transition: NAME [DIRECTION]
 * [TIME]`. A frame is pieces of the two slides' pictures, already drawn, and fills: no slide is
 * drawn again while it plays. On the board the 2D-DMA copies the pieces: the CPU copies PSRAM to
 * PSRAM at about 28 MB/s each way (a whole frame in 64 ms), the DMA at about 150.
 */

#ifndef PRESO_TRANSITIONS_H
#define PRESO_TRANSITIONS_H

#include "canvas.h"
#include "platform.h"
#include "types.h"

namespace preso {

        enum class TransitionKind : uint8_t {
                Cut,      ///< None: the next slide at once.
                Wipe,     ///< The next slide uncovered by an edge moving across.
                Dissolve, ///< The next slide coming through in an ordered-dither pattern.
                Pixelate, ///< The slide breaks into blocks, which become the next.
                Crt,      ///< A tube switched off (a line, a dot) and on again with the next.
                Type,     ///< The next slide's text typed out.
        };

        enum class Direction : uint8_t {
                Left,
                Right,
                Up,
                Down
        };

        struct Transition {
                        TransitionKind kind = TransitionKind::Cut;
                        Direction      direction = Direction::Right; ///< A wipe's: where its edge goes.
                        uint32_t       durationMs = 400;             ///< `type`'s: the most it takes.
        };

        /**
         * @brief Reads a `transition` directive (`wipe left 0.6s`, `dissolve`, `none`): a name,
         *        then a direction and a time in either order. False (and @p why) if it isn't one.
         */
        bool parseTransition(StringView text, Transition *transition, const char **why);

        /** @brief The most pieces a frame of a transition takes. */
        constexpr uint32_t MaxPieces = 4096;

        /**
         * @brief @p t at @p progress (0 to 65536: the start to the end) as pieces of the two
         *        slides' pictures (@p width × @p height) into @p out (MaxPieces of room): their
         *        number, or UINT32_MAX for a transition that isn't pieces (`pixelate`: the DMA
         *        doesn't stretch; composeTransition() draws it). With @p since (a progress the
         *        screen buffer already shows), only what differs from it: a wipe's strip, the
         *        cells dissolved since (`crt` changes everywhere: whole).
         */
        uint32_t transitionPieces(const Transition &t, uint32_t progress, int32_t width, int32_t height, Piece *out,
                                  uint32_t since = UINT32_MAX);

        /**
         * @brief Pixelate's frame at @p progress (for slides @p width wide) as a number: its block
         *        size and which slide. Frames of the same stage are the same picture.
         */
        uint32_t pixelateStage(uint32_t progress, int32_t width);

        /** @brief Draws @p count pieces of @p from and @p to onto @p out (the CPU's way). */
        void applyPieces(Canvas &out, const Canvas &from, const Canvas &to, const Piece *pieces, uint32_t count);

        /**
         * @brief Composes @p t at @p progress from @p from and @p to (the same size and format as
         *        @p out) into @p out, by the CPU. `type` is the presenter's (it draws text); here
         *        it is the next slide.
         */
        void composeTransition(const Transition &t, uint32_t progress, const Canvas &from, const Canvas &to,
                               Canvas &out);

} // namespace preso

#endif // PRESO_TRANSITIONS_H
