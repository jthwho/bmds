/**
 * @file      audiocontrol.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/audiocontrol.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A whole number: an optional sign, then digits (at most 9, so it fits).
        bool parseWhole(StringView text, int32_t &out) {
                bool negative = false;
                if (text.startsWith('-') || text.startsWith('+')) {
                        negative = text.front() == '-';
                        text = text.mid(1);
                }
                if (text.isEmpty() || text.size() > 9) return false;
                int32_t v = 0;
                for (char c : text) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + (c - '0');
                }
                out = negative ? -v : v;
                return true;
        }

        // Decibels as hundredths: `-12`, `-12.5`, `+3.25`, with `dB` after.
        bool parseDecibels(StringView text, int32_t &out) {
                if (text.size() >= 2 && text.right(2).equalsIgnoreCase("db"))
                        text = text.left(text.size() - 2).trimmed();
                bool negative = false;
                if (text.startsWith('-') || text.startsWith('+')) {
                        negative = text.front() == '-';
                        text = text.mid(1);
                }
                const size_t     dot = text.indexOf('.');
                const StringView whole = dot == StringView::NPos ? text : text.left(dot);
                const StringView frac = dot == StringView::NPos ? StringView() : text.mid(dot + 1);
                if (whole.isEmpty() && frac.isEmpty()) return false;
                if (dot != StringView::NPos && frac.isEmpty()) return false;
                if (frac.size() > 2) return false;
                int32_t w = 0;
                if (!whole.isEmpty() && (!parseWhole(whole, w) || whole.startsWith('-') || whole.startsWith('+')))
                        return false;
                int32_t f = 0;
                if (!frac.isEmpty() && !parseWhole(frac, f)) return false;
                if (frac.size() == 1) f *= 10;
                if (w > 1'000'000) return false;
                const int32_t v = w * 100 + f;
                out = negative ? -v : v;
                return true;
        }

        bool parseBool(StringView text, int32_t &out) {
                constexpr StringView Yes[] = {"on", "true", "yes", "1"};
                constexpr StringView No[] = {"off", "false", "no", "0"};
                for (StringView w : Yes)
                        if (text.equalsIgnoreCase(w)) {
                                out = 1;
                                return true;
                        }
                for (StringView w : No)
                        if (text.equalsIgnoreCase(w)) {
                                out = 0;
                                return true;
                        }
                return false;
        }

} // namespace

StringView AudioControl::choice(int32_t index) const {
        StringView rest(choices);
        for (int32_t i = 0; index >= 0; ++i) {
                const size_t     bar = rest.indexOf('|');
                const StringView word = bar == StringView::NPos ? rest : rest.left(bar);
                if (i == index) return word;
                if (bar == StringView::NPos) break;
                rest = rest.mid(bar + 1);
        }
        return StringView();
}

Result<int32_t> AudioControl::parse(StringView text) const {
        text = text.trimmed();
        int32_t v = 0;
        switch (type) {
                case Type::Bool:
                        if (!parseBool(text, v)) return makeError<int32_t>(Error::Invalid);
                        return makeResult(v);
                case Type::Enum:
                        for (int32_t i = min; i <= max; ++i)
                                if (!text.isEmpty() && text == choice(i)) return makeResult(i);
                        return makeError<int32_t>(Error::Invalid);
                case Type::Decibel:
                        if (!parseDecibels(text, v)) return makeError<int32_t>(Error::Invalid);
                        break;
                case Type::Int:
                        if (!parseWhole(text, v)) return makeError<int32_t>(Error::Invalid);
                        break;
        }
        if (v < min || v > max) return makeError<int32_t>(Error::OutOfRange);
        if (step > 1 && (v - min) % step != 0) return makeError<int32_t>(Error::Invalid);
        return makeResult(v);
}

void AudioControl::format(int32_t value, char *out, size_t size) const {
        switch (type) {
                case Type::Bool: std::snprintf(out, size, "%s", value != 0 ? "on" : "off"); break;
                case Type::Enum: {
                        const StringView w = choice(value);
                        if (w.isEmpty()) {
                                std::snprintf(out, size, "%ld", static_cast<long>(value));
                        } else {
                                std::snprintf(out, size, "%.*s", static_cast<int>(w.size()), w.data());
                        }
                        break;
                }
                case Type::Decibel: {
                        const uint32_t a = static_cast<uint32_t>(value < 0 ? -static_cast<int64_t>(value) : value);
                        const char    *sign = value < 0 ? "-" : "";
                        if (a % 100 == 0) {
                                std::snprintf(out, size, "%s%lu dB", sign, static_cast<unsigned long>(a / 100));
                        } else if (a % 10 == 0) {
                                std::snprintf(out, size, "%s%lu.%lu dB", sign, static_cast<unsigned long>(a / 100),
                                              static_cast<unsigned long>(a % 100 / 10));
                        } else {
                                std::snprintf(out, size, "%s%lu.%02lu dB", sign, static_cast<unsigned long>(a / 100),
                                              static_cast<unsigned long>(a % 100));
                        }
                        break;
                }
                case Type::Int: std::snprintf(out, size, "%ld", static_cast<long>(value)); break;
        }
        return;
}

const char *AudioControl::typeName() const {
        switch (type) {
                case Type::Bool: return "bool";
                case Type::Enum: return "enum";
                case Type::Decibel: return "dB";
                case Type::Int: return "int";
        }
        return "?";
}

BMDS_NAMESPACE_END
