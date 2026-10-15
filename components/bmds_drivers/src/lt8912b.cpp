/**
 * @file      lt8912b.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/lt8912b.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Main page registers.
        constexpr uint8_t RegChipIdHigh = 0x00;
        constexpr uint8_t RegChipIdLow = 0x01;
        constexpr uint8_t RegHdmiStatus = 0xc1; // bit 7: hot-plug detected
        constexpr uint8_t HotPlug = 0x80;

        struct RegValue {
                        uint8_t reg;
                        uint8_t value;
        };

        // Main page: the digital clocks on (the HDMI block too, which the
        // hot-plug bit needs), the analog blocks: HDMI transmitter (output off
        // for now), CBUS, the HDMI PLL, the DSI receiver (no P/N swap,
        // equalizer).
        constexpr RegValue PowerUp[] = {
                {0x02, 0xf7}, {0x08, 0xff}, {0x09, 0xff}, {0x0a, 0xff}, {0x0b, 0x7c}, {0x0c, 0xff},
                {0x31, 0xe1}, {0x32, 0xe1}, {0x33, 0x0c}, {0x37, 0x00}, {0x38, 0x22}, {0x60, 0x82},
                {0x39, 0x45}, {0x3a, 0x00}, {0x3b, 0x00}, {0x44, 0x31}, {0x55, 0x44}, {0x57, 0x01},
                {0x5a, 0x02}, {0x3e, 0xd6}, {0x3f, 0xd4}, {0x41, 0x3c},
        };

        // DSI page: the pixel clock recovery (DDS) from the incoming video:
        // its start frequency word, the FIFO's full and empty thresholds, the
        // timer and trend steps. The values Lontium's reference sequence uses.
        constexpr RegValue Dds[] = {
                {0x4e, 0x93}, {0x4f, 0x3e}, {0x50, 0x29}, {0x51, 0x80}, {0x1e, 0x4f}, {0x1f, 0x5e}, {0x20, 0x01},
                {0x21, 0x2c}, {0x22, 0x01}, {0x23, 0xfa}, {0x24, 0x00}, {0x25, 0xc8}, {0x26, 0x00}, {0x27, 0x5e},
                {0x28, 0x01}, {0x29, 0x2c}, {0x2a, 0x01}, {0x2b, 0xfa}, {0x2c, 0x00}, {0x2d, 0xc8}, {0x2e, 0x00},
                {0x42, 0x64}, {0x43, 0x00}, {0x44, 0x04}, {0x45, 0x00}, {0x46, 0x59}, {0x47, 0x00}, {0x48, 0xf2},
                {0x49, 0x06}, {0x4a, 0x00}, {0x4b, 0x72}, {0x4c, 0x45}, {0x4d, 0x00}, {0x52, 0x08}, {0x53, 0x00},
                {0x54, 0xb2}, {0x55, 0x00}, {0x56, 0xe4}, {0x57, 0x0d}, {0x58, 0x00}, {0x59, 0xe4}, {0x5a, 0x8a},
                {0x5b, 0x00}, {0x5c, 0x34}, {0x51, 0x00},
        };

        // Main page: the output PLL fed from the pixel clock, the scaler
        // bypassed (the "LVDS bypass" path the HDMI transmitter takes its
        // pixels from), LVDS itself off.
        constexpr RegValue Bypass[] = {
                {0x44, 0x30}, {0x51, 0x05}, {0x50, 0x24}, {0x51, 0x2d}, {0x52, 0x04},
                {0x69, 0x0e}, {0x69, 0x8e}, {0x6a, 0x00}, {0x6c, 0xb8}, {0x6b, 0x51},
                {0x04, 0xfb}, {0x04, 0xff}, {0x7f, 0x00}, {0xa8, 0x13}, {0x44, 0x31},
        };

        Error writeAll(I2cDevice &d, const RegValue *values, size_t count, unsigned int timeoutMs) {
                for (size_t i = 0; i < count; ++i) {
                        const Error err = d.writeRegister(values[i].reg, values[i].value, timeoutMs);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        template <size_t N> Error writeAll(I2cDevice &d, const RegValue (&values)[N], unsigned int timeoutMs) {
                return writeAll(d, values, N, timeoutMs);
        }

} // namespace

Error Lt8912b::attach() {
        I2cDevice **pages[] = {&_main, &_dsi, &_avi};
        for (uint8_t i = 0; i < 3; ++i) {
                auto [d, err] = _bus.device(static_cast<uint8_t>(_mainAddress + i), 0, TimeoutMs);
                if (err.isError()) return err;
                *pages[i] = d;
        }
        // The digital clocks (not 0x02, a reset): the hot-plug bit reads 0
        // while they're off.
        return writeAll(*_main, PowerUp + 1, 5, TimeoutMs);
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

Error Lt8912b::configure(const VideoTiming &t, uint8_t lanes, bool hdmi) {
        if (_main == nullptr) return Error::InvalidState;
        if (lanes < 1 || lanes > 4) return Error::Invalid;
        Error err = writeAll(*_main, PowerUp, TimeoutMs);
        // The DSI receiver: termination on, settle time, lanes (0 means 4).
        _settleUsed = _settle != 0 ? _settle : 0x10;
        const RegValue receiver[] = {
                {0x10, 0x01}, {0x11, _settleUsed}, {0x13, static_cast<uint8_t>(lanes & 3)}, {0x14, 0x00}, {0x15, 0x00},
                {0x1a, 0x03}, {0x1b, 0x03},
        };
        if (err.isOk()) err = writeAll(*_dsi, receiver, TimeoutMs);
        if (err.isOk()) err = writeAll(*_dsi, Dds, TimeoutMs);
        // A test aid: another DDS frequency word in place of the table's 0x3e2993 (setDds()).
        if (err.isOk() && _dds != 0) {
                const RegValue word[] = {{0x4e, static_cast<uint8_t>(_dds)},
                                         {0x4f, static_cast<uint8_t>(_dds >> 8)},
                                         {0x50, static_cast<uint8_t>(_dds >> 16)}};
                err = writeAll(*_dsi, word, TimeoutMs);
        }
        // The video timing the receiver expects.
        const uint32_t hTotal = t.hTotal();
        const uint32_t vTotal = t.vTotal();
        const RegValue timing[] = {
                {0x18, static_cast<uint8_t>(t.hSync)},
                {0x19, static_cast<uint8_t>(t.vSync)},
                {0x1c, static_cast<uint8_t>(t.hActive)},
                {0x1d, static_cast<uint8_t>(t.hActive >> 8)},
                {0x2f, 0x0c}, // FIFO length
                {0x34, static_cast<uint8_t>(hTotal)},
                {0x35, static_cast<uint8_t>(hTotal >> 8)},
                {0x36, static_cast<uint8_t>(vTotal)},
                {0x37, static_cast<uint8_t>(vTotal >> 8)},
                {0x38, static_cast<uint8_t>(t.vBack)},
                {0x39, static_cast<uint8_t>(t.vBack >> 8)},
                {0x3a, static_cast<uint8_t>(t.vFront)},
                {0x3b, static_cast<uint8_t>(t.vFront >> 8)},
                {0x3c, static_cast<uint8_t>(t.hBack)},
                {0x3d, static_cast<uint8_t>(t.hBack >> 8)},
                {0x3e, static_cast<uint8_t>(t.hFront)},
                {0x3f, static_cast<uint8_t>(t.hFront >> 8)},
        };
        if (err.isOk()) err = writeAll(*_dsi, timing, TimeoutMs);
        // The AVI infoframe: RGB, 16:9 or 4:3 by the shape, no VIC; its
        // checksum makes the header (0x82 0x02 0x0d) and payload sum to 0.
        const uint8_t aspect = t.hActive * 3 == t.vActive * 4 ? 0x01 : 0x02;
        const uint8_t pb2 = static_cast<uint8_t>(aspect << 4 | 0x08);
        const uint8_t pb1 = 0x10;
        const uint8_t pb0 = static_cast<uint8_t>(0x100 - ((0x82 + 0x02 + 0x0d + pb1 + pb2) & 0xff));
        const uint8_t polarity = static_cast<uint8_t>((t.hSyncHigh ? 0x02 : 0) | (t.vSyncHigh ? 0x01 : 0));
        if (err.isOk()) err = _avi->writeRegister(0x3c, 0x41, TimeoutMs); // null packets on
        if (err.isOk()) err = _main->writeRegister(0xab, polarity, TimeoutMs);
        const RegValue infoframe[] = {{0x43, pb0}, {0x44, pb1}, {0x45, pb2}, {0x46, 0x00}, {0x47, 0x00}};
        if (err.isOk()) err = writeAll(*_avi, infoframe, TimeoutMs);
        if (err.isOk()) err = _main->writeRegister(0xb2, hdmi ? 0x01 : 0x00, TimeoutMs);
        if (err.isOk()) err = writeAll(*_main, Bypass, TimeoutMs);
        if (err.isOk()) err = setOutput(true);
        return err;
}

Error Lt8912b::measured(uint16_t &first, uint16_t &lines) {
        if (_main == nullptr) return Error::InvalidState;
        uint8_t v[4] = {};
        for (uint8_t i = 0; i < 4; ++i) {
                auto [b, err] = _main->readRegister(static_cast<uint8_t>(0x9c + i), TimeoutMs);
                if (err.isError()) return err;
                v[i] = b;
        }
        first = static_cast<uint16_t>(v[1] << 8 | v[0]);
        lines = static_cast<uint16_t>(v[3] << 8 | v[2]);
        return Error::Ok;
}

Error Lt8912b::resetReceiver() {
        if (_main == nullptr) return Error::InvalidState;
        const RegValue steps[][2] = {{{0x03, 0x7f}, {0x03, 0xff}}, {{0x05, 0xfb}, {0x05, 0xff}}};
        for (const auto &step : steps) {
                Error err = _main->writeRegister(step[0].reg, step[0].value, TimeoutMs);
                Task::sleep(10);
                if (err.isOk()) err = _main->writeRegister(step[1].reg, step[1].value, TimeoutMs);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

Error Lt8912b::setOutput(bool on) {
        if (_main == nullptr) return Error::InvalidState;
        return _main->writeRegister(0x33, on ? 0x0e : 0x0c, TimeoutMs);
}

BMDS_NAMESPACE_END
