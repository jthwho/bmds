/**
 * @file      es8311.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/audiocodec.h>
#include <bmds/board.h>
#include <bmds/config.h>
#include <bmds/es8311regs.h>
#include <bmds/i2cbus.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The Everest ES8311 mono codec, as an I²S slave on MCLK
 *        (`devplan/audio.md` §2, §16).
 *
 * The chip reports no status, so every register written is read back and
 * compared, and the clock settings are checked against the guide's rules
 * before they are written. A powered-off ES8311 is cut off the bus by the
 * board, so no answer at `attach()` means "unpowered or absent".
 *
 * @par Thread Safety
 * Not thread-safe; owned by an `AudioDevice`.
 */
class Es8311 final : public AudioCodec {
        public:
                /** @brief Controls, in `controls()` order (devplan/audio.md §16). */
                enum Control : uint8_t {
                        DacSlot,
                        DacVolume,
                        DacMute,
                        DacRamp,
                        DacEq,
                        Drc,
                        ControlCount
                };

                /**
                 * @brief A codec at @p address on @p bus, its supply switched by
                 *        @p power (may be invalid).
                 */
                Es8311(I2cBus &bus, uint8_t address, GpioLine power);

                const char              *name() const override { return "ES8311"; }
                Error                    attach() override;
                void                     identity(char *out, size_t size) const override;
                Error                    start(uint32_t mclkHz, uint32_t rateHz) override;
                Error                    stop() override;
                bool                     isRunning() const override { return _running; }
                uint8_t                  outputChannels() const override { return 1; }
                Error                    setVolume(int32_t centiDb) override;
                Span<const AudioControl> controls() const override;
                int32_t                  controlValue(size_t index) const override;
                Error                    setControl(size_t index, int32_t value) override;
                Span<const uint8_t>      registerList() const override;
                Result<uint8_t>          readRegister(uint8_t reg) override;
                Error                    writeRegister(uint8_t reg, uint8_t value) override;
                int16_t                  expectedRegister(uint8_t reg) const override;

                /** @brief Returns the bus. */
                I2cBus &bus() { return _bus; }

                /** @brief Returns the address. */
                uint8_t address() const { return _address; }

        private:
                static constexpr unsigned int TimeoutMs = 50;

                // Writes and reads back; records a fault in _fault under @p what.
                Error put(uint8_t reg, uint8_t value, const char *what);
                // Applies control @p index to the chip.
                Error apply(size_t index);
                // Forgets what was written (the chip was reset).
                void forget();

                I2cBus    &_bus;
                I2cDevice *_device = nullptr;
                GpioLine   _power;
                uint8_t    _address;
                uint16_t   _chipId = 0;
                uint8_t    _version = 0;
                bool       _running = false;
                int32_t    _values[ControlCount];
                uint8_t    _shadow[Es8311Regs::LastRegister + 1] = {};
                uint8_t    _written[(Es8311Regs::LastRegister + 1 + 7) / 8] = {};
};

BMDS_NAMESPACE_END
