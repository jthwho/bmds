/**
 * @file      namefilter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A file dialog's "Files of type" patterns (devplan/mediaplayer.md §6d).
 */

#include <bmds/namefilter.h>

BMDS_NAMESPACE_BEGIN

namespace {

        char folded(char c) {
                return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        // The next pattern of @p patterns from @p at (moved past it and its `;`), trimmed.
        StringView nextPattern(StringView patterns, size_t &at) {
                const size_t start = at;
                while (at < patterns.size() && patterns[at] != ';') ++at;
                const StringView one = patterns.mid(start, at - start).trimmed();
                if (at < patterns.size()) ++at;
                return one;
        }

} // namespace

namespace NameFilter {

        // Greedy with one backtrack point, the last `*`: linear in practice, never exponential.
        bool matchesOne(StringView pattern, StringView name) {
                size_t p = 0, n = 0;
                size_t star = StringView::NPos, resume = 0;
                while (n < name.size()) {
                        if (p < pattern.size() && pattern[p] == '*') {
                                star = p++;
                                resume = n;
                        } else if (p < pattern.size() && (pattern[p] == '?' || folded(pattern[p]) == folded(name[n]))) {
                                ++p;
                                ++n;
                        } else if (star != StringView::NPos) {
                                p = star + 1;
                                n = ++resume;
                        } else {
                                return false;
                        }
                }
                while (p < pattern.size() && pattern[p] == '*') ++p;
                return p == pattern.size();
        }

        bool matches(StringView patterns, StringView name) {
                size_t at = 0;
                bool   any = false;
                while (at < patterns.size()) {
                        const StringView one = nextPattern(patterns, at);
                        if (one.isEmpty()) continue;
                        any = true;
                        if (matchesOne(one, name)) return true;
                }
                return !any;
        }

        bool hasWildcard(StringView text) {
                return text.contains('*') || text.contains('?');
        }

        StringView firstExtension(StringView patterns) {
                size_t at = 0;
                while (at < patterns.size()) {
                        const StringView one = nextPattern(patterns, at);
                        if (one.isEmpty()) continue;
                        if (one.size() < 3 || !one.startsWith("*.")) return StringView();
                        const StringView ext = one.mid(2);
                        return hasWildcard(ext) || ext.contains('.') ? StringView() : ext;
                }
                return StringView();
        }

} // namespace NameFilter

BMDS_NAMESPACE_END
