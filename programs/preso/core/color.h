/**
 * @file      color.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#ifndef PRESO_COLOR_H
#define PRESO_COLOR_H

#include "types.h"

namespace preso {

        /** @brief A colour, 0xRRGGBB. */
        using Color = uint32_t;

        /**
         * @brief Reads a colour as decks write them: `#rgb`, `#rrggbb`, or one of CSS's basic
         *        names (`white`, `navy`...). False if it isn't one.
         */
        bool parseColor(StringView text, Color *color);

        /** @brief Colours pictures are mapped to (a retro theme's: EGA's sixteen). */
        struct Palette {
                        const Color *colors = nullptr;
                        uint32_t     count = 0;
        };

        /** @brief @p a and @p b mixed, @p amount of @p b (0-256). */
        Color mixColors(Color a, Color b, uint32_t amount);

} // namespace preso

#endif // PRESO_COLOR_H
