/**
 * @file      logstream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/log.h>
#include <bmds/logstream.h>

BMDS_NAMESPACE_BEGIN

Result<size_t> LogStream::read(WritableByteSpan, unsigned int) {
        return makeError<size_t>(Error::EndOfFile);
}

Result<size_t> LogStream::write(ByteSpan data, unsigned int) {
        Mutex::Locker lock(_lock, WaitForever);
        for (uint8_t c : data) {
                if (c == '\r') continue;
                if (c == '\n') {
                        emit();
                        continue;
                }
                _line[_used++] = static_cast<char>(c);
                if (_used == LineBytes) emit();
        }
        return makeResult(data.size());
}

void LogStream::emit() {
        if (_used > 0) bmdsInfo("%s: %.*s", _prefix, static_cast<int>(_used), _line);
        _used = 0;
        return;
}

BMDS_NAMESPACE_END
