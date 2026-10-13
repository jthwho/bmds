/**
 * @file      leapseconds.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * SHA-1 (FIPS 180-4's vectors) and IANA's leap-second list (devplan/network.md §31e): the
 * real file kept in thirdparty/tzdata, its hash, a changed digit, the offset around leaps.
 */

#include <doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/leapseconds.h>
#include <bmds/sha1.h>

using bmds::Sha1;
using bmds::StringView;
namespace LeapSeconds = bmds::LeapSeconds;

namespace {

        std::string sha1(const std::string &message, size_t pieces = 1) {
                Sha1         h;
                const size_t step = message.empty() ? 1 : (message.size() + pieces - 1) / pieces;
                for (size_t at = 0; at < message.size(); at += step)
                        h.update(message.data() + at, std::min(step, message.size() - at));
                uint8_t out[Sha1::DigestBytes];
                h.final(out);
                std::string s;
                char        b[3];
                for (uint8_t v : out) {
                        std::snprintf(b, sizeof(b), "%02x", v);
                        s += b;
                }
                return s;
        }

        std::string kept() {
                std::ifstream     f(BMDS_TEST_LEAPSECONDS);
                std::stringstream s;
                s << f.rdbuf();
                return s.str();
        }

        int64_t day(int y, unsigned m, unsigned d) {
                return bmds::Calendar::daysFromCivil(y, m, d) * bmds::Calendar::SecondsPerDay;
        }

        LeapSeconds::Verdict parseText(const std::string &text, LeapSeconds::Table &t) {
                return LeapSeconds::parse(StringView(text.data(), text.size()), t);
        }

} // namespace

TEST_CASE("SHA-1: FIPS 180-4's vectors") {
        CHECK(sha1("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        CHECK(sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
        const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(sha1(two) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        CHECK(sha1(two, 7) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        CHECK(sha1(std::string(1'000'000, 'a'), 13) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("Leap seconds: the list kept in thirdparty/tzdata") {
        const std::string  text = kept();
        LeapSeconds::Table t;
        REQUIRE(parseText(text, t) == LeapSeconds::Verdict::Ok);
        CHECK(t.count == 28);
        CHECK(t.entries[0].at == day(1972, 1, 1));
        CHECK(t.entries[0].offset == 10);
        CHECK(t.entries[27].at == day(2017, 1, 1));
        CHECK(t.entries[27].offset == 37);
        CHECK(t.expires == day(2027, 6, 28));
        CHECK(t.updated > day(2026, 1, 1));

        CHECK(LeapSeconds::offsetAt(t, day(1971, 6, 1)) == 0);
        CHECK(LeapSeconds::offsetAt(t, day(2016, 12, 31) + 86399) == 36);
        CHECK(LeapSeconds::offsetAt(t, day(2017, 1, 1)) == 37);
        CHECK(LeapSeconds::offsetAt(t, day(2026, 10, 13)) == 37);
        const LeapSeconds::Entry *next = LeapSeconds::nextAfter(t, day(2016, 6, 1));
        REQUIRE(next != nullptr);
        CHECK(next->offset == 37);
        CHECK(LeapSeconds::nextAfter(t, day(2026, 10, 13)) == nullptr);
}

TEST_CASE("Leap seconds: changed, cut short, or not a list") {
        const std::string  text = kept();
        LeapSeconds::Table t;
        // A digit of the last offset changed: the hash catches it.
        std::string  changed = text;
        const size_t line = changed.find("\n3692217600");
        REQUIRE(line != std::string::npos);
        const size_t last = changed.find("37", line + 11);
        REQUIRE(last != std::string::npos);
        changed[last + 1] = '8';
        CHECK(parseText(changed, t) == LeapSeconds::Verdict::BadHash);
        // The expiry moved on.
        std::string  later = text;
        const size_t at = later.find("#@");
        REQUIRE(at != std::string::npos);
        later[later.find_first_of("0123456789", at) + 1] ^= 1;
        CHECK(parseText(later, t) == LeapSeconds::Verdict::BadHash);
        // Cut before its hash line.
        CHECK(parseText(text.substr(0, text.find("#h")), t) == LeapSeconds::Verdict::Malformed);
        CHECK(parseText("", t) == LeapSeconds::Verdict::Malformed);
        CHECK(parseText("hello\n", t) == LeapSeconds::Verdict::Malformed);
        CHECK(parseText("#$ 1\n#@ 2\n#h 1 2 3 4 5\n", t) == LeapSeconds::Verdict::Malformed);
        // Comments and blank lines anywhere, CR LF ends: the same list.
        std::string crlf;
        for (char c : text) {
                if (c == '\n')
                        crlf += "\r\n\r\n";
                else
                        crlf += c;
        }
        CHECK(parseText(crlf, t) == LeapSeconds::Verdict::Ok);
}

TEST_CASE("Leap seconds: a small list made here") {
        // Two changes; the hash of "3900000000" "3950000000" "2272060800" "10" "2287785600" "11".
        LeapSeconds::Table t;
        Sha1               h;
        const char        *digits = "390000000039500000002272060800102287785600"
                                    "11";
        h.update(digits, std::string(digits).size());
        uint8_t d[Sha1::DigestBytes];
        h.final(d);
        char words[64];
        std::snprintf(words, sizeof(words), "%x %x %x %x %x", unsigned(d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]),
                      unsigned(d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]),
                      unsigned(d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11]),
                      unsigned(d[12] << 24 | d[13] << 16 | d[14] << 8 | d[15]),
                      unsigned(d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19]));
        const std::string list =
                std::string("# made here\n#$\t3900000000\n#@\t3950000000\n2272060800\t10\t# 1 Jan 1972\n"
                            "2287785600\t11\n#h\t") +
                words + "\n";
        REQUIRE(parseText(list, t) == LeapSeconds::Verdict::Ok);
        CHECK(t.count == 2);
        // Out of order: refused.
        const std::string swapped =
                std::string("#$ 3900000000\n#@ 3950000000\n2287785600 11\n2272060800 10\n#h ") + words + "\n";
        CHECK(parseText(swapped, t) == LeapSeconds::Verdict::Malformed);
}

TEST_CASE("Leap seconds: the kernel's TAI offset") {
        const std::string  text = kept();
        LeapSeconds::Table t;
        REQUIRE(parseText(text, t) == LeapSeconds::Verdict::Ok);
        bmds::Clock::setLeapSeconds(&t);
        bmds::Clock::TaiOffset o = bmds::Clock::taiOffset();
        CHECK(o.haveFile);
        CHECK(o.source == bmds::Clock::TaiSource::File);
        CHECK(o.seconds == 37);
        CHECK(o.fileSeconds == 37);
        CHECK(o.expiresNs == day(2027, 6, 28) * 1'000'000'000);
        CHECK_FALSE(o.leap61);
        // A followed master's valid offset wins; the file's is still told.
        bmds::Clock::setPtpUtcOffset(36, true);
        o = bmds::Clock::taiOffset();
        CHECK(o.source == bmds::Clock::TaiSource::Ptp);
        CHECK(o.seconds == 36);
        CHECK(o.fileSeconds == 37);
        bmds::Clock::setPtpUtcOffset(0, false);
        CHECK(bmds::Clock::taiOffset().source == bmds::Clock::TaiSource::File);
        // None: CLOCK_TAI reads UTC.
        bmds::Clock::setLeapSeconds(nullptr);
        o = bmds::Clock::taiOffset();
        CHECK(o.source == bmds::Clock::TaiSource::None);
        CHECK(o.seconds == 0);
}
