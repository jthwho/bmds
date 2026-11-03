/**
 * @file      headless.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "headless.h"

namespace preso {

        int32_t HeadlessPlatform::openFont(const char *name) {
                // The size: the name's last number ("IBM Plex Sans 30 bold").
                StringView n(name);
                int32_t    size = 0;
                forEachWord(n, [&](StringView word) {
                        int32_t v = word.toInt(-1);
                        if (v > 0) size = v;
                });
                if (size < 4 || size > 255) return -1;
                for (uint32_t i = 0; i < MaxFonts; i++) {
                        if (_sizes[i] == 0) {
                                _sizes[i] = uint16_t(size);
                                return int32_t(i);
                        }
                }
                return -1;
        }

        void HeadlessPlatform::closeFont(int32_t font) {
                if (font >= 0 && uint32_t(font) < MaxFonts) _sizes[font] = 0;
                return;
        }

        uint32_t HeadlessPlatform::openFonts() const {
                uint32_t n = 0;
                for (uint16_t s : _sizes) n += s != 0;
                return n;
        }

        FontMetrics HeadlessPlatform::fontMetrics(int32_t font) {
                FontMetrics m;
                int32_t     size = _sizes[font];
                m.ascent = size * 4 / 5;
                m.descent = size - m.ascent;
                m.lineHeight = size * 5 / 4;
                return m;
        }

        int32_t HeadlessPlatform::textWidth(int32_t font, StringView text) {
                int32_t n = 0;
                for (uint32_t i = 0; i < text.length(); i++) {
                        if ((uint8_t(text[i]) & 0xC0) != 0x80) n++;
                }
                return n * advance(font);
        }

        uint32_t HeadlessPlatform::textFit(int32_t font, StringView text, int32_t maxWidth) {
                int32_t  x = 0;
                uint32_t i = 0;
                while (i < text.length()) {
                        uint32_t next = i + 1;
                        while (next < text.length() && (uint8_t(text[next]) & 0xC0) == 0x80) next++;
                        if (x + advance(font) > maxWidth) break;
                        x += advance(font);
                        i = next;
                }
                return i;
        }

        void HeadlessPlatform::drawText(Canvas &canvas, int32_t font, int32_t x, int32_t y, StringView text,
                                        Color color) {
                // Each character a block of its colour, as tall as a lower-case letter.
                FontMetrics m = fontMetrics(font);
                int32_t     a = advance(font);
                int32_t     n = 0;
                for (uint32_t i = 0; i < text.length(); i++) {
                        if ((uint8_t(text[i]) & 0xC0) == 0x80) continue;
                        if (text[i] != ' ')
                                canvas.fill(Rect{x + n * a, y + m.ascent / 3, a - 1, m.ascent * 2 / 3}, color);
                        n++;
                }
                return;
        }

} // namespace preso
