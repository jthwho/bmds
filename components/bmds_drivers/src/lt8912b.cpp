/**
 * @file      lt8912b.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/lt8912b.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Main page registers.
        constexpr uint8_t RegChipIdHigh = 0x00;
        constexpr uint8_t RegChipIdLow = 0x01;
        constexpr uint8_t RegHdmiStatus = 0xc1; // bit 7: hot-plug detected
        constexpr uint8_t HotPlug = 0x80;

} // namespace

Error Lt8912b::attach() {
        I2cDevice **pages[] = {&_main, &_dsi, &_avi};
        for (uint8_t i = 0; i < 3; ++i) {
                auto [d, err] = _bus.device(static_cast<uint8_t>(_mainAddress + i), 0, TimeoutMs);
                if (err.isError()) return err;
                *pages[i] = d;
        }
        return Error::Ok;
}

Result<uint16_t> Lt8912b::chipId() {
        if (_main == nullptr) return makeError<uint16_t>(Error::InvalidState);
        auto [high, herr] = _main->readRegister(RegChipIdHigh, TimeoutMs);
        if (herr.isError()) return makeError<uint16_t>(herr);
        auto [low, lerr] = _main->readRegister(RegChipIdLow, TimeoutMs);
        if (lerr.isError()) return makeError<uint16_t>(lerr);
        return makeResult(static_cast<uint16_t>(high << 8 | low));
}

Result<bool> Lt8912b::isMonitorConnected() {
        if (_main == nullptr) return makeError<bool>(Error::InvalidState);
        auto [status, err] = _main->readRegister(RegHdmiStatus, TimeoutMs);
        if (err.isError()) return makeError<bool>(err);
        return makeResult((status & HotPlug) != 0);
}

BMDS_NAMESPACE_END
