/**
 * @file      pwmtiming.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A PWM waveform as Linux gives it, and its rounding onto an LEDC channel and timer
 * (devplan/io.md §15.1). Pure, host-tested.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A PWM channel's waveform (Linux's `pwmchip_waveform`): the active part of each period,
 *        `dutyNs` long, starting `offsetNs` after the period's start. A period of 0 is off.
 *
 * @par Thread Safety
 * A plain value.
 */
struct PwmWaveform {
                uint64_t periodNs = 0; ///< The repeating period; 0: off.
                uint64_t dutyNs = 0;   ///< The active part, at most the period.
                uint64_t offsetNs = 0; ///< The active part's start in the period, less than it.

                /** @brief True when the duty fits the period and the offset is inside it (or all 0). */
                bool isValid() const {
                        return periodNs == 0 ? dutyNs == 0 && offsetNs == 0 : dutyNs <= periodNs && offsetNs < periodNs;
                }

                bool operator==(const PwmWaveform &o) const {
                        return periodNs == o.periodNs && dutyNs == o.dutyNs && offsetNs == o.offsetNs;
                }
};

/**
 * @brief An LEDC channel's timing: its timer's period (`div256 / 256 × 2^bits` source ticks, whole
 *        ticks) and the channel's duty and start in counts of `period / 2^bits`.
 *
 * Rounding is Linux's (`PWM_IOCTL_ROUNDWF`): each value down to the next the hardware has, up only
 * when there is none below; of the periods every resolution gives, the longest not above the
 * request wins, ties to the most bits. The source's tick must be whole picoseconds (80 MHz: 12.5 ns).
 *
 * @par Thread Safety
 * A plain value.
 */
struct LedcTiming {
                static constexpr uint32_t MaxBits = 20;               ///< The counter's widest.
                static constexpr uint32_t MinDiv256 = 256;            ///< The divider's least: 1.
                static constexpr uint32_t MaxDiv256 = (1u << 18) - 1; ///< Its most: 1023.996.

                uint32_t ticks = 0;  ///< The period in source ticks; 0: off.
                uint32_t div256 = 0; ///< The timer's divider, 8 fraction bits.
                uint8_t  bits = 0;   ///< The timer's resolution: 2^bits counts a period.
                uint32_t duty = 0;   ///< Active counts: 0 to 2^bits.
                uint32_t hpoint = 0; ///< The count it starts at: 0 to 2^bits - 1.

                /** @brief The timing nearest @p wanted (a valid waveform) at @p sourceHz, rounded as Linux's. */
                static LedcTiming round(const PwmWaveform &wanted, uint32_t sourceHz);

                /** @brief What it makes, in whole ns (each value's fraction of a ns dropped). */
                PwmWaveform waveform(uint32_t sourceHz) const;

                /** @brief True if @p o runs on a timer set up the same way (it can share this one's). */
                bool sharesTimer(const LedcTiming &o) const { return div256 == o.div256 && bits == o.bits; }
};

BMDS_NAMESPACE_END
