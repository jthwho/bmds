/**
 * @file      pwmcalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `pwm_ctl` (slot 143, devplan/io.md §15.1): `/dev/pwmchip0`'s operations. Linux's PWM character
 * device is translated onto it by the SDK's `linux.c`.
 */

#include <cstddef>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/pwmchip.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::KernelCall;
using bmds::PwmChip;
using bmds::PwmWaveform;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

static_assert(BMDS_PWM_HAS_PIN == 1 && BMDS_PWM_REQUESTED == 2 && BMDS_PWM_MINE == 4 && BMDS_PWM_ON == 8, "the flags");

namespace {

        int32_t errnoOf(bmds::Error err) {
                switch (err.code()) {
                        case bmds::Error::Ok: return 0;
                        case bmds::Error::NotExist: return -BMDS_ENODEV;
                        case bmds::Error::Busy: return -BMDS_EBUSY;
                        case bmds::Error::OutOfRange: return -BMDS_EDOM;
                        case bmds::Error::Invalid: return -BMDS_EINVAL;
                        default: return -BMDS_EIO;
                }
        }

        int32_t waveformOp(PwmChip &chip, uint32_t op, bmds_pwm_waveform_t *arg) {
                bmds_pwm_waveform_t w;
                if (const int32_t r = copyIn(arg, w); r < 0) return r;
                PwmWaveform wf;
                wf.periodNs = w.period_ns;
                wf.dutyNs = w.duty_ns;
                wf.offsetNs = w.offset_ns;
                bmds::Error err;
                switch (op) {
                        case BMDS_PWM_REQUEST: return errnoOf(chip.request(w.hwpwm));
                        case BMDS_PWM_FREE: return errnoOf(chip.release(w.hwpwm));
                        case BMDS_PWM_ROUND: err = chip.round(w.hwpwm, wf); break;
                        case BMDS_PWM_GET: err = chip.waveform(w.hwpwm, wf); break;
                        case BMDS_PWM_SET_ROUNDED: return errnoOf(chip.setWaveform(w.hwpwm, wf, false));
                        case BMDS_PWM_SET_EXACT: return errnoOf(chip.setWaveform(w.hwpwm, wf, true));
                        default: return -BMDS_EINVAL;
                }
                if (err.isError()) return errnoOf(err);
                w.period_ns = wf.periodNs;
                w.duty_ns = wf.dutyNs;
                w.offset_ns = wf.offsetNs;
                return copyOut(w, arg);
        }

        int32_t info(PwmChip &chip, bmds_pwm_info_t *arg) {
                bmds_pwm_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                PwmChip::Info in;
                if (const bmds::Error err = chip.info(i.hwpwm, in); err.isError()) return errnoOf(err);
                i.npwm = in.npwm;
                i.flags = (in.hasPin ? BMDS_PWM_HAS_PIN : 0u) | (in.requested ? BMDS_PWM_REQUESTED : 0u) |
                          (in.mine ? BMDS_PWM_MINE : 0u) | (in.on ? BMDS_PWM_ON : 0u);
                i.pin = in.pin;
                i.timer = in.timer;
                i.bits = in.bits;
                i.source_hz = in.sourceHz;
                return copyOut(i, arg);
        }

} // namespace

extern "C" int32_t bmds_k_pwm_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        PwmChip *chip = handle->stream().pwmChip();
        if (chip == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        if (op == BMDS_PWM_INFO) return info(*chip, static_cast<bmds_pwm_info_t *>(arg));
        if (op > BMDS_PWM_SET_EXACT) return -BMDS_EINVAL;
        return waveformOp(*chip, op, static_cast<bmds_pwm_waveform_t *>(arg));
}
