/**
 * @file      filenamelist.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The names in a file dialog's name field (devplan/mediaplayer.md §6d).
 */

#include <bmds/filenamelist.h>

BMDS_NAMESPACE_BEGIN

namespace FileNameList {

        Error split(StringView text, List<StringView> &out) {
                out.clear();
                const StringView t = text.trimmed();
                if (t.isEmpty()) return Error::Ok;
                if (!t.startsWith('"')) return out.append(t);
                size_t at = 0;
                while (at < t.size()) {
                        if (t[at] != '"') {
                                ++at; // between names
                                continue;
                        }
                        const size_t start = ++at;
                        while (at < t.size() && t[at] != '"') ++at;
                        const StringView name = t.mid(start, at - start).trimmed();
                        if (at < t.size()) ++at;
                        if (name.isEmpty()) continue;
                        const Error err = out.append(name);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        Error appendQuoted(StringView name, String &out) {
                Error err = out.isEmpty() ? Error::Ok : out.append(' ');
                if (err.isOk()) err = out.append('"');
                if (err.isOk()) err = out.append(name);
                if (err.isOk()) err = out.append('"');
                return err;
        }

} // namespace FileNameList

BMDS_NAMESPACE_END
