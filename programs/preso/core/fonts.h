/**
 * @file      fonts.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#ifndef PRESO_FONTS_H
#define PRESO_FONTS_H

#include "platform.h"
#include "theme.h"

namespace preso {

        /**
         * @brief The fonts a deck draws in, opened through the platform as they are first
         *        wanted and kept, the least recently used closed past MaxOpen (BMDS gives a
         *        program 16 font handles).
         *
         * A font that won't open is the theme's body family at that size, else a guess: its
         * text measured as if each character were half its size wide, and not drawn.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class FontCache {
                public:
                        static constexpr uint32_t MaxOpen = 12;

                        explicit FontCache(Platform &platform) : _platform(platform) {}
                        ~FontCache() { clear(); }
                        FontCache(const FontCache &) = delete;
                        FontCache &operator=(const FontCache &) = delete;

                        FontMetrics metrics(const FontKey &key);
                        int32_t     width(const FontKey &key, StringView text);
                        uint32_t    fit(const FontKey &key, StringView text, int32_t maxWidth);
                        void        draw(Canvas &canvas, const FontKey &key, int32_t x, int32_t y, StringView text,
                                         Color color);

                        /** @brief Closes every font. */
                        void clear();
                        /** @brief How many fonts wouldn't open (for `preso check`). */
                        uint32_t failures() const { return _failures; }

                private:
                        struct Entry {
                                        FontKey     key; ///< Its family is `family`: a script's font may go first.
                                        char        family[64] = {};
                                        int32_t     handle = -1;
                                        FontMetrics metrics;
                                        uint32_t    used = 0;
                        };

                        Entry &get(const FontKey &key);

                        Platform &_platform;
                        Entry     _entries[MaxOpen];
                        uint32_t  _count = 0;
                        uint32_t  _clock = 0;
                        uint32_t  _failures = 0;
        };

} // namespace preso

#endif // PRESO_FONTS_H
