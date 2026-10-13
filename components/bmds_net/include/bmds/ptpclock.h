/**
 * @file      ptpclock.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

class EthernetDevice;

/**
 * @brief An Ethernet device's 1588 clock as a device file, `/dev/ptpN` (devplan/network.md §31e):
 *        what `ptp_ctl` operates on (the loader's), and Linux's PHC interface through `linux.c`.
 *
 * One per device, for the kernel's life; every fd on it shares it. Reads and writes are refused:
 * it is only operated on.
 *
 * @par Thread Safety
 * Any task.
 */
class PtpClock final : public Stream {
        public:
                /** @brief Adds `/dev/ptpN` for each Ethernet device. After the devices; once. */
                static Error addFiles();

                Result<size_t> read(WritableByteSpan, unsigned int) override {
                        return makeError<size_t>(Error::Invalid);
                }
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                PtpClock      *ptpClock() override { return this; }

                /** @brief Returns the clock's time: ns since 1970 on PTP's timescale (TAI). @nonblocking */
                int64_t timeNs() const;

                /** @brief Returns N of `/dev/ptpN`. */
                uint32_t index() const { return _index; }

                /** @brief Returns the PPS output's pin while it runs, else -1. */
                int32_t ppsPin() const;

                /** @brief Sets it. @return `Ok`, `Busy` (the PTP service has it), `Invalid`, `Timeout`. */
                Error set(int64_t ns);

                /** @brief Adds @p ns in one update. @return As `set()`, `OutOfRange`. */
                Error step(int64_t ns);

                /** @brief Returns its frequency correction, parts per trillion. */
                int64_t frequencyPpt() const;

                /** @brief Sets it (rounded to the clock's step). @return As `set()`, `OutOfRange` (past `MaxAdjustPpb`). */
                Error setFrequencyPpt(int64_t ppt);

                /** @brief The largest frequency correction taken, ppb (the system clock's, the service's). */
                static constexpr int32_t MaxAdjustPpb = 500'000;

        private:
                PtpClock(EthernetDevice &device, uint32_t index) : _device(device), _index(index) {}

                EthernetDevice &_device;
                uint32_t        _index;
};

BMDS_NAMESPACE_END
