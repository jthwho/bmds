/**
 * @file      trayrow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/icon.h>
#include <bmds/list.h>
#include <bmds/stringview.h>
#include <bmds/trayprotocol.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system tray's icons, as its messages leave them
 *        (`devplan/mediaplayer.md` §6e), pure: each known by its program's
 *        reply service and its number; shown left to right by category, then
 *        in the order they came (`Hardware` at the right end), a cell each.
 *
 * @par Thread Safety
 * A value with one owner (the tray's loop).
 */
class TrayRow {
        public:
                /** @brief Icons kept at most. */
                static constexpr size_t MaxIcons = 16;

                /** @brief A cell's width: a 16-pixel icon and the 4 pixels to the next. */
                static constexpr uint16_t CellWidth = 20;

                /** @brief An icon. */
                struct Entry {
                                char                   reply[TrayProtocol::MaxReplyName + 1] = {};
                                uint32_t               id = 0;
                                TrayProtocol::Category category = TrayProtocol::Category::Application;
                                uint32_t               flags = 0;
                                char                   icon[64] = {};
                                char                   toolTip[128] = {};
                                uint32_t               order = 0; // when it came
                                bool                   hasPixels = false;
                                uint32_t               argb[TrayProtocol::IconSide * TrayProtocol::IconSide] = {};

                                bool isVisible() const { return (flags & TrayProtocol::Visible) != 0; }

                                /** @brief Its own pixels as an icon (valid until the row changes). */
                                Icon pixels() const {
                                        return hasPixels ? Icon{TrayProtocol::IconSide, TrayProtocol::IconSide, argb}
                                                         : Icon{};
                                }
                };

                /**
                 * @brief Takes an `Update`: the icon added, or changed. @p moved (if
                 *        given) says whether the icons shown moved (one added, shown,
                 *        hidden, or in another category) rather than only its look.
                 * @return `Ok`, `Full`, `NoMem` or `Invalid`.
                 */
                Error update(const TrayProtocol::Update &message, bool *moved = nullptr);

                /** @brief Takes an icon's pixels. @return `Ok`, `NotFound` (no such icon) or `Invalid` (not 16 x 16). */
                Error setPixels(const TrayProtocol::Pixels &message);

                /** @brief Forgets icon @p id of @p reply. @return False if there was none. */
                bool remove(StringView reply, uint32_t id);

                /** @brief Asked of each program's reply service: true if it is gone. */
                using Gone = bool (*)(void *arg, StringView reply);

                /** @brief Forgets the icons of programs gone. @return How many went. */
                size_t dropGone(Gone gone, void *arg);

                /** @brief Forgets every icon. */
                void clear() { _entries.clear(); }

                /** @brief Returns the icons shown. */
                size_t shownCount() const;

                /** @brief Returns the icon shown at @p position from the left (null past them). */
                const Entry *shown(size_t position) const;

                /** @brief Returns the position shown at @p x from the row's left (-1: none). */
                int positionAt(int x) const;

                /** @brief Returns the row's width: a cell an icon shown. */
                uint16_t width() const { return uint16_t(shownCount() * CellWidth); }

                /** @brief Returns the position of icon @p id of @p reply (-1: not shown). */
                int positionOf(StringView reply, uint32_t id) const;

        private:
                int  indexOf(StringView reply, uint32_t id) const;
                bool before(const Entry &a, const Entry &b) const; // a left of b

                List<Entry> _entries{MemSpace::Psram};
                uint32_t    _nextOrder = 0;
};

BMDS_NAMESPACE_END
