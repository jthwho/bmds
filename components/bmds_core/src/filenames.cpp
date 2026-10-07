/**
 * @file      filenames.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Copies' names, paths in URIs, trash info files (devplan/files.md).
 */

#include <cstdio>
#include <cstring>
#include <bmds/filenames.h>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr bool isUnreserved(char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                       ch == '.' || ch == '_' || ch == '~';
        }

        constexpr int hexValue(char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
        }

        // Appends to a buffer, remembering whether it overflowed (room kept for the NUL).
        struct Writer {
                        Span<char> out;
                        size_t     length = 0;
                        bool       full = false;

                        void put(StringView s) {
                                for (const char ch : s) put(ch);
                                return;
                        }

                        void put(char ch) {
                                if (length + 1 >= out.size()) {
                                        full = true;
                                        return;
                                }
                                out[length++] = ch;
                                return;
                        }

                        Result<StringView> done() {
                                if (full || out.size() == 0) return makeError<StringView>(Error::BufferTooSmall);
                                out[length] = '\0';
                                return makeResult(StringView(out.data(), length));
                        }
        };

} // namespace

Result<StringView> CopyName::make(StringView name, unsigned n, Span<char> out) {
        if (n == 0 || n > MaxCopies || name.isEmpty() || name.contains('/'))
                return makeError<StringView>(Error::Invalid);
        const size_t dot = name.lastIndexOf('.');
        const size_t cut = dot != StringView::NPos && dot > 0 ? dot : name.size();
        char         copy[16];
        if (n == 1)
                std::snprintf(copy, sizeof(copy), " (copy)");
        else
                std::snprintf(copy, sizeof(copy), " (copy %u)", n);
        Writer w{out};
        w.put(name.left(cut));
        w.put(StringView(copy));
        w.put(name.mid(cut));
        return w.done();
}

Result<size_t> Uri::encodePath(StringView path, Span<char> out) {
        static constexpr char Hex[] = "0123456789ABCDEF";
        Writer                w{out};
        for (const char ch : path) {
                if (isUnreserved(ch) || ch == '/') {
                        w.put(ch);
                } else {
                        w.put('%');
                        w.put(Hex[uint8_t(ch) >> 4]);
                        w.put(Hex[uint8_t(ch) & 15]);
                }
        }
        auto [text, err] = w.done();
        if (err.isError()) return makeError<size_t>(err);
        return makeResult(text.size());
}

Result<size_t> Uri::decode(StringView text, Span<char> out) {
        Writer w{out};
        for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '%' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0) {
                        w.put(char(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2])));
                        i += 2;
                } else {
                        w.put(text[i]);
                }
        }
        auto [decoded, err] = w.done();
        if (err.isError()) return makeError<size_t>(err);
        return makeResult(decoded.size());
}

Result<StringView> TrashInfo::format(StringView path, const DateTime &when, Span<char> out) {
        char encoded[512];
        auto [n, err] = Uri::encodePath(path, Span<char>(encoded, sizeof(encoded)));
        if (err.isError()) return makeError<StringView>(err);
        char date[32]; // room for any field's value; a valid date is DateBytes
        std::snprintf(date, sizeof(date), "%04d-%02u-%02uT%02u:%02u:%02u", int(when.year % 10000), unsigned(when.month),
                      unsigned(when.day), unsigned(when.hour), unsigned(when.minute), unsigned(when.second));
        Writer w{out};
        w.put("[Trash Info]\nPath=");
        w.put(StringView(encoded, n));
        w.put("\nDeletionDate=");
        w.put(StringView(date));
        w.put('\n');
        return w.done();
}

Error TrashInfo::parse(StringView text, Span<char> path, char (&date)[DateBytes + 1]) {
        date[0] = '\0';
        IniReader ini(text);
        bool      group = false, found = false;
        for (IniReader::Item item = ini.next(); item.kind != IniReader::Kind::End; item = ini.next()) {
                if (item.kind == IniReader::Kind::Group) group = group || item.name == "Trash Info";
                if (item.kind != IniReader::Kind::Entry || item.name != "Trash Info") continue;
                if (item.key == "Path") {
                        auto [n, err] = Uri::decode(item.value, path);
                        if (err.isError()) return err;
                        found = n > 0 && path[0] == '/';
                } else if (item.key == "DeletionDate" && item.value.size() == DateBytes) {
                        std::memcpy(date, item.value.data(), DateBytes);
                        date[DateBytes] = '\0';
                }
        }
        return group && found ? Error::Ok : Error::CorruptData;
}

BMDS_NAMESPACE_END
