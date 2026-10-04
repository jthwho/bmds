/**
 * @file      datetime.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/datetime.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *WeekdayNames[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                                "Thursday", "Friday", "Saturday"};
        constexpr const char *MonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                              "July",    "August",   "September", "October", "November", "December"};

        int64_t floorDiv(int64_t a, int64_t b) {
                const int64_t q = a / b;
                return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
        }

        // Appends @p value in decimal, at least @p width digits, padded with @p pad.
        Error appendNumber(String &out, int64_t value, int width, char pad) {
                char       buf[24];
                int        n = 0;
                const bool negative = value < 0;
                uint64_t   v = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
                do {
                        buf[n++] = static_cast<char>('0' + v % 10);
                        v /= 10;
                } while (v != 0);
                while (n < width) buf[n++] = pad;
                if (negative) buf[n++] = '-';
                char text[24];
                for (int i = 0; i < n; ++i) text[i] = buf[n - 1 - i];
                return out.append(StringView(text, static_cast<size_t>(n)));
        }

        Error appendName(String &out, const char *name, bool abbreviated) {
                const StringView view(name);
                return out.append(abbreviated ? view.left(3) : view);
        }

        Error appendOffset(String &out, int32_t offset) {
                Error err = out.append(offset < 0 ? '-' : '+');
                if (offset < 0) offset = -offset;
                if (err.isOk()) err = appendNumber(out, offset / 3600, 2, '0');
                if (err.isOk()) err = appendNumber(out, offset / 60 % 60, 2, '0');
                return err;
        }

        Error formatOne(String &out, char conversion, const LocalTime &t) {
                const DateTime &d = t.local;
                const int       hour12 = d.hour % 12 == 0 ? 12 : d.hour % 12;
                switch (conversion) {
                        case 'a': return appendName(out, WeekdayNames[d.weekday % 7], true);
                        case 'A': return appendName(out, WeekdayNames[d.weekday % 7], false);
                        case 'b':
                        case 'h': return appendName(out, MonthNames[(d.month + 11) % 12], true);
                        case 'B': return appendName(out, MonthNames[(d.month + 11) % 12], false);
                        case 'c': return formatTime(out, "%a %b %e %H:%M:%S %Y", t);
                        case 'C': return appendNumber(out, floorDiv(d.year, 100), 2, '0');
                        case 'd': return appendNumber(out, d.day, 2, '0');
                        case 'D': return formatTime(out, "%m/%d/%y", t);
                        case 'e': return appendNumber(out, d.day, 2, ' ');
                        case 'F': return formatTime(out, "%Y-%m-%d", t);
                        case 'H': return appendNumber(out, d.hour, 2, '0');
                        case 'I': return appendNumber(out, hour12, 2, '0');
                        case 'j': return appendNumber(out, d.yearDay + 1, 3, '0');
                        case 'k': return appendNumber(out, d.hour, 2, ' ');
                        case 'l': return appendNumber(out, hour12, 2, ' ');
                        case 'm': return appendNumber(out, d.month, 2, '0');
                        case 'M': return appendNumber(out, d.minute, 2, '0');
                        case 'n': return out.append('\n');
                        case 'N': return appendNumber(out, d.nanosecond, 9, '0');
                        case 'p': return out.append(d.hour < 12 ? "AM" : "PM");
                        case 'r': return formatTime(out, "%I:%M:%S %p", t);
                        case 'R': return formatTime(out, "%H:%M", t);
                        case 's': return appendNumber(out, t.unixSeconds, 1, '0');
                        case 'S': return appendNumber(out, d.second, 2, '0');
                        case 't': return out.append('\t');
                        case 'T': return formatTime(out, "%H:%M:%S", t);
                        case 'u': return appendNumber(out, d.weekday == 0 ? 7 : d.weekday, 1, '0');
                        case 'w': return appendNumber(out, d.weekday, 1, '0');
                        case 'y':
                                return appendNumber(out, d.year % 100 < 0 ? d.year % 100 + 100 : d.year % 100, 2, '0');
                        case 'Y': return appendNumber(out, d.year, 1, '0');
                        case 'z': return appendOffset(out, t.utcOffset);
                        case 'Z': return out.append(StringView(t.abbreviation));
                        case '%': return out.append('%');
                        default: {
                                const char text[] = {'%', conversion};
                                return out.append(StringView(text, 2));
                        }
                }
        }

} // namespace

DateTime DateTime::fromSeconds(int64_t seconds, int32_t nanosecond) {
        const int64_t days = floorDiv(seconds, Calendar::SecondsPerDay);
        const int64_t secondOfDay = seconds - days * Calendar::SecondsPerDay;

        // Inverse of Calendar::daysFromCivil (Howard Hinnant).
        const int64_t  z = days + 719468;
        const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned dayOfMonth = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        const int64_t  year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2);

        DateTime dt;
        dt.year = static_cast<int32_t>(year);
        dt.month = static_cast<uint8_t>(month);
        dt.day = static_cast<uint8_t>(dayOfMonth);
        dt.hour = static_cast<uint8_t>(secondOfDay / 3600);
        dt.minute = static_cast<uint8_t>(secondOfDay / 60 % 60);
        dt.second = static_cast<uint8_t>(secondOfDay % 60);
        dt.weekday = static_cast<uint8_t>(Calendar::weekday(days));
        dt.yearDay = static_cast<uint16_t>(days - Calendar::daysFromCivil(year, 1, 1));
        dt.nanosecond = nanosecond;
        return dt;
}

int64_t DateTime::toSeconds() const {
        return Calendar::daysFromCivil(year, month, day) * Calendar::SecondsPerDay + hour * 3600 + minute * 60 + second;
}

bool DateTime::isValid() const {
        return month >= 1 && month <= 12 && day >= 1 && day <= Calendar::daysInMonth(year, month) && hour < 24 &&
               minute < 60 && second < 60 && nanosecond >= 0 && nanosecond < Timestamp::NsPerSecond;
}

Error formatTime(String &out, StringView format, const LocalTime &time) {
        size_t literal = 0; // Start of the pending run of plain text.
        for (size_t i = 0; i < format.size(); ++i) {
                if (format[i] != '%' || i + 1 == format.size()) continue;
                Error err = out.append(format.mid(literal, i - literal));
                if (err.isOk()) err = formatOne(out, format[i + 1], time);
                if (err.isError()) return err;
                ++i;
                literal = i + 1;
        }
        return out.append(format.mid(literal));
}

BMDS_NAMESPACE_END
