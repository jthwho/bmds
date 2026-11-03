/**
 * @file      effects.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A theme's effects over a finished slide (devplan/preso.md §5): a phosphor's glow, then its
 * scanlines. Done as each step is drawn (ahead of time), not per frame.
 */

#ifndef PRESO_EFFECTS_H
#define PRESO_EFFECTS_H

#include "canvas.h"
#include "theme.h"

namespace preso {

        /**
         * @brief The glow: what is brighter than the theme's background, blurred (at half the
         *        size each way: a sixteenth of the work) and added back, @p strength of 255 meaning twice
 *        the blur (a thin stroke's blur is faint).
         * @return false if there wasn't the memory (the slide is then as it was).
         */
        bool applyGlow(Canvas &canvas, Color background, uint8_t strength, uint8_t radius);

        /** @brief Every other row darkened by @p strength (of 255). */
        void applyScanlines(Canvas &canvas, uint8_t strength);

        /** @brief @p theme's effects, in their order (glow, then scanlines). */
        void applyEffects(Canvas &canvas, const Theme &theme);

} // namespace preso

#endif // PRESO_EFFECTS_H
