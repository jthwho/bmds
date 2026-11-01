/**
 * @file      pkgmanifest.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/pkgtext.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgversion.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The next tab-separated column of @p line from @p pos; false after the last.
        bool column(StringView line, size_t &pos, StringView &out) {
                if (pos > line.size()) return false;
                const size_t tab = line.indexOf('\t', pos);
                const size_t end = tab == StringView::NPos ? line.size() : tab;
                out = line.mid(pos, end - pos);
                pos = end + 1;
                return true;
        }

        bool isFieldKey(StringView key) {
                if (key.size() < 2 || key.size() > 32) return false;
                for (char c : key) {
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
                }
                return true;
        }

        bool isSpace(char c) {
                return c == ' ';
        }

} // namespace

bool PkgManifest::isCleanText(StringView s) {
        size_t i = 0;
        while (i < s.size()) {
                const size_t   at = i;
                const uint32_t c = Utf8::decode(s, i);
                if (c == Utf8::Replacement) {
                        // Malformed, unless it was U+FFFD itself, spelled out.
                        if (i - at != 3 || uint8_t(s[at]) != 0xef || uint8_t(s[at + 1]) != 0xbf ||
                            uint8_t(s[at + 2]) != 0xbd) {
                                return false;
                        }
                }
                if (c < 0x20 || (c >= 0x7f && c < 0xa0)) return false;
        }
        return true;
}

bool PkgManifest::isValidPath(StringView path) {
        if (path.isEmpty() || path.size() > MaxPathBytes || !isCleanText(path)) return false;
        size_t start = 0;
        while (start <= path.size()) {
                size_t end = path.indexOf('/', start);
                if (end == StringView::NPos) end = path.size();
                const StringView name = path.mid(start, end - start);
                if (name.isEmpty() || name.size() > MaxNameBytes || name == ".." || name == ".") return false;
                for (char c : name) {
                        switch (c) {
                                case '"':
                                case '*':
                                case ':':
                                case '<':
                                case '>':
                                case '?':
                                case '\\':
                                case '|': return false;
                                default: break;
                        }
                }
                start = end + 1;
        }
        return true;
}

Error PkgManifest::Reader::next(Entry &out) {
        if (_pos >= _text.size()) return Error::EndOfFile;
        size_t end = _text.indexOf('\n', _pos);
        if (end == StringView::NPos) end = _text.size();
        const StringView line = _text.mid(_pos, end - _pos);
        _pos = end + 1;
        ++_line;
        out = Entry{};

        size_t     at = 0;
        StringView kind;
        if (!column(line, at, kind) || kind.isEmpty()) return Error::CorruptData;
        StringView a, b, c, d, extra;
        const bool hasA = column(line, at, a);
        const bool hasB = hasA && column(line, at, b);
        const bool hasC = hasB && column(line, at, c);
        const bool hasD = hasC && column(line, at, d);
        const bool hasExtra = hasD && column(line, at, extra);

        if (kind.size() > 1) {
                if (!isFieldKey(kind) || !hasA || hasB || !isCleanText(a)) return Error::CorruptData;
                out.kind = Kind::Field;
                out.key = kind;
                out.value = a;
                return Error::Ok;
        }
        switch (kind[0]) {
                case 'f':
                        if (!hasD || hasExtra) return Error::CorruptData;
                        out.kind = Kind::File;
                        out.path = a;
                        if (!isValidPath(a) || !Detail::PkgText::parseOctal(b, 07777, out.mode) ||
                            !Detail::PkgText::parseDecimal(c, out.size) || !Detail::PkgText::parseHash(d, out.hash)) {
                                return Error::CorruptData;
                        }
                        return Error::Ok;
                case 'd':
                        if (!hasA || hasB) return Error::CorruptData;
                        out.kind = Kind::Directory;
                        out.path = a;
                        return isValidPath(a) ? Error::Ok : Error::CorruptData;
                case 'l':
                        if (!hasB || hasC) return Error::CorruptData;
                        out.kind = Kind::Link;
                        out.path = a;
                        out.target = b;
                        if (!isValidPath(a) || b.isEmpty() || b.size() > MaxPathBytes || !isCleanText(b)) {
                                return Error::CorruptData;
                        }
                        return Error::Ok;
                default:
                        if (kind[0] >= 'a' && kind[0] <= 'z') return Error::NotSupported;
                        return Error::CorruptData;
        }
}

Error PkgDepends::Reader::next(Dependency &out) {
        if (_done) return Error::EndOfFile;
        const auto skipSpaces = [this]() {
                while (_pos < _list.size() && isSpace(_list[_pos])) ++_pos;
        };
        skipSpaces();
        if (_pos >= _list.size()) {
                // Only an empty list gets here: a comma at the end is refused where it is read.
                _done = true;
                return Error::EndOfFile;
        }
        out = Dependency{};
        const size_t nameStart = _pos;
        while (_pos < _list.size() && !isSpace(_list[_pos]) && _list[_pos] != ',' && _list[_pos] != '(') ++_pos;
        out.name = _list.mid(nameStart, _pos - nameStart);
        if (!PkgFile::isValidName(out.name)) return Error::CorruptData;
        skipSpaces();
        if (_pos < _list.size() && _list[_pos] == '(') {
                ++_pos;
                skipSpaces();
                if (_list.mid(_pos, 2) != StringView(">=")) return Error::CorruptData;
                _pos += 2;
                skipSpaces();
                const size_t versionStart = _pos;
                while (_pos < _list.size() && !isSpace(_list[_pos]) && _list[_pos] != ')') ++_pos;
                out.minVersion = _list.mid(versionStart, _pos - versionStart);
                if (!PkgVersion::isValid(out.minVersion)) return Error::CorruptData;
                skipSpaces();
                if (_pos >= _list.size() || _list[_pos] != ')') return Error::CorruptData;
                ++_pos;
                skipSpaces();
        }
        if (_pos >= _list.size()) {
                _done = true;
        } else if (_list[_pos] == ',') {
                ++_pos;
                skipSpaces();
                if (_pos >= _list.size()) return Error::CorruptData;
        } else {
                return Error::CorruptData;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
