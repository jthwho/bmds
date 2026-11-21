/**
 * @file      pwmchip.h
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
#include <bmds/pwmtiming.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/dev/pwmchip0`, one per open (devplan/io.md §15.1): the PWM channels (`io/pwmN` gives
 *        each its pin) as Linux's PWM character device. A channel is held by one open at a time;
 *        an operation on one nobody holds takes it for this one; its last close lets its
 *        channels go (their waveforms stay).
 *
 * @par Thread Safety
 * Any task: the channels' lock.
 */
class PwmChip final : public Stream {
        public:
                /** @brief The chip's channels (`io/pwm0` .. `io/pwm7`). */
                static constexpr unsigned Channels = 8;

                /** @brief A channel and the chip, as `BMDS_PWM_INFO` reports them. */
                struct Info {
                                uint32_t npwm = Channels;
                                bool     hasPin = false;    ///< `io/pwmN` gives it a pin.
                                bool     requested = false; ///< An open holds it.
                                bool     mine = false;      ///< This one does.
                                bool     on = false;        ///< Its period isn't 0.
                                int32_t  pin = -1;
                                int32_t  timer = -1; ///< Its LEDC timer, or -1 (off).
                                uint32_t bits = 0;   ///< Its resolution, or 0 (off).
                                uint32_t sourceHz = 0;
                };

                /** @brief Adds `/dev/pwmchip0` (once the ports have started). */
                static Error addFiles();

                /** @brief Holds channel @p hw for this open: `Busy` (another's), `NotExist` (no pin), `Invalid`. @blocking */
                Error request(unsigned hw);

                /** @brief Lets it go: `Invalid` unless this open holds it. @blocking */
                Error release(unsigned hw);

                /** @brief Rounds @p wf as setting it would (Linux's rounding), nothing set. @blocking */
                Error round(unsigned hw, PwmWaveform &wf);

                /** @brief The channel's waveform. @blocking */
                Error waveform(unsigned hw, PwmWaveform &out);

                /**
                 * @brief Sets the channel's waveform, rounded, from its next period; with @p exact,
                 *        `OutOfRange` unless it rounds to itself. `Busy`: no timer free for its period.
                 * @blocking
                 */
                Error setWaveform(unsigned hw, const PwmWaveform &wf, bool exact);

                /** @brief Channel @p hw and the chip. @blocking */
                Error info(unsigned hw, Info &out);

                Result<size_t> read(WritableByteSpan, unsigned int) override {
                        return makeError<size_t>(Error::Invalid);
                }
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::Invalid); }
                uint8_t        readiness() override { return 0; }
                PwmChip       *pwmChip() override { return this; }
                void           handleClosed(uint8_t mode) override;

        private:
                PwmChip() = default;

                Error held(unsigned hw); // the channel, held by this open (taken if nobody holds it)
};

BMDS_NAMESPACE_END
