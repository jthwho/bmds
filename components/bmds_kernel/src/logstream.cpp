/**
 * @file      logstream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/log.h>
#include <bmds/logstream.h>
#include <bmds/logtext.h>
#include <bmds/staticobject.h>

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
        if (_used > 0) Log::writeText(LogLevel::Info, StringView(_prefix), StringView(_line, _used));
        _used = 0;
        return;
}

namespace {

        constinit StaticObject<SyslogStream> syslogStream;

} // namespace

SyslogStream &SyslogStream::instance() {
        if (!syslogStream.isCreated()) syslogStream.create();
        return *syslogStream;
}

Result<size_t> SyslogStream::read(WritableByteSpan, unsigned int) {
        return makeError<size_t>(Error::EndOfFile);
}

Result<size_t> SyslogStream::write(ByteSpan data, unsigned int) {
        const LogText::Syslog m =
                LogText::parseSyslog(StringView(reinterpret_cast<const char *>(data.data()), data.size()));
        if (!m.text.isEmpty()) Log::writeText(Log::levelOfSeverity(m.severity), m.tag, m.text);
        return makeResult(data.size());
}

BMDS_NAMESPACE_END
