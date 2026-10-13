/**
 * @file      leapseconds.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/leapseconds.h>
#include <bmds/sha1.h>

BMDS_NAMESPACE_BEGIN

namespace LeapSeconds {

        namespace {

                bool isSpace(char c) {
                        return c == ' ' || c == '\t' || c == '\r';
                }

                bool isDigit(char c) {
                        return c >= '0' && c <= '9';
                }

                // A run of digits at p (spaces first skipped), hashed as it is.
                bool number(const char *&p, const char *end, uint64_t &out, Sha1 *hash) {
                        while (p < end && isSpace(*p)) ++p;
                        const char *start = p;
                        out = 0;
                        while (p < end && isDigit(*p)) {
                                if (out > (UINT64_MAX - 9) / 10) return false;
                                out = out * 10 + uint64_t(*p - '0');
                                ++p;
                        }
                        if (p == start || p - start > 19) return false;
                        if (hash != nullptr) hash->update(start, size_t(p - start));
                        return true;
                }

                int hexDigit(char c) {
                        if (isDigit(c)) return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                }

                // `#h`'s five words: hex, as many digits as each needs.
                bool hashWords(const char *p, const char *end, uint32_t (&out)[5]) {
                        for (uint32_t &w : out) {
                                while (p < end && isSpace(*p)) ++p;
                                int digits = 0;
                                w = 0;
                                for (int d; p < end && (d = hexDigit(*p)) >= 0; ++p) {
                                        if (++digits > 8) return false;
                                        w = w << 4 | uint32_t(d);
                                }
                                if (digits == 0) return false;
                        }
                        while (p < end && isSpace(*p)) ++p;
                        return p == end;
                }

        } // namespace

        const char *verdictText(Verdict v) {
                switch (v) {
                        case Verdict::Ok: return "good";
                        case Verdict::Malformed: return "not a leap-second list";
                        case Verdict::BadHash: return "its hash doesn't match";
                        case Verdict::TooMany: return "more leap seconds than kept";
                }
                return "?";
        }

        Verdict parse(StringView text, Table &out) {
                out.count = 0;
                uint64_t    updated = 0, expires = 0;
                bool        haveUpdated = false, haveExpires = false, haveHash = false;
                uint32_t    words[5] = {};
                const char *p = text.data();
                const char *end = p + text.size();
                while (p < end) {
                        const char *eol = p;
                        while (eol < end && *eol != '\n') ++eol;
                        const char *q = p;
                        p = eol < end ? eol + 1 : eol;
                        while (q < eol && isSpace(*q)) ++q;
                        if (q == eol) continue;
                        if (*q == '#') {
                                if (eol - q < 2) continue;
                                const char  kind = q[1];
                                const char *r = q + 2;
                                if (kind == '$' || kind == '@') {
                                        uint64_t v;
                                        if (!number(r, eol, v, nullptr)) return Verdict::Malformed;
                                        (kind == '$' ? updated : expires) = v;
                                        (kind == '$' ? haveUpdated : haveExpires) = true;
                                } else if (kind == 'h') {
                                        if (!hashWords(r, eol, words)) return Verdict::Malformed;
                                        haveHash = true;
                                }
                                continue; // any other comment
                        }
                        uint64_t at, offset;
                        if (!number(q, eol, at, nullptr) || !number(q, eol, offset, nullptr)) return Verdict::Malformed;
                        while (q < eol && isSpace(*q)) ++q;
                        if (q != eol && *q != '#') return Verdict::Malformed;
                        if (at < uint64_t(NtpToUnix) || offset > 255) return Verdict::Malformed;
                        const int64_t since1970 = int64_t(at) - NtpToUnix;
                        if (out.count > 0 && since1970 <= out.entries[out.count - 1].at) return Verdict::Malformed;
                        if (out.count == MaxEntries) return Verdict::TooMany;
                        out.entries[out.count++] = {since1970, static_cast<int16_t>(offset)};
                }
                if (!haveUpdated || !haveExpires || !haveHash || out.count == 0) return Verdict::Malformed;
                if (updated < uint64_t(NtpToUnix) || expires < uint64_t(NtpToUnix)) return Verdict::Malformed;
                // SHA-1 over the update and the expiry (wherever they were), then the lines' numbers:
                // a second pass over the lines, cheaper than keeping their digits.
                Sha1 hash;
                char digits[24];
                for (uint64_t v : {updated, expires}) {
                        int n = 0;
                        do digits[n++] = static_cast<char>('0' + v % 10);
                        while ((v /= 10) != 0);
                        for (int i = 0; i < n / 2; ++i) {
                                const char t = digits[i];
                                digits[i] = digits[n - 1 - i];
                                digits[n - 1 - i] = t;
                        }
                        hash.update(digits, size_t(n));
                }
                p = text.data();
                while (p < end) {
                        const char *eol = p;
                        while (eol < end && *eol != '\n') ++eol;
                        const char *q = p;
                        p = eol < end ? eol + 1 : eol;
                        while (q < eol && isSpace(*q)) ++q;
                        if (q == eol || *q == '#') continue;
                        uint64_t v;
                        (void)number(q, eol, v, &hash);
                        (void)number(q, eol, v, &hash);
                }
                uint8_t digest[Sha1::DigestBytes];
                hash.final(digest);
                for (int i = 0; i < 5; ++i) {
                        const uint32_t w = uint32_t(digest[4 * i]) << 24 | uint32_t(digest[4 * i + 1]) << 16 |
                                           uint32_t(digest[4 * i + 2]) << 8 | digest[4 * i + 3];
                        if (w != words[i]) return Verdict::BadHash;
                }
                out.updated = int64_t(updated) - NtpToUnix;
                out.expires = int64_t(expires) - NtpToUnix;
                return Verdict::Ok;
        }

        int16_t offsetAt(const Table &table, int64_t unixSeconds) {
                int16_t offset = 0;
                for (size_t i = 0; i < table.count && table.entries[i].at <= unixSeconds; ++i)
                        offset = table.entries[i].offset;
                return offset;
        }

        const Entry *nextAfter(const Table &table, int64_t unixSeconds) {
                for (size_t i = 0; i < table.count; ++i)
                        if (table.entries[i].at > unixSeconds) return &table.entries[i];
                return nullptr;
        }

} // namespace LeapSeconds

BMDS_NAMESPACE_END
