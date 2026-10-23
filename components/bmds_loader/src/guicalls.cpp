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
        std::atomic<bmds::GuiOpener::CanvasSurface> surfaceMaker{nullptr};

} // namespace

BMDS_NAMESPACE_BEGIN

void GuiOpener::set(Open open, CanvasSurface canvasSurface) {
        opener.store(open, std::memory_order_release);
        surfaceMaker.store(canvasSurface, std::memory_order_release);
        return;
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
        auto [handle, err] = bmds::GuiOpener::canvasSurface(connection->stream(), canvas, k);
        if (err == Error::NotSupported) return -BMDS_ENOTTY;
        if (err == Error::NotExist) return -BMDS_ENOENT;
        if (err == Error::Invalid) return -BMDS_EINVAL;
        if (err == Error::NotReady) return -BMDS_EAGAIN;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return call->addHandle(std::move(handle), BMDS_O_CLOEXEC);
}

} // extern "C"
