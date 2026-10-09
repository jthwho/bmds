/**
 * @file      volumeitem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's volume item and its slider (devplan/audio.md §17, devplan/gui.md §4s).
 */

#include <cstdio>
#include <new>
#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/menulayout.h>
#include <bmds/sysconf.h>

#include "volumeitem.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr int32_t  MuteNotch = VolumeItem::FloorDb - 1; // the slider's bottom
        constexpr uint16_t PopupWidth = 96;                     // "-60 dBFS" and the label's padding
        constexpr uint16_t PopupHeight = 196;                   // the level, the slider, the Mute button
        constexpr uint16_t Margin = 6;                          // inside the popup's bevel
        constexpr uint16_t LevelHeight = 20;
        constexpr uint32_t MutedRed = 0xd02020; // the circle and slash over a muted speaker

        GuiWindow::Config popupConfig() {
                GuiWindow::Config c;
                c.title = "Volume";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Popup;
                c.focusable = false;
                c.kind = "menu";
                return c;
        }

        // The red circle and slash over a 16-pixel icon at (@p x, @p y): a run of pixels a row
        // at a time, from the circle's ring (radius 6.5 to 8) and the band along its diagonal.
        void drawMuted(Painter &painter, int x, int y) {
                Rect   runs[48];
                size_t n = 0;
                for (int row = 0; row < 16; ++row) {
                        int start = -1;
                        for (int col = 0; col <= 16; ++col) {
                                bool red = false;
                                if (col < 16) {
                                        const int dx = 2 * col - 15; // doubled, from the centre (7.5, 7.5)
                                        const int dy = 2 * row - 15;
                                        const int d2 = dx * dx + dy * dy;
                                        const int diag = dx - dy; // the top-left to bottom-right line
                                        red = (d2 >= 13 * 13 && d2 <= 16 * 16) ||
                                              (d2 < 13 * 13 && diag >= -3 && diag <= 3);
                                }
                                if (red && start < 0) start = col;
                                if (!red && start >= 0) {
                                        if (n < sizeof(runs) / sizeof(runs[0]))
                                                runs[n++] = Rect{uint16_t(x + start), uint16_t(y + row),
                                                                 uint16_t(col - start), 1};
                                        start = -1;
                                }
                        }
                }
                (void)painter.fill(Span<const Rect>(runs, n), MutedRed);
                return;
        }

} // namespace

// ---- VolumePopup

VolumePopup::VolumePopup(GuiApplication &app, VolumeItem &item) : GuiWindow(app, popupConfig()), _item(item) {
        _level = Widget::create<Label>(this);
        if (_level != nullptr) _level->setAlign(Label::Align::Center);
        _slider = Widget::create<Slider>(this, Slider::Orientation::Vertical);
        if (_slider != nullptr) {
                _slider->setRange(MuteNotch, 0);
                _slider->setSteps(1, 6);
                _slider->setChanged(changed, this);
                setFocusWidget(_slider);
        }
        _mute = Widget::create<Button>(this, StringView("&Mute"));
        if (_mute != nullptr) {
                _mute->setCentered(true);
                _mute->setClicked(muteClicked, this);
        }
}

void VolumePopup::open(const Rect &anchor) {
        const Rect screen = application().windowManager().screen();
        (void)show(MenuLayout::place(PopupWidth, PopupHeight, anchor, screen, false));
        if (_slider != nullptr) setFocusWidget(_slider);
        return;
}

void VolumePopup::refresh(int32_t db, bool muted) {
        const int32_t v = muted ? MuteNotch : db;
        if (_slider != nullptr) _slider->setValue(v);
        if (_mute != nullptr) _mute->setChecked(muted);
        showLevel(v);
        return;
}

void VolumePopup::muteClicked(void *arg, Button &) {
        auto *self = static_cast<VolumePopup *>(arg);
        self->_item.setMuted(!self->_item.isMuted()); // the settings' change refreshes it
        return;
}

void VolumePopup::showLevel(int32_t value) {
        if (_level == nullptr) return;
        char text[16];
        if (value <= MuteNotch) {
                std::snprintf(text, sizeof(text), "Mute");
        } else {
                std::snprintf(text, sizeof(text), "%ld dBFS", static_cast<long>(value));
        }
        _level->setText(StringView(text));
        return;
}

void VolumePopup::changed(void *arg, Slider &, int32_t value) {
        auto *self = static_cast<VolumePopup *>(arg);
        self->showLevel(value);
        self->_item.setLevel(value);
        return;
}

void VolumePopup::resized() {
        const Rect    &g = geometry();
        const uint16_t inner = Bevel::width(Bevel::Style::Window) + Margin;
        if (_level != nullptr)
                _level->setGeometry(
                        Rect{uint16_t(g.x + inner), uint16_t(g.y + inner), uint16_t(g.width - 2 * inner), LevelHeight});
        const uint16_t buttonTop = uint16_t(g.bottom() - inner - GuiStyle::ButtonHeight);
        if (_mute != nullptr)
                _mute->setGeometry(
                        Rect{uint16_t(g.x + inner), buttonTop, uint16_t(g.width - 2 * inner), GuiStyle::ButtonHeight});
        if (_slider != nullptr) {
                const uint16_t top = uint16_t(g.y + inner + LevelHeight + Margin);
                const uint16_t bottom = uint16_t(buttonTop - Margin);
                _slider->setGeometry(Rect{uint16_t(g.x + (g.width - Slider::Breadth) / 2), top, Slider::Breadth,
                                          uint16_t(bottom > top ? bottom - top : 0)});
        }
        return;
}

void VolumePopup::paint(Painter &painter) {
        const Rect &g = geometry();
        drawBevel(painter, g, Bevel::Style::Window);
        (void)painter.fill(Bevel::inside(g, Bevel::Style::Window), painter.palette().color(Role::Face));
        return;
}

bool VolumePopup::wheelTurned(int notches) {
        return _slider != nullptr && _slider->wheelTurned(notches);
}

bool VolumePopup::keyUnused(const KeyEvent &event) {
        if (event.usage != HidKey::Escape && event.usage != HidKey::Enter) return false;
        close();
        _item.popupClosed();
        return true;
}

void VolumePopup::closedByManager() {
        GuiWindow::closedByManager();
        _item.popupClosed();
        return;
}

// ---- VolumeItem

VolumeItem::VolumeItem(Widget *parent, GuiApplication &app) : Widget(parent), _app(app) {
        refresh();
}

VolumeItem::~VolumeItem() {
        if (_saveTimer >= 0) {
                _app.stopTimer(_saveTimer);
                save(this);
        }
        if (_popup != nullptr) {
                _popup->~VolumePopup();
                Memory::free(_popup);
        }
}

void VolumeItem::refresh() {
        if (auto [v, e] = SysConf::getInt(VolumeKey); e.isOk()) _db = v;
        if (auto [m, e] = SysConf::getBool(MuteKey); e.isOk()) _muted = m;
        if (_muted) {
                std::snprintf(_tip, sizeof(_tip), "Volume: muted");
        } else {
                std::snprintf(_tip, sizeof(_tip), "Volume: %ld dBFS", static_cast<long>(_db));
        }
        update();
        _app.toolTipChanged(this);
        if (_popup != nullptr && _popup->isShown()) _popup->refresh(_db, _muted);
        return;
}

void VolumeItem::set(const char *key, StringView value) {
        if (SysConf::set(StringView(key), value).isError()) return;
        // Kept across a restart: written to /boot once the changes stop.
        if (_saveTimer >= 0) _app.stopTimer(_saveTimer);
        const Result<int> timer = _app.startTimer(SaveDelayMs, false, save, this);
        _saveTimer = timer.isOk() ? timer.value() : -1;
        return;
}

void VolumeItem::save(void *arg) {
        auto *self = static_cast<VolumeItem *>(arg);
        self->_saveTimer = -1;
        (void)SysConf::save(StringView(VolumeKey));
        (void)SysConf::save(StringView(MuteKey));
        return;
}

void VolumeItem::setLevel(int32_t db) {
        if (db < FloorDb) {
                if (!_muted) set(MuteKey, "true"); // the level is kept for unmuting
                return;
        }
        if (db > 0) db = 0;
        if (db != _db) {
                char text[16];
                std::snprintf(text, sizeof(text), "%ld", static_cast<long>(db));
                set(VolumeKey, StringView(text));
        }
        if (_muted) set(MuteKey, "false");
        return;
}

void VolumeItem::setMuted(bool muted) {
        if (muted != _muted) set(MuteKey, muted ? "true" : "false"); // the level is kept meanwhile
        return;
}

GuiSize VolumeItem::sizeHint() const {
        return GuiSize{GuiStyle::ButtonHeight, GuiStyle::ButtonHeight};
}

void VolumeItem::paint(Painter &painter) {
        const Rect    &g = geometry();
        const uint32_t face = painter.palette().color(Role::Face);
        const bool     open = _popup != nullptr && _popup->isShown();
        (void)painter.fill(g, face);
        // Flat, as the classic trays: a thin edge under the pointer, sunk while its slider is open.
        if (open) {
                drawBevel(painter, g, Bevel::Style::Status);
        } else if (_over) {
                drawBevel(painter, g, Bevel::Style::Thin);
        }
        const int x = g.x + (g.width - GuiStyle::IconSize) / 2;
        const int y = g.y + (g.height - GuiStyle::IconSize) / 2;
        // The freedesktop.org name, else the themes' own speaker (installed before the aliases).
        const Icon *icon = _app.icons().find("audio-volume-high", GuiStyle::IconSize);
        if (icon == nullptr) icon = _app.icons().find("speaker", GuiStyle::IconSize);
        if (icon != nullptr) {
                (void)painter.image(x, y, *icon, face);
        } else {
                (void)painter.text(x, y, StringView("Vol"), Fonts::unscii16, painter.palette().color(Role::FaceText),
                                   face, uint16_t(g.width));
        }
        if (_muted) drawMuted(painter, x, y);
        return;
}

void VolumeItem::openPopup() {
        if (_popup == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(VolumePopup));
                if (mem == nullptr) return;
                _popup = ::new (mem) VolumePopup(_app, *this);
        }
        if (_popup->isShown()) {
                _popup->close(); // a second click closes it
                update();
                return;
        }
        GuiWindow *w = window();
        if (w == nullptr) return;
        const Rect s = w->screenRect();
        const Rect g = geometry();
        _popup->refresh(_db, _muted);
        _popup->open(Rect{uint16_t(s.x + g.x), uint16_t(s.y + g.y), g.width, g.height});
        update();
        return;
}

void VolumeItem::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button == PointerEvent::Middle) {
                setMuted(!_muted);
                return;
        }
        if (event.button == PointerEvent::Left || event.button == PointerEvent::Right) openPopup();
        return;
}

void VolumeItem::mouseMoved(const WindowManager::MouseEvent &) {
        if (!_over) {
                _over = true;
                update();
        }
        return;
}

void VolumeItem::mouseLeft() {
        _over = false;
        update();
        return;
}

bool VolumeItem::wheelTurned(int notches) {
        if (_muted && notches < 0) return true; // silent already
        const int32_t db = _db + notches;
        setLevel(db < FloorDb ? FloorDb : db); // up from muted unmutes
        return true;
}

BMDS_NAMESPACE_END
