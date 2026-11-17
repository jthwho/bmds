/**
 * @file      screenwatch.h
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
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Programs watching display 0's picture (`devplan/vnc.md` §5b, §5c): what changed since
 *        each last looked, and those parts copied out, whole frames only.
 *
 * Each watcher keeps its own `ChangeTiles`, fed by every present of the surface the scan shows
 * (the desktop's surface 0, or a program's full screen in its place; a switch between them marks
 * everything). The first change after a capture emptied them calls its `Notify`; a capture takes
 * them as rectangles and copies their pixels from the shown buffer, under the window manager's
 * capture (between its rounds) so no frame is half drawn.
 *
 * While any watcher exists, display 0 is told (`Display::setViewers()`: `display/output` =
 * `remote` turns the output off) and `setActivity()`'s listener is told, so the system can show
 * that the screen is being watched.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace ScreenWatch {

        /** @brief Watchers at once. */
        inline constexpr size_t MaxWatchers = 4;

        /** @brief Told of the first change since a capture emptied the watcher's tiles; must not block. */
        using Notify = void (*)(void *arg);

        /** @brief Told when watchers come or go (the panel's "screen shared" icon); must not block. */
        using Activity = void (*)(void *arg);

        /** @brief A capture's pixels. */
        enum class Format : uint8_t {
                Bgr24,  ///< The surface's own: B, G, R bytes.
                Rgb565, ///< A little-endian 16-bit word, R in the top 5 bits.
        };

        /** @brief What `capture()` copies, and what it did. */
        struct Capture {
                        Format     format = Format::Bgr24;
                        bool       all = false;      ///< Every tile, not only the changed ones.
                        uint8_t   *buffer = nullptr; ///< The rectangles' pixels, packed one after the other.
                        size_t     capacity = 0;     ///< Its bytes.
                        Span<Rect> rects;            ///< Filled with the rectangles copied, in order.
                        // Out:
                        size_t   count = 0;    ///< Rectangles copied.
                        size_t   bytes = 0;    ///< Of the buffer used.
                        uint16_t width = 0;    ///< The shown picture's.
                        uint16_t height = 0;   ///< (a program's full screen may change it)
                        bool     more = false; ///< Changes left that didn't fit: capture again.
        };

        /**
         * @brief Adds a watcher; its tiles start all marked (the whole picture is new to it), and
         *        @p notify is told at once.
         * @param owner Its program's name, as the system shows it (copied).
         * @return Its id, `Full` (`MaxWatchers` already), or `NotReady` (no display running).
         */
        Result<uint8_t> add(Notify notify, void *arg, StringView owner);

        /** @brief Removes watcher @p id (waits for a notification under way). Idempotent. */
        void remove(uint8_t id);

        /** @brief Returns the watchers now. @nonblocking */
        size_t count();

        /**
         * @brief Copies watcher @p id's owner's name into @p out (NUL-terminated, cut to fit).
         * @return False if there is no such watcher.
         */
        bool owner(size_t index, char *out, size_t outBytes);

        /** @brief Sets the one activity listener (null: none). */
        void setActivity(Activity activity, void *arg);

        /**
         * @brief Copies watcher @p id's changed tiles (all with `Capture::all`) out of the shown
         *        picture into @p c's buffer, as rectangles in @p c's `rects`. What doesn't fit
         *        stays marked (`more`); with nothing left, the next change notifies again.
         * @return `Ok`, `NotExist` (no such watcher), `NotReady` (no display), `Timeout`.
         * @blocking While the window manager finishes a round, and for the copy.
         */
        Error capture(uint8_t id, Capture &c, unsigned int timeoutMs);

} // namespace ScreenWatch

BMDS_NAMESPACE_END
