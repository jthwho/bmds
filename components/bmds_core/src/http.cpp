/**
 * @file      http.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/http.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isAlpha(char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        bool isHostChar(char c) {
                return isAlpha(c) || isDigit(c) || c == '.' || c == '-';
        }

        // Reads a decimal number (leading zeros allowed, as HTTP allows them) into @p out.
        bool parseDecimal(StringView s, uint64_t &out) {
                if (s.isEmpty() || s.size() > 19) return false;
                uint64_t n = 0;
                for (char c : s) {
                        if (!isDigit(c)) return false;
                        n = n * 10 + uint64_t(c - '0');
                }
                out = n;
                return true;
        }

        StringView trimmed(StringView s) {
                while (!s.isEmpty() && (s[0] == ' ' || s[0] == '\t')) s = s.mid(1);
                while (!s.isEmpty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t')) s = s.left(s.size() - 1);
                return s;
        }

        // The scheme's length with its "://" (7 or 8), or 0 for neither http nor https.
        size_t schemeBytes(StringView text, bool &tls) {
                if (text.size() >= 7 && text.left(7).equalsIgnoreCase("http://")) {
                        tls = false;
                        return 7;
                }
                if (text.size() >= 8 && text.left(8).equalsIgnoreCase("https://")) {
                        tls = true;
                        return 8;
                }
                return 0;
        }

        // RFC 3986 §5.2.4: @p path without its "." and ".." segments, into @p out.
        Error removeDots(StringView path, String &out) {
                String result;
                Error  err = result.reserve(path.size() + 1);
                while (err.isOk() && !path.isEmpty()) {
                        if (path.startsWith("../")) {
                                path = path.mid(3);
                        } else if (path.startsWith("./")) {
                                path = path.mid(2);
                        } else if (path.startsWith("/./")) {
                                path = path.mid(2);
                        } else if (path == StringView("/.")) {
                                path = "/";
                        } else if (path.startsWith("/../") || path == StringView("/..")) {
                                path = path.size() == 3 ? StringView("/") : path.mid(3);
                                const size_t slash = result.view().lastIndexOf('/');
                                result.truncate(slash == StringView::NPos ? 0 : slash);
                        } else if (path == StringView(".") || path == StringView("..")) {
                                path = StringView();
                        } else {
                                const size_t next = path.indexOf('/', 1);
                                err = result.append(path.left(next));
                                path = next == StringView::NPos ? StringView() : path.mid(next);
                        }
                }
                if (err.isOk()) out = std::move(result);
                return err;
        }

} // namespace

Error Http::parseUrl(StringView text, Url &out) {
        out = Url{};
        if (text.size() > MaxUrlBytes) return Error::Invalid;
        for (char c : text) {
                if (c <= 0x20 || c >= 0x7f) return Error::Invalid;
        }
        const size_t scheme = schemeBytes(text, out.tls);
        if (scheme == 0) return Error::Invalid;
        StringView rest = text.mid(scheme);
        size_t     end = 0;
        while (end < rest.size() && rest[end] != '/' && rest[end] != '?' && rest[end] != '#') ++end;
        StringView   authority = rest.left(end);
        StringView   path = rest.mid(end);
        const size_t hash = path.indexOf('#');
        if (hash != StringView::NPos) path = path.left(hash);
        if (authority.contains('@')) return Error::Invalid;
        const size_t colon = authority.indexOf(':');
        out.host = authority.left(colon);
        if (out.host.isEmpty() || out.host.size() > 253) return Error::Invalid;
        for (char c : out.host) {
                if (!isHostChar(c)) return Error::Invalid;
        }
        out.port = out.tls ? 443 : 80;
        if (colon != StringView::NPos) {
                uint64_t port = 0;
                if (!parseDecimal(authority.mid(colon + 1), port) || port == 0 || port > 65535) return Error::Invalid;
                out.port = uint16_t(port);
        }
        out.path = path;
        return Error::Ok;
}

Result<String> Http::resolve(StringView base, StringView reference) {
        Url b;
        if (parseUrl(base, b).isError()) return Result<String>(String(), Error::Invalid);
        const size_t hash = reference.indexOf('#');
        if (hash != StringView::NPos) reference = reference.left(hash);
        String out;
        Error  err;
        // An absolute URL (a scheme before any '/', '?' or '#').
        size_t i = 0;
        while (i < reference.size() && (isAlpha(reference[i]) || isDigit(reference[i]) || reference[i] == '+' ||
                                        reference[i] == '-' || reference[i] == '.'))
                ++i;
        if (i > 0 && i < reference.size() && reference[i] == ':' && isAlpha(reference[0])) {
                err = out.append(reference);
        } else {
                // The base up to its path: the scheme and the authority, as written.
                const StringView origin = base.left(size_t(b.path.data() - base.data()));
                if (reference.startsWith("//")) {
                        err = out.append(b.tls ? "https:" : "http:");
                        if (err.isOk()) err = out.append(reference);
                } else {
                        // The path to resolve, and the query that follows it untouched.
                        StringView   basePath = b.path;
                        const size_t query = basePath.indexOf('?');
                        if (query != StringView::NPos) basePath = basePath.left(query);
                        String merged;
                        if (reference.isEmpty() || reference.startsWith('?')) {
                                err = merged.append(basePath.isEmpty() ? StringView("/") : basePath);
                                if (err.isOk())
                                        err = merged.append(reference.isEmpty() && query != StringView::NPos
                                                                    ? b.path.mid(query)
                                                                    : reference);
                        } else if (reference.startsWith('/')) {
                                err = merged.append(reference);
                        } else {
                                const size_t slash = basePath.lastIndexOf('/');
                                err = merged.append(slash == StringView::NPos ? StringView("/")
                                                                              : basePath.left(slash + 1));
                                if (err.isOk()) err = merged.append(reference);
                        }
                        // Dots go from the path, not the query.
                        StringView   whole = merged.view();
                        const size_t q = whole.indexOf('?');
                        String       clean;
                        if (err.isOk()) err = removeDots(whole.left(q), clean);
                        if (err.isOk()) err = out.append(origin);
                        if (err.isOk()) err = out.append(clean.isEmpty() ? StringView("/") : clean.view());
                        if (err.isOk() && q != StringView::NPos) err = out.append(whole.mid(q));
                }
        }
        if (err.isError()) return Result<String>(String(), err);
        Url check;
        if (parseUrl(out.view(), check).isError()) return Result<String>(String(), Error::Invalid);
        return Result<String>(std::move(out), Error::Ok);
}

Error Http::buildRequest(const Url &url, uint64_t from, StringView userAgent, String &out) {
        out.clear();
        Error err = out.appendf("GET %s%.*s HTTP/1.1\r\nHost: %.*s", url.path.startsWith('/') ? "" : "/",
                                int(url.path.size()), url.path.data(), int(url.host.size()), url.host.data());
        if (err.isOk() && url.port != (url.tls ? 443 : 80)) err = out.appendf(":%u", unsigned(url.port));
        if (err.isOk())
                err = out.appendf("\r\nUser-Agent: %.*s\r\nAccept: */*\r\n", int(userAgent.size()), userAgent.data());
        if (err.isOk() && from != 0) err = out.appendf("Range: bytes=%llu-\r\n", static_cast<unsigned long long>(from));
        if (err.isOk()) err = out.append("Connection: close\r\n\r\n");
        return err;
}

Http::ResponseParser::ResponseParser() {}

Error Http::ResponseParser::feed(ByteSpan in, size_t &used, ByteSpan &body) {
        used = 0;
        body = ByteSpan();
        switch (_state) {
                case State::BodyLength:
                case State::ChunkData: {
                        const size_t n = _left < in.size() ? size_t(_left) : in.size();
                        body = in.first(n);
                        used = n;
                        _left -= n;
                        if (_left == 0) _state = _state == State::BodyLength ? State::Done : State::ChunkEnd;
                        return Error::Ok;
                }
                case State::BodyToEnd:
                        body = in;
                        used = in.size();
                        return Error::Ok;
                case State::Done: return Error::Ok;
                default: break;
        }
        // A line: up to its '\n', kept across calls.
        size_t n = 0;
        while (n < in.size() && in[n] != '\n') ++n;
        const bool   whole = n < in.size();
        const size_t take = whole ? n + 1 : n;
        const bool   inHead = _state == State::StatusLine || _state == State::Headers || _state == State::Trailers;
        if (_line.size() + n > MaxLineBytes || (inHead && _headBytes + take > MaxHeadBytes)) return Error::CorruptData;
        _headBytes += take;
        used = take;
        Error err = _line.append(StringView(reinterpret_cast<const char *>(in.data()), n));
        if (err.isError() || !whole) return err;
        StringView text = _line.view();
        if (text.endsWith('\r')) text = text.left(text.size() - 1);
        err = line(text);
        _line.clear();
        return err;
}

Error Http::ResponseParser::end() {
        if (_state == State::BodyToEnd) _state = State::Done;
        return _state == State::Done ? Error::Ok : Error::CorruptData;
}

Error Http::ResponseParser::line(StringView text) {
        switch (_state) {
                case State::StatusLine: {
                        // "HTTP/1.x SSS reason"
                        if (text.size() < 12 || !text.startsWith("HTTP/1.") || !isDigit(text[7]) || text[8] != ' ' ||
                            !isDigit(text[9]) || !isDigit(text[10]) || !isDigit(text[11]) ||
                            (text.size() > 12 && text[12] != ' '))
                                return Error::CorruptData;
                        _head = Head{};
                        _sawLength = false;
                        _head.status = (text[9] - '0') * 100 + (text[10] - '0') * 10 + (text[11] - '0');
                        if (_head.status < 100) return Error::CorruptData;
                        _state = State::Headers;
                        return Error::Ok;
                }
                case State::Headers: return text.isEmpty() ? headDone() : header(text);
                case State::ChunkSize: {
                        const size_t     semi = text.indexOf(';');
                        const StringView digits = trimmed(text.left(semi));
                        if (digits.isEmpty() || digits.size() > 15) return Error::CorruptData;
                        uint64_t size = 0;
                        for (char c : digits) {
                                const int v = isDigit(c)               ? c - '0'
                                              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                                       : -1;
                                if (v < 0) return Error::CorruptData;
                                size = size * 16 + uint64_t(v);
                        }
                        if (size == 0) {
                                _state = State::Trailers;
                                _headBytes = 0;
                        } else {
                                _left = size;
                                _state = State::ChunkData;
                        }
                        return Error::Ok;
                }
                case State::ChunkEnd:
                        if (!text.isEmpty()) return Error::CorruptData;
                        _state = State::ChunkSize;
                        return Error::Ok;
                case State::Trailers:
                        if (text.isEmpty()) _state = State::Done;
                        return Error::Ok;
                default: return Error::CorruptData;
        }
}

Error Http::ResponseParser::header(StringView text) {
        const size_t colon = text.indexOf(':');
        if (colon == StringView::NPos || colon == 0) return Error::CorruptData;
        const StringView name = text.left(colon);
        for (char c : name) {
                if (c <= 0x20 || c >= 0x7f) return Error::CorruptData; // a folded line, or not a name
        }
        const StringView value = trimmed(text.mid(colon + 1));
        if (name.equalsIgnoreCase("Content-Length")) {
                uint64_t length = 0;
                if (!parseDecimal(value, length) || (_sawLength && length != _head.length)) return Error::CorruptData;
                _head.length = length;
                _sawLength = true;
        } else if (name.equalsIgnoreCase("Transfer-Encoding")) {
                StringView rest = value;
                while (!rest.isEmpty()) {
                        const size_t     comma = rest.indexOf(',');
                        const StringView coding = trimmed(rest.left(comma));
                        rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        if (coding.isEmpty()) continue;
                        if (!coding.equalsIgnoreCase("chunked")) return Error::NotSupported;
                        _head.chunked = true;
                }
        } else if (name.equalsIgnoreCase("Location")) {
                if (value.size() > MaxUrlBytes) return Error::CorruptData;
                _head.location.clear();
                return _head.location.append(value);
        } else if (name.equalsIgnoreCase("Content-Range")) {
                // "bytes FIRST-LAST/TOTAL" (TOTAL may be "*").
                if (!value.startsWith("bytes ")) return Error::Ok;
                const StringView range = value.mid(6);
                if (range.startsWith("*/")) { // a 416's: the range unsatisfiable, the size said
                        uint64_t total = 0;
                        if (!parseDecimal(range.mid(2), total)) return Error::CorruptData;
                        _head.rangeTotal = total;
                        return Error::Ok;
                }
                const size_t dash = range.indexOf('-');
                const size_t slash = range.indexOf('/');
                uint64_t     first = 0, total = 0;
                if (dash == StringView::NPos || slash == StringView::NPos || slash < dash ||
                    !parseDecimal(range.left(dash), first))
                        return Error::CorruptData;
                _head.rangeStart = first;
                const StringView whole = range.mid(slash + 1);
                if (whole == StringView("*"))
                        _head.rangeTotal = Unknown;
                else if (parseDecimal(whole, total))
                        _head.rangeTotal = total;
                else
                        return Error::CorruptData;
        }
        return Error::Ok;
}

Error Http::ResponseParser::headDone() {
        if (_head.status < 200) { // interim: its head is skipped, the real one follows
                _state = State::StatusLine;
                return Error::Ok;
        }
        if (_head.status == 204 || _head.status == 304) {
                _state = State::Done;
        } else if (_head.chunked) {
                _state = State::ChunkSize;
        } else if (_sawLength) {
                _left = _head.length;
                _state = _left == 0 ? State::Done : State::BodyLength;
        } else {
                _state = State::BodyToEnd;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
