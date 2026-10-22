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

using bmds::Error;
using bmds::KernelCall;

namespace {

        std::atomic<bmds::GuiOpener::Open> opener{nullptr};

} // namespace

BMDS_NAMESPACE_BEGIN

void GuiOpener::set(Open open) {
        opener.store(open, std::memory_order_release);
        return;
}

Result<FileHandle::Ptr> GuiOpener::open(uint32_t display, const Client &client) {
        const Open fn = opener.load(std::memory_order_acquire);
        if (fn == nullptr) return makeError<FileHandle::Ptr>(Error::NotSupported);
        return fn(display, client);
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

} // extern "C"
