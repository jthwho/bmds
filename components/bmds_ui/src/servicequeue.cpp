/**
 * @file      servicequeue.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A window manager service's messages not yet taken (devplan/filebrowser.md §4h).
 */

#include <cstring>
#include <bmds/servicequeue.h>

BMDS_NAMESPACE_BEGIN

Error ServiceQueue::push(ByteSpan message) {
        if (message.isEmpty() || message.size() > MaxMessage) return Error::Invalid;
        const size_t need = _bytes.size() + 2 + message.size();
        if (need > MaxPending) return Error::Full;
        if (_bytes.reserve(need).isError()) return Error::NoMem;
        (void)_bytes.append(uint8_t(message.size() & 0xff));
        (void)_bytes.append(uint8_t(message.size() >> 8));
        for (const uint8_t b : message) (void)_bytes.append(b);
        return Error::Ok;
}

Result<size_t> ServiceQueue::take(WritableByteSpan out) {
        if (_bytes.size() < 2) return makeResult(size_t(0));
        const size_t n = size_t(_bytes[0]) | size_t(_bytes[1]) << 8;
        if (n > out.size()) return makeError<size_t>(Error::BufferTooSmall);
        std::memcpy(out.data(), _bytes.data() + 2, n);
        // The rest moved to the front: a few kilobytes at most.
        const size_t rest = _bytes.size() - 2 - n;
        std::memmove(_bytes.data(), _bytes.data() + 2 + n, rest);
        _bytes.truncate(rest);
        return makeResult(n);
}

BMDS_NAMESPACE_END
