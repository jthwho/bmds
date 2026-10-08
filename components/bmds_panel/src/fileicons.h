/**
 * @file      fileicons.h
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
#include <bmds/icontheme.h>
#include <bmds/list.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The panel's icons for file types, shared by its folder windows
 *        (`devplan/filebrowser.md` §4h): each type's at 16 and 32 pixels, found
 *        once and copied (the theme's cache may drop its own).
 *
 * A type's icon is its own name's (`text-plain`), else its generic one
 * (`text-x-generic`), else `unknown`. One not found is remembered as none
 * until `clear()` (a new theme, a card mounted later), so a folder of
 * unknown files doesn't look for it on every redraw.
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FileIcons {
        public:
                explicit FileIcons(IconTheme &theme) : _theme(theme) {}
                ~FileIcons() { clear(); }
                FileIcons(const FileIcons &) = delete;
                FileIcons &operator=(const FileIcons &) = delete;

                /** @brief Returns type @p type's icon of @p side pixels (16 or 32), or null. */
                const Icon *forType(StringView type, uint16_t side);

                /** @brief Returns icon @p name of @p side pixels (`folder`), or null. */
                const Icon *named(StringView name, uint16_t side);

                /** @brief Forgets every icon (none may be in use). */
                void clear();

                /** @brief Forgets the icons not found (a theme on a card mounted since may have them). */
                void forgetMissing();

        private:
                static constexpr size_t MaxKey = 63;

                // One looked for: its copy, or none.
                struct Kept {
                                char      key[MaxKey + 1] = {};
                                uint16_t  side = 0;
                                bool      isName = false; // looked up by name, not by type
                                Icon      icon;
                                uint32_t *pixels = nullptr; // PSRAM, or null: not found
                };

                const Icon *find(StringView key, uint16_t side, bool isName);

                IconTheme   &_theme;
                List<Kept *> _kept{MemSpace::Psram};
};

BMDS_NAMESPACE_END
