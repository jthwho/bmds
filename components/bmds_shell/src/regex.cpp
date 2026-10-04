/**
 * @file      regex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/regex.h>

BMDS_NAMESPACE_BEGIN

namespace {

        char lower(char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        char upper(char c) {
                return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }

        bool isEscapable(char c) {
                StringView literal = ".*[]^$\\/-";
                return literal.contains(c);
        }

        // Index just past the bracket expression opening at @p i, or 0 if it
        // isn't terminated. A `]` first (after an optional `^`) is literal.
        size_t bracketEnd(StringView p, size_t i) {
                size_t j = i + 1;
                if (j < p.size() && p[j] == '^') ++j;
                if (j < p.size() && p[j] == ']') ++j;
                while (j < p.size() && p[j] != ']') ++j;
                return j < p.size() ? j + 1 : 0;
        }

        // Index just past the atom at @p i (the pattern was checked).
        size_t atomEnd(StringView p, size_t i) {
                if (p[i] == '\\') return i + 2;
                if (p[i] == '[') return bracketEnd(p, i);
                return i + 1;
        }

        bool inBracket(StringView p, size_t i, char c) {
                size_t j = i + 1;
                bool   negate = false;
                if (p[j] == '^') {
                        negate = true;
                        ++j;
                }
                const size_t end = bracketEnd(p, i) - 1; // the closing ']'
                bool         found = false;
                while (j < end) {
                        const char lo = p[j];
                        if (j + 2 < end && p[j + 1] == '-') {
                                if (c >= lo && c <= p[j + 2]) found = true;
                                j += 3;
                        } else {
                                if (c == lo) found = true;
                                ++j;
                        }
                }
                return found != negate;
        }

        bool atomMatches(StringView p, size_t i, char c, bool ignoreCase) {
                if (p[i] == '.') return true;
                if (p[i] == '[') {
                        if (!ignoreCase) return inBracket(p, i, c);
                        return inBracket(p, i, lower(c)) || inBracket(p, i, upper(c));
                }
                const char want = p[i] == '\\' ? p[i + 1] : p[i];
                return ignoreCase ? lower(want) == lower(c) : want == c;
        }

        bool matchHere(StringView p, size_t pi, StringView t, size_t ti, bool ignoreCase) {
                for (;;) {
                        if (pi == p.size()) return true;
                        if (p[pi] == '$' && pi + 1 == p.size()) return ti == t.size();
                        const size_t end = atomEnd(p, pi);
                        if (end < p.size() && p[end] == '*') {
                                // Greedy: take as many as match, then give back.
                                size_t k = ti;
                                while (k < t.size() && atomMatches(p, pi, t[k], ignoreCase)) ++k;
                                for (size_t j = k + 1; j-- > ti;) {
                                        if (matchHere(p, end + 1, t, j, ignoreCase)) return true;
                                }
                                return false;
                        }
                        if (ti == t.size() || !atomMatches(p, pi, t[ti], ignoreCase)) return false;
                        pi = end;
                        ++ti;
                }
        }

} // namespace

Error Regex::check(StringView p) {
        for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\') {
                        if (i + 1 == p.size()) return Error::Invalid;
                        if (!isEscapable(p[i + 1])) return Error::NotSupported;
                        ++i;
                } else if (p[i] == '[') {
                        const size_t end = bracketEnd(p, i);
                        if (end == 0) return Error::Invalid;
                        // [:class:], [=e=], [.coll.]
                        for (size_t j = i + 1; j + 1 < end; ++j) {
                                if (p[j] == '[' && (p[j + 1] == ':' || p[j + 1] == '=' || p[j + 1] == '.'))
                                        return Error::NotSupported;
                        }
                        i = end - 1;
                }
        }
        return Error::Ok;
}

bool Regex::search(StringView p, StringView t, bool ignoreCase) {
        if (!p.isEmpty() && p[0] == '^') return matchHere(p, 1, t, 0, ignoreCase);
        for (size_t ti = 0; ti <= t.size(); ++ti) {
                if (matchHere(p, 0, t, ti, ignoreCase)) return true;
        }
        return false;
}

BMDS_NAMESPACE_END
