/**
 * @file      guidrag.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The toolkit's drag and drop (devplan/clipboard.md §5d).
 */

#include <cstdio>
#include <cstring>
#include <bmds/bitmapfont.h>
#include <bmds/guiapplication.h>
#include <bmds/guidrag.h>
#include <bmds/list.h>
#include <bmds/simd.h>
#include <bmds/textdraw.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

// ---- GuiDropEvent

Widget *GuiDropEvent::source() const {
        return _e.own ? _window.application().dragSource(_e.drag) : nullptr;
}

bool GuiDropEvent::hasType(StringView type) const {
        GuiClipboard *c = _window.application().clipboard();
        return c != nullptr && c->canPaste(Clipboard::Selection::Drag, type);
}

DropAction GuiDropEvent::suggested(DropAction targetDefault) const {
        return DropActions::choose(_e.actions, (_e.modifiers & KeyEvent::Ctrl) != 0,
                                   (_e.modifiers & KeyEvent::Shift) != 0, targetDefault, _e.preferred);
}

Error GuiDropEvent::read(StringView type, GuiClipboard::Received fn, void *arg) const {
        GuiClipboard *c = _window.application().clipboard();
        return c != nullptr ? c->paste(Clipboard::Selection::Drag, type, fn, arg) : Error(Error::NotExist);
}

void GuiDropEvent::finish(DropAction done) const {
        (void)_window.application().windowManager().finishDrop(_e.drag, done);
        return;
}

// ---- DragPicture

DragPicture::DragPicture(const DragImage::Box &box) : _box(box) {
        const size_t n = size_t(box.width > 0 ? box.width : 0) * size_t(box.height > 0 ? box.height : 0);
        if (n == 0 || _pixels.reserve(n).isError()) return;
        for (size_t i = 0; i < n; ++i) (void)_pixels.append(0);
}

void DragPicture::put(int x, int y, uint32_t argb) {
        const int px = x - _box.x, py = y - _box.y;
        if (px < 0 || py < 0 || px >= _box.width || py >= _box.height || _pixels.isEmpty()) return;
        uint32_t      &d = _pixels[size_t(py) * size_t(_box.width) + size_t(px)];
        const uint32_t sa = argb >> 24, da = d >> 24;
        if (sa == 0) return;
        if (sa == 255 || da == 0) {
                d = argb;
                return;
        }
        // Over: the result's alpha and colour (straight alpha).
        const uint32_t oa = sa + da * (255 - sa) / 255;
        uint32_t       out = oa << 24;
        for (int shift = 0; shift < 24; shift += 8) {
                const uint32_t s = (argb >> shift) & 0xff, t = (d >> shift) & 0xff;
                out |= ((s * sa + t * da * (255 - sa) / 255) / (oa ? oa : 1)) << shift;
        }
        d = out;
        return;
}

void DragPicture::icon(int x, int y, const Icon &icon) {
        if (!icon.isValid()) return;
        for (int r = 0; r < icon.height; ++r)
                for (int c = 0; c < icon.width; ++c) {
                        const uint32_t p = icon.pixels[size_t(r) * icon.width + size_t(c)];
                        put(x + c, y + r, (((p >> 24) * Ghost / 255) << 24) | (p & 0xffffff));
                }
        return;
}

void DragPicture::fill(int x, int y, int w, int h, uint32_t rgb) {
        for (int r = 0; r < h; ++r)
                for (int c = 0; c < w; ++c) put(x + c, y + r, (Ghost << 24) | (rgb & 0xffffff));
        return;
}

void DragPicture::text(int x, int y, StringView text, const Font &font, uint32_t rgb, uint16_t maxWidth) {
        // The line's coverage (white drawn over black, smoothed), then each pixel ghosted by it.
        const int w = int(maxWidth < _box.width ? maxWidth : _box.width), h = font.metrics().lineHeight;
        if (w <= 0 || h <= 0 || !font.isValid()) return;
        List<uint8_t> coverage(MemSpace::Psram);
        if (coverage.reserve(size_t(w) * size_t(h)).isError()) return;
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i) (void)coverage.append(0);
        TextTarget t;
        t.pixels = coverage.data();
        t.format = TextBlend::Format::Gray8;
        t.width = w;
        t.height = h;
        t.stride = size_t(w);
        TextPaint paint;
        paint.color = 0xffffff;
        (void)TextDraw::line(t, font, text, 0, 0, paint, maxWidth, Simd::best());
        for (int r = 0; r < h; ++r)
                for (int c = 0; c < w; ++c) {
                        const uint32_t a = coverage[size_t(r) * size_t(w) + size_t(c)];
                        if (a != 0) put(x + c, y + r, ((a * Ghost / 255) << 24) | (rgb & 0xffffff));
                }
        return;
}

void DragPicture::badge(int right, int top, size_t count) {
        char digits[12];
        std::snprintf(digits, sizeof(digits), "%u", unsigned(count));
        const int   w = int(std::strlen(digits)) * 8 + 8, h = 18;
        const int   x = right - w;
        const BitmapFont &f = Fonts::unscii16;
        // A red pill with white digits, its corners cut.
        for (int r = 0; r < h; ++r)
                for (int c = 0; c < w; ++c) {
                        const bool corner = (r == 0 || r == h - 1) && (c == 0 || c == w - 1);
                        if (!corner) put(x + c, top + r, 0xffc83030);
                }
        int at = x + 4;
        for (const char ch : StringView(digits)) {
                const Glyph g = f.glyph(uint8_t(ch));
                for (int r = 0; r < f.height(); ++r)
                        for (int c = 0; c < 8; ++c)
                                if ((g.rows[r] & (0x80u >> c)) != 0) put(at + c, top + 1 + r, 0xffffffff);
                at += 8;
        }
        return;
}

bool DragPicture::crop(int x, int y) {
        if (_pixels.isEmpty()) return false;
        int l = _box.width, t = _box.height, r = -1, b = -1;
        for (int py = 0; py < _box.height; ++py)
                for (int px = 0; px < _box.width; ++px)
                        if ((_pixels[size_t(py) * size_t(_box.width) + size_t(px)] >> 24) != 0) {
                                l = px < l ? px : l;
                                t = py < t ? py : t;
                                r = px > r ? px : r;
                                b = py > b ? py : b;
                        }
        if (r < 0) return false;
        // The pointer inside: the hot spot is kept in the image.
        const int hx = x - _box.x, hy = y - _box.y;
        if (hx >= 0 && hx < _box.width) {
                l = hx < l ? hx : l;
                r = hx > r ? hx : r;
        }
        if (hy >= 0 && hy < _box.height) {
                t = hy < t ? hy : t;
                b = hy > b ? hy : b;
        }
        const int w = r - l + 1, h = b - t + 1;
        for (int py = 0; py < h; ++py)
                std::memmove(&_pixels[size_t(py) * size_t(w)],
                             &_pixels[size_t(py + t) * size_t(_box.width) + size_t(l)], size_t(w) * sizeof(uint32_t));
        _box = DragImage::Box{_box.x + l, _box.y + t, w, h};
        const int nx = x - _box.x, ny = y - _box.y;
        _hotX = uint8_t(nx < 0 ? 0 : nx >= w ? w - 1 : nx);
        _hotY = uint8_t(ny < 0 ? 0 : ny >= h ? h - 1 : ny);
        return true;
}

BMDS_NAMESPACE_END
