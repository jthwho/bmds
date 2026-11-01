/**
 * @file      pkgversion.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdint>
#include <bmds/pkgversion.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        bool isAlpha(char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        // A version cut into its parts; the epoch's digits, or empty.
        struct Parts {
                        StringView epoch;
                        StringView upstream;
                        StringView revision;
        };

        Parts split(StringView v) {
                Parts        p;
                const size_t colon = v.indexOf(':');
                if (colon != StringView::NPos) {
                        p.epoch = v.left(colon);
                        v = v.mid(colon + 1);
                }
                const size_t dash = v.lastIndexOf('-');
                if (dash != StringView::NPos) {
                        p.upstream = v.left(dash);
                        p.revision = v.mid(dash + 1);
                } else {
                        p.upstream = v;
                }
                return p;
        }

        // dpkg's order of a non-digit character: the end and digits 0, letters themselves, `~`
        // below everything, anything else after the letters.
        int order(char c) {
                if (c == '\0' || isDigit(c)) return 0;
                if (isAlpha(c)) return c;
                if (c == '~') return -1;
                return int(uint8_t(c)) + 256;
        }

        // dpkg's verrevcmp(), over views: the character past the end is '\0'.
        int compareRuns(StringView a, StringView b) {
                size_t     i = 0;
                size_t     j = 0;
                const auto at = [](StringView s, size_t k) {
                        return k < s.size() ? s[k] : '\0';
                };
                while (i < a.size() || j < b.size()) {
                        while ((i < a.size() && !isDigit(a[i])) || (j < b.size() && !isDigit(b[j]))) {
                                const int ac = order(at(a, i));
                                const int bc = order(at(b, j));
                                if (ac != bc) return ac - bc;
                                ++i;
                                ++j;
                        }
                        while (at(a, i) == '0') ++i;
                        while (at(b, j) == '0') ++j;
                        int firstDiff = 0;
                        while (isDigit(at(a, i)) && isDigit(at(b, j))) {
                                if (firstDiff == 0) firstDiff = at(a, i) - at(b, j);
                                ++i;
                                ++j;
                        }
                        if (isDigit(at(a, i))) return 1;
                        if (isDigit(at(b, j))) return -1;
                        if (firstDiff != 0) return firstDiff;
                }
                return 0;
        }

        // An epoch's digits as a number, saturating (a valid epoch is short anyway).
        unsigned long epochOf(StringView digits) {
                unsigned long n = 0;
                for (char c : digits) {
                        if (n > 100000000UL) return n;
                        n = n * 10 + unsigned(c - '0');
                }
                return n;
        }

} // namespace

bool PkgVersion::isValid(StringView version) {
        if (version.isEmpty() || version.size() > MaxBytes) return false;
        const Parts p = split(version);
        if (version.indexOf(':') != StringView::NPos) {
                if (p.epoch.isEmpty() || p.epoch.size() > 8) return false;
                for (char c : p.epoch) {
                        if (!isDigit(c)) return false;
                }
        }
        if (p.upstream.isEmpty() || !isDigit(p.upstream[0])) return false;
        for (char c : p.upstream) {
                if (!isDigit(c) && !isAlpha(c) && c != '.' && c != '+' && c != '~' && c != '-') return false;
        }
        if (version.lastIndexOf('-') != StringView::NPos && p.revision.isEmpty()) return false;
        for (char c : p.revision) {
                if (!isDigit(c) && !isAlpha(c) && c != '.' && c != '+' && c != '~') return false;
        }
        return true;
}

int PkgVersion::compare(StringView a, StringView b) {
        const Parts         pa = split(a);
        const Parts         pb = split(b);
        const unsigned long ea = epochOf(pa.epoch);
        const unsigned long eb = epochOf(pb.epoch);
        if (ea != eb) return ea < eb ? -1 : 1;
        const int r = compareRuns(pa.upstream, pb.upstream);
        if (r != 0) return r;
        return compareRuns(pa.revision, pb.revision);
}

BMDS_NAMESPACE_END
