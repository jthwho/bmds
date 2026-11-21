/**
 * @file      i2cbus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <new>
#include <driver/i2c_master.h>
#include <bmds/board.h>
#include <bmds/i2cbus.h>
#include <bmds/i2cclient.h>
#include <bmds/io.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<I2cBus> buses[BoardI2cCount];
        constinit char                 fileNames[BoardI2cCount][8]; // `i2c-0`: /dev's names, kept

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
                // Its pads and controller first (devplan/io.md §3.2): a board whose buses overlap
                // another user's pins is a mistake to see at boot, not a bus that half works.
                char owner[8];
                std::snprintf(owner, sizeof(owner), "i2c%u", static_cast<unsigned>(i));
                const IoUse uses[] = {
                        {Io::pad(b.sda), "SDA"},
                        {Io::pad(b.scl), "SCL"},
                        {Io::instance(IoKind::I2c, b.port),
                         i == static_cast<size_t>(BoardI2c::Main) ? "on-board devices" : "expansion bus"},
                };
                IoResource blocked = NoIoResource;
                auto [claim, claimErr] = Io::claim(owner, uses, 0, &blocked);
                if (claimErr.isError()) {
                        char why[96];
                        (void)Io::busyText(blocked, why, sizeof(why));
                        bmdsErr("i2c%u: can't claim its pins: %s %s", static_cast<unsigned>(i), claimErr.desc(), why);
                        return claimErr;
                }
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
                buses[i].create(i, handle, b.clockHz, std::move(claim));
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


size_t I2cBus::count() {
        size_t n = 0;
        while (n < BoardI2cCount && buses[n].isCreated()) ++n;
        return n;
}

bool I2cBus::isOwned(uint8_t address) const {
        Mutex::Locker lock(_lock, WaitForever);
        for (size_t i = 0; i < _deviceCount; ++i)
                if (_devices[i]._address == address) return true;
        return false;
}

Error I2cBus::transfer(Span<const I2cMessage> messages, unsigned int timeoutMs) {
        if (messages.isEmpty()) return Error::Invalid;
        // The commands: per message a start, its address byte, then its bytes (a read's last one
        // not acknowledged); one stop. As many as the controller holds, or refused.
        i2c_operation_job_t ops[MaxCommands] = {};
        uint8_t             addressBytes[MaxCommands] = {};
        size_t              n = 0;
        const auto          add = [&](const i2c_operation_job_t &op) {
                if (n == MaxCommands) return false;
                ops[n++] = op;
                return true;
        };
        for (size_t m = 0; m < messages.size(); ++m) {
                const I2cMessage &msg = messages[m];
                if (msg.address > 0x7f || (msg.length > 0 && msg.data == nullptr)) return Error::Invalid;
                i2c_operation_job_t start = {};
                start.command = I2C_MASTER_CMD_START;
                addressBytes[m < MaxCommands ? m : 0] = uint8_t(msg.address << 1 | (msg.read ? 1 : 0));
                i2c_operation_job_t addr = {};
                addr.command = I2C_MASTER_CMD_WRITE;
                addr.write.ack_check = true;
                addr.write.data = &addressBytes[m < MaxCommands ? m : 0];
                addr.write.total_bytes = 1;
                if (!add(start) || !add(addr)) return Error::Invalid;
                if (msg.length == 0) continue;
                if (!msg.read) {
                        i2c_operation_job_t w = {};
                        w.command = I2C_MASTER_CMD_WRITE;
                        w.write.ack_check = true;
                        w.write.data = msg.data;
                        w.write.total_bytes = msg.length;
                        if (!add(w)) return Error::Invalid;
                        continue;
                }
                if (msg.length > 1) {
                        i2c_operation_job_t r = {};
                        r.command = I2C_MASTER_CMD_READ;
                        r.read.ack_value = I2C_ACK_VAL;
                        r.read.data = msg.data;
                        r.read.total_bytes = msg.length - 1;
                        if (!add(r)) return Error::Invalid;
                }
                i2c_operation_job_t last = {};
                last.command = I2C_MASTER_CMD_READ;
                last.read.ack_value = I2C_NACK_VAL; // the last byte: the device lets go of the bus
                last.read.data = msg.data + msg.length - 1;
                last.read.total_bytes = 1;
                if (!add(last)) return Error::Invalid;
        }
        i2c_operation_job_t stop = {};
        stop.command = I2C_MASTER_CMD_STOP;
        if (!add(stop)) return Error::Invalid;
        void *raw = nullptr;
        {
                // The device the raw transfers go through, the bus's clock, and no address of its own:
                // the driver otherwise writes a device's address after every start, before ours.
                Mutex::Locker lock(_lock, WaitForever);
                if (_raw == nullptr) {
                        i2c_device_config_t config = {};
                        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
                        config.device_address = I2C_DEVICE_ADDRESS_NOT_USED;
                        config.scl_speed_hz = _clockHz;
                        i2c_master_dev_handle_t handle = nullptr;
                        const esp_err_t err = i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(_handle),
                                                                        &config, &handle);
                        if (err != ESP_OK) return Error::fromEspErr(err);
                        _raw = handle;
                }
                raw = _raw;
        }
        const esp_err_t err = i2c_master_execute_defined_operations(dev(raw), ops, n, idfTimeout(timeoutMs));
        if (err == ESP_ERR_INVALID_RESPONSE) return Error::NotFound; // not acknowledged
        return transferError(err);
}

Error I2cBus::addFiles() {
        for (size_t i = 0; i < count(); ++i) {
                std::snprintf(fileNames[i], sizeof(fileNames[i]), "i2c-%u", static_cast<unsigned>(i));
                Vfs::DeviceFile file;
                file.name = fileNames[i];
                file.arg = &*buses[i];
                file.isDevice = true;
                // A client per open: its own target (Linux's i2c-dev), gone with its handle.
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        const uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                             ((flags & Vfs::Write) ? FileHandle::Write : 0);
                        if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(I2cClient));
                        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                        auto *client = ::new (mem) I2cClient(*static_cast<I2cBus *>(arg));
                        auto [handle, err] = FileHandle::open(*client, mode);
                        if (err.isError()) {
                                client->~I2cClient();
                                Memory::free(mem);
                        }
                        return Result<FileHandle::Ptr>(std::move(handle), err);
                };
                if (const Error err = Vfs::addDeviceFile(file); err.isError()) return err;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
