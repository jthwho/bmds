/**
 * @file      ppsout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/board.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ethernetdevice.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The 1588 clock's PPS on a pin (devplan/network.md §31b): rising on each of its seconds,
 *        100 ms high.
 *
 * The MAC's own PPS doesn't reach the GPIO matrix on this chip, so a GPTimer on the same crystal
 * makes the edges: its alarm toggles the pin through the ETM, with no interrupt between, and its
 * interrupt (after each edge) arms the next one. An alarm count comes from the timer's count
 * latched between two reads of the 1588 clock, and the addend's rate: the timer counts a PLL
 * that is a whole multiple of the crystal the 1588 clock counts, so the rate between them is
 * exact and an edge is off by at most half that bracket (about 120 ns).
 *
 * @par Thread Safety
 * `start()` once, at the driver's start; `rearm()` from any task; `status()` from anywhere.
 */
class PpsOut {
        public:
                using Status = EthernetDevice::PpsStatus;

                constexpr PpsOut() = default;

                /** @brief Starts it on @p pin for @p device's 1588 clock. */
                Error start(GpioNum pin, EthernetDevice &device);

                /** @brief The 1588 clock was stepped or set: the pending edge is armed again. */
                void rearm();

                Status status() const;

        private:
                bool onAlarm(); // the interrupt: arms the next edge
                bool arm(bool rising);

                EthernetDevice       *_device = nullptr;
                void                 *_timer = nullptr; // gptimer_handle_t
                GpioNum               _pin = NoGpio;
                bool                  _hardware = false; // the MAC's own PPS (revision 3 on)
                void                 *_hw = nullptr;     // its timer group (timg_dev_t)
                uint32_t              _num = 0;          // and its number there
                int64_t               _risingAtNs = 0;   // the 1588 time of the last rising edge armed
                bool                  _high = false;     // the level after the pending edge's toggle is the other
                std::atomic<uint32_t> _edges{0};
                std::atomic<uint32_t> _bracketNs{0};
                std::atomic<uint32_t> _late{0};
};

BMDS_NAMESPACE_END
