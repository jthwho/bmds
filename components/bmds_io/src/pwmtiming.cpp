/**
 * @file      pwmtiming.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's PWM rounding onto LEDC's divider, resolution, duty and hpoint (devplan/io.md §15.1).
 */

#include <bmds/pwmtiming.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint64_t PsPerSecond = 1000000000000ull;
        constexpr uint64_t MaxNs = 1000000000000000ull; // beyond any period: keeps the sums in 64 bits

        uint64_t tickPs(uint32_t sourceHz) {
                return PsPerSecond / sourceHz;
        }

        // The most whole ticks of @p unitPs not above @p ns, its fraction of a ns allowed: what
        // "rounded down, sub-ns dropped" means for a value given in ns.
        uint64_t ticksBelow(uint64_t ns, uint64_t unitPs) {
                if (ns > MaxNs) ns = MaxNs;
                return (ns * 1000 + 999) / unitPs;
        }

        // Counts of a period (@p periodPs, at most 2^30 ticks) / 2^bits not above @p ns: @p full at
        // most (ps × 2^20 then stays within 64 bits).
        uint64_t countsBelow(uint64_t ns, uint64_t full, uint64_t periodPs) {
                if (ns > MaxNs) ns = MaxNs;
                const uint64_t ps = ns * 1000 + 999;
                return ps >= periodPs ? full : ps * full / periodPs;
        }

        // Counts × the period's ticks / 2^bits, in ps (counts × ticks may be ~2^50: split the division).
        uint64_t countsPs(uint64_t counts, uint32_t ticks, uint8_t bits, uint64_t unitPs) {
                const uint64_t t = counts * ticks;
                const uint64_t mask = (uint64_t(1) << bits) - 1;
                return (t >> bits) * unitPs + ((t & mask) * unitPs >> bits);
        }

} // namespace

LedcTiming LedcTiming::round(const PwmWaveform &wanted, uint32_t sourceHz) {
        LedcTiming t;
        if (wanted.periodNs == 0) return t;
        const uint64_t unit = tickPs(sourceHz);
        const uint64_t most = ticksBelow(wanted.periodNs, unit);
        for (uint8_t b = 1; b <= MaxBits; ++b) {
                // A whole-tick period: with 8 bits or more the divider's fraction completes within
                // it (a step of 2^(bits-8) ticks); below, the divider is whole (a step of 2^bits).
                const uint64_t step = b >= 8 ? uint64_t(1) << (b - 8) : uint64_t(1) << b;
                const uint64_t least = uint64_t(1) << b; // a divider of 1
                const uint64_t top = b >= 8 ? uint64_t(MaxDiv256) << (b - 8) : uint64_t(MaxDiv256 >> 8) << b;
                if (most < least) continue;
                const uint64_t ticks = (most < top ? most : top) / step * step;
                if (ticks > t.ticks || (ticks == t.ticks && b > t.bits)) {
                        t.ticks = uint32_t(ticks);
                        t.bits = b;
                }
        }
        if (t.ticks == 0) {
                // Shorter than any: the shortest (two ticks), rounded up.
                t.ticks = 2;
                t.bits = 1;
        }
        t.div256 = t.bits >= 8 ? t.ticks >> (t.bits - 8) : (t.ticks >> t.bits) << 8;
        // Duty and offset in counts of ticks / 2^bits, each down (0 is always there).
        const uint64_t full = uint64_t(1) << t.bits;
        const uint64_t periodPs = uint64_t(t.ticks) * unit;
        const uint64_t hpoint = countsBelow(wanted.offsetNs, full, periodPs);
        t.duty = uint32_t(countsBelow(wanted.dutyNs, full, periodPs));
        t.hpoint = uint32_t(hpoint >= full ? full - 1 : hpoint);
        return t;
}

PwmWaveform LedcTiming::waveform(uint32_t sourceHz) const {
        PwmWaveform w;
        if (ticks == 0) return w;
        const uint64_t unit = tickPs(sourceHz);
        w.periodNs = uint64_t(ticks) * unit / 1000;
        w.dutyNs = countsPs(duty, ticks, bits, unit) / 1000;
        w.offsetNs = countsPs(hpoint, ticks, bits, unit) / 1000;
        return w;
}

BMDS_NAMESPACE_END
