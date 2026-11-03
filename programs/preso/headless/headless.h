/**
 * @file      headless.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The platform without a screen (devplan/preso.md §2): files as every platform reads them, and
 * stand-in fonts: every character half its size wide, its lines a quarter more than its size apart,
 * drawn as a block. Layout is the same as with real fonts in all but the widths, so the host
 * tests lay out and draw decks with it. On the board, `preso check` uses the real fonts.
 */

#ifndef PRESO_HEADLESS_H
#define PRESO_HEADLESS_H

#include "../core/platform.h"

namespace preso {

        /**
         * @brief Stand-in fonts, no screen.
         *
         * @par Thread Safety
         * As Platform.
         */
        class HeadlessPlatform : public Platform {
                public:
                        static constexpr uint32_t MaxFonts = 16;

                        int32_t     openFont(const char *name) override;
                        void        closeFont(int32_t font) override;
                        FontMetrics fontMetrics(int32_t font) override;
                        int32_t     textWidth(int32_t font, StringView text) override;
                        uint32_t    textFit(int32_t font, StringView text, int32_t maxWidth) override;
                        void        drawText(Canvas &canvas, int32_t font, int32_t x, int32_t y, StringView text,
                                             Color color) override;

                        /** @brief Fonts open now (the tests check that none leak). */
                        uint32_t openFonts() const;

                private:
                        int32_t advance(int32_t font) const { return _sizes[font] / 2; }

                        uint16_t _sizes[MaxFonts] = {}; // 0: not open
        };

} // namespace preso

#endif // PRESO_HEADLESS_H
