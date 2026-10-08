/**
 * @file      es8311regs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/es8311regs.h>

BMDS_NAMESPACE_BEGIN

namespace Es8311Regs {

        namespace {

                constexpr uint32_t MaxInternalHz = 35'000'000;   // §8.5, DVDD 3.3 V
                constexpr uint32_t MaxMultiplyInHz = 10'000'000; // §8.4, DVDD 3.3 V
                constexpr uint8_t  Multipliers[] = {1, 2, 4, 8};

        } // namespace

        Result<ClockPlan> clockPlan(uint32_t mclkHz, uint32_t rateHz) {
                if (rateHz < MinRateHz || rateHz > MaxRateHz || mclkHz == 0)
                        return makeError<ClockPlan>(Error::NotSupported);
                ClockPlan best;
                for (uint8_t m = 0; m < sizeof(Multipliers); ++m) {
                        for (uint32_t div = 1; div <= 8; ++div) {
                                if (mclkHz % div != 0) continue;
                                const uint32_t divided = mclkHz / div;
                                if (m > 0 && divided >= MaxMultiplyInHz) continue;
                                const uint64_t internal = static_cast<uint64_t>(divided) * Multipliers[m];
                                if (internal > MaxInternalHz || internal % rateHz != 0) continue;
                                const uint64_t ratio = internal / rateHz;
                                if (ratio < 256 || ratio % 16 != 0) continue;
                                if (best.ratio != 0 && ratio >= best.ratio) continue;
                                best.clkManager2 = static_cast<uint8_t>(((div - 1) << 5) | (m << 3));
                                best.internalHz = static_cast<uint32_t>(internal);
                                best.ratio = static_cast<uint16_t>(ratio);
                        }
                }
                if (best.ratio == 0) return makeError<ClockPlan>(Error::NotSupported);
                best.clkManager3 = 0x10; // single speed, ADC OSR 16 (§10.6; the guide's table)
                // DAC OSR: Espressif's table uses 0x20 up to 16 kHz, 0x10 above.
                best.clkManager4 = rateHz <= 16000 ? 0x20 : 0x10;
                best.clkManager5 = 0x00;
                return makeResult(best);
        }

        uint8_t volumeRegister(int32_t centiDb) {
                if (centiDb <= MinVolumeCentiDb) return 0x00;
                if (centiDb >= MaxVolumeCentiDb) return 0xff;
                // 0xBF is 0 dB, 0.5 dB a step; round toward the quieter step.
                const int32_t steps = centiDb >= 0 ? centiDb / 50 : -((-centiDb + 49) / 50);
                return static_cast<uint8_t>(0xbf + steps);
        }

        int32_t volumeCentiDb(uint8_t reg) {
                return (static_cast<int32_t>(reg) - 0xbf) * 50;
        }

} // namespace Es8311Regs

BMDS_NAMESPACE_END
