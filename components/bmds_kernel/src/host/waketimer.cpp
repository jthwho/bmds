/**
 * @file      waketimer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The host's WakeTimer: no hardware timer, so sleeps are the tick's (whole milliseconds).
 */

#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/semaphore.h>
#include <bmds/waketimer.h>

BMDS_NAMESPACE_BEGIN

Error WakeTimer::init() {
        return Error::NotSupported;
}

bool WakeTimer::isReady() {
        return false;
}

Error WakeTimer::sleepUntil(int64_t deadlineNs, Interrupt *interrupt) {
        const int64_t deadlineUs = deadlineNs / 1000 + (deadlineNs % 1000 > 0 ? 1 : 0);
        Semaphore     never(1, 0);
        for (;;) {
                const int64_t left = deadlineUs - Detail::monotonicUs();
                if (left <= 0) return Error::Ok;
                Deadline    d(static_cast<unsigned int>((left + 999) / 1000));
                const Error err = Interrupt::wait(interrupt, never, d);
                if (err == Error::Interrupted) return err;
        }
}

BMDS_NAMESPACE_END
