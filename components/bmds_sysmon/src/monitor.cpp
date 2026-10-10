/**
 * @file      monitor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The System Monitor's view of the counters, pure (devplan/sysmon.md §3.5).
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "monitor.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // @p v / @p scale with one or two decimals and @p suffix (no floating point).
        void scaled(char (&out)[Monitor::TextBytes], int64_t v, int64_t scale, const char *suffix) {
                const bool    negative = v < 0;
                const int64_t a = negative ? -v : v;
                const int64_t whole = a / scale;
                const char   *sign = negative ? "-" : "";
                if (whole >= 100) {
                        std::snprintf(out, sizeof(out), "%s%" PRId64 " %s", sign, (a + scale / 2) / scale, suffix);
                } else if (whole >= 10) {
                        const int64_t tenths = (a * 10 + scale / 2) / scale;
                        std::snprintf(out, sizeof(out), "%s%" PRId64 ".%" PRId64 " %s", sign, tenths / 10, tenths % 10,
                                      suffix);
                } else {
                        const int64_t hundredths = (a * 100 + scale / 2) / scale;
                        std::snprintf(out, sizeof(out), "%s%" PRId64 ".%02" PRId64 " %s", sign, hundredths / 100,
                                      hundredths % 100, suffix);
                }
                return;
        }

        int64_t magnitude(int64_t v) {
                return v < 0 ? -v : v;
        }

} // namespace

Error Monitor::relist() {
        _entries.clear();
        _ids.clear();
        for (size_t i = 0;; ++i) {
                Entry e;
                if (_source.counter(i, e.counter).isError()) break;
                if (_entries.append(e).isError() || _ids.append(e.counter.id).isError()) return Error::NoMem;
        }
        if (_values.reserve(_ids.size()).isError() || _health.reserve(_ids.size()).isError()) return Error::NoMem;
        _values.clear();
        _health.clear();
        for (size_t i = 0; i < _ids.size(); ++i) {
                (void)_values.append(Stat::Unavailable); // reserved
                (void)_health.append(Source::Unjudged);
        }
        _lastNs = 0; // no rates across a new catalogue
        return Error::Ok;
}

Error Monitor::refresh() {
        _status = _source.status();
        if (!_listed || _status.generation != _generation) {
                // Listed until a listing sees one generation throughout (counters came or went meanwhile).
                for (int tries = 0; tries < 3; ++tries) {
                        const Error err = relist();
                        if (err.isError()) return err;
                        const uint32_t gen = _source.status().generation;
                        const bool     steady =
                                _entries.isEmpty() || (_entries[0].counter.generation == gen &&
                                                       _entries[_entries.size() - 1].counter.generation == gen);
                        _generation = gen;
                        if (steady) break;
                }
                _listed = true;
        }
        int64_t       now = 0;
        const size_t  n = _source.read(_ids.span(), _values.span(), _health.span(), now);
        const int64_t elapsed = _lastNs != 0 ? now - _lastNs : 0;
        for (size_t i = 0; i < n && i < _entries.size(); ++i) {
                Entry        &e = _entries[i];
                const int64_t v = _values[i];
                if (e.counter.kind == Stat::Kind::Total) {
                        // A value that fell is a restart: no rate this time.
                        const int64_t elapsedUs = elapsed / 1000;
                        e.rate = elapsedUs > 0 && v != Stat::Unavailable && e.value != Stat::Unavailable && v >= e.value
                                         ? (v - e.value) * 1000 / elapsedUs * 1000 +
                                                   (v - e.value) * 1000 % elapsedUs * 1000 / elapsedUs
                                         : Stat::Unavailable;
                }
                e.value = v;
                e.health = _health[i];
        }
        _lastNs = now;
        return Error::Ok;
}

Stat::Health Monitor::health(size_t i) const {
        const uint8_t h = _entries[i].health;
        return h <= uint8_t(Stat::Health::Critical) ? Stat::Health(h) : Stat::Health::Normal;
}

size_t Monitor::find(StringView name) const {
        for (size_t i = 0; i < _entries.size(); ++i)
                if (name == StringView(_entries[i].counter.name)) return i;
        return NotFound;
}

int64_t Monitor::valueOf(StringView name) const {
        const size_t i = find(name);
        return i == NotFound ? Stat::Unavailable : _entries[i].value;
}

int64_t Monitor::rateOf(StringView name) const {
        const size_t i = find(name);
        return i == NotFound ? Stat::Unavailable : _entries[i].rate;
}

Stat::Health Monitor::worst(StringView pattern) const {
        Stat::Health w = Stat::Health::Normal;
        for (size_t i = 0; i < _entries.size(); ++i) {
                const Stat::Health h = health(i);
                if (h > w && Stat::Overrides::matches(pattern, StringView(_entries[i].counter.name))) w = h;
        }
        return w;
}

const char *Monitor::format(char (&out)[TextBytes], Stat::Unit unit, int64_t value) {
        if (value == Stat::Unavailable) {
                std::snprintf(out, sizeof(out), "-");
                return out;
        }
        const int64_t m = magnitude(value);
        switch (unit) {
                case Stat::Unit::Bytes:
                        if (m < 1024)
                                std::snprintf(out, sizeof(out), "%" PRId64 " B", value);
                        else if (m < 1024 * 1024)
                                scaled(out, value, 1024, "KB");
                        else if (m < int64_t(1024) * 1024 * 1024)
                                scaled(out, value, 1024 * 1024, "MB");
                        else
                                scaled(out, value, int64_t(1024) * 1024 * 1024, "GB");
                        return out;
                case Stat::Unit::Micros:
                        if (m < 1000)
                                std::snprintf(out, sizeof(out), "%" PRId64 " µs", value);
                        else if (m < 1000000)
                                scaled(out, value, 1000, "ms");
                        else
                                scaled(out, value, 1000000, "s");
                        return out;
                case Stat::Unit::Nanos:
                        if (m < 1000)
                                std::snprintf(out, sizeof(out), "%" PRId64 " ns", value);
                        else if (m < 1000000)
                                scaled(out, value, 1000, "µs");
                        else if (m < 1000000000)
                                scaled(out, value, 1000000, "ms");
                        else
                                scaled(out, value, 1000000000, "s");
                        return out;
                case Stat::Unit::Permille:
                        std::snprintf(out, sizeof(out), "%s%" PRId64 ".%" PRId64 " %%", value < 0 ? "-" : "", m / 10,
                                      m % 10);
                        return out;
                case Stat::Unit::Hz:
                        if (m < 1000000)
                                std::snprintf(out, sizeof(out), "%" PRId64 " Hz", value);
                        else
                                scaled(out, value, 1000000, "MHz");
                        return out;
                case Stat::Unit::MilliHz:
                        std::snprintf(out, sizeof(out), "%s%" PRId64 ".%03" PRId64 " Hz", value < 0 ? "-" : "",
                                      m / 1000, m % 1000);
                        return out;
                case Stat::Unit::Ppm: std::snprintf(out, sizeof(out), "%" PRId64 " ppm", value); return out;
                case Stat::Unit::Ppb: std::snprintf(out, sizeof(out), "%" PRId64 " ppb", value); return out;
                case Stat::Unit::Frames: std::snprintf(out, sizeof(out), "%" PRId64 " frames", value); return out;
                case Stat::Unit::Cycles: std::snprintf(out, sizeof(out), "%" PRId64 " cycles", value); return out;
                case Stat::Unit::Decibels: std::snprintf(out, sizeof(out), "%" PRId64 " dB", value); return out;
                case Stat::Unit::Count: break;
        }
        std::snprintf(out, sizeof(out), "%" PRId64, value);
        return out;
}

const char *Monitor::formatValue(char (&out)[TextBytes], size_t i) const {
        const Entry &e = _entries[i];
        if (e.counter.kind == Stat::Kind::State && e.value != Stat::Unavailable) {
                const StringView label = Stat::label(e.counter.labels, e.value);
                if (!label.isEmpty()) {
                        std::snprintf(out, sizeof(out), "%.*s", int(label.size()), label.data());
                        return out;
                }
        }
        return format(out, e.counter.unit, e.value);
}

const char *Monitor::formatRate(char (&out)[TextBytes], size_t i) const {
        const Entry &e = _entries[i];
        out[0] = '\0';
        if (e.counter.kind != Stat::Kind::Total || e.rate == Stat::Unavailable) return out;
        format(out, e.counter.unit, e.rate);
        const size_t n = std::strlen(out);
        if (n + 3 <= sizeof(out)) std::memcpy(out + n, "/s", 3);
        return out;
}

BMDS_NAMESPACE_END
