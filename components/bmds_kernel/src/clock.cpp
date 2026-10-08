/**
 * @file      clock.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/detail/platform.h>
#include <bmds/seqlocked.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The zone's rules, and its name when set by name: one value, so
        // they always agree.
        struct KernelZone {
                        TimeZone        rules;
                        Clock::ZoneName name;
        };

        constinit SeqLocked<ClockModel>            model;
        constinit SeqLocked<KernelZone>            zone;
        constinit std::atomic<Clock::ZoneResolver> resolver{nullptr};

        // A name as the database has them: no path, no ".".
        bool validName(StringView name) {
                return !name.isEmpty() && name.size() <= Clock::MaxZoneName && name[0] != '/' &&
                       name.indexOf('.') == StringView::NPos;
        }

        // Looks @p name up; what the zone becomes. NotExist: refused.
        Error resolve(StringView name, KernelZone &out) {
                out = KernelZone();
                std::memcpy(out.name.name, name.data(), name.size());
                const Clock::ZoneResolver r = resolver.load(std::memory_order_acquire);
                const Error               err = r != nullptr ? r(name, out.rules) : Error(Error::NotReady);
                if (err == Error::NotExist) {
                        out.name.pending = Clock::ZonePending::Unknown;
                        out.rules = TimeZone();
                } else if (err.isError()) {
                        out.name.pending = Clock::ZonePending::NoDatabase;
                        out.rules = TimeZone();
                }
                return err;
        }

} // namespace

int64_t Clock::monotonicNs() {
        return Detail::monotonicUs() * 1000;
}

Timestamp Clock::now() {
        return model.load().wallAt(monotonicNs());
}

bool Clock::isSet() {
        return model.load().source() != ClockSource::None;
}

ClockStatus Clock::status() {
        return model.load().status(monotonicNs());
}

void Clock::saveResetRecord() {
        const ClockModel m = model.load();
        if (m.source() == ClockSource::None) return;
        Detail::ClockRecord record;
        // Read the two time bases back to back, so the offset is tight.
        const int64_t persistentNs = Detail::persistentTimeUs() * 1000;
        record.wallMinusPersistentNs = m.wallAt(monotonicNs()).ns - persistentNs;
        record.frequencyPpb = m.frequencyPpb();
        Detail::saveClockRecord(record);
        return;
}

Error Clock::set(Timestamp wall, ClockSource source) {
        if (source == ClockSource::None) return Error::Invalid;
        model.update([&](ClockModel &m) { m.set(monotonicNs(), wall, source); });
        Detail::setLibcTime(wall.ns);
        saveResetRecord();
        return Error();
}

int64_t Clock::adjust(int64_t offsetNs) {
        const int64_t previous = model.update([&](ClockModel &m) { return m.adjust(monotonicNs(), offsetNs); });
        saveResetRecord();
        return previous;
}

Error Clock::setFrequency(int32_t ppb) {
        const Error err = model.update([&](ClockModel &m) { return m.setFrequency(monotonicNs(), ppb); });
        if (err.isOk()) saveResetRecord();
        return err;
}

namespace {

        // The zone @p tz names (rules, or a name: resolved, or pending), into @p next.
        Error prepareZone(StringView tz, KernelZone &next) {
                if (!TimeZone::isName(tz)) {
                        const Result<TimeZone> parsed = TimeZone::parse(tz);
                        if (parsed.error().isError()) return parsed.error();
                        next.rules = parsed.value();
                        return Error();
                }
                const StringView name = tz.startsWith(':') ? tz.mid(1) : tz;
                if (!validName(name)) return Error::Invalid;
                const Error err = resolve(name, next);
                return err == Error::NotExist ? err : Error();
        }

} // namespace

Error Clock::setTimeZone(StringView tz) {
        KernelZone  next;
        const Error err = prepareZone(tz, next);
        if (err.isOk()) zone.store(next);
        return err;
}

Error Clock::checkTimeZone(StringView tz) {
        KernelZone next;
        return prepareZone(tz, next);
}

TimeZone Clock::timeZone() {
        return zone.load().rules;
}

Clock::ZoneName Clock::zoneName() {
        return zone.load().name;
}

void Clock::setZoneResolver(ZoneResolver r) {
        resolver.store(r, std::memory_order_release);
        retryZoneName();
        return;
}

void Clock::retryZoneName() {
        const ZoneName pending = zone.load().name;
        if (pending.pending == ZonePending::None) return;
        KernelZone next;
        (void)resolve(pending.view(), next);
        // Only if it wasn't set again meanwhile.
        zone.update([&](KernelZone &z) {
                if (z.name.pending != ZonePending::None && z.name.view() == pending.view()) z = next;
        });
        return;
}

LocalTime Clock::localTime(Timestamp time) {
        return zone.load().rules.localTime(time);
}

BMDS_NAMESPACE_END
