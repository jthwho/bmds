/**
 * @file      es8311regs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The Everest ES8311's registers and the arithmetic on them that
 *        doesn't need the chip (`devplan/audio.md` §2, §16): portable, so the
 *        host tests check it.
 *
 * Section numbers are the *ES8311 User Guide*'s (rev 1.00). Registers it
 * leaves out carry the source of what we know.
 */
namespace Es8311Regs {

        inline constexpr uint8_t Reset = 0x00;        ///< CSM_ON, MSC, reset bits (§9.1).
        inline constexpr uint8_t ClkManager1 = 0x01;  ///< MCLK source and clock gates (§8.3).
        inline constexpr uint8_t ClkManager2 = 0x02;  ///< Pre-divider and multiplier (§8.4).
        inline constexpr uint8_t ClkManager3 = 0x03;  ///< ADC speed mode and OSR (§10.6).
        inline constexpr uint8_t ClkManager4 = 0x04;  ///< DAC OSR (not in the guide; Espressif's driver).
        inline constexpr uint8_t ClkManager5 = 0x05;  ///< ADC and DAC clock dividers (§8.5).
        inline constexpr uint8_t ClkManager6 = 0x06;  ///< BCLK invert and divider (§8.6).
        inline constexpr uint8_t ClkManager7 = 0x07;  ///< Tri-state, LRCK divider high (§8.7).
        inline constexpr uint8_t ClkManager8 = 0x08;  ///< LRCK divider low (§8.7).
        inline constexpr uint8_t SdpIn = 0x09;        ///< DAC serial port: slot, mute, word, format (§7).
        inline constexpr uint8_t SdpOut = 0x0a;       ///< ADC serial port (§7).
        inline constexpr uint8_t System0b = 0x0b;     ///< Not in the guide (Espressif: 0x00).
        inline constexpr uint8_t System0c = 0x0c;     ///< Not in the guide (Espressif: 0x00).
        inline constexpr uint8_t Power = 0x0d;        ///< Analog power, VMID (§9.2).
        inline constexpr uint8_t PowerAdc = 0x0e;     ///< PGA and ADC modulator power (§10.1, §10.2).
        inline constexpr uint8_t LowPower = 0x0f;     ///< Low-power modes (§9.3).
        inline constexpr uint8_t System10 = 0x10;     ///< Not in the guide (Espressif: 0x1f).
        inline constexpr uint8_t System11 = 0x11;     ///< Not in the guide (Espressif: 0x7f).
        inline constexpr uint8_t PowerDac = 0x12;     ///< Bit 1 PDN_DAC (§11); the rest not in the guide.
        inline constexpr uint8_t Output = 0x13;       ///< Not in the guide (Espressif: 0x10, the headphone drive).
        inline constexpr uint8_t AdcInput = 0x14;     ///< DMIC, input select, PGA gain (§10.1).
        inline constexpr uint8_t AdcRamp = 0x15;      ///< ADC ramp rate, DMIC sense (§10.7).
        inline constexpr uint8_t AdcScale = 0x16;     ///< ADC scale (§10.6).
        inline constexpr uint8_t AdcVolume = 0x17;    ///< ADC volume (§10.3.2).
        inline constexpr uint8_t AdcHpf1 = 0x1b;      ///< ADC high-pass stage 1 (§10.3.1).
        inline constexpr uint8_t AdcEq = 0x1c;        ///< ADC EQ bypass, HPF (§10.3.1, §10.3.3).
        inline constexpr uint8_t DacMute = 0x31;      ///< DAC mutes (§11.3).
        inline constexpr uint8_t DacVolume = 0x32;    ///< DAC volume (§11.3).
        inline constexpr uint8_t DacDrc = 0x34;       ///< DRC enable, window (§11.3).
        inline constexpr uint8_t DacDrcLevels = 0x35; ///< DRC levels (§11.3).
        inline constexpr uint8_t DacRamp = 0x37;      ///< DAC ramp rate, EQ bypass (§11.2, §11.4).
        inline constexpr uint8_t Gpio = 0x44;         ///< ADC2DAC, ADCDAT select (§10.4, §11.1).
        inline constexpr uint8_t Gp45 = 0x45;         ///< Not in the guide (Espressif: 0x00).
        inline constexpr uint8_t ChipId1 = 0xfd;      ///< Chip ID, high (Espressif's driver).
        inline constexpr uint8_t ChipId2 = 0xfe;      ///< Chip ID, low (Espressif's driver).
        inline constexpr uint8_t ChipVersion = 0xff;  ///< Chip version (Espressif's driver).

        /** @brief The chip ID in 0xFD/0xFE (read on the board: 0x8311, version 0x01). */
        inline constexpr uint16_t ExpectedChipId = 0x8311;

        /** @brief The registers that hold state: 0x00 to 0x45 (the EQ coefficients among them). */
        inline constexpr uint8_t LastRegister = 0x45;

        /** @brief The DAC's lowest and highest rates (§1). */
        inline constexpr uint32_t MinRateHz = 8000;
        inline constexpr uint32_t MaxRateHz = 96000;

        /** @brief The clock manager's registers for one MCLK and rate. */
        struct ClockPlan {
                        uint8_t  clkManager2 = 0; ///< 0x02: DIV_PRE (7:5), MULT_PRE (4:3).
                        uint8_t  clkManager3 = 0; ///< 0x03: single speed, ADC OSR.
                        uint8_t  clkManager4 = 0; ///< 0x04: DAC OSR.
                        uint8_t  clkManager5 = 0; ///< 0x05: ADC and DAC dividers.
                        uint32_t internalHz = 0;  ///< The internal master clock it makes.
                        uint16_t ratio = 0;       ///< Internal DAC clock to LRCK.
        };

        /**
         * @brief Finds the clock settings for MCLK @p mclkHz and rate @p rateHz.
         *
         * Searches the pre-divider (÷1 to ÷8) and multiplier (×1, 2, 4, 8) for the
         * smallest DAC clock ratio of at least 256 that is a multiple of 16
         * (§8.5), the internal clock at most 35 MHz, and, when multiplying, the
         * divider's output under 10 MHz (§8.4; DVDD is 3.3 V on this board). Ties
         * go to the smaller multiplier. The ADC and DAC dividers stay ÷1.
         *
         * @return The plan, or `NotSupported` (no setting meets the rules, or the
         *         rate is outside 8 to 96 kHz).
         * @nonblocking
         */
        Result<ClockPlan> clockPlan(uint32_t mclkHz, uint32_t rateHz);

        /** @brief The DAC volume's range in hundredths of a dB (0x32: −95.5 to +32 dB, §11.3). */
        inline constexpr int32_t MinVolumeCentiDb = -9550;
        inline constexpr int32_t MaxVolumeCentiDb = 3200;

        /** @brief The 0x32 value for @p centiDb (clamped, rounded down to 0.5 dB). */
        uint8_t volumeRegister(int32_t centiDb);

        /** @brief The volume 0x32 value @p reg sets, in hundredths of a dB. */
        int32_t volumeCentiDb(uint8_t reg);

} // namespace Es8311Regs

BMDS_NAMESPACE_END
