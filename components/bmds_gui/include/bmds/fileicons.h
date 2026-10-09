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
 * @brief Icons for file types, shared by a program's folder views
 *        (`devplan/filebrowser.md` §4h): each type's at 16 and 32 pixels, found
 *        once and copied (the theme's cache may drop its own).
 *
 * A type's icon is its own name's (`text-plain`), else its generic one
 * (`text-x-generic`), else `unknown`. One not found is remembered as none
 * until `clear()` (a new theme, a card mounted later), so a folder of
 * unknown files doesn't look for it on every redraw. Drawing asks with
 * `loadedForType()` and `loadedNamed()`: it runs on the window manager's task
 * (a small stack, the compositor waiting), so a theme's file is read on the
 * loop instead (`loadAsked()`).
 *
 * @par Thread Safety
 * The application's loop, except as `setAsked()` says.
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

                /** @brief Told an icon was asked for that isn't loaded (from any task, the application's lock held). */
                using Asked = void (*)(void *arg);

                /** @brief Has @p fn told when `loadedForType()` or `loadedNamed()` asks for an icon. */
                void setAsked(Asked fn, void *arg) {
                        _asked = fn;
                        _askedArg = arg;
                        return;
                }

                /**
                 * @brief As `forType()`, without reading a file: the icon if it is
                 *        loaded, else null and the load asked for (`setAsked()`). For
                 *        drawing, which runs on the window manager's task.
                 */
                const Icon *loadedForType(StringView type, uint16_t side);

                /** @brief As `named()`, without reading a file (as `loadedForType()`). */
                const Icon *loadedNamed(StringView name, uint16_t side);

                /** @brief Loads the icons asked for. @return True if any was looked for. */
                bool loadAsked();

                /**
                 * @brief Returns @p icon (one returned here) dimmed: its alpha halved, made
                 *        once (a cut file's, `devplan/filebrowser.md` §4j). @p icon itself
                 *        if it isn't one of these, or without the memory.
                 */
                const Icon *dimmed(const Icon *icon);

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
                                Icon      dim;
                                uint32_t *dimPixels = nullptr; // PSRAM, made when first asked for
                };

                // One asked for while drawing, to load on the loop.
                struct Wanted {
                                char     key[MaxKey + 1] = {};
                                uint16_t side = 0;
                                bool     isName = false;
                };

                const Icon *find(StringView key, uint16_t side, bool isName);
                const Kept *kept(StringView key, uint16_t side, bool isName) const;
                const Icon *loaded(StringView key, uint16_t side, bool isName);

                IconTheme   &_theme;
                List<Kept *> _kept{MemSpace::Psram};
                List<Wanted> _wanted{MemSpace::Psram};
                Asked        _asked = nullptr;
                void        *_askedArg = nullptr;
};

BMDS_NAMESPACE_END
