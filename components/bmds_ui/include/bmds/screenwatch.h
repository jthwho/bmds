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

class Surface;

/**
 * @brief Programs watching display 0's picture (`devplan/vnc.md` §5b, §5c): what changed since
 *        each last looked, and those parts copied out, whole frames only.
 *
 * Each watcher keeps its own `ChangeTiles`, fed by every present of the surface the scan shows
 * (the desktop's surface 0, or a program's full screen in its place; a switch between them marks
 * everything). The first change after a capture emptied them calls its `Notify`; a capture takes
 * them as rectangles and copies their pixels from a whole frame: the desktop's latest present
 * (shown, or waiting for its swap), its swaps held meanwhile; a program's full screen under the
 * window manager's capture.
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

        /** @brief A copy inside the picture (`Capture::copies`): @p from's pixels to (@p toX, @p toY). */
        struct Copy {
                        Rect     from;
                        uint16_t toX = 0;
                        uint16_t toY = 0;
        };

        /** @brief Copies a watcher keeps until it captures (more: their destinations as changes). */
        inline constexpr size_t MaxCopies = 16;

        /** @brief What `capture()` copies, and what it did. */
        struct Capture {
                        Format     format = Format::Bgr24;
                        bool       all = false;      ///< Every tile, not only the changed ones.
                        bool       noPointer = false; ///< What the pointer covers in its place (vnc.md §20).
                        uint8_t   *buffer = nullptr; ///< The rectangles' pixels, packed one after the other.
                        size_t     capacity = 0;     ///< Its bytes.
                        Span<Rect> rects;            ///< Filled with the rectangles copied, in order.
                        /**
                         * Filled with the copies to make first, in order, each reading the picture
                         * as the ones before left it (vnc.md §24 C3); empty, or too small for them
                         * all: their destinations are rectangles instead.
                         */
                        Span<Copy> copies;
                        // Out:
                        size_t   count = 0;    ///< Rectangles copied.
                        size_t   bytes = 0;    ///< Of the buffer used.
                        uint16_t width = 0;    ///< The shown picture's.
                        uint16_t height = 0;   ///< (a program's full screen may change it)
                        bool     more = false; ///< Changes left that didn't fit: capture again.
                        size_t   copyCount = 0; ///< Copies written to `copies`.
                        int32_t  pointerX = -1; ///< The pointer's tip in the copied picture; -1: none drawn.
                        int32_t  pointerY = -1;
        };

        /**
         * @brief Adds a watcher; its tiles start all marked (the whole picture is new to it), and
         *        @p notify is told at once.
         * @param owner Its program's name, as the system shows it (copied).
         * @param client Its GUI client's number (`GuiApplication::client()`): the tray marks that
         *        program's own icons while it watches (`isWatching()`).
         * @return Its id, `Full` (`MaxWatchers` already), or `NotReady` (no display running).
         */
        Result<uint8_t> add(Notify notify, void *arg, StringView owner, uint32_t client);

        /** @brief Returns true if GUI client @p client watches the screen. @nonblocking (a short lock) */
        bool isWatching(uint32_t client);

        /**
         * @brief Writes the watching GUI clients' numbers into @p out (a client watching twice is
         *        there twice).
         * @return How many watch (all of them, though only @p max are written).
         */
        size_t clients(uint32_t *out, size_t max);

        /**
         * @brief Sets watcher @p id's pointer listener (null: none), told with its `add()` argument
         *        when the pointer's picture changes (`pointerChanged()`); must not block.
         */
        void setPointerNotify(uint8_t id, Notify notify);

        /**
         * @brief The window manager's: the pointer's picture changed this round (devplan/vnc.md
         *        §20). Its watchers told. @nonblocking (a short lock)
         */
        void pointerChanged();

        /**
         * @brief The window manager's, before each present of @p surface (the desktop's): the
         *        pointer drawn over @p area (empty: none) of the buffer being drawn, its tip at
         *        @p x, @p y, @p under what it covers (@p area's pixels, B, G, R, rows packed).
         *        Kept with that buffer for captures without the pointer, and where it is.
         */
        void pointerDrawn(const Surface &surface, const Rect &area, const uint8_t *under, uint16_t x, uint16_t y);

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
         *        Without the pointer (`noPointer`), tiles only the pointer changed aren't copied:
         *        none may be (`count` 0), the pointer's place the news.
         * @return `Ok`, `NotExist` (no such watcher), `NotReady` (no display), `Timeout`.
         * @blocking For the copy (a program's full screen: also while the window manager finishes
         *           a round and its frame shows).
         */
        Error capture(uint8_t id, Capture &c, unsigned int timeoutMs);

} // namespace ScreenWatch

BMDS_NAMESPACE_END
