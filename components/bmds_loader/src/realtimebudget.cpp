/**
 * @file      realtimebudget.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/realtimebudget.h>

BMDS_NAMESPACE_BEGIN

RealTimeBudget::Action RealTimeBudget::account(int64_t nowUs, uint64_t usedUs) {
        Action action = Action::None;
        if (_periodStart < 0) _periodStart = nowUs;
        if (nowUs - _periodStart >= PeriodUs) {
                // Whole periods passed: the next one starts with what it has had so far.
                _periodStart += (nowUs - _periodStart) / PeriodUs * PeriodUs;
                _used = 0;
                if (_throttled) action = Action::Release;
                _throttled = false;
        }
        _used += usedUs;
        if (!_throttled && _used >= static_cast<uint64_t>(RuntimeUs)) {
                _throttled = true;
                // Released and spent again in one call: they stay throttled.
                action = action == Action::Release ? Action::None : Action::Throttle;
        }
        return action;
}

void RealTimeBudget::reset() {
        _periodStart = -1;
        _used = 0;
        _throttled = false;
        return;
}

BMDS_NAMESPACE_END
