/**
 * @file      logtext.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/logtext.h>

BMDS_NAMESPACE_BEGIN

namespace LogText {

        namespace {

                bool isDigit(char c) {
                        return c >= '0' && c <= '9';
                }

                // `<PRI>`: its length, or 0.
                size_t readPriority(StringView s, uint8_t &severity) {
                        if (s.size() < 3 || s[0] != '<') return 0;
                        unsigned value = 0;
                        size_t   i = 1;
                        while (i < s.size() && i <= 3 && isDigit(s[i])) value = value * 10 + unsigned(s[i++] - '0');
                        if (i == 1 || i >= s.size() || s[i] != '>' || value > 191) return 0;
                        severity = static_cast<uint8_t>(value & 7);
                        return i + 1;
                }

                // `Mmm dd hh:mm:ss `: its length, or 0.
                size_t readTimestamp(StringView s) {
                        constexpr size_t Length = 16;
                        if (s.size() < Length) return 0;
                        static constexpr const char Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
                        bool                        month = false;
                        for (size_t m = 0; m < 12 && !month; ++m)
                                month = s[0] == Months[m * 3] && s[1] == Months[m * 3 + 1] && s[2] == Months[m * 3 + 2];
                        if (!month || s[3] != ' ' || !(s[4] == ' ' || isDigit(s[4])) || !isDigit(s[5]) || s[6] != ' ')
                                return 0;
                        for (size_t i : {7u, 8u, 10u, 11u, 13u, 14u})
                                if (!isDigit(s[i])) return 0;
                        if (s[9] != ':' || s[12] != ':' || s[15] != ' ') return 0;
                        return Length;
                }

                // `tag:` and the one space after it: its length (the tag's in @p tagBytes), or 0.
                size_t readTag(StringView s, size_t &tagBytes) {
                        const size_t limit = s.size() < MaxTag + 1 ? s.size() : MaxTag + 1;
                        for (size_t i = 0; i < limit; ++i) {
                                const char c = s[i];
                                if (c == ':') {
                                        if (i == 0) return 0;
                                        tagBytes = i;
                                        return i + 1 < s.size() && s[i + 1] == ' ' ? i + 2 : i + 1;
                                }
                                if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f) return 0;
                        }
                        return 0;
                }

        } // namespace

        Syslog parseSyslog(StringView message) {
                Syslog     out;
                StringView rest = message;
                if (const size_t n = readPriority(rest, out.severity); n != 0) {
                        out.hasPriority = true;
                        rest = rest.mid(n);
                }
                if (const size_t n = readTimestamp(rest); n != 0) rest = rest.mid(n);
                size_t tagBytes = 0;
                if (const size_t n = readTag(rest, tagBytes); n != 0) {
                        out.tag = rest.left(tagBytes);
                        rest = rest.mid(n);
                }
                out.text = rest;
                return out;
        }

        bool parsePriority(StringView text, uint8_t &severity) {
                if (!text.isEmpty() && isDigit(text[0])) {
                        unsigned value = 0;
                        for (char c : text) {
                                if (!isDigit(c)) return false;
                                value = value * 10 + unsigned(c - '0');
                                if (value > 191) return false;
                        }
                        severity = static_cast<uint8_t>(value & 7);
                        return true;
                }
                StringView   level = text;
                const size_t dot = text.indexOf('.');
                if (dot != StringView::NPos) {
                        const StringView facility = text.left(dot);
                        level = text.mid(dot + 1);
                        static constexpr const char *Facilities[] = {
                                "kern",   "user",   "mail",   "daemon",   "auth",   "syslog", "lpr",
                                "news",   "uucp",   "cron",   "authpriv", "ftp",    "local0", "local1",
                                "local2", "local3", "local4", "local5",   "local6", "local7", "security"};
                        bool known = false;
                        for (const char *f : Facilities) known = known || facility == StringView(f);
                        if (!known) return false;
                }
                static constexpr struct {
                                const char *name;
                                uint8_t     severity;
                } Levels[] = {{"emerg", 0},   {"panic", 0}, {"alert", 1},  {"crit", 2}, {"err", 3},  {"error", 3},
                              {"warning", 4}, {"warn", 4},  {"notice", 5}, {"info", 6}, {"debug", 7}};
                for (const auto &l : Levels) {
                        if (level == StringView(l.name)) {
                                severity = l.severity;
                                return true;
                        }
                }
                return false;
        }

        StringView nextLine(StringView &rest) {
                const size_t end = rest.indexOf('\n');
                StringView   line = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (!line.isEmpty() && line[line.size() - 1] == '\r') line = line.left(line.size() - 1);
                return line;
        }

        size_t cleanLine(StringView line, char *out, size_t capacity) {
                const size_t n = line.size() < capacity ? line.size() : capacity;
                for (size_t i = 0; i < n; ++i) {
                        const unsigned char c = static_cast<unsigned char>(line[i]);
                        out[i] = c == '\t' ? ' ' : c < ' ' || c == 0x7f ? '?' : static_cast<char>(c);
                }
                return n;
        }

        bool parseStamp(StringView line, uint64_t &uptimeMs, size_t &stampBytes) {
                if (line.size() < 8 || line[0] != '[') return false;
                size_t i = 1;
                while (i < line.size() && line[i] == ' ') ++i;
                uint64_t seconds = 0;
                size_t   digits = 0;
                while (i < line.size() && isDigit(line[i]) && digits < 12) {
                        seconds = seconds * 10 + uint64_t(line[i++] - '0');
                        ++digits;
                }
                if (digits == 0 || i + 6 > line.size() || line[i] != '.') return false;
                uint64_t ms = 0;
                for (size_t k = 1; k <= 3; ++k) {
                        if (!isDigit(line[i + k])) return false;
                        ms = ms * 10 + uint64_t(line[i + k] - '0');
                }
                if (line[i + 4] != ']' || line[i + 5] != ' ') return false;
                uptimeMs = seconds * 1000 + ms;
                stampBytes = i + 6;
                return true;
        }

} // namespace LogText

BMDS_NAMESPACE_END
