/**
 * @file      logtext.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system log's text forms (devplan/logging.md): syslog messages as musl's
 * syslog() sends them, and the kernel's uptime stamps.
 */

#include <doctest.h>

#include <string>
#include <bmds/logtext.h>

using bmds::StringView;
namespace LogText = bmds::LogText;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

} // namespace

TEST_CASE("LogText: musl's syslog() message") {
        const auto m = LogText::parseSyslog("<38>Oct 11 16:47:16 dropbear[21]: Pubkey auth succeeded\n");
        CHECK(m.hasPriority);
        CHECK(m.severity == 6);
        CHECK(str(m.tag) == "dropbear[21]");
        CHECK(str(m.text) == "Pubkey auth succeeded\n");
}

TEST_CASE("LogText: priorities, their severity, and bad ones as text") {
        CHECK(LogText::parseSyslog("<0>x").severity == 0);
        CHECK(LogText::parseSyslog("<191>x").severity == 7);
        CHECK(LogText::parseSyslog("<84>x: y").severity == 4);
        for (const char *bad : {"<192>x", "<>x", "<1234>x", "<3x", "<-1>x", "<3"}) {
                const auto m = LogText::parseSyslog(bad);
                CHECK_FALSE(m.hasPriority);
                CHECK(m.severity == LogText::DefaultSeverity);
                CHECK(str(m.text) == bad);
        }
}

TEST_CASE("LogText: the timestamp is skipped only when it is one") {
        CHECK(str(LogText::parseSyslog("<13>Jan  1 00:00:00 hello").text) == "hello");
        CHECK(str(LogText::parseSyslog("Dec 31 23:59:59 tag: hi").tag) == "tag");
        const auto notTime = LogText::parseSyslog("<13>Foo  1 00:00:00 hello");
        CHECK(notTime.tag.isEmpty());
        CHECK(str(notTime.text) == "Foo  1 00:00:00 hello");
        CHECK(str(LogText::parseSyslog("<13>Jan  1 00:00:0x hello").text) == "Jan  1 00:00:0x hello");
}

TEST_CASE("LogText: tags") {
        CHECK(str(LogText::parseSyslog("sshd: x").tag) == "sshd");
        CHECK(str(LogText::parseSyslog("sshd: x").text) == "x");
        CHECK(str(LogText::parseSyslog("sshd:x").text) == "x");
        CHECK(str(LogText::parseSyslog("sshd:").text) == "");
        CHECK(LogText::parseSyslog("two words: x").tag.isEmpty());
        CHECK(LogText::parseSyslog(": x").tag.isEmpty());
        CHECK(LogText::parseSyslog("no colon here").tag.isEmpty());
        const std::string longTag(LogText::MaxTag + 1, 'a');
        CHECK(LogText::parseSyslog((longTag + ": x").c_str()).tag.isEmpty());
        const std::string maxTag(LogText::MaxTag, 'a');
        CHECK(LogText::parseSyslog((maxTag + ": x").c_str()).tag.size() == LogText::MaxTag);
        CHECK(LogText::parseSyslog("").text.isEmpty());
}

TEST_CASE("LogText: uptime stamps") {
        uint64_t ms = 0;
        size_t   n = 0;
        CHECK(LogText::parseStamp("[    0.581] I BMDS init", ms, n));
        CHECK(ms == 581);
        CHECK(n == 12);
        CHECK(LogText::parseStamp("[123456.007] W x", ms, n));
        CHECK(ms == 123456007);
        CHECK(n == 13);
        for (const char *bad : {"", "[", "[ 1.00] x", "[ 1.000]x", "  1.000] x", "[ .000] x", "[ 1,000] x"})
                CHECK_FALSE(LogText::parseStamp(bad, ms, n));
}

TEST_CASE("LogText: lines and their cleaning") {
        StringView rest("one\r\ntwo\n\nthree");
        CHECK(str(LogText::nextLine(rest)) == "one");
        CHECK(str(LogText::nextLine(rest)) == "two");
        CHECK(str(LogText::nextLine(rest)) == "");
        CHECK(str(LogText::nextLine(rest)) == "three");
        CHECK(rest.isEmpty());
        char         out[16];
        const char   in[] = "a\tb\x1b[1m\x7f\xc3\xa9";
        const size_t n = LogText::cleanLine(StringView(in, sizeof(in) - 1), out, sizeof(out));
        CHECK(std::string(out, n) == "a b?[1m?\xc3\xa9");
        CHECK(LogText::cleanLine("abcdef", out, 3) == 3);
}

TEST_CASE("LogText: logger's priorities") {
        uint8_t sev = 99;
        CHECK(LogText::parsePriority("user.notice", sev));
        CHECK(sev == 5);
        CHECK(LogText::parsePriority("err", sev));
        CHECK(sev == 3);
        CHECK(LogText::parsePriority("local7.debug", sev));
        CHECK(sev == 7);
        CHECK(LogText::parsePriority("daemon.warn", sev));
        CHECK(sev == 4);
        CHECK(LogText::parsePriority("12", sev));
        CHECK(sev == 4);
        CHECK(LogText::parsePriority("191", sev));
        CHECK(sev == 7);
        for (const char *bad : {"", "192", "1x", "bogus", "user.", ".info", "nope.info", "user.nope", "user.info.x"})
                CHECK_FALSE(LogText::parsePriority(bad, sev));
}
