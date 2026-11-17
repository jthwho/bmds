/**
 * @file      guiopener.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/programmemory.h>
#include <bmds/rect.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where `gui_open` and `gui_canvas_surface` find the GUI service
 *        (`devplan/guiprograms.md` §8, §11 3c). The service is above the loader (the toolkit
 *        depends on it), so it registers itself here at boot, and the system calls ask through it.
 *
 * @par Thread Safety
 * `set()` once, at boot, before programs run; `open()` and `canvasSurface()` from any task.
 */
namespace GuiOpener {

        /** @brief Who is connecting: for the service's records and, later, its permissions. */
        struct Client {
                        int32_t    pid = 0;
                        StringView program; ///< The program's path.
        };

        /**
         * @brief Opens a connection to display @p display for @p client: the client's end.
         * @return The end, `NotSupported` (no such display, or no window system running),
         *         `Full`, or `NoMem`.
         */
        using Open = Result<FileHandle::Ptr> (*)(uint32_t display, const Client &client);

        /** @brief What a canvas's surface is to be (`gui_canvas_surface`'s config, checked). */
        struct SurfaceConfig {
                        uint16_t width = 0; ///< 0 × 0: the canvas's size.
                        uint16_t height = 0;
                        uint8_t  format = 3;   ///< `SwapChain::Format` (the ABI's `BMDS_PIXEL_...`).
                        uint8_t  buffers = 2;  ///< 2 or 3.
                        bool     fast = false; ///< Buffers in internal RAM.
                        ProgramMemory *memory = nullptr; ///< A U-mode program's: the buffers there.
        };

        /**
         * @brief Gives canvas @p canvas of the connection whose client's end is @p connection a
         *        surface (`CANVAS-SURFACE`): the program's handle to it. Blocks until the
         *        connection's loop has run the requests before it.
         * @return The handle, or `NotSupported` (not a local GUI connection), `NotExist` (no such
         *         object), `Invalid` (not a canvas, the config), `NotReady` (the canvas has no size
         *         yet), `NoMem`.
         */
        using CanvasSurface = Result<FileHandle::Ptr> (*)(Stream &connection, uint32_t canvas,
                                                          const SurfaceConfig &config);

        /** @brief A screen capture's request and result (`gui_screen_capture`'s, checked). */
        struct ScreenCapture {
                        bool       rgb565 = false; ///< Else B, G, R bytes.
                        bool       all = false;    ///< Every part, not only what changed.
                        uint8_t   *buffer = nullptr;
                        size_t     capacity = 0;
                        Span<Rect> rects; ///< The kernel's: filled with what was copied.
                        // Out:
                        size_t   count = 0;
                        size_t   bytes = 0;
                        uint16_t width = 0;
                        uint16_t height = 0;
                        bool     more = false;
        };

        /**
         * @brief Copies the screen's changes for the connection whose client's end is
         *        @p connection, which must be watching the screen (`Shell.watchScreen`). On the
         *        caller's task.
         * @return `Ok`, `NotSupported` (not a local GUI connection), `PermissionDenied` (not watching),
         *         `NotReady` (no display), `Timeout`.
         */
        using ScreenCaptureFn = Error (*)(Stream &connection, ScreenCapture &capture);

        /** @brief Sets the service's opener, its canvas surfaces' maker and its screen capture. */
        void set(Open open, CanvasSurface canvasSurface, ScreenCaptureFn screenCapture);

        /** @brief Opens through the service's opener (`NotSupported` without one). */
        Result<FileHandle::Ptr> open(uint32_t display, const Client &client);

        /** @brief Makes a canvas's surface through the service (`NotSupported` without one). */
        Result<FileHandle::Ptr> canvasSurface(Stream &connection, uint32_t canvas, const SurfaceConfig &config);

        /** @brief Captures the screen through the service (`NotSupported` without one). */
        Error screenCapture(Stream &connection, ScreenCapture &capture);

} // namespace GuiOpener

BMDS_NAMESPACE_END
