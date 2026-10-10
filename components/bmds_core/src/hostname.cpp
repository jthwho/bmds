/**
 * @file      hostname.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's host name (devplan/notifications.md §2).
 */

#include <cstring>
#include <bmds/hostname.h>

BMDS_NAMESPACE_BEGIN

namespace HostName {

        bool isValid(StringView name) {
                if (name.isEmpty()) return true;
                if (name.size() > MaxBytes || name[0] == '-' || name[name.size() - 1] == '-') return false;
                for (const char c : name) {
                        const bool ok =
                                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
                        if (!ok) return false;
                }
                return true;
        }

        size_t resolve(StringView configured, StringView serial, char (&out)[MaxBytes + 1]) {
                size_t     n = 0;
                const auto put = [&](StringView text) {
                        for (size_t i = 0; i < text.size() && n < MaxBytes; ++i) out[n++] = text[i];
                };
                if (!configured.isEmpty()) {
                        put(configured);
                        out[n] = '\0';
                        return n;
                }
                // The serial's letters and digits, lower case: its last few.
                char   tail[DefaultSerialChars];
                size_t kept = 0;
                for (size_t i = serial.size(); i-- > 0 && kept < DefaultSerialChars;) {
                        char c = serial[i];
                        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) tail[DefaultSerialChars - ++kept] = c;
                }
                if (kept == 0) {
                        put(StringView("bmds"));
                } else {
                        put(StringView(DefaultPrefix));
                        put(StringView(tail + DefaultSerialChars - kept, kept));
                }
                out[n] = '\0';
                return n;
        }

} // namespace HostName

BMDS_NAMESPACE_END
