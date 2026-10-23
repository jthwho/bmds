/**
 * @file      canvassurface.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/swapchain.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class GuiApplication;
class Painter;

/**
 * @brief A canvas's surface (`CANVAS-SURFACE`, `devplan/guiprograms.md` §11 3c): a swap chain a
 *        local program draws and presents through the surface calls, which its canvas shows.
 *
 * Counted: the program's descriptor holds one reference and the canvas another, so either may go
 * first. While attached, a present posts to the canvas's loop, which redraws what changed; the
 * canvas paints the newest presented buffer (`paint()`). Detached (the canvas gone, or given
 * another surface), presents still work but nothing shows them.
 *
 * It has no events of its own (the canvas's come by the protocol) and no palette, title or
 * pointer mode.
 *
 * @par Thread Safety
 * Thread-safe: the program's threads, the loop's (`attach()`, `detach()`, the change callback),
 * and the painter's.
 */
class CanvasSurface final : public SwapChain {
        public:
                /** @brief Told on the loop's thread what changed of the picture (its own pixels). */
                using Changed = void (*)(void *arg, const Rect &rect);

                /**
                 * @brief Makes a surface of @p count buffers of @p width × @p height in @p format
                 *        (RGB565 or BGR24), in @p space, and the program's handle to it.
                 * @return The handle (which holds the first reference), or `Invalid`, `NoMem`.
                 */
                static Result<FileHandle::Ptr> make(uint16_t width, uint16_t height, Format format, uint8_t count,
                                                    MemSpace space);

                /** @brief Returns @p handle's canvas surface, or null. */
                static CanvasSurface *of(FileHandle &handle);

                /** @brief Takes another reference. */
                void ref();

                /** @brief Gives a reference back: the last frees it. */
                void unref();

                /**
                 * @brief Shows it in a canvas, whose loop @p app is: presents call @p changed(@p arg)
                 *        there. Takes a reference, given back by `detach()`. The loop's thread.
                 */
                void attach(GuiApplication &app, Changed changed, void *arg);

                /** @brief No longer shown: no change is told after it. The loop's thread. */
                void detach();

                /**
                 * @brief Paints the newest presented buffer at (@p x, @p y), at most @p width ×
                 *        @p height of it. The painter's thread.
                 * @return True if there was one (else nothing is painted).
                 */
                bool paint(Painter &painter, int x, int y, uint16_t width, uint16_t height);

                // SwapChain
                Result<uint32_t> present(uint8_t index, Span<const Rect> damage) override;
                Info             info() override;

                // Stream
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                uint8_t        readiness() override { return 0; }
                WaitList      *readinessWaiters() override { return &_pollers; }
                void           handleClosed(uint8_t mode) override;

        private:
                CanvasSurface() = default;
                ~CanvasSurface() = default;

                static void changedDue(void *arg); // the loop's

                std::atomic<uint32_t> _refs{1};
                // Under _lock: where presents are told, and what changed since last told.
                GuiApplication *_app = nullptr;
                Changed         _changed = nullptr;
                void           *_arg = nullptr;
                Rect            _damage;
                bool            _posted = false; // a changedDue() is on its way
                WaitListOf<1>   _pollers;        // never readable
};

BMDS_NAMESPACE_END
