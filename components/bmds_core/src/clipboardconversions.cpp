/**
 * @file      clipboardconversions.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Converting the clipboard's types (devplan/clipboard.md C6).
 */

#include <bmds/clipboardconversions.h>
#include <bmds/clipboardtype.h>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Builtin = ClipboardConversions::Builtin;

        struct BuiltinEntry {
                        const char *from;
                        const char *to;
                        Builtin     builtin;
        };

        constexpr BuiltinEntry Builtins[] = {
                {"text/uri-list", "text/plain;charset=utf-8", Builtin::UriListToText},
                {"text/plain", "text/plain;charset=utf-8", Builtin::Latin1ToText},
                {"text/plain;charset=us-ascii", "text/plain;charset=utf-8", Builtin::Latin1ToText},
                {"text/plain;charset=iso-8859-1", "text/plain;charset=utf-8", Builtin::Latin1ToText},
                {"text/plain;charset=utf-8", "text/plain", Builtin::TextToLatin1},
        };

        // The FNV-1a hash of @p type's normal form, or an error (not a type).
        Result<uint32_t> typeHash(StringView type) {
                char buffer[ClipboardType::BufferBytes];
                auto [normal, err] = ClipboardType::normalize(type, Span<char>(buffer, sizeof(buffer)));
                if (err.isError()) return makeError<uint32_t>(err);
                uint32_t h = 2166136261u;
                for (const char ch : normal) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
                return makeResult(h);
        }

        constexpr char   FilePrefix[] = "file://";
        constexpr size_t FilePrefixBytes = sizeof(FilePrefix) - 1;

        constexpr int hexValue(char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
        }

        // The `=` after `FROM>TO` in @p line, from @p from (TO's start): the first not in TO's
        // parameters (`;name=value`: a value ends at `;`, `=` or a space), or NPos.
        size_t separator(StringView line, size_t from) {
                enum {
                        Type,
                        Name,
                        Value
                } at = Type;
                for (size_t i = from; i < line.size(); ++i) {
                        const char ch = line[i];
                        if (ch == ';') {
                                at = Name;
                        } else if (ch == '=') {
                                if (at != Name) return i;
                                at = Value;
                        } else if ((ch == ' ' || ch == '\t') && at == Value) {
                                at = Type; // `TO = COMMAND`
                        }
                }
                return StringView::NPos;
        }

} // namespace

Error ClipboardConversions::addConverter(StringView from, StringView to, StringView command, Builtin builtin) {
        auto [fh, ferr] = typeHash(from);
        auto [th, terr] = typeHash(to);
        if (ferr.isError() || terr.isError()) return Error::Invalid;
        for (size_t i = 0; i < _count; ++i) {
                const Converter &c = _converters[i];
                if (c.fromHash == fh && c.toHash == th && ClipboardType::equal(c.from, from) &&
                    ClipboardType::equal(c.to, to))
                        return Error::Exists;
        }
        if (_count >= MaxConverters) return Error::Full;
        Converter &c = _converters[_count++];
        c.from = from;
        c.to = to;
        c.command = command;
        c.builtin = builtin;
        c.fromHash = fh;
        c.toHash = th;
        return Error::Ok;
}

Error ClipboardConversions::add(StringView from, StringView to, StringView command) {
        command = command.trimmed();
        if (command.isEmpty()) return Error::Invalid;
        return addConverter(from.trimmed(), to.trimmed(), command, Builtin::None);
}

Error ClipboardConversions::read(StringView text, Report report, void *arg) {
        IniReader ini(text);
        for (IniReader::Item item = ini.next(); item.kind != IniReader::Kind::End; item = ini.next()) {
                if (item.kind != IniReader::Kind::Entry || !item.name.equalsIgnoreCase("Convert")) continue;
                // The line itself: a type's parameters have `=` in them, so the key isn't the pair.
                const size_t arrow = item.text.indexOf('>');
                const size_t equals = arrow == StringView::NPos ? StringView::NPos : separator(item.text, arrow + 1);
                Error        err = Error::Invalid;
                if (equals != StringView::NPos)
                        err = add(item.text.left(arrow), item.text.mid(arrow + 1, equals - arrow - 1),
                                  item.text.mid(equals + 1));
                if (err == Error::Full) return err;
                if (err == Error::Invalid && report != nullptr) report(arg, item.line, item.text);
        }
        return Error::Ok;
}

Error ClipboardConversions::addBuiltins() {
        for (const BuiltinEntry &b : Builtins) {
                const Error err = addConverter(StringView(b.from), StringView(b.to), StringView(), b.builtin);
                if (err == Error::Full) return err;
        }
        return Error::Ok;
}

Result<ClipboardConversions::Path> ClipboardConversions::find(Span<const StringView> offered, StringView wanted) const {
        auto [wh, werr] = typeHash(wanted);
        if (werr.isError()) return makeError<Path>(Error::Invalid);
        const size_t n = offered.size() < MaxOffered ? offered.size() : MaxOffered;
        uint32_t     oh[MaxOffered] = {};
        bool         valid[MaxOffered] = {};
        for (size_t i = 0; i < n; ++i) {
                auto [h, err] = typeHash(offered[i]);
                oh[i] = h;
                valid[i] = err.isOk();
        }
        Path path;
        // As is.
        for (size_t i = 0; i < n; ++i) {
                if (valid[i] && oh[i] == wh && ClipboardType::equal(offered[i], wanted)) {
                        path.offered = static_cast<uint8_t>(i);
                        return makeResult(path);
                }
        }
        // One converter.
        for (size_t i = 0; i < n; ++i) {
                if (!valid[i]) continue;
                for (size_t c = 0; c < _count; ++c) {
                        const Converter &k = _converters[c];
                        if (k.fromHash != oh[i] || k.toHash != wh) continue;
                        if (!ClipboardType::equal(k.from, offered[i]) || !ClipboardType::equal(k.to, wanted)) continue;
                        path.offered = static_cast<uint8_t>(i);
                        path.steps = 1;
                        path.converter[0] = static_cast<uint8_t>(c);
                        return makeResult(path);
                }
        }
        // Two.
        for (size_t i = 0; i < n; ++i) {
                if (!valid[i]) continue;
                for (size_t c1 = 0; c1 < _count; ++c1) {
                        const Converter &a = _converters[c1];
                        if (a.fromHash != oh[i] || !ClipboardType::equal(a.from, offered[i])) continue;
                        for (size_t c2 = 0; c2 < _count; ++c2) {
                                const Converter &b = _converters[c2];
                                if (b.fromHash != a.toHash || b.toHash != wh) continue;
                                if (!ClipboardType::equal(b.from, a.to) || !ClipboardType::equal(b.to, wanted))
                                        continue;
                                path.offered = static_cast<uint8_t>(i);
                                path.steps = 2;
                                path.converter[0] = static_cast<uint8_t>(c1);
                                path.converter[1] = static_cast<uint8_t>(c2);
                                return makeResult(path);
                        }
                }
        }
        return makeError<Path>(Error::NotFound);
}

size_t ClipboardConverter::put(ByteSpan data, uint8_t *out) {
        size_t n = 0;
        switch (_builtin) {
                case Builtin::UriListToText:
                        for (const uint8_t b : data) n += uriByte(b, out + n);
                        break;
                case Builtin::Latin1ToText:
                        for (const uint8_t b : data) {
                                if (b < 0x80) {
                                        out[n++] = b;
                                } else {
                                        out[n++] = static_cast<uint8_t>(0xC0 | (b >> 6));
                                        out[n++] = static_cast<uint8_t>(0x80 | (b & 0x3F));
                                }
                        }
                        break;
                case Builtin::TextToLatin1:
                        for (const uint8_t b : data) {
                                uint32_t     cp[2];
                                const size_t k = _utf8.feed(b, cp);
                                for (size_t j = 0; j < k; ++j)
                                        out[n++] = cp[j] <= 0xFF ? static_cast<uint8_t>(cp[j]) : uint8_t('?');
                        }
                        break;
                case Builtin::None:
                        for (const uint8_t b : data) out[n++] = b;
                        break;
        }
        return n;
}

size_t ClipboardConverter::finish(uint8_t *out) {
        size_t n = 0;
        if (_builtin == Builtin::UriListToText) n = uriFlush(out);
        if (_builtin == Builtin::TextToLatin1 && _utf8.isPending()) out[n++] = '?'; // cut short
        _utf8.reset();
        _uri = Uri::LineStart;
        _held = 0;
        _newline = false;
        return n;
}

// One byte of a `text/uri-list`: a line each URI, `#` lines comments; `file://HOST/PATH` gives
// PATH, its %XX decoded; other URIs as they are. Lines are joined with LF, none after the last.
size_t ClipboardConverter::uriByte(uint8_t b, uint8_t *out) {
        size_t n = 0;
        switch (_uri) {
                case Uri::LineStart:
                        if (b == '\r' || b == '\n') return 0; // a blank line
                        if (b == '#') {
                                _uri = Uri::Comment;
                                return 0;
                        }
                        if (_newline) out[n++] = '\n';
                        _newline = false;
                        _uri = Uri::Scheme;
                        _held = 0;
                        [[fallthrough]];
                case Uri::Scheme:
                        if (b == static_cast<uint8_t>(FilePrefix[_held])) {
                                if (++_held == FilePrefixBytes) _uri = Uri::Host;
                                return n;
                        }
                        n += uriFlush(out + n); // not a file: the URI as it is
                        _uri = Uri::Plain;
                        return n + uriByte(b, out + n);
                case Uri::Host:
                        if (b == '/') {
                                out[n++] = '/';
                                _uri = Uri::Path;
                        } else if (b == '\n') {
                                _uri = Uri::LineStart;
                                _newline = true;
                        }
                        return n;
                case Uri::Percent:
                        if (hexValue(static_cast<char>(b)) >= 0) {
                                _hex[_held++] = static_cast<char>(b);
                                if (_held == 2) {
                                        out[n++] = static_cast<uint8_t>(hexValue(_hex[0]) * 16 + hexValue(_hex[1]));
                                        _uri = Uri::Path;
                                }
                                return n;
                        }
                        n += uriFlush(out + n); // not an escape: as written
                        _uri = Uri::Path;
                        [[fallthrough]];
                case Uri::Path:
                        if (b == '%') {
                                _uri = Uri::Percent;
                                _held = 0;
                                return n;
                        }
                        [[fallthrough]];
                case Uri::Plain:
                        if (b == '\r') return n;
                        if (b == '\n') {
                                _uri = Uri::LineStart;
                                _newline = true;
                                return n;
                        }
                        out[n++] = b;
                        return n;
                case Uri::Comment:
                        if (b == '\n') _uri = Uri::LineStart;
                        return n;
        }
        return n;
}

// What a line holds back, as written: a scheme's start, a % and its digits.
size_t ClipboardConverter::uriFlush(uint8_t *out) {
        size_t n = 0;
        if (_uri == Uri::Scheme) {
                for (size_t i = 0; i < _held; ++i) out[n++] = static_cast<uint8_t>(FilePrefix[i]);
        } else if (_uri == Uri::Percent) {
                out[n++] = '%';
                for (size_t i = 0; i < _held; ++i) out[n++] = static_cast<uint8_t>(_hex[i]);
        }
        _held = 0;
        return n;
}

BMDS_NAMESPACE_END
