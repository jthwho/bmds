/**
 * @file      i2cbus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <driver/i2c_master.h>
#include <bmds/board.h>
#include <bmds/i2cbus.h>
#include <bmds/log.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<I2cBus> buses[BoardI2cCount];

        // IDF's timeout: -1 waits for ever.
        int idfTimeout(unsigned int timeoutMs) {
                return timeoutMs == WaitForever ? -1 : static_cast<int>(timeoutMs);
        }

        Error transferError(esp_err_t err) {
                switch (err) {
                        case ESP_OK: return Error::Ok;
                        case ESP_ERR_TIMEOUT: return Error::Timeout;
                        case ESP_ERR_INVALID_RESPONSE: // no acknowledge
                        case ESP_FAIL: return Error::IoError;
                        default: return Error::fromEspErr(err);
                }
        }

        i2c_master_dev_handle_t dev(void *handle) {
                return static_cast<i2c_master_dev_handle_t>(handle);
        }

} // namespace

Error I2cDevice::write(ByteSpan data, unsigned int timeoutMs) {
        return transferError(i2c_master_transmit(dev(_handle), data.data(), data.size(), idfTimeout(timeoutMs)));
}

Error I2cDevice::writeRead(ByteSpan out, WritableByteSpan in, unsigned int timeoutMs) {
        return transferError(i2c_master_transmit_receive(dev(_handle), out.data(), out.size(), in.data(), in.size(),
                                                         idfTimeout(timeoutMs)));
}

Error I2cDevice::writeRegister(uint8_t reg, uint8_t value, unsigned int timeoutMs) {
        const uint8_t bytes[2] = {reg, value};
        return write(ByteSpan(bytes, sizeof(bytes)), timeoutMs);
}

Result<uint8_t> I2cDevice::readRegister(uint8_t reg, unsigned int timeoutMs) {
        uint8_t     value = 0;
        const Error err = writeRead(ByteSpan(&reg, 1), WritableByteSpan(&value, 1), timeoutMs);
        if (err.isError()) return makeError<uint8_t>(err);
        return makeResult(value);
}

Error I2cBus::init() {
        for (size_t i = 0; i < BoardI2cCount; ++i) {
                const BoardI2cBus &b = board().i2c[i];
                if (!b.isValid()) continue;
                i2c_master_bus_config_t config = {};
                config.i2c_port = static_cast<i2c_port_num_t>(b.port);
                config.sda_io_num = static_cast<gpio_num_t>(b.sda);
                config.scl_io_num = static_cast<gpio_num_t>(b.scl);
                config.clk_source = I2C_CLK_SRC_DEFAULT;
                config.glitch_ignore_cnt = 7;
                config.flags.enable_internal_pullup = 0; // the board's pull-ups
                i2c_master_bus_handle_t handle = nullptr;
                const esp_err_t         err = i2c_new_master_bus(&config, &handle);
                if (err != ESP_OK) {
                        bmdsErr("i2c%u: %s", static_cast<unsigned>(i), esp_err_to_name(err));
                        return Error::fromEspErr(err);
                }
                buses[i].create(i, handle, b.clockHz);
        }
        return Error::Ok;
}

I2cBus *I2cBus::bus(size_t number) {
        if (number >= BoardI2cCount || !buses[number].isCreated()) return nullptr;
        return &*buses[number];
}

Result<I2cDevice *> I2cBus::device(uint8_t address, uint32_t speedHz, unsigned int timeoutMs) {
        if (speedHz == 0) speedHz = _clockHz;
        Mutex::Locker lock(_lock, timeoutMs);
        if (!lock.isLocked()) return makeError<I2cDevice *>(Error::Timeout);
        for (size_t i = 0; i < _deviceCount; ++i) {
                if (_devices[i]._address == address && _devices[i]._speedHz == speedHz) return makeResult(&_devices[i]);
        }
        if (_deviceCount == MaxDevices) return makeError<I2cDevice *>(Error::Full);
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = speedHz;
        i2c_master_dev_handle_t handle = nullptr;
        const esp_err_t         err =
                i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(_handle), &config, &handle);
        if (err != ESP_OK) return makeError<I2cDevice *>(Error::fromEspErr(err));
        I2cDevice &d = _devices[_deviceCount++];
        d._handle = handle;
        d._address = address;
        d._speedHz = speedHz;
        return makeResult(&d);
}

Error I2cBus::probe(uint8_t address, unsigned int timeoutMs) {
        const esp_err_t err =
                i2c_master_probe(static_cast<i2c_master_bus_handle_t>(_handle), address, idfTimeout(timeoutMs));
        if (err == ESP_ERR_NOT_FOUND) return Error::NotFound;
        return transferError(err);
}

BMDS_NAMESPACE_END
