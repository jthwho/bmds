/**
 * @file      navigationhistory.h
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
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The folders a window has shown, for Back and Forward
 *        (`devplan/filebrowser.md` §4h, FB2), pure.
 *
 * A list of paths and where in it the window is: a visit drops what was
 * ahead, Back and Forward move along it. `MaxKept` at most; the oldest goes.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class NavigationHistory {
        public:
                /** @brief Paths kept at most. */
                static constexpr size_t MaxKept = 32;

                /**
                 * @brief Records going to @p path: what was ahead dropped (nothing if
                 *        @p path is the current one).
                 * @return `Ok` or `NoMem` (as it was).
                 */
                Error visit(StringView path);

                /** @brief Returns the current path (empty before a visit). */
                StringView current() const { return _paths.isEmpty() ? StringView() : _paths[_at].view(); }

                /** @brief Returns true if there is one to go back to. */
                bool canGoBack() const { return _at > 0; }

                /** @brief Returns true if there is one to go forward to. */
                bool canGoForward() const { return _at + 1 < _paths.size(); }

                /** @brief Goes back one. @return The path now current, or empty (none: unchanged). */
                StringView back();

                /** @brief Goes forward one. @return The path now current, or empty (none: unchanged). */
                StringView forward();

                /** @brief Returns the paths kept. */
                size_t count() const { return _paths.size(); }

                /** @brief Forgets every path. */
                void clear() {
                        _paths.clear();
                        _at = 0;
                        return;
                }

                /**
                 * @brief Returns the folder @p path is in: `/a/b` → `/a`, `/a` → `/`,
                 *        `/` → `/` (a trailing `/` ignored).
                 */
                static StringView parentOf(StringView path);

        private:
                List<String> _paths{MemSpace::Psram};
                size_t       _at = 0; // the current one's index
};

BMDS_NAMESPACE_END
