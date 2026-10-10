/**
 * @file      stat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system monitor's counters, pure (devplan/sysmon.md §3.2-§3.4).
 */

#include <cstring>
#include <bmds/stat.h>

BMDS_NAMESPACE_BEGIN

namespace Stat {

        namespace {

                Health worse(Health a, Health b) {
                        return uint8_t(a) > uint8_t(b) ? a : b;
                }

                uint16_t eventHold(const Limits &limits) {
                        return limits.hold != 0 ? limits.hold : DefaultEventHold;
                }

                // A Peak judged Above is an event: it rose past a level at some moment.
                bool isEventRule(const Limits &limits, Kind kind) {
                        return limits.rule == Rule::Zero || (limits.rule == Rule::Above && kind == Kind::Peak);
                }

                bool parseInt(StringView text, int64_t &out) {
                        if (text == StringView("none")) {
                                out = NoLevel;
                                return true;
                        }
                        size_t i = 0;
                        bool   negative = false;
                        if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
                        if (i == text.size()) return false;
                        uint64_t v = 0;
                        for (; i < text.size(); ++i) {
                                if (text[i] < '0' || text[i] > '9') return false;
                                const uint64_t next = v * 10 + uint64_t(text[i] - '0');
                                if (next < v || next > uint64_t(INT64_MAX)) return false;
                                v = next;
                        }
                        out = negative ? -int64_t(v) : int64_t(v);
                        return true;
                }

                bool parseRule(StringView text, Rule &out) {
                        static constexpr Rule Rules[] = {Rule::None,  Rule::Zero,  Rule::Above,
                                                         Rule::Below, Rule::Drift, Rule::State};
                        for (Rule r : Rules) {
                                if (text == StringView(ruleName(r))) {
                                        out = r;
                                        return true;
                                }
                        }
                        return false;
                }

                enum SetBits : uint8_t {
                        SetRule = 1u << 0,
                        SetHealth = 1u << 1,
                        SetHold = 1u << 2,
                        SetWarn = 1u << 3,
                        SetCrit = 1u << 4,
                };

        } // namespace

        const char *unitName(Unit unit) {
                switch (unit) {
                        case Unit::Count: return "";
                        case Unit::Bytes: return "bytes";
                        case Unit::Micros: return "us";
                        case Unit::Permille: return "permille";
                        case Unit::Hz: return "Hz";
                        case Unit::MilliHz: return "mHz";
                        case Unit::Ppm: return "ppm";
                        case Unit::Frames: return "frames";
                        case Unit::Cycles: return "cycles";
                        case Unit::Decibels: return "dB";
                        case Unit::Nanos: return "ns";
                        case Unit::Ppb: return "ppb";
                }
                return "?";
        }

        const char *kindName(Kind kind) {
                switch (kind) {
                        case Kind::Total: return "total";
                        case Kind::Level: return "level";
                        case Kind::Peak: return "peak";
                        case Kind::State: return "state";
                        case Kind::Info: return "info";
                }
                return "?";
        }

        const char *ruleName(Rule rule) {
                switch (rule) {
                        case Rule::None: return "none";
                        case Rule::Zero: return "zero";
                        case Rule::Above: return "above";
                        case Rule::Below: return "below";
                        case Rule::Drift: return "drift";
                        case Rule::State: return "state";
                }
                return "?";
        }

        const char *healthName(Health health) {
                switch (health) {
                        case Health::Normal: return "normal";
                        case Health::Warning: return "warning";
                        case Health::Critical: return "critical";
                }
                return "?";
        }

        StringView label(const char *labels, int64_t index) {
                if (labels == nullptr || index < 0) return {};
                StringView rest(labels);
                for (int64_t i = 0;; ++i) {
                        const size_t bar = rest.indexOf('|');
                        if (i == index) return bar == StringView::NPos ? rest : rest.left(bar);
                        if (bar == StringView::NPos) return {};
                        rest = rest.mid(bar + 1);
                }
        }

        // ---- Judge ---------------------------------------------------------------------------

        Health Judge::judgeLevel(const Limits &limits, int64_t value) const {
                if (value == Unavailable) return Health::Normal;
                switch (limits.rule) {
                        case Rule::Above:
                                if (limits.crit != NoLevel && value > limits.crit) return Health::Critical;
                                if (limits.warn != NoLevel && value > limits.warn) return Health::Warning;
                                return Health::Normal;
                        case Rule::Below:
                                if (limits.crit != NoLevel && value < limits.crit) return Health::Critical;
                                if (limits.warn != NoLevel && value < limits.warn) return Health::Warning;
                                return Health::Normal;
                        case Rule::Drift: {
                                const int64_t size = value < 0 ? -value : value;
                                if (limits.crit != NoLevel && size > limits.crit) return Health::Critical;
                                if (limits.warn != NoLevel && size > limits.warn) return Health::Warning;
                                return Health::Normal;
                        }
                        case Rule::State: {
                                if (value < 0 || value > 62) return Health::Normal;
                                const int64_t bit = int64_t(1) << value;
                                if (limits.crit != NoLevel && (limits.crit & bit) != 0) return Health::Critical;
                                if (limits.warn != NoLevel && (limits.warn & bit) != 0) return Health::Warning;
                                return Health::Normal;
                        }
                        default: return Health::Normal;
                }
        }

        Health Judge::update(const Limits &limits, Kind kind, int64_t value, int64_t nowMs) {
                if (value == Unavailable) { // not measured now: nothing to judge, and starts again after
                        *this = Judge();
                        return _health;
                }
                const bool    first = _lastMs == NoValue;
                const bool    timed = _lastMs >= 0;
                const int64_t previousMs = timed ? _lastMs : nowMs;
                _delta = 0;
                if (kind == Kind::Total || kind == Kind::Peak) {
                        if (!first) _delta = value < _last ? value : value - _last; // fell: reset, counted from 0
                }
                _rate = Unavailable;
                if (kind == Kind::Total && timed && nowMs > _lastMs) _rate = _delta * 1000 / (nowMs - _lastMs);
                _last = value;
                _lastMs = nowMs;

                if (isEventRule(limits, kind)) {
                        if (_delta > 0) {
                                const Health h = limits.rule == Rule::Zero ? limits.health : judgeLevel(limits, value);
                                if (h != Health::Normal) {
                                        // A new problem, or a worse one, within the hold.
                                        const bool held =
                                                _eventMs >= 0 && nowMs - _eventMs < int64_t(eventHold(limits)) * 1000;
                                        _eventHealth = held ? worse(_eventHealth, h) : h;
                                        if (!held) _eventStartMs = previousMs; // it rose since the last look
                                        _eventMs = nowMs;
                                }
                        }
                        const bool within = _eventMs >= 0 && nowMs - _eventMs < int64_t(eventHold(limits)) * 1000;
                        _health = within ? _eventHealth : Health::Normal;
                        return _health;
                }

                const Health candidate = judgeLevel(limits, kind == Kind::Total ? _rate : value);
                if (candidate == Health::Normal) {
                        _badSinceMs = -1;
                        _pending = Health::Normal;
                        _health = Health::Normal;
                        return _health;
                }
                if (_health != Health::Normal) {
                        _health = candidate; // already out of normal: follows at once
                        return _health;
                }
                if (_badSinceMs < 0) _badSinceMs = previousMs; // out since the interval began
                _pending = candidate;
                if (nowMs - _badSinceMs >= int64_t(limits.hold) * 1000) _health = candidate;
                return _health;
        }

        int64_t Judge::since() const {
                if (_health == Health::Normal) return -1;
                return _badSinceMs >= 0 ? _badSinceMs : _eventStartMs;
        }

        void Judge::pause() {
                const int64_t last = _last;
                const bool    had = _lastMs != NoValue;
                *this = Judge();
                if (had) {
                        _last = last;
                        _lastMs = NoTime;
                }
                return;
        }

        Health eventHealth(const Limits &limits, Kind kind, int64_t value) {
                if (value == Unavailable) return Health::Normal;
                if (limits.rule == Rule::Zero) return value > 0 ? limits.health : Health::Normal;
                if (limits.rule == Rule::Above && kind == Kind::Peak) {
                        if (limits.crit != NoLevel && value > limits.crit) return Health::Critical;
                        if (limits.warn != NoLevel && value > limits.warn) return Health::Warning;
                }
                return Health::Normal;
        }

        // ---- ProblemLog ----------------------------------------------------------------------

        ProblemLog::Episode *ProblemLog::openFor(uint32_t id) {
                for (Episode &e : _episodes)
                        if (e.id == id && e.isOpen()) return &e;
                return nullptr;
        }

        ProblemLog::Episode *ProblemLog::make() {
                if (_episodes.size() < Capacity) {
                        // All the room at once, so the episodes never move.
                        if (_episodes.capacity() < Capacity && _episodes.reserve(Capacity).isError()) return nullptr;
                        if (_episodes.append(Episode{}).isError()) return nullptr;
                        return &_episodes.back();
                }
                Episode *oldest = nullptr;
                for (Episode &e : _episodes) // the oldest closed one, else the oldest
                        if (!e.isOpen() && (oldest == nullptr || e.changed < oldest->changed)) oldest = &e;
                if (oldest == nullptr) {
                        for (Episode &e : _episodes)
                                if (oldest == nullptr || e.changed < oldest->changed) oldest = &e;
                }
                *oldest = Episode{};
                return oldest;
        }

        namespace {
                void setName(char (&out)[MaxName + 1], StringView name) {
                        const size_t n = name.size() < MaxName ? name.size() : MaxName;
                        std::memcpy(out, name.data(), n);
                        out[n] = '\0';
                        return;
                }
        } // namespace

        Error ProblemLog::note(uint32_t id, StringView name, Health before, Health after, int64_t value, uint64_t count,
                               int64_t nowMs, int64_t sinceMs) {
                if (after == Health::Normal) {
                        if (before != Health::Normal) close(id, nowMs);
                        return Error::Ok;
                }
                Episode *e = openFor(id);
                if (e == nullptr) {
                        e = make();
                        if (e == nullptr) return Error::NoMem;
                        e->id = id;
                        e->flags = Open;
                        e->startMs = sinceMs >= 0 && sinceMs <= nowMs ? sinceMs : nowMs;
                        e->firstValue = value;
                        setName(e->name, name);
                }
                e->health = worse(e->health, after);
                e->lastValue = value;
                e->endMs = nowMs;
                e->count += count;
                e->changed = ++_changes;
                return Error::Ok;
        }

        Error ProblemLog::noteEarlier(uint32_t id, StringView name, Health health, int64_t value, uint64_t count,
                                      int64_t nowMs) {
                Episode *e = make();
                if (e == nullptr) return Error::NoMem;
                e->id = id;
                e->flags = Earlier;
                e->health = health;
                e->startMs = nowMs;
                e->endMs = nowMs;
                e->firstValue = value;
                e->lastValue = value;
                e->count = count;
                setName(e->name, name);
                e->changed = ++_changes;
                return Error::Ok;
        }

        void ProblemLog::closeAll(int64_t nowMs) {
                for (Episode &e : _episodes) {
                        if (!e.isOpen()) continue;
                        e.flags = uint8_t(e.flags & ~Open);
                        e.endMs = nowMs;
                        e.changed = ++_changes;
                }
                return;
        }

        void ProblemLog::close(uint32_t id, int64_t nowMs) {
                Episode *e = openFor(id);
                if (e == nullptr) return;
                e->flags = uint8_t(e->flags & ~Open);
                e->endMs = nowMs;
                e->changed = ++_changes;
                return;
        }

        size_t ProblemLog::changedAfter(uint64_t after, Span<Episode> out) const {
                size_t   n = 0;
                uint64_t last = after;
                while (n < out.size()) {
                        const Episode *next = nullptr;
                        for (const Episode &e : _episodes)
                                if (e.changed > last && (next == nullptr || e.changed < next->changed)) next = &e;
                        if (next == nullptr) break;
                        out[n++] = *next;
                        last = next->changed;
                }
                return n;
        }

        Health ProblemLog::unseen() const {
                Health h = Health::Normal;
                for (const Episode &e : _episodes) // what happened before anyone watched is news, not alarm
                        if ((e.flags & Earlier) == 0 && (e.isOpen() || e.changed > _seen)) h = worse(h, e.health);
                return h;
        }

        void ProblemLog::clearClosed() {
                for (size_t i = _episodes.size(); i-- > 0;)
                        if (!_episodes[i].isOpen()) _episodes.removeAt(i);
                return;
        }

        // ---- Overrides -----------------------------------------------------------------------

        bool Overrides::matches(StringView pattern, StringView name) {
                size_t p = 0, n = 0;
                size_t star = StringView::NPos, mark = 0;
                while (n < name.size()) {
                        if (p < pattern.size() && pattern[p] == '*') {
                                star = p++;
                                mark = n;
                        } else if (p < pattern.size() && pattern[p] == name[n]) {
                                ++p;
                                ++n;
                        } else if (star != StringView::NPos) {
                                p = star + 1;
                                n = ++mark;
                        } else {
                                return false;
                        }
                }
                while (p < pattern.size() && pattern[p] == '*') ++p;
                return p == pattern.size();
        }

        bool Overrides::parseEntry(StringView text, Entry &out) {
                const size_t colon = text.indexOf(':');
                if (colon == StringView::NPos) return false;
                const StringView pattern = text.left(colon).trimmed();
                if (pattern.isEmpty() || pattern.size() > MaxName) return false;
                std::memcpy(out.pattern, pattern.data(), pattern.size());
                out.pattern[pattern.size()] = '\0';
                StringView rest = text.mid(colon + 1);
                if (rest.trimmed().isEmpty()) return false;
                while (!rest.isEmpty()) {
                        const size_t     comma = rest.indexOf(',');
                        const StringView item = (comma == StringView::NPos ? rest : rest.left(comma)).trimmed();
                        rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        const size_t eq = item.indexOf('=');
                        if (eq == StringView::NPos) {
                                if (item == StringView("off")) {
                                        out.rule = Rule::None;
                                        out.set |= SetRule;
                                } else if (item == StringView("warn") || item == StringView("crit")) {
                                        out.rule = Rule::Zero;
                                        out.health = item == StringView("warn") ? Health::Warning : Health::Critical;
                                        out.set |= SetRule | SetHealth;
                                } else {
                                        return false;
                                }
                                continue;
                        }
                        const StringView key = item.left(eq).trimmed();
                        const StringView value = item.mid(eq + 1).trimmed();
                        if (key == StringView("rule")) {
                                if (!parseRule(value, out.rule)) return false;
                                out.set |= SetRule;
                        } else if (key == StringView("warn")) {
                                if (!parseInt(value, out.warn)) return false;
                                out.set |= SetWarn;
                        } else if (key == StringView("crit")) {
                                if (!parseInt(value, out.crit)) return false;
                                out.set |= SetCrit;
                        } else if (key == StringView("hold")) {
                                int64_t s = 0;
                                if (!parseInt(value, s) || s < 0 || s > 65535) return false;
                                out.hold = uint16_t(s);
                                out.set |= SetHold;
                        } else {
                                return false;
                        }
                }
                return true;
        }

        Error Overrides::parse(StringView text, size_t *bad) {
                _entries.clear();
                size_t skipped = 0;
                while (!text.isEmpty()) {
                        const size_t     semi = text.indexOf(';');
                        const StringView item = (semi == StringView::NPos ? text : text.left(semi)).trimmed();
                        text = semi == StringView::NPos ? StringView() : text.mid(semi + 1);
                        if (item.isEmpty()) continue;
                        Entry e;
                        if (!parseEntry(item, e)) {
                                ++skipped;
                                continue;
                        }
                        if (_entries.append(e).isError()) return Error::NoMem;
                }
                if (bad != nullptr) *bad = skipped;
                return Error::Ok;
        }

        bool Overrides::apply(StringView name, Limits &limits) const {
                bool any = false;
                for (const Entry &e : _entries) {
                        if (!matches(StringView(e.pattern), name)) continue;
                        any = true;
                        if ((e.set & SetRule) != 0) limits.rule = e.rule;
                        if ((e.set & SetHealth) != 0) limits.health = e.health;
                        if ((e.set & SetHold) != 0) limits.hold = e.hold;
                        if ((e.set & SetWarn) != 0) limits.warn = e.warn;
                        if ((e.set & SetCrit) != 0) limits.crit = e.crit;
                }
                return any;
        }

        // ---- History -------------------------------------------------------------------------

        Error History::setup(size_t series, size_t depth) {
                release();
                if (series == 0 || depth == 0) return Error::Invalid;
                if (_samples.reserve(series * depth).isError()) return Error::NoMem;
                for (size_t i = 0; i < series * depth; ++i) (void)_samples.append(0);
                _series = series;
                _depth = depth;
                return Error::Ok;
        }

        void History::release() {
                _samples.clear();
                _series = _depth = _next = _count = 0;
                return;
        }

        void History::push(Span<const int32_t> samples) {
                if (_depth == 0) return;
                for (size_t s = 0; s < _series; ++s) _samples[s * _depth + _next] = s < samples.size() ? samples[s] : 0;
                _next = (_next + 1) % _depth;
                if (_count < _depth) ++_count;
                return;
        }

        size_t History::copy(size_t series, Span<int32_t> out) const {
                if (series >= _series) return 0;
                const size_t n = out.size() < _count ? out.size() : _count;
                const size_t start = (_next + _depth - n) % _depth; // the newest n
                for (size_t i = 0; i < n; ++i) out[i] = _samples[series * _depth + (start + i) % _depth];
                return n;
        }

} // namespace Stat

BMDS_NAMESPACE_END
