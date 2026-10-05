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
#include <bmds/framebuffer.h>
#include <bmds/lt8912b.h>
#include <bmds/result.h>
#include <bmds/scanout.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An HDMI output: the bridge chip, the scanout and the RGB565
 *        framebuffer it shows (`devplan/display.md` §5b).
 *
 * Numbered: the board's bridge is display 0. `start()` runs a mode the
 * framebuffer way (the 2D-DMA widening each block into a free-running ring
 * of three blocks); the bring-up tests of the `display` command use the
 * parts directly. One user at a time may claim it (the desktop), so the
 * tests don't pull the picture from under it.
 *
 * @par Thread Safety
 * `start()`, `stop()` and the claim from one task at a time.
 */
class Display {
        public:
                /** @brief Displays a board can have. */
                static constexpr size_t MaxDisplays = 1;

                /** @brief Most lines a ring block holds (`devplan/display.md` V11). */
                static constexpr uint16_t MaxBlockLines = 24;

                /** @brief Fewest lines a block worth having (each block costs a fixed amount). */
                static constexpr uint16_t MinBlockLines = 4;

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

                /** @brief Returns the most lines up to @p most that divide @p height (0: none of 4 or more). */
                static uint16_t blockLines(uint16_t height, uint16_t most);

                /**
                 * @brief Shows @p mode (fitted by `fitMode()`) from the framebuffer,
                 *        cleared to black. Stops whatever ran before.
                 * @return `Ok`, `NotSupported`, `Invalid` (no block size divides
                 *         the height), `NoMem`, or the bridge's or scanout's error.
                 * @blocking
                 */
                Error start(const VideoTiming &mode);

                /**
                 * @brief Shows @p mode (fitted) composed block by block by @p blocks
                 *        straight into the ring (`devplan/windows.md` §4b): no
                 *        framebuffer. Stops whatever ran before.
                 * @return As the other `start()`.
                 * @blocking
                 */
                Error start(const VideoTiming &mode, BlockSource &blocks);

                /** @brief Returns the lines a ring block holds (0 when not running). */
                uint16_t blockLines() const { return _blockLines; }

                /** @brief Stops the picture and frees the framebuffer. Idempotent. */
                void stop();

                /** @brief Returns true while `start()`'s picture runs. */
                bool isRunning() const { return _scanout.isRunning(); }

                /** @brief Returns the mode running (as fitted). */
                const VideoTiming &timing() const { return _timing; }

                /** @brief Returns the framebuffer. */
                Framebuffer &framebuffer() { return _framebuffer; }

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
                Error       begin(const VideoTiming &mode, BlockSource *blocks);

                Lt8912b     _bridge;
                Scanout     _scanout;
                Framebuffer _framebuffer;
                VideoTiming _timing;
                uint16_t    _blockLines = 0;
                const char *_owner = nullptr;
                uint8_t     _number;
                bool        _attached = false;
};

BMDS_NAMESPACE_END
