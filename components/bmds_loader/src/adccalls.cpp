/**
 * @file      adccalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `adc_ctl` (slot 147, devplan/io.md §15.7): `/dev/adc0`'s operations.
 */

#include <cstddef>
#include <bmds/adcdevice.h>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::AdcDevice;
using bmds::KernelCall;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

static_assert(sizeof(bmds_adc_sample_t) == sizeof(AdcDevice::Sample), "a sample read as it is");

namespace {

        int32_t errnoOf(bmds::Error err) {
                switch (err.code()) {
                        case bmds::Error::Ok: return 0;
                        case bmds::Error::NotExist: return -BMDS_ENODEV;
                        case bmds::Error::Busy: return -BMDS_EBUSY;
                        case bmds::Error::Invalid: return -BMDS_EINVAL;
                        default: return -BMDS_EIO;
                }
        }

        int32_t info(AdcDevice &dev, bmds_adc_info_t *arg) {
                bmds_adc_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                AdcDevice::Info in;
                (void)AdcDevice::status(in, &dev);
                i.flags = (in.configured ? BMDS_ADC_CONFIGURED : 0u) | (in.streaming ? BMDS_ADC_STREAMING : 0u) |
                          (in.mine ? BMDS_ADC_MINE : 0u);
                i.channels = in.channels;
                i.rate_hz = in.rateHz;
                i.start_ns = in.startNs;
                i.samples = in.samples;
                i.lost = in.lost;
                i.buffer = in.buffer;
                i.reserved = 0;
                return copyOut(i, arg);
        }

        int32_t channel(bmds_adc_channel_t *arg) {
                bmds_adc_channel_t c;
                if (const int32_t r = copyIn(arg, c); r < 0) return r;
                AdcDevice::Channel ch;
                if (const bmds::Error err = AdcDevice::channel(c.index, ch); err.isError()) return errnoOf(err);
                c.pin = ch.pin;
                c.unit = ch.unit;
                c.channel = ch.channel;
                c.atten_cdb = ch.attenCdb;
                c.divider = ch.divider;
                c.reserved = 0;
                return copyOut(c, arg);
        }

        int32_t reading(bmds_adc_reading_t *arg) {
                bmds_adc_reading_t r;
                if (const int32_t e = copyIn(arg, r); e < 0) return e;
                AdcDevice::Reading got;
                if (const bmds::Error err = AdcDevice::reading(r.pin, got); err.isError()) return errnoOf(err);
                r.raw = got.raw;
                r.mv = got.mv;
                r.time_ns = got.timeNs;
                return copyOut(r, arg);
        }

} // namespace

extern "C" int32_t bmds_k_adc_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        AdcDevice *dev = handle->stream().adcDevice();
        if (dev == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_ADC_INFO: return info(*dev, static_cast<bmds_adc_info_t *>(arg));
                case BMDS_ADC_CHANNEL: return channel(static_cast<bmds_adc_channel_t *>(arg));
                case BMDS_ADC_READ: return reading(static_cast<bmds_adc_reading_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
