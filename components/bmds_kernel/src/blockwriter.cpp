/**
 * @file      blockwriter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/blockwriter.h>

#include <cstring>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

BlockWriter::~BlockWriter() {
        Memory::free(_buffer);
}

void BlockWriter::reset(uint64_t position) {
        _used = 0;
        _start = position;
        return;
}

Error BlockWriter::writeAll(Stream &to, ByteSpan data) {
        while (!data.isEmpty()) {
                auto [n, err] = to.write(data, WaitForever);
                if (err.isError()) return err;
                if (n == 0) return Error::IoError;
                data = data.mid(n);
        }
        return Error::Ok;
}

Error BlockWriter::write(Stream &to, ByteSpan data) {
        if (_buffer == nullptr && !_noMemory) {
                _buffer = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, BlockBytes));
                _noMemory = _buffer == nullptr;
        }
        if (_buffer == nullptr) {
                _start += data.size();
                return writeAll(to, data);
        }
        while (!data.isEmpty()) {
                // This block ends at the next multiple of BlockBytes in the file.
                const size_t limit = BlockBytes - static_cast<size_t>(_start % BlockBytes);
                const size_t n = data.size() < limit - _used ? data.size() : limit - _used;
                std::memcpy(_buffer + _used, data.data(), n);
                _used += n;
                data = data.mid(n);
                if (_used == limit) {
                        const Error err = flush(to);
                        if (err.isError()) return err;
                }
        }
        return Error::Ok;
}

Error BlockWriter::flush(Stream &to) {
        if (_used == 0) return Error::Ok;
        const Error err = writeAll(to, ByteSpan(_buffer, _used));
        if (err.isOk()) {
                _start += _used;
                _used = 0;
        }
        return err;
}

BMDS_NAMESPACE_END
