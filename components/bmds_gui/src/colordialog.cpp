/**
 * @file      colordialog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A color swatch and the color dialog (devplan/gui.md §4u).
 */

#include <bmds/bevel.h>
#include <bmds/colordialog.h>
#include <bmds/guiwindow.h>
#include <bmds/lineedit.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        // The standard sixteen (VGA's).
        constexpr uint32_t StandardColors[] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080,
                                               0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00,
                                               0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};

        constexpr uint16_t ContentWidth = 380;
        constexpr uint16_t ContentHeight = 164;
        constexpr uint16_t BarMargin = 8; // the value bar's arrow, left of its strip
        constexpr uint16_t BarStrip = 16;

        // A 1-pixel ring just inside @p r.
        void ring(Painter &painter, const Rect &r, uint32_t rgb) {
                if (r.width < 3 || r.height < 3) return;
                const Rect sides[] = {{r.x, r.y, r.width, 1},
                                      {r.x, uint16_t(r.bottom() - 1), r.width, 1},
                                      {r.x, uint16_t(r.y + 1), 1, uint16_t(r.height - 2)},
                                      {uint16_t(r.right() - 1), uint16_t(r.y + 1), 1, uint16_t(r.height - 2)}};
                (void)painter.fill(Span<const Rect>(sides), rgb);
                return;
        }

        Rect shrunk(const Rect &r, uint16_t by) {
                if (r.width <= 2 * by || r.height <= 2 * by) return Rect{r.x, r.y, 0, 0};
                return Rect{uint16_t(r.x + by), uint16_t(r.y + by), uint16_t(r.width - 2 * by),
                            uint16_t(r.height - 2 * by)};
        }

        bool focusShown(const Widget &w) {
                const GuiWindow *window = w.window();
                return w.hasFocus() && window != nullptr && window->isActive();
        }

        // Rows of a gradient made in the strip a band at a time (`Painter::rows()`).
        struct Band {
                        uint32_t (*color)(const void *owner, uint16_t x, uint16_t y);
                        const void *owner;
                        uint16_t    top;   // the band's first row, from the gradient's top
                        uint16_t    rows;  // its rows
                        uint16_t    width; // the gradient's
        };

        uint16_t makeBand(void *arg, uint8_t *strip, size_t stride, uint16_t column, uint16_t room) {
                const auto *b = static_cast<const Band *>(arg);
                if (column >= b->width) return 0;
                const uint16_t n = b->width - column < room ? uint16_t(b->width - column) : room;
                for (uint16_t j = 0; j < b->rows; ++j) {
                        uint8_t *out = strip + j * stride;
                        for (uint16_t i = 0; i < n; ++i) {
                                const uint32_t rgb = b->color(b->owner, uint16_t(column + i), uint16_t(b->top + j));
                                *out++ = uint8_t(rgb);
                                *out++ = uint8_t(rgb >> 8);
                                *out++ = uint8_t(rgb >> 16);
                        }
                }
                return n;
        }

        // A gradient over @p area (its x and width on 4-pixel columns), @p color(owner, x, y) a pixel.
        void drawGradient(Painter &painter, const Rect &area, uint32_t (*color)(const void *, uint16_t, uint16_t),
                          const void *owner) {
                const uint16_t most = painter.stripRows();
                for (uint16_t top = 0; top < area.height && most > 0; top = uint16_t(top + most)) {
                        Band b{color, owner, top, uint16_t(area.height - top < most ? area.height - top : most),
                               area.width};
                        if (!painter.isVisible(Rect{area.x, uint16_t(area.y + top), area.width, b.rows})) continue;
                        (void)painter.rows(area.x, area.y + top, area.width, b.rows, 1, false, makeBand, &b);
                }
                return;
        }

} // namespace

// ---- ColorSwatch

ColorSwatch::ColorSwatch(Widget *parent, uint32_t rgb) : Widget(parent) {
        setFocusable(true);
        setColor(rgb);
}

void ColorSwatch::setColor(uint32_t rgb) {
        _rgb = rgb & 0xffffffu;
        ColorModel::formatHex(_rgb, _tip);
        update();
        return;
}

void ColorSwatch::setHint(uint16_t width, uint16_t height) {
        _hint = GuiSize{width, height};
        return;
}

GuiSize ColorSwatch::sizeHint() const {
        return _hint;
}

void ColorSwatch::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &r = geometry();
        drawBevel(painter, r, Bevel::Style::Field);
        Rect in = Bevel::inside(r, Bevel::Style::Field);
        if (focusShown(*this)) {
                // The focus: a ring in the highlight's color, the face's inside it, then the color.
                ring(painter, in, p.color(Role::Highlight));
                ring(painter, shrunk(in, 1), p.color(Role::Face));
                in = shrunk(in, 2);
        }
        (void)painter.fill(in, isEnabled() ? _rgb : p.color(Role::Face));
        return;
}

void ColorSwatch::click() {
        if (isEnabled() && _clicked != nullptr) _clicked(_clickedArg, *this);
        return;
}

void ColorSwatch::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left) return;
        _down = true;
        return;
}

void ColorSwatch::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || !_down) return;
        _down = false;
        const Rect &r = geometry();
        if (event.x >= r.x && event.y >= r.y && uint32_t(event.x) < r.right() && uint32_t(event.y) < r.bottom())
                click();
        return;
}

bool ColorSwatch::keyPressed(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || event.hasCtrl() || event.hasAlt()) return false;
        if (event.usage != HidKey::Space && event.usage != HidKey::Enter && event.usage != HidKey::KeypadEnter)
                return false;
        click();
        return true;
}

// ---- The square and the bar

namespace detail {

        /** @brief The color dialog's hue (across) and saturation (up) square, at full value. */
        class ColorPlane final : public Widget {
                public:
                        ColorPlane(Widget *parent, ColorDialog &dialog) : Widget(parent), _dialog(dialog) {
                                setFocusable(true);
                        }

                        GuiSize sizeHint() const override { return GuiSize{180, 128}; }

                        void paint(Painter &painter) override {
                                const WindowPalette &p = painter.palette();
                                const Rect          &r = geometry();
                                const Rect           g = area();
                                if (g.width < r.width)
                                        (void)painter.fill(
                                                Rect{uint16_t(g.right()), r.y, uint16_t(r.width - g.width), r.height},
                                                p.color(Role::Face));
                                if (g.isEmpty()) return;
                                drawGradient(painter, g, pixel, this);
                                // The mark: a black ring around a white one, inside the square.
                                const ColorModel::Hsv &hsv = _dialog._hsv;
                                int                    cx = g.x + int(uint32_t(hsv.hue) * g.width / 360);
                                int cy = g.y + int(uint32_t(255 - hsv.saturation) * (g.height - 1) / 255);
                                cx = cx < g.x + 4 ? g.x + 4 : cx > int(g.right()) - 5 ? int(g.right()) - 5 : cx;
                                cy = cy < g.y + 4 ? g.y + 4 : cy > int(g.bottom()) - 5 ? int(g.bottom()) - 5 : cy;
                                ring(painter, Rect{uint16_t(cx - 4), uint16_t(cy - 4), 9, 9}, 0x000000);
                                ring(painter, Rect{uint16_t(cx - 3), uint16_t(cy - 3), 7, 7}, 0xffffff);
                                if (focusShown(*this)) ring(painter, g, p.color(Role::Highlight));
                                return;
                        }

                        void mousePressed(const WindowManager::MouseEvent &event) override {
                                if (event.button != PointerEvent::Left) return;
                                _dragging = true;
                                pick(event.x, event.y);
                                return;
                        }

                        void mouseMoved(const WindowManager::MouseEvent &event) override {
                                if (_dragging) pick(event.x, event.y);
                                return;
                        }

                        void mouseReleased(const WindowManager::MouseEvent &event) override {
                                if (event.button == PointerEvent::Left) _dragging = false;
                                return;
                        }

                        void mouseLeft() override {
                                _dragging = false;
                                return;
                        }

                        bool keyPressed(const KeyEvent &event) override {
                                if (event.action == KeyEvent::Action::Release || event.hasCtrl() || event.hasAlt())
                                        return false;
                                const int       by = event.hasShift() ? 10 : 1;
                                ColorModel::Hsv hsv = _dialog._hsv;
                                int             hue = hsv.hue, sat = hsv.saturation;
                                switch (event.usage) {
                                        case HidKey::Left: hue = (hue + 360 - by) % 360; break;
                                        case HidKey::Right: hue = (hue + by) % 360; break;
                                        case HidKey::Up: sat = sat + by > 255 ? 255 : sat + by; break;
                                        case HidKey::Down: sat = sat - by < 0 ? 0 : sat - by; break;
                                        default: return false;
                                }
                                hsv.hue = uint16_t(hue);
                                hsv.saturation = uint8_t(sat);
                                _dialog.chooseHsv(hsv);
                                return true;
                        }

                private:
                        // The square's pixels: on 4-pixel columns.
                        Rect area() const {
                                const Rect &r = geometry();
                                return Rect{r.x, r.y, uint16_t(r.width & ~3u), r.height};
                        }

                        static uint32_t pixel(const void *owner, uint16_t x, uint16_t y) {
                                const Rect      g = static_cast<const ColorPlane *>(owner)->area();
                                ColorModel::Hsv hsv;
                                hsv.hue = uint16_t(uint32_t(x) * 360 / g.width);
                                hsv.saturation = uint8_t(255 - (g.height > 1 ? uint32_t(y) * 255 / (g.height - 1) : 0));
                                hsv.value = 255;
                                return ColorModel::toRgb(hsv);
                        }

                        void pick(int x, int y) {
                                const Rect g = area();
                                if (g.isEmpty()) return;
                                const int       dx = x < g.x ? 0 : x >= int(g.right()) ? g.width - 1 : x - g.x;
                                const int       dy = y < g.y ? 0 : y >= int(g.bottom()) ? g.height - 1 : y - g.y;
                                ColorModel::Hsv hsv = _dialog._hsv;
                                hsv.hue = uint16_t(uint32_t(dx) * 360 / g.width);
                                hsv.saturation =
                                        uint8_t(255 - (g.height > 1 ? uint32_t(dy) * 255 / (g.height - 1) : 0));
                                if (hsv.value == 0) hsv.value = 255; // black would hide what was picked
                                _dialog.chooseHsv(hsv);
                                return;
                        }

                        ColorDialog &_dialog;
                        bool         _dragging = false;
        };

        /** @brief The color dialog's value bar: the hue and saturation chosen, from full value down to black. */
        class ColorBar final : public Widget {
                public:
                        ColorBar(Widget *parent, ColorDialog &dialog) : Widget(parent), _dialog(dialog) {
                                setFocusable(true);
                        }

                        GuiSize sizeHint() const override { return GuiSize{BarMargin + BarStrip + 4, 128}; }

                        void paint(Painter &painter) override {
                                const WindowPalette &p = painter.palette();
                                const Rect          &r = geometry();
                                const Rect           s = strip();
                                (void)painter.fill(Rect{r.x, r.y, BarMargin, r.height}, p.color(Role::Face));
                                if (r.right() > s.right())
                                        (void)painter.fill(Rect{uint16_t(s.right()), r.y,
                                                                uint16_t(r.right() - s.right()), r.height},
                                                           p.color(Role::Face));
                                if (s.isEmpty()) return;
                                drawGradient(painter, s, pixel, this);
                                const int y = s.y + int(uint32_t(255 - _dialog._hsv.value) * (s.height - 1) / 255);
                                GuiStyle::drawArrow(painter, Rect{r.x, uint16_t(y - 3), BarMargin, 7},
                                                    GuiStyle::Arrow::Right, p.color(Role::FaceText));
                                if (focusShown(*this)) ring(painter, s, p.color(Role::Highlight));
                                return;
                        }

                        void mousePressed(const WindowManager::MouseEvent &event) override {
                                if (event.button != PointerEvent::Left) return;
                                _dragging = true;
                                pick(event.y);
                                return;
                        }

                        void mouseMoved(const WindowManager::MouseEvent &event) override {
                                if (_dragging) pick(event.y);
                                return;
                        }

                        void mouseReleased(const WindowManager::MouseEvent &event) override {
                                if (event.button == PointerEvent::Left) _dragging = false;
                                return;
                        }

                        void mouseLeft() override {
                                _dragging = false;
                                return;
                        }

                        bool keyPressed(const KeyEvent &event) override {
                                if (event.action == KeyEvent::Action::Release || event.hasCtrl() || event.hasAlt())
                                        return false;
                                const int       by = event.hasShift() ? 10 : 1;
                                ColorModel::Hsv hsv = _dialog._hsv;
                                int             v = hsv.value;
                                if (event.usage == HidKey::Up) {
                                        v = v + by > 255 ? 255 : v + by;
                                } else if (event.usage == HidKey::Down) {
                                        v = v - by < 0 ? 0 : v - by;
                                } else {
                                        return false;
                                }
                                hsv.value = uint8_t(v);
                                _dialog.chooseHsv(hsv);
                                return true;
                        }

                private:
                        Rect strip() const {
                                const Rect &r = geometry();
                                if (r.width < BarMargin + BarStrip) return Rect{r.x, r.y, 0, 0};
                                return Rect{uint16_t(r.x + BarMargin), r.y, BarStrip, r.height};
                        }

                        static uint32_t pixel(const void *owner, uint16_t x, uint16_t y) {
                                (void)x;
                                const auto     *self = static_cast<const ColorBar *>(owner);
                                const Rect      s = self->strip();
                                ColorModel::Hsv hsv = self->_dialog._hsv;
                                hsv.value = uint8_t(255 - (s.height > 1 ? uint32_t(y) * 255 / (s.height - 1) : 0));
                                return ColorModel::toRgb(hsv);
                        }

                        void pick(int y) {
                                const Rect s = strip();
                                if (s.isEmpty()) return;
                                const int       dy = y < s.y ? 0 : y >= int(s.bottom()) ? s.height - 1 : y - s.y;
                                ColorModel::Hsv hsv = _dialog._hsv;
                                hsv.value = uint8_t(255 - (s.height > 1 ? uint32_t(dy) * 255 / (s.height - 1) : 0));
                                _dialog.chooseHsv(hsv);
                                return;
                        }

                        ColorDialog &_dialog;
                        bool         _dragging = false;
        };

} // namespace detail

// ---- ColorDialog

ColorDialog::ColorDialog(GuiApplication &app, StringView title) : Dialog(app, title, ContentWidth, ContentHeight) {
        (void)build();
        (void)addButton("OK", Accepted);
        (void)addButton("Cancel", Rejected);
        setCancelResult(Rejected);
        setPresets(Span<const uint32_t>(StandardColors));
}

ColorDialog::~ColorDialog() = default;

Error ColorDialog::build() {
        BoxLayout *content = this->content();
        if (content == nullptr) return Error::NoMem;
        auto *top = Widget::create<BoxLayout>(content, BoxLayout::Direction::Horizontal);
        auto *presets = Widget::create<BoxLayout>(content, BoxLayout::Direction::Horizontal);
        if (top == nullptr || presets == nullptr) return Error::NoMem;
        top->setSpacing(8);
        top->setStretch(1);
        presets->setSpacing(4);
        _plane = Widget::create<detail::ColorPlane>(top, *this);
        _bar = Widget::create<detail::ColorBar>(top, *this);
        auto *side = Widget::create<BoxLayout>(top, BoxLayout::Direction::Vertical);
        if (_plane == nullptr || _bar == nullptr || side == nullptr) return Error::NoMem;
        _plane->setStretch(1);
        side->setSpacing(4);
        auto *newLabel = Widget::create<Label>(side, "New");
        _new = Widget::create<ColorSwatch>(side);
        auto *oldLabel = Widget::create<Label>(side, "Old");
        _old = Widget::create<ColorSwatch>(side);
        _hex = Widget::create<LineEdit>(side);
        if (newLabel == nullptr || _new == nullptr || oldLabel == nullptr || _old == nullptr || _hex == nullptr)
                return Error::NoMem;
        _new->setHint(88, 26);
        _new->setFocusable(false);
        _old->setHint(88, 26);
        _old->setFocusable(false);
        _old->setClicked(presetClicked, this); // the old color, taken back
        _hex->setMaxLength(7);
        _hex->setAccepted(hexTaken, this);
        _hex->setFocusLost(hexTaken, this);
        _hex->setCancelled(hexCancelled, this);
        for (ColorSwatch *&p : _presets) {
                p = Widget::create<ColorSwatch>(presets);
                if (p == nullptr) return Error::NoMem;
                p->setHint(20, 18);
                p->setFocusable(false);
                p->setClicked(presetClicked, this);
        }
        return Error::Ok;
}

Error ColorDialog::open(uint32_t rgb) {
        _original = rgb & 0xffffffu;
        if (_old != nullptr) _old->setColor(_original);
        _rgb = _original;
        _hsv = ColorModel::fromRgb(_rgb);
        shown(true);
        return Dialog::open();
}

void ColorDialog::setPresets(Span<const uint32_t> colors) {
        for (size_t i = 0; i < MaxPresets; ++i) {
                if (_presets[i] == nullptr) continue;
                _presets[i]->setVisible(i < colors.size());
                if (i < colors.size()) _presets[i]->setColor(colors[i]);
        }
        return;
}

void ColorDialog::choose(uint32_t rgb) {
        rgb &= 0xffffffu;
        ColorModel::Hsv hsv = ColorModel::fromRgb(rgb);
        // A grey has no hue, black no saturation either: kept from before, so the square stays put.
        if (hsv.saturation == 0 || hsv.value == 0) hsv.hue = _hsv.hue;
        if (hsv.value == 0) hsv.saturation = _hsv.saturation;
        _hsv = hsv;
        _rgb = rgb;
        shown(true);
        if (_changed != nullptr) _changed(_changedArg, *this, _rgb);
        return;
}

void ColorDialog::chooseHsv(const ColorModel::Hsv &hsv) {
        _hsv = hsv;
        _rgb = ColorModel::toRgb(hsv);
        shown(true);
        if (_changed != nullptr) _changed(_changedArg, *this, _rgb);
        return;
}

void ColorDialog::shown(bool hexToo) {
        if (_new != nullptr) _new->setColor(_rgb);
        if (hexToo && _hex != nullptr) {
                char text[8];
                ColorModel::formatHex(_rgb, text);
                _hex->setText(StringView(text));
        }
        if (_plane != nullptr) _plane->update();
        if (_bar != nullptr) _bar->update();
        return;
}

void ColorDialog::hexTaken(void *arg, LineEdit &edit) {
        auto                  *self = static_cast<ColorDialog *>(arg);
        const Result<uint32_t> rgb = ColorModel::parseHex(edit.text());
        if (rgb.isOk() && rgb.value() != self->_rgb) {
                self->choose(rgb.value());
        } else {
                self->shown(true); // put back
        }
        return;
}

void ColorDialog::hexCancelled(void *arg, LineEdit &) {
        static_cast<ColorDialog *>(arg)->shown(true);
        return;
}

void ColorDialog::presetClicked(void *arg, ColorSwatch &swatch) {
        static_cast<ColorDialog *>(arg)->choose(swatch.color());
        return;
}

BMDS_NAMESPACE_END
