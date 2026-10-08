/**
 * @file      furniturelayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The geometry of menu bars, splitters, progress bars and dialogs' buttons
 * (devplan/filebrowser.md §4e).
 */

#include <bmds/furniturelayout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t columnUp(uint32_t x) {
                return (x + 3u) & ~3u;
        }

} // namespace

void MenuBarLayout::place(Span<const uint16_t> textWidths, uint16_t x, uint16_t y, uint16_t height, Span<Rect> out) {
        uint32_t at = columnUp(x);
        for (size_t i = 0; i < textWidths.size() && i < out.size(); ++i) {
                const uint32_t w = columnUp(uint32_t(textWidths[i]) + 2 * Pad);
                out[i] = Rect{static_cast<uint16_t>(at), y, static_cast<uint16_t>(w), height};
                at += w;
        }
        return;
}

int MenuBarLayout::titleAt(Span<const Rect> titles, int x, int y) {
        for (size_t i = 0; i < titles.size(); ++i) {
                const Rect &r = titles[i];
                if (x >= r.x && y >= r.y && uint32_t(x) < r.right() && uint32_t(y) < r.bottom()) return int(i);
        }
        return -1;
}

size_t MenuBarLayout::neighbour(size_t from, int step, size_t count) {
        if (count == 0) return 0;
        const long n = long(count);
        long       to = (long(from) + step) % n;
        if (to < 0) to += n;
        return size_t(to);
}

uint16_t SplitterLayout::firstSize(int wanted, uint16_t length, uint16_t firstLeast, uint16_t secondLeast,
                                   uint16_t unit) {
        const int u = unit > 0 ? unit : 1;
        const int room = int(length) - Handle; // both sides
        if (room <= 0) return 0;
        int most = (room - int(secondLeast)) / u * u;
        int least = (int(firstLeast) + u - 1) / u * u;
        if (least > room / u * u) least = room / u * u;
        if (most < least) most = least;
        int size = wanted < 0 ? 0 : wanted / u * u;
        if (size < least) size = least;
        if (size > most) size = most;
        return static_cast<uint16_t>(size);
}

uint16_t ProgressLayout::blocks(uint64_t value, uint64_t maximum, uint16_t width) {
        const uint16_t all = capacity(width);
        if (maximum == 0 || value >= maximum) return maximum == 0 ? 0 : all;
        return static_cast<uint16_t>(value * all / maximum);
}

uint16_t ButtonRow::buttonWidth(uint16_t textWidth) {
        const uint32_t w = columnUp(uint32_t(textWidth) + 16); // ButtonPad each side
        return static_cast<uint16_t>(w > MinWidth ? w : MinWidth);
}

uint16_t ButtonRow::width(Span<const uint16_t> textWidths) {
        uint32_t w = 0;
        for (size_t i = 0; i < textWidths.size(); ++i) w += buttonWidth(textWidths[i]) + (i > 0 ? Spacing : 0);
        return static_cast<uint16_t>(w);
}

void ButtonRow::place(Span<const uint16_t> textWidths, uint16_t right, uint16_t y, uint16_t height, Span<Rect> out) {
        const uint32_t total = width(textWidths);
        uint32_t       x = right > total ? (right - total) & ~3u : 0;
        for (size_t i = 0; i < textWidths.size() && i < out.size(); ++i) {
                const uint16_t w = buttonWidth(textWidths[i]);
                out[i] = Rect{static_cast<uint16_t>(x), y, w, height};
                x += w + Spacing;
        }
        return;
}

BMDS_NAMESPACE_END
