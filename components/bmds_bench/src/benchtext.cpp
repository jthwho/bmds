/**
 * @file      benchtext.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <bmds/bench.h>
#include <bmds/benchtext.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct TagName {
                        uint16_t    bit;
                        const char *name;
        };

        constexpr TagName TagNames[] = {
                {BenchQuick, "quick"},   {BenchLong, "long"}, {BenchScreen, "screen"},   {BenchAudio, "audio"},
                {BenchWrites, "writes"}, {BenchPeer, "peer"}, {BenchProgram, "program"},
        };

        // `*` any run of characters, `?` one.
        bool glob(StringView pattern, StringView text) {
                size_t p = 0, t = 0, star = StringView::NPos, mark = 0;
                while (t < text.size()) {
                        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
                                ++p;
                                ++t;
                        } else if (p < pattern.size() && pattern[p] == '*') {
                                star = p++;
                                mark = t;
                        } else if (star != StringView::NPos) {
                                p = star + 1;
                                t = ++mark;
                        } else {
                                return false;
                        }
                }
                while (p < pattern.size() && pattern[p] == '*') ++p;
                return p == pattern.size();
        }

        Error appendSafe(StringView text, String &out) {
                for (char c : text) {
                        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                        c == '.' || c == '_';
                        if (Error err = out.append(ok ? c : '_'); err.isError()) return err;
                }
                return Error::Ok;
        }

} // namespace

Error BenchContext::paramUnsigned(StringView name, uint64_t &out) const {
        const StringView text = param(name);
        if (text.isEmpty() || text.size() > 19) return Error::Invalid;
        uint64_t v = 0;
        for (char c : text) {
                if (c < '0' || c > '9') return Error::Invalid;
                v = v * 10 + uint64_t(c - '0');
        }
        out = v;
        return Error::Ok;
}

BenchStats BenchText::summarize(Span<double> samples) {
        BenchStats s;
        if (samples.size() == 0) return s;
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        s.runs = static_cast<uint32_t>(n);
        s.min = samples[0];
        s.max = samples[n - 1];
        s.median = n % 2 == 1 ? samples[n / 2] : (samples[n / 2 - 1] + samples[n / 2]) / 2;
        double sum = 0;
        for (double v : samples) sum += v;
        s.mean = sum / double(n);
        s.spread = s.median != 0 ? (s.max - s.min) / 2 / std::fabs(s.median) * 100 : 0;
        s.noisy = s.spread > NoisyPercent;
        return s;
}

uint16_t BenchText::tagNamed(StringView name) {
        for (const TagName &t : TagNames) {
                if (name == StringView(t.name)) return t.bit;
        }
        return 0;
}

Error BenchText::appendTags(uint16_t tags, String &out) {
        bool first = true;
        for (const TagName &t : TagNames) {
                if ((tags & t.bit) == 0) continue;
                if (!first) {
                        if (Error err = out.append(','); err.isError()) return err;
                }
                first = false;
                if (Error err = out.append(StringView(t.name)); err.isError()) return err;
        }
        return Error::Ok;
}

bool BenchText::matches(StringView pattern, StringView name, uint16_t tags) {
        if (pattern.startsWith('@')) {
                const StringView tag = pattern.mid(1);
                if (tag == "all") return true;
                const uint16_t bit = tagNamed(tag);
                return bit != 0 && (tags & bit) != 0;
        }
        if (pattern.contains('*') || pattern.contains('?')) return glob(pattern, name);
        if (name == pattern) return true;
        return name.size() > pattern.size() && name.startsWith(pattern) && name[pattern.size()] == '.';
}

Error BenchText::fileName(StringView serial, StringView version, const When &when, String &out) {
        out.clear();
        if (Error err = appendSafe(serial.isEmpty() ? StringView("unknown") : serial, out); err.isError()) return err;
        if (Error err = out.append('-'); err.isError()) return err;
        if (Error err = appendSafe(version, out); err.isError()) return err;
        return out.appendf("-%04d%02d%02d-%02d%02d%02d.json", when.year, when.month, when.day, when.hour, when.minute,
                           when.second);
}

Error BenchText::appendIso8601(const When &when, String &out) {
        const int offset = when.offsetMinutes < 0 ? -when.offsetMinutes : when.offsetMinutes;
        return out.appendf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", when.year, when.month, when.day, when.hour,
                           when.minute, when.second, when.offsetMinutes < 0 ? '-' : '+', offset / 60, offset % 60);
}

void BenchText::formatValue(double value, char *out, size_t outBytes) {
        if (outBytes == 0) return;
        if (!std::isfinite(value)) {
                std::snprintf(out, outBytes, "-");
        } else if (std::fabs(value) >= 1000) {
                std::snprintf(out, outBytes, "%.0f", value);
        } else {
                std::snprintf(out, outBytes, "%.4g", value);
        }
        return;
}

BMDS_NAMESPACE_END
