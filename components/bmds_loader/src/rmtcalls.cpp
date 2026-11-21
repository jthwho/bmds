/**
 * @file      rmtcalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `rmt_ctl` (slot 142, devplan/io.md §13.1): `/dev/rmtN`'s operations. Linux has no RMT device;
 * the symbols themselves go by `read()` and `write()`.
 */

#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/rmtchannel.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::KernelCall;
using bmds::RmtChannel;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

namespace {

        int32_t info(RmtChannel &c, bmds_rmt_info_t *arg) {
                bmds_rmt_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                RmtChannel::Info in;
                if (c.info(in).isError()) return -BMDS_EINVAL;
                i.port = in.port;
                i.channel = in.channel;
                i.flags = (in.receive ? BMDS_RMT_RECEIVE : 0) | (in.running ? BMDS_RMT_RUNNING : 0) |
                          (in.sending ? BMDS_RMT_SENDING : 0) | (in.continuous ? BMDS_RMT_CONTINUOUS : 0) |
                          (in.idleHigh ? BMDS_RMT_IDLE_HIGH : 0);
                i.resolution_hz = in.resolution;
                i.source_hz = in.sourceHz;
                i.buffer = in.buffer;
                i.underruns = in.underruns;
                i.overruns = in.overruns;
                i.reserved = 0;
                i.start_ns = in.startNs;
                i.symbols = in.symbols;
                i.ticks = in.ticks;
                i.filler_ticks = in.fillerTicks;
                return copyOut(i, arg);
        }

} // namespace

extern "C" int32_t bmds_k_rmt_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        RmtChannel *channel = handle->stream().rmtChannel();
        if (channel == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_RMT_INFO: return info(*channel, static_cast<bmds_rmt_info_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
