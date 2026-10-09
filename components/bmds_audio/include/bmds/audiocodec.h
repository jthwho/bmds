/**
 * @file      audiocodec.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/audiocontrol.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A codec chip behind an I²S port: what an `AudioDevice` asks of it
 *        (`devplan/audio.md` §16).
 *
 * The device clocks the codec (MCLK, BCLK, LRCK) and the codec only follows.
 * Controls are kept by the codec while it is stopped and applied at each
 * start, which resets the chip. A failure records the register and error
 * (`fault()`), for `audio status`.
 *
 * @par Thread Safety
 * Not thread-safe; the owning `AudioDevice` serializes calls.
 */
class AudioCodec {
        public:
                /** @brief Where the last failure happened. */
                struct Fault {
                                Error       error = Error::Ok; ///< What went wrong (`Ok`: nothing has).
                                int16_t     reg = -1;          ///< The register written or read, or -1.
                                uint8_t     wrote = 0;         ///< The value written (a readback mismatch).
                                uint8_t     read = 0;          ///< The value read back.
                                const char *what = "";         ///< The step: "chip id", "start", ...
                };

                AudioCodec() = default;
                AudioCodec(const AudioCodec &) = delete;
                AudioCodec &operator=(const AudioCodec &) = delete;

                /** @brief Returns the chip's name, e.g. `"ES8311"`. */
                virtual const char *name() const = 0;

                /**
                 * @brief Powers the chip (if the board switches it) and reads its identity.
                 * @return `Ok`, `IoError` (no answer), or the bus's error.
                 * @blocking
                 */
                virtual Error attach() = 0;

                /**
                 * @brief Writes "id 0x8311 version 0x00" (or what the chip offers)
                 *        into @p out, as read at `attach()`.
                 */
                virtual void identity(char *out, size_t size) const = 0;

                /**
                 * @brief Resets the chip and sets it up to play @p rateHz from an MCLK
                 *        of @p mclkHz, with the controls' values. MCLK must be running.
                 * @return `Ok`, `NotSupported` (no clock setting fits), `DeviceError`
                 *         (a register read back wrong), or the bus's error.
                 * @blocking
                 */
                virtual Error start(uint32_t mclkHz, uint32_t rateHz) = 0;

                /**
                 * @brief Mutes the chip and stops its converters, without a click (an
                 *        ES8311 keeps its analog side up). Call before MCLK stops.
                 * @blocking
                 */
                virtual Error stop() = 0;

                /** @brief Returns true between a successful `start()` and `stop()`. */
                virtual bool isRunning() const = 0;

                /** @brief Returns the channels the chip plays (1: one, fed from either slot). */
                virtual uint8_t outputChannels() const = 0;

                /**
                 * @brief Sets the hardware volume to @p centiDb (clamped to the chip's
                 *        range; its floor for a mute), ramped by the chip, at once if
                 *        running, else at `start()`.
                 * @blocking
                 */
                virtual Error setVolume(int32_t centiDb) = 0;

                /** @brief Returns the controls the chip offers. */
                virtual Span<const AudioControl> controls() const = 0;

                /** @brief Returns control @p index's value. */
                virtual int32_t controlValue(size_t index) const = 0;

                /**
                 * @brief Sets control @p index to @p value (checked by the caller with
                 *        `AudioControl::parse()`), at once if running.
                 * @blocking
                 */
                virtual Error setControl(size_t index, int32_t value) = 0;

                /** @brief Returns the registers `audio regs` shows, in order. */
                virtual Span<const uint8_t> registerList() const = 0;

                /**
                 * @brief Reads register @p reg.
                 * @blocking
                 */
                virtual Result<uint8_t> readRegister(uint8_t reg) = 0;

                /**
                 * @brief Writes @p value to register @p reg, raw: for debugging. The
                 *        value becomes what the chip is expected to hold.
                 * @blocking
                 */
                virtual Error writeRegister(uint8_t reg, uint8_t value) = 0;

                /**
                 * @brief Returns what @p reg is expected to hold, or -1 if nothing has
                 *        been written to it since the chip was reset.
                 */
                virtual int16_t expectedRegister(uint8_t reg) const = 0;

                /** @brief Returns the last failure. */
                const Fault &fault() const { return _fault; }

        protected:
                ~AudioCodec() = default;

                Fault _fault;
};

BMDS_NAMESPACE_END
