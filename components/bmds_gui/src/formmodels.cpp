/**
 * @file      formmodels.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Scrolling by rows, a number field's value, colors, lists as text (devplan/gui.md §4u).
 */

#include <bmds/formmodels.h>

BMDS_NAMESPACE_BEGIN

// ---- RowScroll

size_t RowScroll::lastTop(Span<const uint32_t> stops, uint32_t total, uint32_t page) {
        for (size_t i = 0; i < stops.size(); ++i)
                if (total <= stops[i] || total - stops[i] <= page) return i;
        return stops.isEmpty() ? 0 : stops.size() - 1;
}

size_t RowScroll::snap(Span<const uint32_t> stops, uint32_t value, size_t current, size_t most) {
        if (stops.isEmpty()) return 0;
        if (most >= stops.size()) most = stops.size() - 1;
        if (current > most) current = most;
        size_t   best = 0;
        uint32_t bestDistance = UINT32_MAX;
        for (size_t i = 0; i <= most; ++i) {
                const uint32_t d = stops[i] > value ? stops[i] - value : value - stops[i];
                if (d < bestDistance) {
                        best = i;
                        bestDistance = d;
                }
        }
        if (best == current && value > stops[current] && current < most) return current + 1;
        if (best == current && value < stops[current] && current > 0) return current - 1;
        return best;
}

size_t RowScroll::reveal(Span<const uint32_t> stops, uint32_t top, uint32_t bottom, size_t row, size_t current,
                         uint32_t page) {
        if (row >= stops.size()) return current;
        if (current >= stops.size() || top < stops[current] || row < current) return row;
        if (bottom <= stops[current] + uint64_t(page)) return current;
        for (size_t t = current + 1; t <= row; ++t)
                if (bottom <= stops[t] + uint64_t(page)) return t;
        return row;
}

// ---- NumberModel

void NumberModel::setRange(int32_t minimum, int32_t maximum) {
        _min = minimum < maximum ? minimum : maximum;
        _max = minimum < maximum ? maximum : minimum;
        (void)setValue(_value);
        return;
}

void NumberModel::setSteps(uint32_t step, uint32_t page) {
        _step = step > 0 ? step : 1;
        _page = page > 0 ? page : 1;
        return;
}

bool NumberModel::setValue(int64_t value) {
        const int32_t v = value < _min ? _min : value > _max ? _max : static_cast<int32_t>(value);
        if (v == _value) return false;
        _value = v;
        return true;
}

bool NumberModel::step(int64_t count, bool page) {
        return setValue(int64_t(_value) + count * int64_t(page ? _page : _step));
}

Result<int64_t> NumberModel::parse(StringView text) {
        size_t i = 0, n = text.size();
        while (i < n && text[i] == ' ') ++i;
        while (n > i && text[n - 1] == ' ') --n;
        bool negative = false;
        if (i < n && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
        if (i == n) return makeError<int64_t>(Error::Invalid);
        int64_t value = 0;
        for (; i < n; ++i) {
                if (text[i] < '0' || text[i] > '9') return makeError<int64_t>(Error::Invalid);
                if (value < (int64_t(1) << 40)) value = value * 10 + (text[i] - '0'); // past int32, kept there
        }
        return makeResult(negative ? -value : value);
}

// ---- ColorModel

namespace {

        uint32_t scaled(uint32_t a, uint32_t b) {
                return (a * b + 127) / 255; // a * b / 255, rounded
        }

        // @p a / @p b (b > 0), halves away from zero.
        int roundedDiv(int a, int b) {
                return a >= 0 ? (2 * a + b) / (2 * b) : -((2 * -a + b) / (2 * b));
        }

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

} // namespace

uint32_t ColorModel::toRgb(const Hsv &hsv) {
        const uint32_t v = hsv.value, s = hsv.saturation, h = hsv.hue % 360;
        const uint32_t f = ((h % 60) * 255 + 30) / 60; // within the sixth, 0-255
        const uint32_t p = scaled(v, 255 - s);
        const uint32_t q = scaled(v, 255 - scaled(s, f));
        const uint32_t t = scaled(v, 255 - scaled(s, 255 - f));
        uint32_t       r = v, g = t, b = p;
        switch (h / 60) {
                case 0: r = v, g = t, b = p; break;
                case 1: r = q, g = v, b = p; break;
                case 2: r = p, g = v, b = t; break;
                case 3: r = p, g = q, b = v; break;
                case 4: r = t, g = p, b = v; break;
                default: r = v, g = p, b = q; break;
        }
        return (r << 16) | (g << 8) | b;
}

ColorModel::Hsv ColorModel::fromRgb(uint32_t rgb) {
        const int r = int((rgb >> 16) & 0xff), g = int((rgb >> 8) & 0xff), b = int(rgb & 0xff);
        const int most = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const int least = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const int delta = most - least;
        Hsv       out;
        out.value = static_cast<uint8_t>(most);
        if (most == 0 || delta == 0) return out;
        out.saturation = static_cast<uint8_t>((delta * 255 + most / 2) / most);
        int h = 0;
        if (most == r) {
                h = roundedDiv(60 * (g - b), delta);
        } else if (most == g) {
                h = 120 + roundedDiv(60 * (b - r), delta);
        } else {
                h = 240 + roundedDiv(60 * (r - g), delta);
        }
        if (h < 0) h += 360;
        out.hue = static_cast<uint16_t>(h % 360);
        return out;
}

Result<uint32_t> ColorModel::parseHex(StringView text) {
        size_t i = 0, n = text.size();
        while (i < n && text[i] == ' ') ++i;
        while (n > i && text[n - 1] == ' ') --n;
        if (i < n && text[i] == '#') ++i;
        if (n - i != 6) return makeError<uint32_t>(Error::Invalid);
        uint32_t rgb = 0;
        for (; i < n; ++i) {
                const int d = hexDigit(text[i]);
                if (d < 0) return makeError<uint32_t>(Error::Invalid);
                rgb = (rgb << 4) | uint32_t(d);
        }
        return makeResult(rgb);
}

void ColorModel::formatHex(uint32_t rgb, char (&out)[8]) {
        constexpr char Digits[] = "0123456789abcdef";
        out[0] = '#';
        for (int i = 0; i < 6; ++i) out[1 + i] = Digits[(rgb >> (20 - 4 * i)) & 0xf];
        out[7] = '\0';
        return;
}

// ---- ListText

namespace {

        // Item @p index's bytes [start, end) in @p text, or false.
        bool span(StringView text, char separator, size_t index, size_t &start, size_t &end) {
                if (text.isEmpty()) return false;
                size_t at = 0;
                for (size_t i = 0; i < index; ++i) {
                        const size_t next = text.mid(at).indexOf(separator);
                        if (next == StringView::NPos) return false;
                        at += next + 1;
                }
                const size_t next = text.mid(at).indexOf(separator);
                start = at;
                end = next == StringView::NPos ? text.size() : at + next;
                return true;
        }

        // @p out = @p items in order (empty views as empty items), between separators.
        Error join(Span<const StringView> items, char separator, String &out) {
                out.clear();
                for (size_t i = 0; i < items.size(); ++i) {
                        if (i > 0)
                                if (Error e = out.append(separator); e.isError()) return e;
                        if (Error e = out.append(items[i]); e.isError()) return e;
                }
                return Error::Ok;
        }

} // namespace

size_t ListText::count(StringView text, char separator) {
        if (text.isEmpty()) return 0;
        size_t n = 1;
        for (char c : text)
                if (c == separator) ++n;
        return n;
}

StringView ListText::item(StringView text, char separator, size_t index) {
        size_t start = 0, end = 0;
        return span(text, separator, index, start, end) ? text.mid(start, end - start) : StringView();
}

Error ListText::insert(StringView text, char separator, size_t index, StringView item, String &out) {
        const size_t n = count(text, separator);
        if (n == 0) return out.assign(item);
        if (index >= n) {
                const StringView all[2] = {text, item};
                return join(Span<const StringView>(all), separator, out);
        }
        size_t start = 0, end = 0;
        (void)span(text, separator, index, start, end);
        out.clear();
        if (Error e = out.append(text.left(start)); e.isError()) return e;
        if (Error e = out.append(item); e.isError()) return e;
        if (Error e = out.append(separator); e.isError()) return e;
        return out.append(text.mid(start));
}

Error ListText::replace(StringView text, char separator, size_t index, StringView item, String &out) {
        size_t start = 0, end = 0;
        if (!span(text, separator, index, start, end)) return Error::Invalid;
        out.clear();
        if (Error e = out.append(text.left(start)); e.isError()) return e;
        if (Error e = out.append(item); e.isError()) return e;
        return out.append(text.mid(end));
}

Error ListText::remove(StringView text, char separator, size_t index, String &out) {
        size_t start = 0, end = 0;
        if (!span(text, separator, index, start, end)) return Error::Invalid;
        out.clear();
        if (end < text.size()) {
                // Not the last: it and the separator after it go.
                if (Error e = out.append(text.left(start)); e.isError()) return e;
                return out.append(text.mid(end + 1));
        }
        // The last: the separator before it goes (none if it was the only one).
        return out.append(text.left(start > 0 ? start - 1 : 0));
}

Error ListText::move(StringView text, char separator, size_t from, size_t to, String &out) {
        const size_t n = count(text, separator);
        if (from >= n || to >= n) return Error::Invalid;
        const StringView moving = item(text, separator, from);
        String           rest(MemSpace::Psram);
        if (Error e = remove(text, separator, from, rest); e.isError()) return e;
        if (n == 1) return out.assign(moving);
        // Without it, an only empty item would read as none: put it back by hand.
        if (rest.isEmpty() && n == 2) {
                const StringView items[2] = {to == 0 ? moving : StringView(), to == 0 ? StringView() : moving};
                return join(Span<const StringView>(items), separator, out);
        }
        return insert(rest.view(), separator, to, moving, out);
}

BMDS_NAMESPACE_END
