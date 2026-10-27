/**
 * @file      volumecontrol.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The volume control, a tray icon in the panel's process, and its slider (devplan/audio.md §17,
 * devplan/gui.md §4s, devplan/mediaplayer.md §6e).
 */

#include <cstdio>
#include <new>
#include <bmds/bevel.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/menulayout.h>
#include <bmds/sysconf.h>

#include "volumecontrol.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr int32_t  MuteNotch = VolumeControl::FloorDb - 1; // the slider's bottom
        constexpr uint16_t PopupWidth = 96;                        // "-60 dBFS" and the label's padding
        constexpr uint16_t PopupHeight = 196;                      // the level, the slider, the Mute button
        constexpr uint16_t Margin = 6;                             // inside the popup's bevel
        constexpr uint16_t LevelHeight = 20;

        enum Choice : int {
                ChooseMute = 1,
                ChooseMixer,
                ChooseSettings,
        };

        GuiWindow::Config popupConfig() {
                GuiWindow::Config c;
                c.title = "Volume";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Popup;
                c.focusable = false;
                c.kind = "menu";
                return c;
        }

} // namespace

// ---- VolumePopup

VolumePopup::VolumePopup(GuiApplication &app, VolumeControl &control) : GuiWindow(app, popupConfig()), _item(control) {
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
        const uint16_t buttonTop = uint16_t(g.bottom() - inner - buttonHeight());
        if (_mute != nullptr)
                _mute->setGeometry(
                        Rect{uint16_t(g.x + inner), buttonTop, uint16_t(g.width - 2 * inner), buttonHeight()});
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

// ---- VolumeControl

VolumeControl::VolumeControl(GuiApplication &app) : _app(app), _icon(app), _menu(app) {
        _icon.setCategory(TrayIcon::Category::Hardware);
        _icon.setActivated(activated, this);
        _icon.setScrolled(scrolled, this);
        _icon.setContextMenu(&_menu);
        _menu.setTriggered(chosen, this);
        refresh();
}

VolumeControl::~VolumeControl() {
        if (_saveTimer >= 0) {
                _app.stopTimer(_saveTimer);
                save(this);
        }
        if (_popup != nullptr) {
                _popup->~VolumePopup();
                Memory::free(_popup);
        }
}

Error VolumeControl::start() {
        return _icon.show();
}

void VolumeControl::refresh() {
        if (auto [v, e] = SysConf::getInt(VolumeKey); e.isOk()) _db = v;
        if (auto [m, e] = SysConf::getBool(MuteKey); e.isOk()) _muted = m;
        if (_muted) {
                std::snprintf(_tip, sizeof(_tip), "Volume: muted");
        } else {
                std::snprintf(_tip, sizeof(_tip), "Volume: %ld dBFS", static_cast<long>(_db));
        }
        // The level's speaker: muted, quiet (no waves), middling (one), loud (two).
        _icon.setIcon(_muted || _db <= FloorDb ? StringView("audio-volume-muted")
                      : _db < LowDb            ? StringView("audio-volume-low")
                      : _db < HighDb           ? StringView("audio-volume-medium")
                                               : StringView("audio-volume-high"));
        _icon.setToolTip(StringView(_tip));
        fillMenu();
        if (_popup != nullptr && _popup->isShown()) _popup->refresh(_db, _muted);
        return;
}

void VolumeControl::fillMenu() {
        _menu.clear();
        (void)_menu.addItem("&Mute", ChooseMute, _muted ? MenuLayout::Checked : 0);
        (void)_menu.addSeparator();
        (void)_menu.addItem("Mi&xer...", ChooseMixer, MenuLayout::Disabled); // the mixer window, later
        if (_openSettings != nullptr) (void)_menu.addItem("Sound &Settings...", ChooseSettings);
        return;
}

void VolumeControl::set(const char *key, StringView value) {
        if (SysConf::set(StringView(key), value).isError()) return;
        // Kept across a restart: written to /cfg once the changes stop.
        if (_saveTimer >= 0) _app.stopTimer(_saveTimer);
        const Result<int> timer = _app.startTimer(SaveDelayMs, false, save, this);
        _saveTimer = timer.isOk() ? timer.value() : -1;
        return;
}

void VolumeControl::save(void *arg) {
        auto *self = static_cast<VolumeControl *>(arg);
        self->_saveTimer = -1;
        (void)SysConf::save(StringView(VolumeKey));
        (void)SysConf::save(StringView(MuteKey));
        return;
}

void VolumeControl::setLevel(int32_t db) {
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

void VolumeControl::setMuted(bool muted) {
        if (muted != _muted) set(MuteKey, muted ? "true" : "false"); // the level is kept meanwhile
        return;
}

void VolumeControl::openPopup() {
        if (_popup == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(VolumePopup));
                if (mem == nullptr) return;
                _popup = ::new (mem) VolumePopup(_app, *this);
        }
        if (_popup->isShown()) {
                _popup->close(); // a second click closes it
                return;
        }
        const Rect at = _icon.geometry();
        if (at.width == 0) return; // the tray hasn't said where it is yet
        _popup->refresh(_db, _muted);
        _popup->open(at);
        return;
}

void VolumeControl::activated(void *arg, TrayIcon &, TrayIcon::Reason reason) {
        auto *self = static_cast<VolumeControl *>(arg);
        if (reason == TrayIcon::Reason::Trigger) {
                self->openPopup();
        } else if (reason == TrayIcon::Reason::MiddleClick) {
                self->setMuted(!self->_muted);
        }
        return; // Context: its menu, opened by the icon
}

void VolumeControl::scrolled(void *arg, TrayIcon &, int notches) {
        auto *self = static_cast<VolumeControl *>(arg);
        if (self->_muted && notches < 0) return; // silent already
        const int32_t db = self->_db + notches;
        self->setLevel(db < FloorDb ? FloorDb : db); // up from muted unmutes
        return;
}

void VolumeControl::chosen(void *arg, int item) {
        auto *self = static_cast<VolumeControl *>(arg);
        if (item == ChooseMute) self->setMuted(!self->_muted);
        if (item == ChooseSettings && self->_openSettings != nullptr)
                self->_openSettings(self->_openSettingsArg, "sound");
        return;
}

BMDS_NAMESPACE_END
