/**
 * @file      parliocalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `parlio_ctl` (slot 145, devplan/io.md §15.5): `/dev/parlioN`'s operations.
 */

#include <cstddef>
#include <bmds/errno.h>
#include <bmds/parlioport.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::KernelCall;
using bmds::ParlioPort;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

namespace {

        int32_t info(ParlioPort &port, bmds_parlio_info_t *arg) {
                bmds_parlio_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                ParlioPort::Info in;
                (void)port.info(in);
                i.port = in.port;
                i.flags = (in.receive ? BMDS_PARLIO_RECEIVE : 0u) | (in.running ? BMDS_PARLIO_RUNNING : 0u) |
                          (in.externalClock ? BMDS_PARLIO_EXTERNAL_CLOCK : 0u) |
                          (in.monitor ? BMDS_PARLIO_MONITOR : 0u);
                i.width = in.width;
                i.clock_hz = in.clockHz;
                i.buffer = in.buffer;
                i.start_ns = in.startNs;
                i.bytes = in.bytes;
                i.lost = in.lost;
                i.transfers = in.transfers;
                i.idle = in.idle;
                i.reserved = 0;
                return copyOut(i, arg);
        }

} // namespace

extern "C" int32_t bmds_k_parlio_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        ParlioPort *port = handle->stream().parlioPort();
        if (port == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_PARLIO_INFO: return info(*port, static_cast<bmds_parlio_info_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
