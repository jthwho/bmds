/**
 * @file      i2cclient.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `/dev/i2c-N` (devplan/io.md §11.3): a client per open file over the system's buses.
 */

#include <bmds/i2cclient.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

Error I2cClient::setTarget(uint8_t address, bool force) {
        if (address > 0x7f) return Error::Invalid;
        if (force) _force = true;
        if (!mayReach(address)) return Error::Busy;
        _target = address;
        return Error::Ok;
}

Result<size_t> I2cClient::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs;
        if (_target == NoTarget) return makeError<size_t>(Error::Invalid);
        const size_t n = buf.size() < MaxBytes ? buf.size() : MaxBytes;
        I2cMessage   m;
        m.address = uint16_t(_target);
        m.read = true;
        m.data = buf.data();
        m.length = n;
        const Error err = _bus.transfer(Span<const I2cMessage>(&m, 1), TimeoutMs);
        if (err.isError()) return makeError<size_t>(err);
        return makeResult(n);
}

Result<size_t> I2cClient::write(ByteSpan data, unsigned int timeoutMs) {
        (void)timeoutMs;
        if (_target == NoTarget) return makeError<size_t>(Error::Invalid);
        const size_t n = data.size() < MaxBytes ? data.size() : MaxBytes;
        I2cMessage   m;
        m.address = uint16_t(_target);
        m.data = const_cast<uint8_t *>(data.data()); // a write only reads it
        m.length = n;
        const Error err = _bus.transfer(Span<const I2cMessage>(&m, 1), TimeoutMs);
        if (err.isError()) return makeError<size_t>(err);
        return makeResult(n);
}

void I2cClient::handleClosed(uint8_t mode) {
        (void)mode;
        // Opened once (its device file makes one per open): its last handle went.
        this->~I2cClient();
        Memory::free(this);
        return;
}

BMDS_NAMESPACE_END
