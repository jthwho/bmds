/**
 * @file      clockmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/clockmodel.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // @p deltaNs * @p ppb / 1e9 without overflowing 64 bits (no 128-bit
        // arithmetic on the target): whole seconds and the rest separately.
        int64_t scale(int64_t deltaNs, int32_t ppb) {
                const int64_t seconds = deltaNs / Timestamp::NsPerSecond;
                const int64_t rest = deltaNs % Timestamp::NsPerSecond;
                return seconds * ppb + rest * ppb / Timestamp::NsPerSecond;
        }

} // namespace

const char *clockSourceName(ClockSource source) {
        switch (source) {
                case ClockSource::None: return "none";
                case ClockSource::Manual: return "manual";
                case ClockSource::Rtc: return "rtc";
                case ClockSource::Ntp: return "ntp";
                case ClockSource::Ptp: return "ptp";
                case ClockSource::Gps: return "gps";
                case ClockSource::Host: return "host";
        }
        return "unknown";
}

int64_t ClockModel::slewed(int64_t monoNs) const {
        if (_slewTotal == 0) return 0;
        const int64_t elapsed = monoNs - _slewStart;
        if (elapsed <= 0) return 0;
        const int64_t magnitude = _slewTotal < 0 ? -_slewTotal : _slewTotal;
        const int64_t applied = elapsed / SlewDivisor < magnitude ? elapsed / SlewDivisor : magnitude;
        return _slewTotal < 0 ? -applied : applied;
}

Timestamp ClockModel::wallAt(int64_t monoNs) const {
        const int64_t delta = monoNs - _anchorMono;
        return Timestamp{_anchorWall + delta + scale(delta, _frequencyPpb) + slewed(monoNs)};
}

void ClockModel::reanchor(int64_t monoNs) {
        const int64_t wall = wallAt(monoNs).ns;
        _slewTotal -= slewed(monoNs);
        _slewStart = monoNs;
        _anchorMono = monoNs;
        _anchorWall = wall;
        return;
}

void ClockModel::set(int64_t monoNs, Timestamp wall, ClockSource source) {
        _anchorMono = monoNs;
        _anchorWall = wall.ns;
        _slewTotal = 0;
        _slewStart = monoNs;
        _lastSet = wall.ns;
        _source = static_cast<uint8_t>(source);
        return;
}

int64_t ClockModel::adjust(int64_t monoNs, int64_t offsetNs) {
        reanchor(monoNs);
        const int64_t previous = _slewTotal;
        _slewTotal = offsetNs;
        return previous;
}

Error ClockModel::setFrequency(int64_t monoNs, int32_t ppb) {
        if (ppb > MaxFrequencyPpb || ppb < -MaxFrequencyPpb) return Error::OutOfRange;
        reanchor(monoNs);
        _frequencyPpb = ppb;
        return Error();
}

ClockStatus ClockModel::status(int64_t monoNs) const {
        ClockStatus s;
        s.source = source();
        s.lastSet = Timestamp{_lastSet};
        s.slewRemainingNs = _slewTotal - slewed(monoNs);
        s.frequencyPpb = _frequencyPpb;
        return s;
}

BMDS_NAMESPACE_END
