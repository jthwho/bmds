/**
 * @file      display.h
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
#include <bmds/lt8912b.h>
#include <bmds/result.h>
#include <bmds/scanout.h>
#include <bmds/surface.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An HDMI output: the bridge chip, the scanout, and surface 0, the
 *        picture it shows (`devplan/display.md` §5c).
 *
 * Numbered: the board's bridge is display 0. One user at a time may claim
 * it (the desktop), so commands don't pull the picture from under it.
 *
 * @par Thread Safety
 * `start()`, `stop()` and the claim from one task at a time.
 */
class Display {
        public:
                /** @brief Displays a board can have. */
                static constexpr size_t MaxDisplays = 1;

                /**
                 * @brief Returns display @p number, its bridge attached on first use.
                 * @return The display, `NotSupported` (no bridge on this board, or
                 *         no such number), or the bridge's error.
                 */
                static Result<Display *> display(size_t number);

                Display(I2cBus &bus, uint8_t bridgeAddress, uint8_t number);
                Display(const Display &) = delete;
                Display &operator=(const Display &) = delete;

                /** @brief Returns the number. */
                uint8_t number() const { return _number; }

                /**
                 * @brief Returns @p mode fitted to a pixel clock the DSI link can
                 *        make and carry (the front porch takes the difference).
                 * @return The fitted mode, or `NotSupported` (too fast for the link).
                 */
                static Result<VideoTiming> fitMode(const VideoTiming &mode);

                /**
                 * @brief Shows @p mode (fitted by `fitMode()`) from surface 0, made
                 *        for it with @p buffering and black. Stops whatever ran
                 *        before.
                 * @return `Ok`, `NotSupported`, `Invalid` (a size the scan can't
                 *         take), `NoMem`, or the bridge's or scanout's error.
                 * @blocking
                 */
                Error start(const VideoTiming &mode, Surface::Buffering buffering);

                /** @brief Stops the picture and frees surface 0. Idempotent. */
                void stop();

                /** @brief Returns true while showing. */
                bool isRunning() const { return _scanout.isRunning(); }

                /** @brief Returns the mode running (as fitted). */
                const VideoTiming &timing() const { return _timing; }

                /** @brief Returns surface 0. */
                Surface &surface() { return _surface; }

                /** @brief Returns the scanout. */
                Scanout &scanout() { return _scanout; }

                /** @brief Returns the bridge chip. */
                Lt8912b &bridge() { return _bridge; }

                /**
                 * @brief Claims the display for @p owner (a name that outlives the claim).
                 * @return `Ok`, or `Busy` (claimed by another).
                 */
                Error claim(const char *owner);

                /** @brief Ends the claim. */
                void release() {
                        _owner = nullptr;
                        return;
                }

                /** @brief Returns who claimed it, or null. */
                const char *owner() const { return _owner; }

        private:
                static void onLinkRestart(void *arg);

                Lt8912b     _bridge;
                Scanout     _scanout;
                Surface     _surface;
                VideoTiming _timing;
                const char *_owner = nullptr;
                uint8_t     _number;
                bool        _attached = false;
};

BMDS_NAMESPACE_END
