/**
 * @file      previews.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Previews of the color settings on real-looking elements (devplan/controlpanel.md §12): the
 * window manager's in the fonts set now too.
 */

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <bmds/bitmapfont.h>
#include <bmds/glyphrenderer.h>
#include <bmds/windowstack.h>
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

        constexpr uint16_t Cell = 8;  // unscii-16's cell (the terminal's)
        constexpr uint16_t Line = 16; // and its height

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

namespace {

        using Frame = WindowFrame;

        constexpr uint16_t Pad = 6;          // the client's content from its edges, and between rows
        constexpr uint16_t InactiveTop = 10; // the windows' tops
        constexpr uint16_t ActiveTop = 26;
        constexpr uint16_t Hang = 10; // how far the tooltip reaches below the active window

        int colDown(int x) {
                return x & ~3;
        }

        // A row of @p font's text at the top of @p r, cut to @p r and @p limit.
        void text(Painter &p, const Rect &r, StringView s, const Font &font, uint32_t fg, uint32_t bg,
                  const Rect &limit) {
                const Rect cut = r.intersected(limit);
                if (cut.isEmpty()) return;
                const int y = GuiStyle::textTop(r, font);
                GuiStyle::drawClipped(p, cut,
                                      [&](Painter &clipped) { (void)clipped.text(r.x, y, s, font, fg, bg, r.width); });
                return;
        }

        // The active window's client area, from its top: the labels, the button and field, the item.
        uint16_t clientHeight(const Font &ui) {
                const uint16_t line = uint16_t(ui.metrics().lineHeight);
                return uint16_t(Pad + line + Pad + GuiStyle::buttonHeight(ui) + Pad + line + 4 + Pad + 2);
        }

        uint16_t activeHeight(const Font &ui) {
                return Frame::outerHeight(clientHeight(ui), Frame::Standard);
        }

        uint16_t inactiveHeight(const Font &ui) {
                return uint16_t(activeHeight(ui) - 24);
        }

} // namespace

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
        return GuiSize{360, heightForWidth(360)};
}

uint16_t PalettePreview::heightForWidth(uint16_t) const {
        // The active window and the tooltip under it; the inactive one and the desktop's text under it.
        const Font     ui = font();
        const uint32_t active = ActiveTop + activeHeight(ui) + Hang + 8;
        const uint32_t inactive = InactiveTop + inactiveHeight(ui) + 8 + uint32_t(ui.metrics().lineHeight) + 6;
        return uint16_t(active > inactive ? active : inactive);
}

// A window as the window manager draws it (WindowFrame's parts, its buttons' glyphs and bevels, the
// title in the title font, its ink in the bar's middle), in the colors set now.
void PalettePreview::window(Painter &p, const Rect &r, bool active, const char *title) {
        using Part = Frame::Part;
        const Rect    &g = geometry();
        const Frame    f(r, Frame::Standard);
        const uint32_t frame = _c[active ? P::ActiveFrame : P::InactiveFrame];
        Rect           edges[4];
        const size_t   n = f.edges(edges);
        for (size_t i = 0; i < n; ++i) fill(p, edges[i].x, edges[i].y, edges[i].width, edges[i].height, frame, g);
        const Rect bar = f.titleBar();
        fill(p, bar.x, bar.y, bar.width, bar.height, frame, g);
        struct Box {
                        Part     part;
                        size_t   face, mark;
                        uint32_t icon;
        };
        const Box boxes[] = {
                {Part::Menu, P::Button, P::ButtonMark, IconGlyph::Menu},
                {Part::Minimize, P::Button, P::ButtonMark, IconGlyph::Minimize},
                {Part::Maximize, P::Button, P::ButtonMark, IconGlyph::Maximize},
                {Part::Close, P::CloseBox, P::CloseMark, IconGlyph::Close},
        };
        for (const Box &b : boxes) {
                const Rect br = f.button(b.part);
                if (br.isEmpty() || !g.contains(br)) continue;
                TextCell icon;
                icon.codepoint = b.icon;
                icon.fg = _c[b.mark];
                icon.bg = _c[b.face];
                (void)p.cells(br.x, br.y, Span<const TextCell>(&icon, 1), Fonts::icons16, br.width);
                bevel(p, br.x, br.y, br.width, br.height, true, _c, g);
        }
        const Rect tr = f.title();
        if (!tr.isEmpty()) {
                const Font titleFont = GuiStyle::font(FontName::Role::Title);
                const int  x = col(tr.x);
                text(p, Rect{uint16_t(x), bar.y, uint16_t(tr.right() > x ? tr.right() - x : 0), bar.height},
                     StringView(title), titleFont, _c[active ? P::ActiveTitle : P::InactiveTitle], frame, g);
        }
        const Rect in = f.client();
        fill(p, in.x, in.y, in.width, in.height, active ? _c[P::Face] : _c[P::Client], g);
        if (!active) return;
        // The face's things: text and disabled text; a button and a field; a selected item.
        const Font     ui = font();
        const uint16_t line = uint16_t(ui.metrics().lineHeight), bh = GuiStyle::buttonHeight(ui);
        const int      left = col(in.x + 8);
        const Rect     inside = in.intersected(g);
        int            y = in.y + Pad;
        const uint16_t textW = uint16_t(col(int(ui.textWidth("Text"))) + 12);
        text(p, Rect{uint16_t(left), uint16_t(y), textW, line}, "Text", ui, _c[P::FaceText], _c[P::Face], inside);
        text(p, Rect{uint16_t(left + textW), uint16_t(y), uint16_t(in.right() - (left + textW)), line}, "Disabled", ui,
             _c[P::DisabledText], _c[P::Face], inside);
        y += line + Pad;
        // The button as Button draws it: its text a pad in, centred on its ink.
        const int bw = col(int(ui.textWidth("OK")) + 2 * GuiStyle::ButtonPad + 3);
        bevel(p, left, y, bw, bh, true, _c, inside);
        fill(p, left + 2, y + 2, bw - 4, bh - 4, _c[P::Face], inside);
        const int okX = col(left + (bw - int(ui.textWidth("OK"))) / 2);
        text(p, Rect{uint16_t(okX), uint16_t(y + 2), uint16_t(left + bw - 2 - okX), uint16_t(bh - 4)}, "OK", ui,
             _c[P::FaceText], _c[P::Face], inside);
        const int fx = col(left + bw + 8), fw = int(in.right()) - 8 - fx;
        if (fw > 8) {
                bevel(p, fx, y, fw, bh, false, _c, inside);
                fill(p, fx + 2, y + 2, fw - 4, bh - 4, _c[P::FaceHighlight], inside);
                text(p, Rect{uint16_t(col(fx + 4)), uint16_t(y + 2), uint16_t(fw - 8), uint16_t(bh - 4)}, "Field", ui,
                     _c[P::FaceText], _c[P::FaceHighlight], inside);
        }
        y += bh + Pad;
        const uint16_t sw = uint16_t(col(int(ui.textWidth("Selected item")) + 8 + 3));
        const Rect     sel{uint16_t(left), uint16_t(y), sw, uint16_t(line + 4)};
        fill(p, sel.x, sel.y, sel.width, sel.height, _c[P::Highlight], inside);
        text(p, Rect{uint16_t(left + 4), sel.y, uint16_t(sw - 4), sel.height}, "Selected item", ui,
             _c[P::HighlightText], _c[P::Highlight], sel.intersected(inside));
        return;
}

void PalettePreview::paint(Painter &p) {
        const Rect &g = geometry();
        desktop(p, g);
        const Font     ui = font();
        const uint16_t line = uint16_t(ui.metrics().lineHeight);
        // On 4-pixel columns, as windows are: their buttons' glyphs and their text start on them.
        const int w = g.width;
        const int ix = col(g.x + 12), ax = col(g.x + w / 4);
        window(p, Rect{uint16_t(ix), uint16_t(g.y + InactiveTop), uint16_t(colDown(w * 2 / 5)), inactiveHeight(ui)},
               false, "Inactive");
        const Rect active{uint16_t(ax), uint16_t(g.y + ActiveTop), uint16_t(colDown(w * 5 / 8)), activeHeight(ui)};
        window(p, active, true, "Active window");
        text(p, Rect{uint16_t(ix), uint16_t(g.bottom() - line - 6), uint16_t(ax > ix ? ax - ix : 0), line}, "Desktop",
             ui, _c[P::DesktopText], _c[P::Desktop], g);
        // A tooltip over the active window's corner, the pointer by it.
        const int tw = col(int(ui.textWidth("Tooltip")) + 8 + 3), th = line + 4;
        const int tx = col(int(active.right()) - tw), ty = int(active.bottom()) + Hang - th;
        fill(p, tx, ty, tw, th, _c[P::ToolTipText], g);
        fill(p, tx + 1, ty + 1, tw - 2, th - 2, _c[P::ToolTip], g);
        text(p, Rect{uint16_t(tx + 4), uint16_t(ty + 1), uint16_t(tw - 5), uint16_t(th - 2)}, "Tooltip", ui,
             _c[P::ToolTipText], _c[P::ToolTip], g);
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
