/**
 * @file      icontheme.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/icon.h>
#include <bmds/memspace.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Icons by name, decoded and kept (`devplan/gui.md` §4h, §4j).
 *
 * A name (`utilities-terminal`, as a Desktop Entry's `Icon=`) is looked for
 * as `SIDExSIDE/NAME.png` in `/boot/icons` (the user's), then the theme's
 * directory on the card (`/sd0/p0/share/icons/THEME`, `DefaultTheme` unless
 * set), then the theme it inherits (its `index.theme`'s `Inherits=`, the
 * first), then `DefaultTheme`, then `hicolor`; an icon of another size there
 * is passed over. An absolute path is read as it is. A
 * program's own icon comes from the program (`ProgramIcons`). An icon found
 * is kept, so it costs its file once: one allocation each, its name and its
 * pixels together, on a list (nothing until the first). Past `MaxBytes` kept,
 * the one used longest ago goes. One not found isn't kept: it is looked for
 * again when asked for again (a card mounted since).
 *
 * @par Thread Safety
 * One user at a time (a GUI application's thread).
 */
class IconTheme {
        public:
                /**
                 * @brief Bytes of icons kept at most (about 60 of 16 × 16, or 15 of
                 *        32 × 32); past them, the icon used longest ago goes. An icon
                 *        returned stays valid while it is among those kept: far more
                 *        than a desktop shows at once.
                 */
                static constexpr size_t MaxBytes = 64 * 1024;

                /** @brief The longest name or path kept. */
                static constexpr size_t MaxName = 95;

                /** @brief The longest theme name. */
                static constexpr size_t MaxTheme = 31;

                /** @brief The theme used unless set, and the last one looked in. */
                static constexpr const char *DefaultTheme = "flat-color";

                /** @brief Where themes are (one directory each). */
                static constexpr const char *ThemesDir = "/sd0/p0/share/icons";

                /** @brief The largest side read (pixels). */
                static constexpr uint16_t MaxSide = 64;

                IconTheme() = default;
                ~IconTheme();
                IconTheme(const IconTheme &) = delete;
                IconTheme &operator=(const IconTheme &) = delete;

                /**
                 * @brief Uses theme @p name (its directory under `ThemesDir`: `flat-color`,
                 *        `glyphs`); forgets the icons kept (none may be in use).
                 */
                void setTheme(StringView name);

                /**
                 * @brief Returns icon @p name (or an absolute path to a PNG) of
                 *        @p side × @p side pixels, or null (none of that size).
                 */
                const Icon *find(StringView name, uint16_t side);

                /**
                 * @brief Returns the icon built into the program at @p path (a path
                 *        as the launcher runs it), nearest @p side and no bigger
                 *        than `MaxSide`, or null.
                 */
                const Icon *program(StringView path, uint16_t side);

                /**
                 * @brief A launcher entry's icon: @p name if found, else the
                 *        program's own (@p path), else `application-x-executable`
                 *        (null without even that).
                 */
                const Icon *forProgram(StringView name, StringView path, uint16_t side);

                /** @brief Forgets every icon kept (their pixels freed: none of them may be in use). */
                void clear();

                /** @brief Returns the bytes kept now (icons, their names and headers). */
                size_t bytesKept() const { return _bytes; }

        private:
                // One icon kept: this, then its name (NUL-ended), then its pixels (4-byte aligned).
                struct Kept {
                                Kept    *next;
                                Icon     icon;
                                uint32_t age;
                                uint32_t bytes; // the whole allocation
                                uint16_t side;
                                bool     fromProgram;
                                uint8_t  keyLength;

                                const char *key() const { return reinterpret_cast<const char *>(this + 1); }
                };

                Kept       *lookup(StringView key, uint16_t side, bool fromProgram);
                void        readInherits();
                void        evictFor(size_t bytes); // the oldest freed until @p bytes more fit
                const Icon *keep(StringView key, uint16_t side, bool fromProgram, const uint8_t *png, size_t bytes);

                Kept    *_first = nullptr; // the icons kept, the newest first
                size_t   _bytes = 0;
                char     _theme[MaxTheme + 1] = "flat-color"; // DefaultTheme
                char     _inherits[MaxTheme + 1] = {};
                bool     _inheritsRead = false;
                uint32_t _clock = 0;
};

BMDS_NAMESPACE_END
