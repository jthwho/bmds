/**
 * @file      glob.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/glob.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Matches @p c against the bracket expression at @p p[i] ('[').
        // Returns false if the expression isn't terminated (then '[' is literal);
        // otherwise sets @p end past ']' and @p matched.
        bool bracket(StringView p, size_t i, char c, size_t &end, bool &matched) {
                size_t j = i + 1;
                bool   negate = false;
                if (j < p.size() && (p[j] == '!' || p[j] == '^')) {
                        negate = true;
                        ++j;
                }
                bool found = false;
                bool first = true;
                while (j < p.size() && (first || p[j] != ']')) {
                        first = false;
                        char lo = p[j];
                        if (lo == '\\' && j + 1 < p.size()) lo = p[++j];
                        if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
                                char hi = p[j + 2];
                                j += 2;
                                if (hi == '\\' && j + 1 < p.size()) hi = p[++j];
                                if (c >= lo && c <= hi) found = true;
                        } else if (c == lo) {
                                found = true;
                        }
                        ++j;
                }
                if (j >= p.size()) return false;
                end = j + 1;
                matched = found != negate;
                return true;
        }

} // namespace

bool Glob::match(StringView p, StringView t) {
        size_t pi = 0, ti = 0;
        size_t starP = StringView::NPos, starT = 0;
        while (ti < t.size()) {
                if (pi < p.size()) {
                        const char c = p[pi];
                        if (c == '*') {
                                starP = ++pi;
                                starT = ti;
                                continue;
                        }
                        if (c == '?') {
                                ++pi;
                                ++ti;
                                continue;
                        }
                        size_t end = 0;
                        bool   matched = false;
                        if (c == '[' && bracket(p, pi, t[ti], end, matched)) {
                                if (matched) {
                                        pi = end;
                                        ++ti;
                                        continue;
                                }
                        } else {
                                const bool   escaped = c == '\\' && pi + 1 < p.size();
                                const char   want = escaped ? p[pi + 1] : c;
                                const size_t step = escaped ? 2 : 1;
                                if (want == t[ti]) {
                                        pi += step;
                                        ++ti;
                                        continue;
                                }
                        }
                }
                if (starP == StringView::NPos) return false;
                pi = starP;
                ti = ++starT;
        }
        while (pi < p.size() && p[pi] == '*') ++pi;
        return pi == p.size();
}

bool Glob::hasMagic(StringView p) {
        for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\') {
                        ++i;
                } else if (p[i] == '*' || p[i] == '?' || p[i] == '[') {
                        return true;
                }
        }
        return false;
}

size_t Glob::matchPrefix(StringView p, StringView t, bool longest) {
        for (size_t k = 0; k <= t.size(); ++k) {
                const size_t n = longest ? t.size() - k : k;
                if (match(p, t.left(n))) return n;
        }
        return StringView::NPos;
}

size_t Glob::matchSuffix(StringView p, StringView t, bool longest) {
        for (size_t k = 0; k <= t.size(); ++k) {
                const size_t n = longest ? t.size() - k : k;
                if (match(p, t.right(n))) return n;
        }
        return StringView::NPos;
}

BMDS_NAMESPACE_END
