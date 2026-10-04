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
                size_t end = 0;
                bool   matched = false;
                if (p[i] == '\\') {
                        ++i;
                } else if (p[i] == '*' || p[i] == '?' || (p[i] == '[' && bracket(p, i, '\0', end, matched))) {
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

namespace {

        // The pattern's text with its escapes removed.
        Error unescape(StringView p, String &out) {
                for (size_t i = 0; i < p.size(); ++i) {
                        if (p[i] == '\\' && i + 1 < p.size()) ++i;
                        Error err = out.append(p[i]);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // Expands @p rest (components separated by '/') below @p prefix (empty,
        // or ending in '/'), appending to @p out.
        Error walk(String &prefix, StringView rest, const Vfs::Context &context, List<String> &out) {
                const size_t     slash = rest.indexOf('/');
                const StringView component = slash == StringView::NPos ? rest : rest.left(slash);
                StringView       after = slash == StringView::NPos ? StringView() : rest.mid(slash + 1);
                while (after.startsWith('/')) after = after.mid(1);
                const bool   last = slash == StringView::NPos;
                const bool   dirOnly = !last && after.isEmpty(); // a trailing '/'
                const size_t base = prefix.size();
                Error        err;
                if (!Glob::hasMagic(component)) {
                        err = unescape(component, prefix);
                        if (err.isOk() && (last || dirOnly)) {
                                const Result<FileInfo> info = Vfs::stat(prefix, context);
                                if (info.error().isOk() && (last || info.value().isDirectory())) {
                                        if (dirOnly) err = prefix.append('/');
                                        auto [copy, cerr] = prefix.clone();
                                        err = err.isOk() ? cerr : err;
                                        if (err.isOk()) err = out.append(std::move(copy));
                                }
                        } else if (err.isOk()) {
                                err = prefix.append('/');
                                if (err.isOk()) err = walk(prefix, after, context, out);
                        }
                        prefix.truncate(base);
                        return err;
                }
                const bool dotOk = component.startsWith('.') || component.startsWith("\\.");
                // The matching subdirectories are collected first, so that only one
                // directory is open at a time.
                List<String> subdirs;
                {
                        auto [dir, derr] =
                                Vfs::openDirectory(prefix.isEmpty() ? StringView(".") : prefix.view(), context);
                        if (derr.isError()) return Error::Ok; // unreadable: no matches
                        for (;;) {
                                auto [info, nerr] = dir.next();
                                if (nerr.isError()) break;
                                const StringView name = info.name;
                                if (name == "." || name == ".." || (name.startsWith('.') && !dotOk)) continue;
                                if (!Glob::match(component, name)) continue;
                                if (!last && !info.isDirectory()) continue;
                                if (!last && !dirOnly) {
                                        err = subdirs.append(std::move(info.name));
                                } else {
                                        String path;
                                        err = path.append(prefix);
                                        if (err.isOk()) err = path.append(name);
                                        if (err.isOk() && dirOnly) err = path.append('/');
                                        if (err.isOk()) err = out.append(std::move(path));
                                }
                                if (err.isError()) return err;
                        }
                }
                for (const String &name : subdirs) {
                        err = prefix.append(name);
                        if (err.isOk()) err = prefix.append('/');
                        if (err.isOk()) err = walk(prefix, after, context, out);
                        prefix.truncate(base);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

} // namespace

Result<size_t> Glob::expandPaths(StringView pattern, const Vfs::Context &context, List<String> &out) {
        const size_t first = out.size();
        String       prefix;
        Error        err;
        if (pattern.startsWith('/')) {
                err = prefix.append('/');
                while (pattern.startsWith('/')) pattern = pattern.mid(1);
        }
        if (err.isOk()) err = walk(prefix, pattern, context, out);
        if (err.isError()) {
                while (out.size() > first) out.removeAt(out.size() - 1);
                return makeError<size_t>(err);
        }
        sortNames(out, first);
        return makeResult(out.size() - first);
}

bool Glob::nameLess(StringView a, StringView b) {
        const size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
                if (x != y) return x < y;
        }
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
}

void Glob::sortNames(List<String> &list, size_t first) {
        // Insertion sort: Strings only move, and expansions are short.
        for (size_t i = first + 1; i < list.size(); ++i) {
                String item = std::move(list[i]);
                size_t j = i;
                for (; j > first && nameLess(item, list[j - 1]); --j) list[j] = std::move(list[j - 1]);
                list[j] = std::move(item);
        }
        return;
}

BMDS_NAMESPACE_END
