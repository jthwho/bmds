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
#include <bmds/iconid.h>
#include <bmds/iconpack.h>
#include <bmds/memspace.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Icons by number or name (`devplan/gui.md` §4h, §4j; `devplan/rom.md`
 *        §3f, §3i).
 *
 * **The bmds theme** (`DefaultTheme`) is one file in the ROM (`PackPath`), its
 * pixels decoded already and used where they lie: an icon of it costs no
 * memory and is never dropped. Every icon BMDS's own programs use is in it,
 * by number (`IconId`, `find(IconId, side)`: an index) and by name.
 *
 * **Another theme** (`setTheme()`: a directory of `ThemesDir` on the card)
 * comes first: a name is looked for as `SIDExSIDE/NAME.png` in `/cfg/icons`
 * (the user's), then the theme's directory, then the theme it inherits (its
 * `index.theme`'s `Inherits=`, the first), then the bmds theme, then
 * `hicolor`; an icon of another size there is passed over. An id is looked
 * for by its name in the chosen theme (and what it inherits), then in the
 * bmds theme. With the bmds theme chosen, a name is looked for in
 * `/cfg/icons`, then in it; an id is its icon at once.
 *
 * An absolute path is read as it is. A program's own icon comes from the
 * program (`ProgramIcons`). An icon read from a file is kept, so it costs its
 * file once: one allocation each, its name and its pixels together, on a list
 * (nothing until the first). Past `MaxBytes` kept, the one used longest ago
 * goes, except those found by id (kept until `setTheme()`). One not found
 * isn't kept: it is looked for again when asked for again (a card mounted
 * since).
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

                /** @brief The theme used unless set, and the last one looked in (in the ROM). */
                static constexpr const char *DefaultTheme = "bmds";

                /** @brief The bmds theme's file. */
                static constexpr const char *PackPath = "/rom/share/icons/bmds.icons";

                /** @brief Where themes are (one directory each). */
                static constexpr const char *ThemesDir = "/sd0/p0/share/icons";

                /** @brief The largest side read (pixels). */
                static constexpr uint16_t MaxSide = 64;

                IconTheme() = default;
                ~IconTheme();
                IconTheme(const IconTheme &) = delete;
                IconTheme &operator=(const IconTheme &) = delete;

                /**
                 * @brief Uses theme @p name (`bmds`, or its directory under `ThemesDir`:
                 *        `flat-color`, `glyphs`); forgets the icons kept (none may be in
                 *        use; the bmds theme's stay valid).
                 */
                void setTheme(StringView name);

                /** @brief Returns the theme used. */
                StringView theme() const { return StringView(_theme); }

                /**
                 * @brief Returns icon @p name (or an absolute path to a PNG) of
                 *        @p side × @p side pixels, or null (none of that size). It stays
                 *        valid while it is among those kept (`MaxBytes`), or for good if
                 *        `isPermanent()`.
                 */
                const Icon *find(StringView name, uint16_t side);

                /**
                 * @brief Returns icon @p id of @p side × @p side pixels (16 or 32), or
                 *        null (none of that size, or no ROM). It stays valid until
                 *        `setTheme()` (for good if `isPermanent()`).
                 */
                const Icon *find(IconId id, uint16_t side);

                /** @brief Returns true if @p icon is the bmds theme's (in the ROM: valid for good, no copy needed). */
                bool isPermanent(const Icon *icon) const;

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
                                bool     pinned; // found by id: not dropped until setTheme()
                                uint8_t  keyLength;

                                const char *key() const { return reinterpret_cast<const char *>(this + 1); }
                };

                Kept       *lookup(StringView key, uint16_t side, bool fromProgram);
                bool            isBmds() const;
                const IconPack *pack(); // the bmds theme, mapped the first time: null without the ROM
                const Icon     *packIcon(uint32_t id, uint16_t side);
                const Icon     *findFile(StringView name, uint16_t side, bool userToo); // the theme's files, kept
                const Icon     *readKept(const char *path, StringView key, uint16_t side, bool anySize);
                const Icon     *keepMapped(StringView key, uint16_t side, const Icon *icon, bool pinned);
                void        readInherits();
                void        evictFor(size_t bytes); // the oldest freed until @p bytes more fit
                const Icon *keep(StringView key, uint16_t side, bool fromProgram, const uint8_t *png, size_t bytes);

                Kept    *_first = nullptr; // the icons kept, the newest first
                size_t   _bytes = 0;
                IconPack _pack;
                Icon    *_packIcons = nullptr; // PSRAM, the first time: an Icon per id and size
                bool     _packTried = false;
                char     _theme[MaxTheme + 1] = "bmds"; // DefaultTheme
                char     _inherits[MaxTheme + 1] = {};
                bool     _inheritsRead = false;
                uint32_t _clock = 0;
};

BMDS_NAMESPACE_END
