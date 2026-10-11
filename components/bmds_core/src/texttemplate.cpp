/**
 * @file      texttemplate.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text templates (devplan/templates.md): ${NAME} and ${NAME:ARG}.
 */

#include <bmds/texttemplate.h>

BMDS_NAMESPACE_BEGIN

namespace {

        enum class Name {
                Home,
                HostName,
                Serial,
                Date,
                Time
        };

        struct Known {
                        const char *name;
                        Name        id;
                        bool        takesArg;
        };

        constexpr Known Names[] = {
                {"HOME", Name::Home, false}, {"HOSTNAME", Name::HostName, false}, {"SERIAL", Name::Serial, false},
                {"DATE", Name::Date, true},  {"TIME", Name::Time, true},
        };

        bool isNameStart(char c) {
                return c >= 'A' && c <= 'Z';
        }

        bool isNameChar(char c) {
                return isNameStart(c) || (c >= '0' && c <= '9') || c == '_';
        }

        // Walks @p text: each literal run and each name to @p out (null: only checked).
        Error walk(StringView text, const TextTemplate::Values *values, String *out) {
                const size_t n = text.size();
                size_t       i = 0;
                while (i < n) {
                        size_t run = i;
                        while (run < n && text[run] != '$') ++run;
                        if (run > i && out != nullptr) {
                                const Error err = out->append(text.mid(i, run - i));
                                if (err.isError()) return err;
                        }
                        if (run >= n) break;
                        i = run + 1; // past the '$'
                        if (i < n && text[i] == '$') {
                                if (out != nullptr) {
                                        const Error err = out->append('$');
                                        if (err.isError()) return err;
                                }
                                ++i;
                                continue;
                        }
                        if (i >= n || text[i] != '{') return Error::Invalid;
                        ++i;
                        const size_t nameStart = i;
                        if (i >= n || !isNameStart(text[i])) return Error::Invalid;
                        while (i < n && isNameChar(text[i])) ++i;
                        const StringView name = text.mid(nameStart, i - nameStart);
                        StringView       arg;
                        bool             hasArg = false;
                        if (i < n && text[i] == ':') {
                                const size_t argStart = ++i;
                                while (i < n && text[i] != '}') ++i;
                                arg = text.mid(argStart, i - argStart);
                                hasArg = true;
                        }
                        if (i >= n || text[i] != '}') return Error::Invalid;
                        ++i;
                        const Known *known = nullptr;
                        for (const Known &k : Names)
                                if (name == k.name) known = &k;
                        if (known == nullptr || (hasArg && !known->takesArg)) return Error::Invalid;
                        if (out == nullptr) continue;
                        Error err;
                        switch (known->id) {
                                case Name::Home: err = out->append(values->home); break;
                                case Name::HostName: err = out->append(values->hostname); break;
                                case Name::Serial: err = out->append(values->serial); break;
                                case Name::Date:
                                case Name::Time: {
                                        const LocalTime epoch;
                                        const char     *fallback = known->id == Name::Date
                                                                           ? TextTemplate::DefaultDateFormat
                                                                           : TextTemplate::DefaultTimeFormat;
                                        err = formatTime(*out, hasArg ? arg : StringView(fallback),
                                                         values->time != nullptr ? *values->time : epoch);
                                        break;
                                }
                        }
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

} // namespace

Error TextTemplate::check(StringView text) {
        return walk(text, nullptr, nullptr);
}

Error TextTemplate::expand(StringView text, const Values &values, String &out) {
        out.clear();
        return walk(text, &values, &out);
}

BMDS_NAMESPACE_END
