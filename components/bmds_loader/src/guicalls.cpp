/**
 * @file      guicalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI service's system call (devplan/guiprograms.md §8): a connection to it, through the
 * opener the service registered at boot.
 */

#include <atomic>
#include <bmds/errno.h>
#include <bmds/guiopener.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;

namespace {

        std::atomic<bmds::GuiOpener::Open>          opener{nullptr};
        std::atomic<bmds::GuiOpener::CanvasSurface>   surfaceMaker{nullptr};
        std::atomic<bmds::GuiOpener::ScreenCaptureFn> screenCapturer{nullptr};

        // Rectangles taken in one call (a caller with room for more calls again: `more`).
        constexpr uint32_t MaxCaptureRects = 64;

} // namespace

BMDS_NAMESPACE_BEGIN

void GuiOpener::set(Open open, CanvasSurface canvasSurface, ScreenCaptureFn screenCapture) {
        opener.store(open, std::memory_order_release);
        surfaceMaker.store(canvasSurface, std::memory_order_release);
        screenCapturer.store(screenCapture, std::memory_order_release);
        return;
}

Error GuiOpener::screenCapture(Stream &connection, ScreenCapture &capture) {
        const ScreenCaptureFn fn = screenCapturer.load(std::memory_order_acquire);
        if (fn == nullptr) return Error::NotSupported;
        return fn(connection, capture);
}

Result<FileHandle::Ptr> GuiOpener::open(uint32_t display, const Client &client) {
        const Open fn = opener.load(std::memory_order_acquire);
        if (fn == nullptr) return makeError<FileHandle::Ptr>(Error::NotSupported);
        return fn(display, client);
}

Result<FileHandle::Ptr> GuiOpener::canvasSurface(Stream &connection, uint32_t canvas, const SurfaceConfig &config) {
        const CanvasSurface fn = surfaceMaker.load(std::memory_order_acquire);
        if (fn == nullptr) return makeError<FileHandle::Ptr>(Error::NotSupported);
        return fn(connection, canvas, config);
}

BMDS_NAMESPACE_END

extern "C" {

int32_t bmds_k_gui_open(uint32_t display, uint32_t flags) {
        KernelCall call;
        if ((flags & ~BMDS_O_CLOEXEC) != 0) return -BMDS_EINVAL;
        bmds::GuiOpener::Client client;
        client.pid = call.thread().id(BMDS_PROC_SELF);
        client.program = bmds::StringView(call.thread().run->path);
        auto [handle, err] = bmds::GuiOpener::open(display, client);
        if (err == Error::NotSupported) return -BMDS_ENODEV;
        if (err == Error::Full) return -BMDS_EBUSY;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return call->addHandle(std::move(handle), flags);
}

int32_t bmds_k_gui_canvas_surface(int32_t conn, uint32_t canvas, const bmds_surface_config_t *config) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr connection = call->handle(conn);
        if (!connection) return -BMDS_EBADF;
        bmds::GuiOpener::SurfaceConfig k;
        if (config != nullptr) {
                bmds_surface_config_t c;
                const int32_t         r = bmds::detail::copyIn(config, c);
                if (r < 0) return r;
                // The window's fields (title, aspect, the window flags) aren't a canvas's.
                if (c.width > 1280 || c.height > 720 || (c.width == 0) != (c.height == 0) ||
                    (c.format != BMDS_PIXEL_RGB565 && c.format != BMDS_PIXEL_BGR24) || c.buffers < 2 || c.buffers > 3 ||
                    (c.flags & ~BMDS_SURFACE_FAST) != 0)
                        return -BMDS_EINVAL;
                k.width = uint16_t(c.width);
                k.height = uint16_t(c.height);
                k.format = uint8_t(c.format);
                k.buffers = uint8_t(c.buffers);
                k.fast = (c.flags & BMDS_SURFACE_FAST) != 0;
        }
        // A U-mode program's in its own memory, PSRAM (protection.md §15, as surface_open).
        k.memory = call.thread().run->memory;
        if (k.memory != nullptr) k.fast = false;
        auto [handle, err] = bmds::GuiOpener::canvasSurface(connection->stream(), canvas, k);
        if (err == Error::NotSupported) return -BMDS_ENOTTY;
        if (err == Error::NotExist) return -BMDS_ENOENT;
        if (err == Error::Invalid) return -BMDS_EINVAL;
        if (err == Error::NotReady) return -BMDS_EAGAIN;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return call->addHandle(std::move(handle), BMDS_O_CLOEXEC);
}

int32_t bmds_k_gui_screen_capture(int32_t conn, bmds_screen_capture_t *capture) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr connection = call->handle(conn);
        if (!connection) return -BMDS_EBADF;
        bmds_screen_capture_t c;
        int32_t               r = bmds::detail::copyIn(capture, c);
        if (r < 0) return r;
        if ((c.format != BMDS_PIXEL_BGR24 && c.format != BMDS_PIXEL_RGB565) ||
            (c.flags & ~(BMDS_CAPTURE_ALL | BMDS_CAPTURE_NO_POINTER)) != 0 || c.buffer == nullptr || c.capacity == 0 ||
            c.rects == nullptr || c.max_rects == 0)
                return -BMDS_EINVAL;
        const uint32_t maxRects = c.max_rects < MaxCaptureRects ? c.max_rects : MaxCaptureRects;
        if (!bmds::detail::canWrite(c.buffer, c.capacity) ||
            !bmds::detail::canWrite(c.rects, maxRects * sizeof(bmds_rect_t)))
                return -BMDS_EFAULT;
        bmds::Rect                     rects[MaxCaptureRects];
        bmds::GuiOpener::ScreenCapture k;
        k.rgb565 = c.format == BMDS_PIXEL_RGB565;
        k.all = (c.flags & BMDS_CAPTURE_ALL) != 0;
        k.noPointer = (c.flags & BMDS_CAPTURE_NO_POINTER) != 0;
        k.share = c.share; // 0 from programs built before it (a 48-byte struct)
        k.buffer = static_cast<uint8_t *>(c.buffer);
        k.capacity = c.capacity;
        k.rects = bmds::Span<bmds::Rect>(rects, maxRects);
        const Error err = bmds::GuiOpener::screenCapture(connection->stream(), k);
        if (err == Error::NotSupported) return -BMDS_ENOTTY;
        if (err == Error::PermissionDenied) return -BMDS_EACCES;
        if (err == Error::NotReady) return -BMDS_EAGAIN;
        if (err == Error::Timeout) return -BMDS_ETIMEDOUT;
        if (err.isError()) return bmds::Process::errnoFor(err);
        for (size_t i = 0; i < k.count; ++i)
                c.rects[i] = bmds_rect_t{rects[i].x, rects[i].y, rects[i].width, rects[i].height};
        c.rect_count = uint32_t(k.count);
        c.bytes = uint32_t(k.bytes);
        c.width = k.width;
        c.height = k.height;
        c.more = k.more ? 1u : 0u;
        c.pointer_x = k.pointerX;
        c.pointer_y = k.pointerY;
        r = bmds::detail::copyOut(c, capture);
        return r < 0 ? r : 0;
}

} // extern "C"
