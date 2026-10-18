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
#include <bmds/iconid.h>
#include <bmds/list.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Icons for file types, shared by a program's folder views
 *        (`devplan/filebrowser.md` §4h): each type's at 16 and 32 pixels, found
 *        once and copied (the theme's cache may drop its own).
 *
 * A type's icon is its own name's (`text-plain`), else its generic one
 * (`text-x-generic`), else `unknown`. The bmds theme's icons are in the ROM
 * (`IconTheme::isPermanent()`): those are used where they are, not copied. One not found is remembered as none
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

                /**
                 * @brief Returns icon @p id of @p side pixels, or null: the theme's own
                 *        (`IconTheme::find(IconId, uint16_t)`), valid until a new theme.
                 */
                const Icon *named(IconId id, uint16_t side) { return _theme.find(id, side); }

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

                /** @brief The longest application folder path looked up. */
                static constexpr size_t MaxAppPath = 95;

                /** @brief Returns the application folder @p folder's icon (`IconTheme::forAppFolder()`), or null. */
                const Icon *forApp(StringView folder, uint16_t side);

                /** @brief As `forApp()`, without reading a file (as `loadedForType()`). */
                const Icon *loadedForApp(StringView folder, uint16_t side);

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

                /**
                 * @brief Sets whether the trash has anything in it: `/trash` and links
                 *        to it are drawn full or empty (`devplan/filebrowser.md` §4k). The
                 *        owner follows it (a watch on `/trash`) and redraws.
                 */
                void setTrashFull(bool full) { _trashFull = full; }

                /** @brief Returns `setTrashFull()`'s. */
                bool isTrashFull() const { return _trashFull; }

        private:
                static constexpr size_t MaxKey = 95; // IconTheme::MaxName: an application folder's path

                // What a key is.
                enum class Kind : uint8_t {
                        Type,
                        Name,
                        App // an application folder's path
                };

                // One looked for: its copy, or none.
                struct Kept {
                                char      key[MaxKey + 1] = {};
                                uint16_t  side = 0;
                                Kind      kind = Kind::Type;
                                Icon      icon;           // invalid: not found
                                uint32_t *pixels = nullptr; // PSRAM: the copy (null: none, or the ROM's own)
                                Icon      dim;
                                uint32_t *dimPixels = nullptr; // PSRAM, made when first asked for
                };

                // One asked for while drawing, to load on the loop.
                struct Wanted {
                                char     key[MaxKey + 1] = {};
                                uint16_t side = 0;
                                Kind     kind = Kind::Type;
                };

                const Icon *find(StringView key, uint16_t side, Kind kind);
                const Kept *kept(StringView key, uint16_t side, Kind kind) const;
                const Icon *loaded(StringView key, uint16_t side, Kind kind);

                IconTheme   &_theme;
                List<Kept *> _kept{MemSpace::Psram};
                List<Wanted> _wanted{MemSpace::Psram};
                Asked        _asked = nullptr;
                void        *_askedArg = nullptr;
                bool         _trashFull = false;
};

BMDS_NAMESPACE_END
