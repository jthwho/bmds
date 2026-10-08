/**
 * @file      es8311.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ES8311 (devplan/audio.md §2, §16). Section numbers are the ES8311 User
 * Guide's (rev 1.00). Values for registers the guide leaves out are
 * Espressif's (esp_codec_dev's es8311.c), noted where written.
 */

#include <array>
#include <cstdio>
#include <driver/gpio.h>
#include <bmds/es8311.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

namespace R = Es8311Regs;

namespace {

        using Type = AudioControl::Type;

        // devplan/audio.md §16: names and values are the `audio ctl` interface.
        constexpr AudioControl Controls[Es8311::ControlCount] = {
                {"dac-slot", "The I2S slot the DAC plays", Type::Enum, 0, 1, 1, "left|right"},
                {"dac-volume", "DAC digital volume", Type::Decibel, R::MinVolumeCentiDb, R::MaxVolumeCentiDb, 50, ""},
                {"dac-mute", "Mute the DAC", Type::Bool, 0, 1, 1, ""},
                {"dac-ramp", "Volume soft ramp: 0 off, n = 0.25 dB per 2^(n+1) frames", Type::Int, 0, 15, 1, ""},
                {"dac-eq", "The DAC's equalizer (bypassed when off)", Type::Bool, 0, 1, 1, ""},
                {"drc", "Dynamic range control", Type::Bool, 0, 1, 1, ""},
        };
        constexpr int32_t Defaults[Es8311::ControlCount] = {0, 0, 0, 0, 0, 0};

        // 0x00 to 0x45, then the chip ID and version.
        constexpr std::array<uint8_t, R::LastRegister + 1 + 3> RegisterList = [] {
                std::array<uint8_t, R::LastRegister + 1 + 3> list{};
                for (size_t i = 0; i <= R::LastRegister; ++i) list[i] = static_cast<uint8_t>(i);
                list[R::LastRegister + 1] = R::ChipId1;
                list[R::LastRegister + 2] = R::ChipId2;
                list[R::LastRegister + 3] = R::ChipVersion;
                return list;
        }();

        constexpr uint8_t SdpWord32 = 0x10; // SDP_*_WL = 100: 32-bit words, I2S format (§7)

} // namespace

Es8311::Es8311(I2cBus &bus, uint8_t address, GpioLine power) : _bus(bus), _power(power), _address(address) {
        for (size_t i = 0; i < ControlCount; ++i) _values[i] = Defaults[i];
}

Error Es8311::attach() {
        _fault = {};
        if (_power.isValid()) {
                // Level first, so turning the pin into an output doesn't pulse it off.
                const auto pin = static_cast<gpio_num_t>(_power.gpio);
                (void)gpio_set_level(pin, _power.activeLow ? 0 : 1);
                gpio_config_t io = {};
                io.pin_bit_mask = 1ull << _power.gpio;
                io.mode = GPIO_MODE_OUTPUT;
                if (gpio_config(&io) != ESP_OK) {
                        _fault = {Error::IoError, -1, 0, 0, "power pin"};
                        return Error::IoError;
                }
                (void)gpio_set_level(pin, _power.activeLow ? 0 : 1);
        }
        if (_device == nullptr) {
                auto [device, err] = _bus.device(_address, 0, TimeoutMs);
                if (err.isError()) {
                        _fault = {err, -1, 0, 0, "bus device"};
                        return err;
                }
                _device = device;
        }
        auto [hi, e1] = _device->readRegister(R::ChipId1, TimeoutMs);
        auto [lo, e2] = e1.isOk() ? _device->readRegister(R::ChipId2, TimeoutMs) : makeError<uint8_t>(e1);
        auto [ver, e3] = e2.isOk() ? _device->readRegister(R::ChipVersion, TimeoutMs) : makeError<uint8_t>(e2);
        if (e3.isError()) {
                _fault = {e3, R::ChipId1, 0, 0, "chip id"};
                return e3;
        }
        _chipId = static_cast<uint16_t>(hi << 8 | lo);
        _version = ver;
        return Error::Ok;
}

void Es8311::identity(char *out, size_t size) const {
        std::snprintf(out, size, "id 0x%04x%s, version 0x%02x", _chipId,
                      _chipId == R::ExpectedChipId ? "" : " (unexpected)", _version);
        return;
}

void Es8311::forget() {
        for (uint8_t &b : _written) b = 0;
        return;
}

Error Es8311::put(uint8_t reg, uint8_t value, const char *what) {
        if (_device == nullptr) return Error::InvalidState;
        Error err = _device->writeRegister(reg, value, TimeoutMs);
        if (err.isError()) {
                _fault = {err, reg, value, 0, what};
                return err;
        }
        if (reg <= R::LastRegister) {
                _shadow[reg] = value;
                _written[reg / 8] = static_cast<uint8_t>(_written[reg / 8] | (1u << (reg % 8)));
        }
        auto [back, rerr] = _device->readRegister(reg, TimeoutMs);
        if (rerr.isError()) {
                _fault = {rerr, reg, value, 0, what};
                return rerr;
        }
        if (back != value) {
                _fault = {Error::DeviceError, reg, value, back, what};
                return Error::DeviceError;
        }
        return Error::Ok;
}

Error Es8311::start(uint32_t mclkHz, uint32_t rateHz) {
        _running = false;
        _fault = {};
        auto [plan, perr] = R::clockPlan(mclkHz, rateHz);
        if (perr.isError()) {
                _fault = {perr, R::ClkManager2, 0, 0, "clock plan"};
                return perr;
        }
        // §9.1: reset bits set and CSM off, a few milliseconds, then CSM on with
        // the resets released (below, after the clocks are set).
        Error err = put(R::Reset, 0x1f, "reset");
        if (err.isError()) return err;
        forget();
        _shadow[R::Reset] = 0x1f;
        _written[0] = 1;
        Task::sleep(5);
        struct Write {
                        uint8_t reg, value;
        };
        const Write setup[] = {
                {R::Gpio, 0x08},        // Espressif: "enhance I2C noise immunity" (bit 3, not in the guide)
                {R::ClkManager1, 0x30}, // MCLK and BCLK gates on, while the dividers are set
                {R::ClkManager2, plan.clkManager2},
                {R::ClkManager3, plan.clkManager3},
                {R::AdcScale, 0x24}, // Espressif's (bits 2:0 ADC_SCALE 24 dB, the default, §10.6)
                {R::ClkManager4, plan.clkManager4},
                {R::ClkManager5, plan.clkManager5},
                {R::System0b, 0x00},    // Espressif
                {R::System0c, 0x00},    // Espressif
                {R::System10, 0x1f},    // Espressif
                {R::System11, 0x7f},    // Espressif
                {R::Reset, 0x80},       // CSM on, slave, resets released (§9.1, §8.2)
                {R::ClkManager1, 0x3f}, // MCLK from the MCLK pin, every clock on (§8.3)
                {R::SdpIn, static_cast<uint8_t>((_values[DacSlot] != 0 ? 0x80 : 0x00) | SdpWord32)},
                {R::SdpOut, static_cast<uint8_t>(0x40 | SdpWord32)}, // ADC out muted: playback only
                {R::Output, 0x10},                                   // Espressif: the headphone drive on
                {R::AdcHpf1, 0x0a},                                  // Espressif
                {R::AdcEq, 0x6a},    // Espressif: ADC EQ bypassed, DC offset held (§10.3)
                {R::Power, 0x01},    // analog on, VMID normal charge (§9.2)
                {R::PowerDac, 0x00}, // DAC on (bit 1, §11)
                {R::Gp45, 0x00},     // Espressif
        };
        // The noise-immunity write twice: Espressif's driver found the first
        // write after power-up can fail.
        (void)_device->writeRegister(R::Gpio, 0x08, TimeoutMs);
        for (const Write &w : setup) {
                err = put(w.reg, w.value, "start");
                if (err.isError()) return err;
        }
        for (size_t i = 0; i < ControlCount; ++i) {
                if (i == DacSlot) continue; // in SdpIn above
                err = apply(i);
                if (err.isError()) return err;
        }
        _running = true;
        return Error::Ok;
}

Error Es8311::stop() {
        if (_device == nullptr) return Error::Ok;
        _running = false;
        // Mute, then the DAC off, the digital side reset with CSM off and the
        // clocks gated (§9.1). The analog side and VMID stay up: powering them
        // down (0x0D = 0xFC, §9.2) drops both outputs by half the supply, which
        // the jack's coupling capacitors pass on as a loud click (heard on the
        // board, 2026-10-08). The steps here are silent.
        const uint8_t mute = static_cast<uint8_t>(_shadow[R::DacMute] | 0x60);
        Error         err = put(R::DacMute, mute, "stop");
        if (err.isOk()) err = put(R::DacVolume, 0x00, "stop");
        if (err.isOk()) err = put(R::PowerDac, 0x02, "stop");
        if (err.isOk()) err = put(R::Reset, 0x1f, "stop");
        if (err.isOk()) err = put(R::ClkManager1, 0x00, "stop");
        return err;
}

Span<const AudioControl> Es8311::controls() const {
        return Span<const AudioControl>(Controls);
}

int32_t Es8311::controlValue(size_t index) const {
        return index < ControlCount ? _values[index] : 0;
}

Error Es8311::setControl(size_t index, int32_t value) {
        if (index >= ControlCount) return Error::Invalid;
        _values[index] = value;
        return _running ? apply(index) : Error::Ok;
}

Error Es8311::apply(size_t index) {
        const int32_t v = _values[index];
        switch (index) {
                case DacSlot:
                        return put(R::SdpIn, static_cast<uint8_t>((_shadow[R::SdpIn] & 0x7f) | (v != 0 ? 0x80 : 0x00)),
                                   "dac-slot");
                case DacVolume: return put(R::DacVolume, R::volumeRegister(v), "dac-volume");
                case DacMute:
                        return put(R::DacMute,
                                   static_cast<uint8_t>((_shadow[R::DacMute] & 0x9f) | (v != 0 ? 0x60 : 0x00)),
                                   "dac-mute");
                case DacRamp:
                case DacEq: {
                        // 0x37: DAC_RAMPRATE (7:4), DAC_EQBYPASS (3), set together.
                        const uint8_t ramp = static_cast<uint8_t>(_values[DacRamp] << 4);
                        const uint8_t bypass = _values[DacEq] != 0 ? 0x00 : 0x08;
                        return put(R::DacRamp, static_cast<uint8_t>(ramp | bypass), Controls[index].name);
                }
                case Drc:
                        return put(R::DacDrc,
                                   static_cast<uint8_t>((_shadow[R::DacDrc] & 0x7f) | (v != 0 ? 0x80 : 0x00)), "drc");
                default: return Error::Invalid;
        }
}

Span<const uint8_t> Es8311::registerList() const {
        return Span<const uint8_t>(RegisterList.data(), RegisterList.size());
}

Result<uint8_t> Es8311::readRegister(uint8_t reg) {
        if (_device == nullptr) return makeError<uint8_t>(Error::InvalidState);
        return _device->readRegister(reg, TimeoutMs);
}

Error Es8311::writeRegister(uint8_t reg, uint8_t value) {
        return put(reg, value, "raw write");
}

int16_t Es8311::expectedRegister(uint8_t reg) const {
        if (reg > R::LastRegister || (_written[reg / 8] & (1u << (reg % 8))) == 0) return -1;
        return _shadow[reg];
}

BMDS_NAMESPACE_END
