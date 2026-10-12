/**
 * @file      syslog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the system log's text forms (bmds/logtext.h,
 * devplan/logging.md): the input is a message as any program may write it to
 * /dev/log, taken apart and cleaned line by line as the kernel does, and also
 * read as a log line with an uptime stamp. Nothing may fault, read out of
 * bounds or overflow, and every view must stay inside the input.
 */

#include <cstdint>
#include <cstdlib>
#include <bmds/logtext.h>

using bmds::StringView;
namespace LogText = bmds::LogText;

namespace {

        void inside(StringView part, StringView whole) {
                if (part.isEmpty()) return;
                if (part.data() < whole.data() || part.data() + part.size() > whole.data() + whole.size()) std::abort();
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const StringView      message(reinterpret_cast<const char *>(data), size);
        const LogText::Syslog m = LogText::parseSyslog(message);
        if (m.severity > 7 || m.tag.size() > LogText::MaxTag) std::abort();
        inside(m.tag, message);
        inside(m.text, message);
        char       line[160];
        StringView rest = m.text;
        while (!rest.isEmpty()) {
                const StringView l = LogText::nextLine(rest);
                inside(l, message);
                if (LogText::cleanLine(l, line, sizeof(line)) > sizeof(line)) std::abort();
        }
        uint64_t ms = 0;
        size_t   stamp = 0;
        if (LogText::parseStamp(message, ms, stamp) && stamp > size) std::abort();
        return 0;
}
