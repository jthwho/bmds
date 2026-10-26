/**
 * @file      previews.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Previews of the color settings on real-looking elements (devplan/controlpanel.md §12).
 */

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <bmds/font.h>
#include <bmds/patternfiles.h>
#include <bmds/sysconf.h>

#include "previews.h"
#include "themes.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The palette's roles, as WindowPalette numbers them.
        namespace P {
                enum : size_t {
                        Desktop,
                        ActiveFrame,
                        InactiveFrame,
                        ActiveTitle,
                        InactiveTitle,
                        CloseBox,
                        CloseMark,
                        Client,
                        Outline,
                        CursorEdge,
                        CursorFill,
                        Button,
                        ButtonMark,
                        Face,
                        FaceText,
                        FaceHighlight,
                        FaceLight,
                        FaceShadow,
                        FaceDark,
                        Highlight,
                        HighlightText,
                        DisabledText,
                        DesktopText,
                        ToolTip,
                        ToolTipText,
                };
        } // namespace P

        constexpr uint16_t Cell = 8;    // unscii-16's cell
        constexpr uint16_t Line = 16;   // and its height
        constexpr uint16_t TitleH = 18; // a preview window's title bar

        uint16_t col(uint32_t x) {
                return static_cast<uint16_t>((x + 3) & ~3u); // text on 4-pixel columns
        }

        // Reads @p set's colors from the registry into @p out.
        void read(const Themes::Set &set, uint32_t *out, size_t count) {
                for (size_t i = 0; i < count && i < set.names.size(); ++i) {
                        char key[64];
                        std::snprintf(key, sizeof(key), "%s%s", set.prefix, set.names[i]);
                        const Result<uint32_t> c = SysConf::getColor(StringView(key));
                        out[i] = c.isOk() ? c.value() : (set.themes.isEmpty() ? 0 : set.themes[0].colors[i]);
                }
                return;
        }

        // A pattern tiled from a corner of the window's (@p dx, @p dy): the preview's, not the window's.
        class Shifted final : public Backdrop {
                public:
                        Shifted(const TiledPattern &pattern, int dx, int dy) : _pattern(pattern), _dx(dx), _dy(dy) {}

                        void make(int x, int y, uint16_t width, uint16_t height, uint8_t *out,
                                  size_t stride) const override {
                                _pattern.make(x - _dx, y - _dy, width, height, out, stride);
                                return;
                        }

                        bool tile(Tile &out) const override {
                                if (!_pattern.tile(out)) return false;
                                out.x += _dx;
                                out.y += _dy;
                                return true;
                        }

                private:
                        const TiledPattern &_pattern;
                        int                 _dx;
                        int                 _dy;
        };

        void fill(Painter &p, int x, int y, int w, int h, uint32_t rgb, const Rect &limit) {
                if (w <= 0 || h <= 0) return;
                const Rect r = Rect{uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h)}.intersected(limit);
                if (!r.isEmpty()) (void)p.fill(r, rgb);
                return;
        }

        void text(Painter &p, int x, int y, StringView s, uint32_t fg, uint32_t bg, const Rect &limit) {
                const uint16_t cx = col(uint32_t(x));
                if (cx >= limit.right() || y < limit.y || uint32_t(y) + Line > limit.bottom()) return;
                (void)p.text(cx, y, s, Fonts::unscii16, fg, bg, static_cast<uint16_t>(limit.right() - cx));
                return;
        }

        // A two-pixel bevel: raised (lit at the top left) or sunken, in the palette @p c.
        void bevel(Painter &p, int x, int y, int w, int h, bool raised, const uint32_t *c, const Rect &limit) {
                const uint32_t outerLit = raised ? c[P::FaceHighlight] : c[P::FaceShadow];
                const uint32_t innerLit = raised ? c[P::FaceLight] : c[P::FaceDark];
                const uint32_t outerDark = raised ? c[P::FaceDark] : c[P::FaceHighlight];
                const uint32_t innerDark = raised ? c[P::FaceShadow] : c[P::FaceLight];
                fill(p, x, y, w, 1, outerLit, limit);
                fill(p, x, y, 1, h, outerLit, limit);
                fill(p, x + 1, y + 1, w - 2, 1, innerLit, limit);
                fill(p, x + 1, y + 1, 1, h - 2, innerLit, limit);
                fill(p, x, y + h - 1, w, 1, outerDark, limit);
                fill(p, x + w - 1, y, 1, h, outerDark, limit);
                fill(p, x + 1, y + h - 2, w - 2, 1, innerDark, limit);
                fill(p, x + w - 2, y + 1, 1, h - 2, innerDark, limit);
                return;
        }

} // namespace

// ---- PalettePreview

PalettePreview::PalettePreview(Widget *parent) : Widget(parent) {
        reload();
}

void PalettePreview::reload() {
        read(Themes::system(), _c, Roles);
        // The desktop's pattern, as the panel shows it.
        _pattern.reset();
        String wallpaper(MemSpace::Psram), name(MemSpace::Psram), colors(MemSpace::Psram);
        if (SysConf::get(StringView(PatternFiles::WallpaperKey), wallpaper).isOk() && wallpaper.view() == "pattern" &&
            SysConf::get(StringView(PatternFiles::PatternKey), name).isOk() &&
            SysConf::get(StringView(PatternFiles::ColorsKey), colors).isOk()) {
                const Result<int32_t> scale = SysConf::getInt(StringView(PatternFiles::ScaleKey));
                PatternFiles::Bytes   file;
                if (PatternFiles::load(name.view(), file).isOk())
                        (void)_pattern.load(file.bytes(), colors.view(), scale.isOk() ? unsigned(scale.value()) : 1,
                                            _c[P::Desktop]);
        }
        update();
        return;
}

// The desktop: the pattern on the 4-pixel columns inside it, the color at its few edge columns.
void PalettePreview::desktop(Painter &p, const Rect &g) {
        (void)p.fill(g, _c[P::Desktop]);
        if (!_pattern.isValid()) return;
        const int left = (g.x + 3) & ~3, right = int(g.right()) & ~3;
        if (right <= left) return;
        const Shifted shifted(_pattern, g.x, g.y);
        (void)shifted.paint(p, Rect{uint16_t(left), g.y, uint16_t(right - left), g.height});
        return;
}

GuiSize PalettePreview::sizeHint() const {
        return GuiSize{360, 168};
}

uint16_t PalettePreview::heightForWidth(uint16_t) const {
        return 168;
}

void PalettePreview::window(Painter &p, const Rect &r, bool active, const char *title) {
        const Rect    &g = geometry();
        const uint32_t frame = _c[active ? P::ActiveFrame : P::InactiveFrame];
        // The frame and title bar, then the client.
        fill(p, r.x, r.y, r.width, r.height, frame, g);
        text(p, r.x + 6, r.y + 1, StringView(title), _c[active ? P::ActiveTitle : P::InactiveTitle], frame, g);
        if (active) {
                const int bx = int(r.right()) - 2 - 16;
                fill(p, bx, r.y + 2, 14, 14, _c[P::CloseBox], g);
                fill(p, bx + 3, r.y + 8, 8, 2, _c[P::CloseMark], g); // a bar for the close mark
                fill(p, bx - 18, r.y + 2, 14, 14, _c[P::Button], g);
                fill(p, bx - 15, r.y + 12, 8, 2, _c[P::ButtonMark], g); // minimize's line
        }
        const Rect in{uint16_t(r.x + 2), uint16_t(r.y + TitleH), uint16_t(r.width - 4),
                      uint16_t(r.height - TitleH - 2)};
        fill(p, in.x, in.y, in.width, in.height, active ? _c[P::Face] : _c[P::Client], g);
        if (!active) return;
        // The face's things: text and disabled text; a button and a field; a selected item.
        int y = in.y + 6;
        text(p, in.x + 8, y, "Text", _c[P::FaceText], _c[P::Face], g);
        text(p, in.x + 56, y, "Disabled", _c[P::DisabledText], _c[P::Face], g);
        y += 22;
        bevel(p, in.x + 8, y, 56, 22, true, _c, g);
        fill(p, in.x + 10, y + 2, 52, 18, _c[P::Face], g);
        text(p, in.x + 28, y + 3, "OK", _c[P::FaceText], _c[P::Face], g);
        bevel(p, in.x + 72, y, int(in.width) - 80, 22, false, _c, g);
        fill(p, in.x + 74, y + 2, int(in.width) - 84, 18, _c[P::FaceHighlight], g);
        text(p, in.x + 78, y + 3, "Field", _c[P::FaceText], _c[P::FaceHighlight], g);
        y += 30;
        fill(p, in.x + 8, y, 104, 18, _c[P::Highlight], g);
        text(p, in.x + 12, y + 1, "Selected", _c[P::HighlightText], _c[P::Highlight], g);
        return;
}

void PalettePreview::paint(Painter &p) {
        const Rect &g = geometry();
        desktop(p, g);
        const int w = g.width;
        window(p, Rect{uint16_t(g.x + 12), uint16_t(g.y + 10), uint16_t(w * 2 / 5), 92}, false, "Inactive");
        const Rect active{uint16_t(g.x + w / 4), uint16_t(g.y + 26), uint16_t(w * 5 / 8), 118};
        window(p, active, true, "Active window");
        text(p, g.x + 12, g.bottom() - 20, "Desktop", _c[P::DesktopText], _c[P::Desktop], g);
        // A tooltip over the active window's corner, the pointer by it.
        const int tx = int(active.right()) - 72, ty = int(active.bottom()) - 10;
        fill(p, tx, ty, 72, 20, _c[P::ToolTipText], g);
        fill(p, tx + 1, ty + 1, 70, 18, _c[P::ToolTip], g);
        text(p, tx + 4, ty + 2, "Tooltip", _c[P::ToolTipText], _c[P::ToolTip], g);
        const int px = tx - 14, py = ty - 14;
        for (int i = 0; i < 11; ++i) {
                fill(p, px, py + i, 1, 1, _c[P::CursorEdge], g);
                if (i > 1) fill(p, px + 1, py + i, i - 1, 1, _c[P::CursorFill], g);
                fill(p, px + i, py + i, 1, 1, _c[P::CursorEdge], g);
        }
        fill(p, px, py + 11, 7, 1, _c[P::CursorEdge], g);
        return;
}

// ---- TerminalPreview

TerminalPreview::TerminalPreview(Widget *parent) : Widget(parent) {
        reload();
}

void TerminalPreview::reload() {
        read(Themes::terminal(), _c, Colors);
        update();
        return;
}

GuiSize TerminalPreview::sizeHint() const {
        return GuiSize{360, 7 * Line + 8};
}

uint16_t TerminalPreview::heightForWidth(uint16_t) const {
        return 7 * Line + 8;
}

void TerminalPreview::paint(Painter &p) {
        const Rect    &g = geometry();
        const uint32_t bg = _c[1], fg = _c[0];
        const auto     ansi = [this](int i) {
                return _c[3 + i];
        };
        (void)p.fill(g, bg);
        // A line of runs: text and its color, a cell at a time from the left.
        struct Run {
                        const char *text;
                        uint32_t    color;
        };
        const int  left = g.x + 8;
        int        y = g.y + 4;
        const auto line = [&](std::initializer_list<Run> runs) {
                int x = left;
                for (const Run &r : runs) {
                        text(p, x, y, StringView(r.text), r.color, bg, g);
                        x += int(std::strlen(r.text)) * Cell;
                }
                y += Line;
        };
        line({{"user@bmds0", ansi(10)}, {":", fg}, {"~", ansi(12)}, {"$ ls", fg}});
        line({{"Documents  Music  ", ansi(12)}, {"notes.txt  ", fg}, {"run.sh", ansi(10)}});
        line({{"user@bmds0", ansi(10)}, {":", fg}, {"~", ansi(12)}, {"$ make", fg}});
        line({{"warning: ", ansi(11)}, {"unused variable  ", fg}, {"error: ", ansi(9)}, {"build failed", fg}});
        // The 16 colors as blocks, their numbers in black or white as reads best.
        for (int row = 0; row < 2; ++row) {
                for (int i = 0; i < 8; ++i) {
                        const int      n = row * 8 + i;
                        const uint32_t c = ansi(n);
                        const uint32_t lum = ((c >> 16) & 0xff) * 3 + ((c >> 8) & 0xff) * 6 + (c & 0xff);
                        const int      x = left + i * 4 * Cell;
                        fill(p, x, y, 3 * Cell, Line, c, g);
                        char num[4];
                        std::snprintf(num, sizeof(num), "%2d", n);
                        text(p, x + 4, y, StringView(num), lum > 1280 ? 0x000000 : 0xffffff, c, g);
                }
                y += Line;
        }
        line({{"user@bmds0", ansi(10)}, {":", fg}, {"~", ansi(12)}, {"$ ", fg}});
        fill(p, left + 14 * Cell, y - Line, Cell, Line, _c[2], g); // the cursor after the prompt
        return;
}

BMDS_NAMESPACE_END
