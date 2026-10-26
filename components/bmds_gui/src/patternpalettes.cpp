/**
 * @file      patternpalettes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop patterns' palettes: built in (PNG text chunks) and saved (an INI text), read and
 * edited line by line (devplan/wallpaper.md §4c).
 */

#include <bmds/patternpalettes.h>
#include <bmds/pngdecoder.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView PalettePrefix("Palette:");

        // A line of the text: where it starts, ends (before its LF) and the next begins.
        struct Line {
                        size_t     start = 0;
                        size_t     end = 0;
                        size_t     next = 0;
                        StringView trimmed;
        };

        bool nextLine(StringView text, size_t at, Line &out) {
                if (at >= text.size()) return false;
                size_t end = text.indexOf('\n', at);
                out.next = end == StringView::NPos ? text.size() : end + 1;
                if (end == StringView::NPos) end = text.size();
                out.start = at;
                out.end = end;
                StringView l = text.mid(at, end - at);
                if (l.endsWith('\r')) l = l.left(l.size() - 1);
                out.trimmed = l.trimmed();
                return true;
        }

        bool isGroup(StringView t, StringView &name) {
                if (t.size() < 2 || t[0] != '[' || t[t.size() - 1] != ']') return false;
                name = t.mid(1, t.size() - 2).trimmed();
                return true;
        }

        bool isEntry(StringView t, StringView &key, StringView &value) {
                if (t.isEmpty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return false;
                const size_t eq = t.indexOf('=');
                if (eq == StringView::NPos) return false;
                key = t.left(eq).trimmed();
                value = t.mid(eq + 1).trimmed();
                return !key.isEmpty();
        }

        // Calls @p fn(key, value) for each entry of @p pattern's groups.
        template <typename Fn> void eachEntry(StringView text, StringView pattern, Fn fn) {
                bool in = false;
                Line l;
                for (size_t at = 0; nextLine(text, at, l); at = l.next) {
                        StringView name, key, value;
                        if (isGroup(l.trimmed, name))
                                in = name == pattern;
                        else if (in && isEntry(l.trimmed, key, value))
                                fn(key, value);
                }
                return;
        }

        bool hasControl(StringView s) {
                for (char c : s)
                        if (uint8_t(c) < 0x20 || c == 0x7f) return true;
                return false;
        }

        bool isValidPattern(StringView name) {
                return !name.isEmpty() && name.size() <= 64 && !hasControl(name) &&
                       name.indexOf('[') == StringView::NPos && name.indexOf(']') == StringView::NPos &&
                       name.trimmed().size() == name.size();
        }

        Error appendLine(String &out, StringView key, StringView colors) {
                Error err = out.append(key);
                if (err.isOk()) err = out.append('=');
                if (err.isOk()) err = out.append(colors);
                if (err.isOk()) err = out.append('\n');
                return err;
        }

} // namespace

size_t PatternPalettes::builtIn(ByteSpan png, Span<Palette> out) {
        size_t     n = 0;
        size_t     at = 8;
        StringView keyword, text;
        while (Png::nextText(png, at, keyword, text)) {
                if (!keyword.startsWith(PalettePrefix) || keyword.size() == PalettePrefix.size()) continue;
                if (n < out.size()) out[n] = Palette{keyword.mid(PalettePrefix.size()), text.trimmed()};
                ++n;
        }
        return n;
}

size_t PatternPalettes::saved(StringView text, StringView pattern, Span<Palette> out) {
        size_t n = 0;
        eachEntry(text, pattern, [&](StringView key, StringView value) {
                if (key == StringView(CurrentKey)) return;
                if (n < out.size()) out[n] = Palette{key, value};
                ++n;
        });
        return n;
}

bool PatternPalettes::current(StringView text, StringView pattern, StringView &colors) {
        bool found = false;
        eachEntry(text, pattern, [&](StringView key, StringView value) {
                if (found || key != StringView(CurrentKey)) return;
                colors = value;
                found = true;
        });
        return found;
}

Error PatternPalettes::set(StringView text, StringView pattern, StringView key, StringView colors, String &out) {
        out.clear();
        if (!isValidPattern(pattern) || (key != StringView(CurrentKey) && !isValidName(key)) || hasControl(colors))
                return Error::Invalid;
        // Where: the entry's line to replace, else the end of the group's last entry (or its
        // heading) to add after.
        size_t replaceStart = StringView::NPos, replaceNext = 0, insertAt = StringView::NPos;
        bool   in = false;
        Line   l;
        for (size_t at = 0; nextLine(text, at, l); at = l.next) {
                StringView name, k, value;
                if (isGroup(l.trimmed, name)) {
                        in = name == pattern;
                        if (in && insertAt == StringView::NPos) insertAt = l.next;
                } else if (in && isEntry(l.trimmed, k, value)) {
                        insertAt = l.next;
                        if (k == key && replaceStart == StringView::NPos) {
                                replaceStart = l.start;
                                replaceNext = l.next;
                        }
                }
        }
        Error err;
        if (replaceStart != StringView::NPos) {
                err = out.append(text.left(replaceStart));
                if (err.isOk()) err = appendLine(out, key, colors);
                if (err.isOk()) err = out.append(text.mid(replaceNext));
        } else if (insertAt != StringView::NPos) {
                err = out.append(text.left(insertAt));
                if (err.isOk() && !text.left(insertAt).isEmpty() && !text.left(insertAt).endsWith('\n'))
                        err = out.append('\n');
                if (err.isOk()) err = appendLine(out, key, colors);
                if (err.isOk()) err = out.append(text.mid(insertAt));
        } else {
                err = out.append(text);
                if (err.isOk() && !text.isEmpty() && !text.endsWith('\n')) err = out.append('\n');
                if (err.isOk() && !text.isEmpty()) err = out.append('\n'); // a blank line between groups
                if (err.isOk()) err = out.append('[');
                if (err.isOk()) err = out.append(pattern);
                if (err.isOk()) err = out.append("]\n");
                if (err.isOk()) err = appendLine(out, key, colors);
        }
        return err.isOk() ? Error(Error::Ok) : Error(Error::NoMem);
}

Error PatternPalettes::remove(StringView text, StringView pattern, StringView key, String &out) {
        out.clear();
        bool  in = false, removed = false;
        Line  l;
        Error err;
        for (size_t at = 0; err.isOk() && nextLine(text, at, l); at = l.next) {
                StringView name, k, value;
                if (isGroup(l.trimmed, name)) in = name == pattern;
                if (in && isEntry(l.trimmed, k, value) && k == key) {
                        removed = true;
                        continue;
                }
                err = out.append(text.mid(l.start, l.next - l.start));
        }
        if (err.isError()) return Error::NoMem;
        return removed ? Error(Error::Ok) : Error(Error::NotExist);
}

bool PatternPalettes::isValidName(StringView name) {
        if (name.isEmpty() || name.size() > MaxName || name.trimmed().size() != name.size() || hasControl(name))
                return false;
        for (char c : name)
                if (c == '=' || c == '[' || c == ']' || c == ';' || c == '#') return false;
        return name != StringView(CurrentKey) && name != StringView(OriginalName);
}

BMDS_NAMESPACE_END
