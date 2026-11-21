/**
 * @file      adcdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/dev/adc0`, one per open (devplan/io.md §15.7): one-shot readings of `io/adc0`'s pins,
 *        calibrated to millivolts, for any open; the first open to `read()` streams them (at
 *        `io/adc0`'s rate, until it closes), its reads giving `Sample` records.
 *
 * @par Thread Safety
 * Any task; the ADC's lock. The driver's interrupt counts the stream's frames.
 */
class AdcDevice final : public Stream {
        public:
                /** @brief What `BMDS_ADC_INFO` reports. */
                struct Info {
                                bool     configured = false;
                                bool     streaming = false;
                                bool     mine = false;
                                uint32_t channels = 0;
                                uint32_t rateHz = 0;
                                uint32_t buffer = 0;
                                int64_t  startNs = 0;
                                uint64_t samples = 0; ///< Conversions streamed, those lost counted.
                                uint64_t lost = 0;
                };

                /** @brief A pin of `io/adc0`'s (`BMDS_ADC_CHANNEL`). */
                struct Channel {
                                int32_t  pin = -1;
                                uint32_t unit = 0;
                                uint32_t channel = 0;
                                uint32_t attenCdb = 0;
                                uint32_t divider = 0; ///< The board's (`BoardPin::analogDivider`), or 0.
                };

                /** @brief A one-shot reading (`BMDS_ADC_READ`). */
                struct Reading {
                                uint32_t raw = 0;
                                int32_t  mv = 0;
                                int64_t  timeNs = 0;
                };

                /** @brief A streamed conversion, as `read()` gives them (`bmds_adc_sample_t`). */
                struct Sample {
                                uint8_t  pin = 0;
                                uint8_t  reserved = 0;
                                uint16_t raw = 0;
                                uint16_t mv = 0;
                                uint16_t reserved2 = 0;
                };
                static_assert(sizeof(Sample) == 8, "bmds_adc_sample_t");

                /** @brief Adds `/dev/adc0` (once the ADC's setting is watched). */
                static Error addFiles();

                /** @brief The ADC and its stream; `mine` for @p self's. @blocking */
                static Error status(Info &out, const AdcDevice *self);

                /** @brief Pin @p index of `io/adc0`'s: `Invalid` past them, `NotExist` without `io/adc0`. @blocking */
                static Error channel(unsigned index, Channel &out);

                /** @brief A one-shot reading of @p pin: `NotExist` (not `io/adc0`'s), `Busy` (its unit streams). @blocking */
                static Error reading(int pin, Reading &out);

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override { return &_pollers; }
                AdcDevice     *adcDevice() override { return this; }
                void           handleClosed(uint8_t mode) override;

                /** @brief The stream's interrupt: a frame in, its reader and pollers woken. */
                void wakeFromIsr();

        private:
                AdcDevice() = default;

                Semaphore     _ready{1, 0};
                WaitListOf<4> _pollers;
};

BMDS_NAMESPACE_END
