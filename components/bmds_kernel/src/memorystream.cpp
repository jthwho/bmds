/**
 * @file      memorystream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <utility>
#include <bmds/memorystream.h>
#include <bmds/random.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit NullStream nullStream;

        constexpr size_t MaxRandomRead = 65536;

} // namespace

Result<size_t> MemoryStream::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs;
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        const size_t left = _data.size() - _readPos;
        if (left == 0) return makeError<size_t>(Error::EndOfFile);
        const size_t n = buf.size() < left ? buf.size() : left;
        std::memcpy(buf.data(), _data.data() + _readPos, n);
        _readPos += n;
        if (_readPos == _data.size()) {
                // Drained: reuse the buffer from the start.
                _data.truncate(0);
                _readPos = 0;
        }
        return makeResult(n);
}

Result<size_t> MemoryStream::write(ByteSpan data, unsigned int timeoutMs) {
        (void)timeoutMs;
        Error err = _data.append(StringView(reinterpret_cast<const char *>(data.data()), data.size()));
        if (err.isError()) return Result<size_t>(0, err);
        return makeResult(data.size());
}

String MemoryStream::takeContents() {
        if (_readPos != 0) {
                _data.remove(0, _readPos);
                _readPos = 0;
        }
        String taken = std::move(_data);
        _data = String(taken.space());
        return taken;
}

void MemoryStream::clear() {
        _data.clear();
        _readPos = 0;
        return;
}

NullStream &NullStream::instance() {
        return nullStream;
}

Result<size_t> ZeroStream::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs;
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        std::memset(buf.data(), 0, buf.size());
        return makeResult(buf.size());
}

Result<size_t> RandomStream::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs;
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        // The kernel's pool (devplan/random.md). At most 64 KB a read, so no
        // read holds the CPU long.
        const size_t n = buf.size() < MaxRandomRead ? buf.size() : MaxRandomRead;
        Random::fill(buf.data(), n);
        return makeResult(n);
}

BMDS_NAMESPACE_END
