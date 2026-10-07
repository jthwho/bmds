/**
 * @file      clipboardtype.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The clipboard's MIME types (devplan/clipboard.md C5).
 */

#include <bmds/clipboardtype.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // RFC 2045's token characters.
        constexpr bool isTokenChar(char ch) {
                if (ch >= 'a' && ch <= 'z') return true;
                if (ch >= 'A' && ch <= 'Z') return true;
                if (ch >= '0' && ch <= '9') return true;
                return StringView("!#$%&'*+-.^_`{|}~").contains(ch);
        }

        bool isToken(StringView s) {
                if (s.isEmpty()) return false;
                for (const char ch : s)
                        if (!isTokenChar(ch)) return false;
                return true;
        }

        // A quoted string, quotes included (a backslash escapes the next character).
        bool isQuoted(StringView s) {
                if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
                for (size_t i = 1; i + 1 < s.size(); ++i) {
                        if (s[i] == '\\') {
                                if (++i + 1 >= s.size()) return false;
                        } else if (s[i] == '"') {
                                return false;
                        }
                }
                return true;
        }

        // Appends to a buffer, remembering whether it overflowed.
        struct Writer {
                        Span<char> out;
                        size_t     length = 0;
                        bool       full = false;

                        void add(StringView s, bool lower) {
                                for (char ch : s) {
                                        if (length >= ClipboardType::MaxType || length + 1 >= out.size()) {
                                                full = true;
                                                return;
                                        }
                                        out[length++] = lower ? StringView::toLower(ch) : ch;
                                }
                                return;
                        }
        };

        struct Extension {
                        const char *extension;
                        const char *type;
        };

        // Common files' types; append as needed.
        constexpr Extension Extensions[] = {
                {"txt", "text/plain;charset=utf-8"},
                {"md", "text/markdown;charset=utf-8"},
                {"csv", "text/csv;charset=utf-8"},
                {"html", "text/html;charset=utf-8"},
                {"htm", "text/html;charset=utf-8"},
                {"json", "application/json"},
                {"xml", "application/xml"},
                {"desktop", "application/x-desktop"},
                {"png", "image/png"},
                {"bmp", "image/bmp"},
                {"gif", "image/gif"},
                {"jpg", "image/jpeg"},
                {"jpeg", "image/jpeg"},
                {"svg", "image/svg+xml"},
                {"wav", "audio/wav"},
                {"pdf", "application/pdf"},
                {"zip", "application/zip"},
        };

} // namespace

Result<StringView> ClipboardType::normalize(StringView type, Span<char> out) {
        type = type.trimmed();
        if (type.equalsIgnoreCase("text")) type = Text;
        if (out.size() == 0) return makeError<StringView>(Error::BufferTooSmall);
        const size_t     semi = type.indexOf(';');
        const StringView full = (semi == StringView::NPos ? type : type.left(semi)).trimmed();
        const size_t     slash = full.indexOf('/');
        if (slash == StringView::NPos) return makeError<StringView>(Error::Invalid);
        const StringView major = full.left(slash), minor = full.mid(slash + 1);
        if (!isToken(major) || !isToken(minor)) return makeError<StringView>(Error::Invalid);
        Writer w{out};
        w.add(major, true);
        w.add("/", false);
        w.add(minor, true);
        // The parameters: `;name=value` each, a quoted value's `;` not splitting it.
        StringView rest = semi == StringView::NPos ? StringView() : type.mid(semi + 1);
        while (!rest.isEmpty()) {
                size_t end = 0;
                bool   quoted = false;
                for (; end < rest.size(); ++end) {
                        const char ch = rest[end];
                        if (quoted && ch == '\\') {
                                ++end;
                        } else if (ch == '"') {
                                quoted = !quoted;
                        } else if (ch == ';' && !quoted) {
                                break;
                        }
                }
                if (end > rest.size()) end = rest.size();
                const StringView param = rest.left(end).trimmed();
                rest = end < rest.size() ? rest.mid(end + 1) : StringView();
                if (param.isEmpty()) continue; // `text/plain;`
                const size_t equals = param.indexOf('=');
                if (equals == StringView::NPos) return makeError<StringView>(Error::Invalid);
                const StringView name = param.left(equals).trimmed();
                StringView       value = param.mid(equals + 1).trimmed();
                if (!isToken(name) || !(isToken(value) || isQuoted(value)))
                        return makeError<StringView>(Error::Invalid);
                const bool charset = name.equalsIgnoreCase("charset");
                if (charset && value.equalsIgnoreCase("utf8")) value = "utf-8";
                w.add(";", false);
                w.add(name, true);
                w.add("=", false);
                w.add(value, charset);
        }
        if (w.full) return makeError<StringView>(Error::BufferTooSmall);
        out[w.length] = '\0';
        return makeResult(StringView(out.data(), w.length));
}

bool ClipboardType::isValid(StringView type) {
        char buffer[BufferBytes];
        return normalize(type, Span<char>(buffer, sizeof(buffer))).isOk();
}

bool ClipboardType::equal(StringView a, StringView b) {
        char a1[BufferBytes], b1[BufferBytes];
        auto [na, ea] = normalize(a, Span<char>(a1, sizeof(a1)));
        if (ea.isError()) return false;
        auto [nb, eb] = normalize(b, Span<char>(b1, sizeof(b1)));
        return eb.isOk() && na == nb;
}

StringView ClipboardType::fromFileName(StringView name) {
        const size_t dot = name.lastIndexOf('.');
        const size_t slash = name.lastIndexOf('/');
        if (dot == StringView::NPos || (slash != StringView::NPos && dot < slash)) return StringView();
        const StringView extension = name.mid(dot + 1);
        for (const Extension &e : Extensions)
                if (extension.equalsIgnoreCase(e.extension)) return StringView(e.type);
        return StringView();
}

BMDS_NAMESPACE_END
