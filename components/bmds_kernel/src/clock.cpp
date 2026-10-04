/**
 * @file      clock.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/clock.h>
#include <bmds/detail/platform.h>
#include <bmds/seqlocked.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit SeqLocked<ClockModel> model;
        constinit SeqLocked<TimeZone>   zone;

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

Error Clock::setTimeZone(StringView posixTz) {
        const Result<TimeZone> parsed = TimeZone::parse(posixTz);
        if (parsed.error().isError()) return parsed.error();
        zone.store(parsed.value());
        return Error();
}

TimeZone Clock::timeZone() {
        return zone.load();
}

LocalTime Clock::localTime(Timestamp time) {
        return zone.load().localTime(time);
}

BMDS_NAMESPACE_END
