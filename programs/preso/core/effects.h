/**
 * @file      effects.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A theme's effects over a finished slide (devplan/preso.md §5): its scanlines. Done as each
 * step is drawn (ahead of time), and over what a frame redraws.
 */

#ifndef PRESO_EFFECTS_H
#define PRESO_EFFECTS_H

#include "canvas.h"
#include "theme.h"

namespace preso {

        /** @brief Every other row darkened by @p strength (of 255). */
        void applyScanlines(Canvas &canvas, uint8_t strength);

        /** @brief @p theme's effects (its scanlines). */
        void applyEffects(Canvas &canvas, const Theme &theme);

} // namespace preso

#endif // PRESO_EFFECTS_H
