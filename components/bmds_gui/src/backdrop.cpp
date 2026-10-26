/**
 * @file      backdrop.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A view's backdrop drawn through the painter's strip (devplan/wallpaper.md §4d, §4e).
 */

#include <bmds/backdrop.h>
#include <bmds/icon.h>
#include <bmds/painter.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A band of rows being made: where it is, and the icon blended over it (if any).
        struct Band {
                        const Backdrop *backdrop;
                        int             x;
                        int             y;
                        uint16_t        width;
                        uint16_t        rows;
                        const Icon     *icon;    // null: the backdrop alone
                        uint16_t        iconRow; // the band's first row in the icon
        };

        uint16_t makeBand(void *arg, uint8_t *strip, size_t stride, uint16_t column, uint16_t room) {
                const Band    &b = *static_cast<const Band *>(arg);
                const uint16_t w = room < b.width - column ? uint16_t(room & ~3u) : uint16_t(b.width - column);
                if (w == 0) return 0;
                b.backdrop->make(b.x + column, b.y, w, b.rows, strip, stride);
                if (b.icon == nullptr) return w;
                for (uint16_t r = 0; r < b.rows; ++r) {
                        const uint32_t *from = b.icon->pixels + size_t(b.iconRow + r) * b.icon->width + column;
                        uint8_t        *p = strip + size_t(r) * stride;
                        for (uint16_t i = 0; i < w; ++i, p += 3) {
                                const uint32_t a = from[i] >> 24;
                                if (a == 0) continue;
                                const uint32_t under = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                const uint32_t rgb = blendOver(from[i], under);
                                p[0] = uint8_t(rgb);
                                p[1] = uint8_t(rgb >> 8);
                                p[2] = uint8_t(rgb >> 16);
                        }
                }
                return w;
        }

} // namespace

Error Backdrop::paint(Painter &painter, const Rect &rect) const {
        if (rect.isEmpty()) return painter.error();
        const int      left = rect.x & ~3, right = (int(rect.right()) + 3) & ~3;
        const uint16_t band = painter.stripRows();
        for (int y = rect.y; y < int(rect.bottom()) && band != 0; y += band) {
                const uint16_t rows = uint16_t(int(rect.bottom()) - y < band ? int(rect.bottom()) - y : band);
                Band           b{this, left, y, uint16_t(right - left), rows, nullptr, 0};
                (void)painter.rows(left, y, b.width, rows, 1, false, makeBand, &b);
        }
        return painter.error();
}

Error Backdrop::paintIcon(Painter &painter, int x, int y, const Icon &icon) const {
        const uint16_t width = uint16_t(icon.width & ~3u); // whole groups of 4, as Painter::image()
        const uint16_t band = painter.stripRows();
        if (!icon.isValid() || width == 0 || band == 0) return painter.error();
        for (uint16_t top = 0; top < icon.height; top = uint16_t(top + band)) {
                const uint16_t rows = uint16_t(icon.height - top < band ? icon.height - top : band);
                Band           b{this, x, y + top, width, rows, &icon, top};
                (void)painter.rows(x, y + top, width, rows, 1, false, makeBand, &b);
        }
        return painter.error();
}

BMDS_NAMESPACE_END
