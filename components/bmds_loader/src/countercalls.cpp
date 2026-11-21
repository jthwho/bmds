/**
 * @file      countercalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `counterdev_ctl` (slot 144, devplan/io.md §15.3): `/dev/counter0`'s operations. Linux's counter
 * character device is translated onto it by the SDK's `linux.c`; its events are read as they are.
 */

#include <cstddef>
#include <bmds/counterchip.h>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::CounterChip;
using bmds::KernelCall;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

static_assert(BMDS_COUNTERDEV_COMPONENT_NONE == CounterChip::ComponentNone &&
                      BMDS_COUNTERDEV_COMPONENT_COUNT == CounterChip::ComponentCount &&
                      BMDS_COUNTERDEV_SCOPE_COUNT == CounterChip::ScopeCount &&
                      BMDS_COUNTERDEV_EVENT_THRESHOLD == CounterChip::EventThreshold,
              "Linux's numbers, in the kernel and the ABI");
static_assert(sizeof(bmds_counterdev_event_t) == sizeof(CounterChip::Event), "an event read as it is");

namespace {

        int32_t errnoOf(bmds::Error err) {
                switch (err.code()) {
                        case bmds::Error::Ok: return 0;
                        case bmds::Error::NotExist: return -BMDS_ENODEV;
                        case bmds::Error::Invalid: return -BMDS_EINVAL;
                        case bmds::Error::NoMem: return -BMDS_ENOMEM;
                        default: return -BMDS_EIO;
                }
        }

        int32_t watchOp(CounterChip &chip, uint32_t op, bmds_counterdev_watch_t *arg) {
                bmds_counterdev_watch_t w;
                if (const int32_t r = copyIn(arg, w); r < 0) return r;
                if (op == BMDS_COUNTERDEV_ENABLE_EVENTS) return errnoOf(chip.enableEvents());
                if (op == BMDS_COUNTERDEV_DISABLE_EVENTS) return errnoOf(chip.disableEvents());
                CounterChip::Watch k;
                k.type = w.type;
                k.scope = w.scope;
                k.parent = w.parent;
                k.id = w.id;
                k.event = w.event;
                k.channel = w.channel;
                return errnoOf(chip.addWatch(k));
        }

        int32_t countOp(CounterChip &chip, uint32_t op, bmds_counterdev_count_t *arg) {
                bmds_counterdev_count_t c;
                if (const int32_t r = copyIn(arg, c); r < 0) return r;
                if (op == BMDS_COUNTERDEV_PRESET) return errnoOf(chip.preset(c.count, c.value));
                if (op == BMDS_COUNTERDEV_THRESHOLD)
                        return errnoOf(chip.setThreshold(c.count, (c.flags & BMDS_COUNTERDEV_ARMED) != 0, c.value));
                int64_t v = 0, t = 0;
                if (const bmds::Error err = chip.value(c.count, v, t); err.isError()) return errnoOf(err);
                CounterChip::Info in;
                (void)chip.info(c.count, in);
                c.value = v;
                c.time_ns = t;
                c.flags = in.hasThreshold ? BMDS_COUNTERDEV_ARMED : 0u;
                return copyOut(c, arg);
        }

        int32_t info(CounterChip &chip, bmds_counterdev_info_t *arg) {
                bmds_counterdev_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                CounterChip::Info in;
                if (const bmds::Error err = chip.info(i.count, in); err.isError()) return errnoOf(err);
                i.ncounts = in.ncounts;
                i.flags = (in.configured ? BMDS_COUNTERDEV_CONFIGURED : 0u) |
                          (in.monitor ? BMDS_COUNTERDEV_MONITOR : 0u) |
                          (in.hasThreshold ? BMDS_COUNTERDEV_HAS_THRESHOLD : 0u);
                i.function = in.function;
                i.pin_a = in.pinA;
                i.pin_b = in.pinB;
                i.filter_ns = in.filterNs;
                i.threshold = in.threshold;
                i.events_lost = in.eventsLost;
                return copyOut(i, arg);
        }

} // namespace

extern "C" int32_t bmds_k_counterdev_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        CounterChip *chip = handle->stream().counterChip();
        if (chip == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_COUNTERDEV_ADD_WATCH:
                case BMDS_COUNTERDEV_ENABLE_EVENTS:
                case BMDS_COUNTERDEV_DISABLE_EVENTS:
                        return watchOp(*chip, op, static_cast<bmds_counterdev_watch_t *>(arg));
                case BMDS_COUNTERDEV_READ:
                case BMDS_COUNTERDEV_PRESET:
                case BMDS_COUNTERDEV_THRESHOLD: return countOp(*chip, op, static_cast<bmds_counterdev_count_t *>(arg));
                case BMDS_COUNTERDEV_INFO: return info(*chip, static_cast<bmds_counterdev_info_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
