/**
 * @file      inireader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <initializer_list>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

IniReader::Item IniReader::next() {
        while (!_rest.isEmpty()) {
                const size_t end = _rest.indexOf('\n');
                StringView   line = end == StringView::NPos ? _rest : _rest.left(end);
                _rest = end == StringView::NPos ? StringView() : _rest.mid(end + 1);
                ++_line;
                line = line.trimmed(); // the CR of a CR LF among the spaces
                if (line.isEmpty() || line.front() == '#' || line.front() == ';') continue;
                Item item;
                item.text = line;
                item.line = _line;
                if (line.front() == '[') {
                        if (line.back() == ']' && line.size() > 2) {
                                item.kind = Kind::Group;
                                item.name = line.mid(1, line.size() - 2).trimmed();
                                _group = item.name;
                        } else {
                                item.kind = Kind::Invalid;
                        }
                        return item;
                }
                const size_t equals = line.indexOf('=');
                if (equals == StringView::NPos || equals == 0) {
                        item.kind = Kind::Invalid;
                        return item;
                }
                item.kind = Kind::Entry;
                item.name = _group;
                item.key = line.left(equals).trimmed();
                item.value = line.mid(equals + 1).trimmed();
                return item;
        }
        return Item();
}

StringView IniReader::nextListItem(StringView &rest) {
        while (!rest.isEmpty()) {
                size_t i = 0;
                while (i < rest.size() && rest[i] != ';') i += rest[i] == '\\' && i + 1 < rest.size() ? 2 : 1;
                const StringView item = rest.left(i).trimmed();
                rest = rest.mid(i + 1);
                if (!item.isEmpty()) return item;
        }
        return StringView();
}

Error IniReader::unescape(StringView value, char *out, size_t outBytes) {
        if (outBytes == 0) return Error::BufferTooSmall;
        size_t n = 0;
        for (size_t i = 0; i < value.size(); ++i) {
                char ch = value[i];
                if (ch == '\\' && i + 1 < value.size()) {
                        switch (value[++i]) {
                                case 's': ch = ' '; break;
                                case 'n': ch = '\n'; break;
                                case 't': ch = '\t'; break;
                                case 'r': ch = '\r'; break;
                                default: ch = value[i]; break; // \\, \; and anything else: itself
                        }
                }
                if (n + 1 >= outBytes) {
                        out[0] = '\0';
                        return Error::BufferTooSmall;
                }
                out[n++] = ch;
        }
        out[n] = '\0';
        return Error();
}

Error IniReader::toBool(StringView value, bool &out) {
        for (const StringView yes : {StringView("true"), StringView("1"), StringView("yes")}) {
                if (value.equalsIgnoreCase(yes)) {
                        out = true;
                        return Error();
                }
        }
        for (const StringView no : {StringView("false"), StringView("0"), StringView("no")}) {
                if (value.equalsIgnoreCase(no)) {
                        out = false;
                        return Error();
                }
        }
        return Error::Invalid;
}

BMDS_NAMESPACE_END
