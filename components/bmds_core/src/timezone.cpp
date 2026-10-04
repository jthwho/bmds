/**
 * @file      timezone.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/timezone.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isAlpha(char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        // Reads an unsigned number of at most @p maxDigits digits.
        bool parseNumber(StringView &text, size_t maxDigits, int32_t &value) {
                size_t digits = 0;
                value = 0;
                while (digits < text.size() && digits < maxDigits && isDigit(text[digits])) {
                        value = value * 10 + (text[digits] - '0');
                        ++digits;
                }
                if (digits == 0) return false;
                text = text.mid(digits);
                return true;
        }

        bool consume(StringView &text, char c) {
                if (text.isEmpty() || text[0] != c) return false;
                text = text.mid(1);
                return true;
        }

        // `[+|-]hh[:mm[:ss]]`, hours up to @p maxHours; returns seconds.
        bool parseTime(StringView &text, int32_t maxHours, int32_t &seconds) {
                const bool negative = consume(text, '-');
                if (!negative) (void)consume(text, '+');
                int32_t hours = 0;
                int32_t minutes = 0;
                int32_t secs = 0;
                if (!parseNumber(text, 3, hours) || hours > maxHours) return false;
                if (consume(text, ':')) {
                        if (!parseNumber(text, 2, minutes) || minutes > 59) return false;
                        if (consume(text, ':') && (!parseNumber(text, 2, secs) || secs > 59)) return false;
                }
                seconds = hours * 3600 + minutes * 60 + secs;
                if (negative) seconds = -seconds;
                return true;
        }

        // A zone name: three or more letters, or `<...>`.
        bool parseName(StringView &text, char (&name)[LocalTime::MaxAbbreviation + 1]) {
                size_t end = 0;
                size_t next = 0;
                if (consume(text, '<')) {
                        while (end < text.size() && text[end] != '>') {
                                const char c = text[end];
                                if (!isAlpha(c) && !isDigit(c) && c != '+' && c != '-') return false;
                                ++end;
                        }
                        if (end == text.size()) return false;
                        next = end + 1;
                } else {
                        while (end < text.size() && isAlpha(text[end])) ++end;
                        next = end;
                }
                const size_t length = end;
                if (length < 3 || length > LocalTime::MaxAbbreviation) return false;
                std::memcpy(name, text.data(), length);
                name[length] = '\0';
                text = text.mid(next);
                return true;
        }

        void copyName(char (&to)[LocalTime::MaxAbbreviation + 1], const char *from) {
                std::strncpy(to, from, LocalTime::MaxAbbreviation);
                to[LocalTime::MaxAbbreviation] = '\0';
                return;
        }

        // POSIX allows rule times from -167 to 167 hours.
        constexpr int32_t MaxRuleHours = 167;
        constexpr int32_t MaxOffsetHours = 24;

} // namespace

bool TimeZone::parseRule(StringView &text, Rule &rule) {
        int32_t n = 0;
        if (consume(text, 'M')) {
                int32_t week = 0;
                int32_t weekday = 0;
                if (!parseNumber(text, 2, n) || n < 1 || n > 12 || !consume(text, '.') || !parseNumber(text, 1, week) ||
                    week < 1 || week > 5 || !consume(text, '.') || !parseNumber(text, 1, weekday) || weekday > 6)
                        return false;
                rule.kind = Rule::MonthWeekDay;
                rule.month = static_cast<uint8_t>(n);
                rule.week = static_cast<uint8_t>(week);
                rule.weekday = static_cast<uint8_t>(weekday);
        } else if (consume(text, 'J')) {
                if (!parseNumber(text, 3, n) || n < 1 || n > 365) return false;
                rule.kind = Rule::Julian1;
                rule.day = static_cast<uint16_t>(n);
        } else {
                if (!parseNumber(text, 3, n) || n > 365) return false;
                rule.kind = Rule::Julian0;
                rule.day = static_cast<uint16_t>(n);
        }
        rule.time = 7200;
        if (consume(text, '/') && !parseTime(text, MaxRuleHours, rule.time)) return false;
        return true;
}

Result<TimeZone> TimeZone::parse(StringView spec) {
        TimeZone zone;
        if (spec.size() > MaxSpec) return makeError<TimeZone>(Error::Invalid);
        std::memcpy(zone._spec, spec.data(), spec.size());
        zone._spec[spec.size()] = '\0';
        if (spec.isEmpty() || spec == "UTC" || spec == "GMT") {
                copyName(zone._stdName, spec.isEmpty() ? "UTC" : zone._spec);
                return makeResult(zone);
        }

        StringView text = spec;
        int32_t    west = 0;
        if (!parseName(text, zone._stdName) || !parseTime(text, MaxOffsetHours, west))
                return makeError<TimeZone>(Error::Invalid);
        zone._stdOffset = -west;
        if (text.isEmpty()) return makeResult(zone);

        if (!parseName(text, zone._dstName)) return makeError<TimeZone>(Error::Invalid);
        zone._hasDst = 1;
        zone._dstOffset = zone._stdOffset + 3600;
        if (!text.isEmpty() && text[0] != ',') {
                if (!parseTime(text, MaxOffsetHours, west)) return makeError<TimeZone>(Error::Invalid);
                zone._dstOffset = -west;
        }
        if (text.isEmpty()) {
                // US rules, as glibc assumes.
                zone._start = Rule{7200, 0, Rule::MonthWeekDay, 3, 2, 0, {}};
                zone._end = Rule{7200, 0, Rule::MonthWeekDay, 11, 1, 0, {}};
                return makeResult(zone);
        }
        if (!consume(text, ',') || !parseRule(text, zone._start) || !consume(text, ',') ||
            !parseRule(text, zone._end) || !text.isEmpty())
                return makeError<TimeZone>(Error::Invalid);
        return makeResult(zone);
}

int64_t TimeZone::transition(int64_t year, const Rule &rule, int32_t offsetBefore) {
        int64_t days = 0;
        switch (rule.kind) {
                case Rule::Julian1:
                        // Day 1-365; February 29 is never counted.
                        days = Calendar::daysFromCivil(year, 1, 1) + rule.day - 1 +
                               (Calendar::isLeapYear(year) && rule.day >= 60 ? 1 : 0);
                        break;
                case Rule::Julian0: days = Calendar::daysFromCivil(year, 1, 1) + rule.day; break;
                default: {
                        const int64_t  first = Calendar::daysFromCivil(year, rule.month, 1);
                        const unsigned firstWeekday = Calendar::weekday(first);
                        unsigned       day = 1 + (rule.weekday + 7 - firstWeekday) % 7 + (rule.week - 1u) * 7;
                        while (day > static_cast<unsigned>(Calendar::daysInMonth(year, rule.month))) day -= 7;
                        days = first + day - 1;
                        break;
                }
        }
        return days * Calendar::SecondsPerDay + rule.time - offsetBefore;
}

bool TimeZone::isDst(int64_t utcSeconds) const {
        if (_hasDst == 0) return false;
        const int64_t year = DateTime::fromSeconds(utcSeconds + _stdOffset).year;
        const int64_t start = transition(year, _start, _stdOffset);
        const int64_t end = transition(year, _end, _dstOffset);
        if (start < end) return utcSeconds >= start && utcSeconds < end;
        // Southern hemisphere: DST spans the new year.
        return utcSeconds < end || utcSeconds >= start;
}

LocalTime TimeZone::localTime(Timestamp time) const {
        LocalTime     t;
        const int64_t seconds = time.seconds();
        const bool    dst = isDst(seconds);
        t.unixSeconds = seconds;
        t.utcOffset = dst ? _dstOffset : _stdOffset;
        t.isDst = dst ? 1 : 0;
        t.local = DateTime::fromSeconds(seconds + t.utcOffset, time.nanoseconds());
        copyName(t.abbreviation, dst ? _dstName : (_stdName[0] != '\0' ? _stdName : "UTC"));
        return t;
}

int64_t TimeZone::toUtc(int64_t localSeconds) const {
        const int64_t asStandard = localSeconds - _stdOffset;
        if (_hasDst == 0) return asStandard;
        const int64_t asDst = localSeconds - _dstOffset;
        const bool    standardFits = !isDst(asStandard);
        const bool    dstFits = isDst(asDst);
        if (standardFits && dstFits) return asStandard < asDst ? asStandard : asDst;
        if (dstFits) return asDst;
        return asStandard;
}

BMDS_NAMESPACE_END
