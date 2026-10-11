/**
 * @file      configtypes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/configtypes.h>

BMDS_NAMESPACE_BEGIN

namespace Config {

        namespace {

                bool isKeyChar(char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                }

                int hexDigit(char c) {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                }

                size_t longest(const Param &param) {
                        return param.max > 0 ? size_t(param.max) : MaxValue;
                }

                // An absolute path without empty, `.` or `..` components or a trailing `/`.
                Error appendPath(StringView path, String &out) {
                        if (!path.startsWith('/')) return Error::Invalid;
                        const size_t start = out.size();
                        size_t       i = 0;
                        while (i < path.size()) {
                                size_t end = path.indexOf('/', i);
                                if (end == StringView::NPos) end = path.size();
                                const StringView part = path.mid(i, end - i);
                                i = end + 1;
                                if (part.isEmpty() || part == ".") continue;
                                if (part == "..") {
                                        const size_t slash = out.view().lastIndexOf('/');
                                        out.truncate(slash == StringView::NPos || slash < start ? start : slash);
                                        continue;
                                }
                                Error err = out.append('/');
                                if (err.isOk()) err = out.append(part);
                                if (err.isError()) return err;
                        }
                        return out.size() == start ? out.append('/') : Error();
                }

                bool parseInt(StringView text, int64_t &out) {
                        size_t i = 0;
                        bool   negative = false;
                        if (!text.isEmpty() && (text[0] == '-' || text[0] == '+')) {
                                negative = text[0] == '-';
                                i = 1;
                        }
                        if (i == text.size()) return false;
                        int64_t v = 0;
                        for (; i < text.size(); ++i) {
                                if (text[i] < '0' || text[i] > '9') return false;
                                v = v * 10 + (text[i] - '0');
                                if (v > int64_t(INT32_MAX) + 1) return false;
                        }
                        out = negative ? -v : v;
                        return true;
                }

        } // namespace

        bool isValidKey(StringView key) {
                if (key.isEmpty() || key.size() > MaxKey || key.front() == '/' || key.back() == '/') return false;
                char previous = 0;
                for (char c : key) {
                        if (c == '/' && previous == '/') return false;
                        if (c != '/' && !isKeyChar(c)) return false;
                        previous = c;
                }
                return true;
        }

        const char *typeName(Type type) {
                switch (type) {
                        case Type::Bool: return "bool";
                        case Type::Int: return "int";
                        case Type::String: return "string";
                        case Type::Color: return "color";
                        case Type::Enum: return "enum";
                        case Type::Path: return "path";
                        case Type::Paths: return "paths";
                        case Type::Secret: return "secret";
                }
                return "?";
        }

        Error canonical(const Param &param, StringView input, String &out) {
                out.clear();
                switch (param.type) {
                        case Type::Bool: {
                                if (input == "true" || input == "yes" || input == "on" || input == "1")
                                        return out.assign("true");
                                if (input == "false" || input == "no" || input == "off" || input == "0")
                                        return out.assign("false");
                                return Error::Invalid;
                        }
                        case Type::Int: {
                                int64_t v = 0;
                                if (!parseInt(input, v)) return Error::Invalid;
                                if (param.min < param.max && (v < param.min || v > param.max)) return Error::OutOfRange;
                                return out.appendf("%ld", static_cast<long>(v));
                        }
                        case Type::String:
                        case Type::Secret:
                                if (input.size() > longest(param)) return Error::OutOfRange;
                                for (char c : input)
                                        if (c == '\0') return Error::Invalid;
                                return out.assign(input);
                        case Type::Color: {
                                StringView hex = input.startsWith('#') ? input.mid(1) : input;
                                if (hex.size() != 6) return Error::Invalid;
                                for (char c : hex)
                                        if (hexDigit(c) < 0) return Error::Invalid;
                                for (char c : hex) {
                                        const Error err = out.append(c >= 'A' && c <= 'F' ? char(c - 'A' + 'a') : c);
                                        if (err.isError()) return err;
                                }
                                return Error::Ok;
                        }
                        case Type::Enum: {
                                StringView words = param.choices != nullptr ? StringView(param.choices) : StringView();
                                while (!words.isEmpty()) {
                                        const size_t     bar = words.indexOf('|');
                                        const StringView word = bar == StringView::NPos ? words : words.left(bar);
                                        if (!input.isEmpty() && word == input) return out.assign(input);
                                        words = bar == StringView::NPos ? StringView() : words.mid(bar + 1);
                                }
                                return Error::Invalid;
                        }
                        case Type::Path: {
                                const Error err = appendPath(input, out);
                                if (err.isError()) return err;
                                return out.size() > longest(param) ? Error(Error::OutOfRange) : Error();
                        }
                        case Type::Paths: {
                                StringView rest = input;
                                while (!rest.isEmpty()) {
                                        const size_t     semi = rest.indexOf(';');
                                        const StringView one = semi == StringView::NPos ? rest : rest.left(semi);
                                        rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                                        if (one.isEmpty()) continue;
                                        if (!out.isEmpty()) {
                                                const Error err = out.append(';');
                                                if (err.isError()) return err;
                                        }
                                        const Error err = appendPath(one, out);
                                        if (err.isError()) return err;
                                }
                                return out.size() > longest(param) ? Error(Error::OutOfRange) : Error();
                        }
                }
                return Error::Invalid;
        }

        int32_t toInt(StringView value) {
                int64_t v = 0;
                return parseInt(value, v) && v >= INT32_MIN && v <= INT32_MAX ? int32_t(v) : 0;
        }

        bool toBool(StringView value) {
                return value == "true";
        }

        uint32_t toColor(StringView value) {
                if (value.size() != 6) return 0;
                uint32_t rgb = 0;
                for (char c : value) {
                        const int d = hexDigit(c);
                        if (d < 0) return 0;
                        rgb = rgb << 4 | uint32_t(d);
                }
                return rgb;
        }

        size_t parseStore(StringView text, StoreLine fn, void *arg) {
                size_t bad = 0;
                String value;
                while (!text.isEmpty()) {
                        const size_t     end = text.indexOf('\n');
                        const StringView line = end == StringView::NPos ? text : text.left(end);
                        text = end == StringView::NPos ? StringView() : text.mid(end + 1);
                        if (line.trimmed().isEmpty() || line.trimmed().startsWith('#')) continue;
                        const size_t equals = line.indexOf('=');
                        if (equals == StringView::NPos || !isValidKey(line.left(equals))) {
                                ++bad;
                                continue;
                        }
                        value.clear();
                        const StringView raw = line.mid(equals + 1);
                        Error            err;
                        for (size_t i = 0; i < raw.size() && err.isOk(); ++i) {
                                char c = raw[i];
                                if (c == '\\' && i + 1 < raw.size()) {
                                        c = raw[++i] == 'n' ? '\n' : raw[i];
                                }
                                err = value.append(c);
                        }
                        if (err.isError()) {
                                ++bad;
                                continue;
                        }
                        fn(arg, line.left(equals), value.view());
                }
                return bad;
        }

        Error appendStoreLine(String &out, StringView key, StringView value) {
                Error err = out.append(key);
                if (err.isOk()) err = out.append('=');
                for (size_t i = 0; i < value.size() && err.isOk(); ++i) {
                        if (value[i] == '\n') {
                                err = out.append("\\n");
                        } else if (value[i] == '\\') {
                                err = out.append("\\\\");
                        } else {
                                err = out.append(value[i]);
                        }
                }
                if (err.isOk()) err = out.append('\n');
                return err;
        }

} // namespace Config

BMDS_NAMESPACE_END
