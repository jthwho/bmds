/**
 * @file      notificationserver.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's notification server (devplan/notifications.md §3.4): balloons by the tray, a sound
 * by urgency, and the centre (a tray icon and its list).
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/audiodevice.h>
#include <bmds/bevel.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/bitmapfont.h>
#include <bmds/icon.h>
#include <bmds/inputevent.h>
#include <bmds/lineeditmodel.h>
#include <bmds/memory.h>
#include <bmds/menulayout.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/windowstack.h>

#include "notificationserver.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Pad = 8;
        constexpr uint16_t BigIcon = 32;
        constexpr uint16_t SmallIcon = 16;
        constexpr uint16_t CloseSide = 16;
        constexpr uint16_t Edge = 2; // a critical one's red edge
        constexpr uint32_t CriticalColor = 0xc00000;
        constexpr uint32_t UnreadColor = 0xd02020;
        constexpr uint16_t Align = WindowStack::Align;
        constexpr char     CloseGlyph[] = "\xee\x80\x84"; // IconGlyph::Close, U+E004

        BMDS_SYSCONF(notifySound, "notify/sound", Config::Type::Bool, "true",
                     "Normal and critical notifications play the system's notify sound", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(notifyTimeout, "notify/timeout", Config::Type::Int, "6000",
                     "How long a notification's balloon shows, in ms, unless it says", 1000, 60000, nullptr, nullptr)
        BMDS_SYSCONF(notifyBalloons, "notify/balloons", Config::Type::Bool, "true",
                     "Notifications show balloons (false: only in the centre)", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(notifyQuiet, "notify/quiet", Config::Type::Bool, "false",
                     "Do Not Disturb until turned off: no balloons or sounds but critical ones", 0, 0, nullptr, nullptr)

        // Balloon Time's choices, in seconds.
        constexpr uint32_t BalloonSeconds[] = {3, 6, 10, 20};

        int64_t monotonicMs() {
                return int64_t(Clock::monotonicNs() / 1000000);
        }

        GuiWindow::Config balloonConfig() {
                GuiWindow::Config c;
                c.title = "Notification";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Notification;
                c.focusable = false;
                c.focusOnPress = false;
                c.kind = "notification";
                return c;
        }

        GuiWindow::Config centreConfig() {
                GuiWindow::Config c;
                c.title = "Notifications";
                c.decorations = WindowFrame::None;
                c.level = WindowManager::Level::Popup;
                c.focusable = false;
                c.kind = "menu";
                return c;
        }

        // "12 s", "3 min", "2 h", "4 d" ago.
        void ago(char (&out)[16], int64_t ns) {
                const int64_t s = ns > 0 ? ns / 1000000000 : 0;
                if (s < 60)
                        std::snprintf(out, sizeof(out), "now");
                else if (s < 3600)
                        std::snprintf(out, sizeof(out), "%" PRId64 " min", s / 60);
                else if (s < 86400)
                        std::snprintf(out, sizeof(out), "%" PRId64 " h", s / 3600);
                else
                        std::snprintf(out, sizeof(out), "%" PRId64 " d", s / 86400);
                return;
        }

        StringView firstLine(const char *text) {
                const StringView t(text);
                const size_t     nl = t.indexOf('\n');
                return nl == StringView::NPos ? t : t.left(nl);
        }

        // @p label with each & doubled: a button takes & as its key's mark (gui.md §4r).
        void literal(char (&out)[2 * Notification::LabelBytes], const char *label) {
                size_t n = 0;
                for (const char *c = label; *c != '\0' && n + 2 < sizeof(out); ++c) {
                        if (*c == '&') out[n++] = '&';
                        out[n++] = *c;
                }
                out[n] = '\0';
                return;
        }

        uint16_t textX() {
                return Pad + BigIcon + Pad;
        }

} // namespace

// ---- NotificationBalloon

NotificationBalloon::NotificationBalloon(GuiApplication &app, NotificationServer &server)
    : GuiWindow(app, balloonConfig()), _server(server) {}

uint16_t NotificationBalloon::setNotification(const Notification &n) {
        _n = n;
        _id = n.id;
        _changed = n.changed;
        _icon = _server.iconOf(_n, BigIcon);
        const uint16_t width = uint16_t(Width - textX() - Pad);
        _lineCount = TextWrap::wrap(StringView(_n.body), uint16_t(width / 8), Span<StringView>(_lines, MaxLines));
        for (Button *&b : _buttons) {
                if (b != nullptr) b->destroy();
                b = nullptr;
        }
        for (size_t i = 0; i < _n.actionCount; ++i) {
                char label[2 * Notification::LabelBytes];
                literal(label, _n.actions[i]);
                _buttons[i] = Widget::create<Button>(this, StringView(label));
                if (_buttons[i] == nullptr) continue;
                _buttons[i]->setData(uint32_t(i + 1));
                _buttons[i]->setClicked(actionClicked, this);
        }
        const uint16_t text = uint16_t((2 + _lineCount) * textHeight());
        uint16_t       h = uint16_t(Pad + (text > BigIcon ? text : BigIcon) + Pad);
        if (_n.actionCount > 0) h = uint16_t(h + buttonHeight() + Pad);
        _height = h;
        resized();
        update();
        return h;
}

Rect NotificationBalloon::closeRect() const {
        const Rect &g = geometry();
        return Rect{uint16_t(g.right() - Pad - CloseSide), uint16_t(g.y + Pad), CloseSide, CloseSide};
}

void NotificationBalloon::resized() {
        const Rect &g = geometry();
        // The buttons at the bottom right, each as wide as its label.
        uint16_t right = uint16_t(g.right() - Pad);
        for (size_t i = Notification::MaxActions; i-- > 0;) {
                Button *b = _buttons[i];
                if (b == nullptr) continue;
                const uint16_t w =
                        uint16_t((font().textWidth(StringView(_n.actions[i])) + 24 + Align - 1) & ~uint32_t(Align - 1));
                right = uint16_t(right - w);
                b->setGeometry(Rect{right, uint16_t(g.bottom() - Pad - buttonHeight()), w, buttonHeight()});
                right = uint16_t(right - Align);
        }
        return;
}

void NotificationBalloon::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        const uint32_t       face = p.color(Role::Face);
        drawBevel(painter, g, Bevel::Style::Window);
        Rect in = Bevel::inside(g, Bevel::Style::Window);
        if (_n.urgency == Notification::Urgency::Critical) {
                const Rect edges[] = {
                        {in.x, in.y, in.width, Edge},
                        {in.x, uint16_t(in.bottom() - Edge), in.width, Edge},
                        {in.x, uint16_t(in.y + Edge), Edge, uint16_t(in.height - 2 * Edge)},
                        {uint16_t(in.right() - Edge), uint16_t(in.y + Edge), Edge, uint16_t(in.height - 2 * Edge)}};
                (void)painter.fill(Span<const Rect>(edges), CriticalColor);
                in = Rect{uint16_t(in.x + Edge), uint16_t(in.y + Edge), uint16_t(in.width - 2 * Edge),
                          uint16_t(in.height - 2 * Edge)};
        }
        (void)painter.fill(in, face);
        if (_icon != nullptr) (void)painter.image(uint16_t(g.x + Pad), uint16_t(g.y + Pad), *_icon, face);
        const uint16_t x = uint16_t(g.x + textX());
        const Rect     close = closeRect();
        const uint16_t room = uint16_t(close.x > x + Pad ? close.x - x - Pad : 0);
        uint16_t       y = uint16_t(g.y + Pad);
        (void)painter.text(x, y, StringView(_n.app), font(), p.color(Role::DisabledText), face, room);
        (void)painter.text(close.x, close.y, StringView(CloseGlyph), Fonts::icons16, p.color(Role::FaceText), face,
                           CloseSide);
        y = uint16_t(y + textHeight());
        (void)painter.text(x, y, StringView(_n.title), font(), p.color(Role::FaceText), face, room);
        const uint16_t width = uint16_t(g.right() - Pad - x);
        for (size_t i = 0; i < _lineCount; ++i) {
                y = uint16_t(y + textHeight());
                (void)painter.text(x, y, _lines[i], font(), p.color(Role::FaceText), face, width);
        }
        return;
}

void NotificationBalloon::mousePressed(const WindowManager::MouseEvent &event) {
        if (_id == 0) return;
        const Rect close = closeRect();
        const bool onClose = event.x >= close.x && event.x < int(close.right()) && event.y >= close.y &&
                             event.y < int(close.bottom());
        if (event.button == PointerEvent::Left && !onClose) {
                _server.activate(_id, 0, true);
        } else if (event.button == PointerEvent::Left || event.button == PointerEvent::Right) {
                _server.dismiss(_id); // the close box, or a right click anywhere
        }
        return;
}

void NotificationBalloon::pointerEntered() {
        _hovered = true;
        _server.hoverChanged(*this, true);
        return;
}

void NotificationBalloon::pointerLeft() {
        _hovered = false;
        _server.hoverChanged(*this, false);
        return;
}

void NotificationBalloon::actionClicked(void *arg, Button &button) {
        auto *self = static_cast<NotificationBalloon *>(arg);
        if (self->_id != 0) self->_server.activate(self->_id, uint32_t(button.data()), true);
        return;
}

// ---- NotificationCentre

NotificationCentre::NotificationCentre(GuiApplication &app, NotificationServer &server)
    : GuiWindow(app, centreConfig()), _server(server) {
        _clear = Widget::create<Button>(this, StringView("C&lear All"));
        if (_clear != nullptr) {
                _clear->setCentered(true);
                _clear->setClicked(clearClicked, this);
        }
}

uint16_t NotificationCentre::rowsTop() const {
        return uint16_t(geometry().y + Bevel::width(Bevel::Style::Window) + Pad / 2);
}

size_t NotificationCentre::visibleRows() const {
        const Rect    &g = geometry();
        const uint16_t bottom = uint16_t(g.bottom() - Bevel::width(Bevel::Style::Window) - Pad - buttonHeight() - Pad);
        return bottom > rowsTop() ? size_t(bottom - rowsTop()) / rowHeight() : 0;
}

void NotificationCentre::open(const Rect &anchor) {
        refresh();
        _first = 0;
        const Rect     screen = application().windowManager().screen();
        const size_t   fit = (screen.height / 2) / rowHeight();
        const size_t   rows = _count == 0 ? 1 : _count < fit ? _count : fit;
        const uint16_t h = uint16_t(2 * Bevel::width(Bevel::Style::Window) + Pad / 2 + rows * rowHeight() + Pad +
                                    buttonHeight() + Pad);
        (void)show(MenuLayout::place(Width, h, anchor, screen, false));
        return;
}

void NotificationCentre::refresh() {
        const size_t total = Notifications::ids(Span<uint32_t>(_ids, NotificationLog::Capacity));
        _count = total < NotificationLog::Capacity ? total : NotificationLog::Capacity;
        if (_first >= _count) _first = _count > 0 ? _count - 1 : 0;
        if (_clear != nullptr) _clear->setEnabled(_count > 0);
        update();
        return;
}

void NotificationCentre::resized() {
        const Rect    &g = geometry();
        const uint16_t inner = uint16_t(Bevel::width(Bevel::Style::Window) + Pad);
        if (_clear != nullptr)
                _clear->setGeometry(Rect{uint16_t(g.right() - inner - 112),
                                         uint16_t(g.bottom() - inner - buttonHeight()), 112, buttonHeight()});
        return;
}

void NotificationCentre::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        const uint32_t       face = p.color(Role::Face);
        drawBevel(painter, g, Bevel::Style::Window);
        (void)painter.fill(Bevel::inside(g, Bevel::Style::Window), face);
        // Text and icons are drawn from 4-pixel columns.
        const uint16_t left =
                uint16_t((g.x + Bevel::width(Bevel::Style::Window) + Pad / 2 + Align - 1) & ~uint32_t(Align - 1));
        const uint16_t right = uint16_t(g.right() - Bevel::width(Bevel::Style::Window) - Pad / 2);
        uint16_t       y = rowsTop();
        if (_count == 0) {
                (void)painter.text(uint16_t(left + Pad), uint16_t(y + Pad / 2), StringView("No notifications"), font(),
                                   p.color(Role::DisabledText), face, uint16_t(right - left));
                return;
        }
        const int64_t now = Clock::now().ns;
        const size_t  rows = visibleRows();
        for (size_t r = 0; r < rows && _first + r < _count; ++r, y = uint16_t(y + rowHeight())) {
                Notification n;
                if (Notifications::get(_ids[_first + r], n).isError()) continue;
                if (r > 0)
                        (void)painter.fill(Rect{left, uint16_t(y - 2), uint16_t(right - left), 1},
                                           p.color(Role::FaceShadow));
                if (!n.read) (void)painter.fill(Rect{left, uint16_t(y + 6), 4, 4}, UnreadColor);
                const Icon    *icon = _server.iconOf(n, SmallIcon);
                const uint16_t ix = uint16_t(left + 8);
                if (icon != nullptr) (void)painter.image(ix, uint16_t(y + 2), *icon, face);
                const uint16_t tx = uint16_t(ix + SmallIcon + Pad);
                char           when[16];
                ago(when, now - n.postedNs);
                const uint16_t whenWidth = uint16_t(font().textWidth(StringView(when)));
                const uint16_t closeX = uint16_t((right - CloseSide) & ~uint32_t(Align - 1));
                const uint16_t whenX = uint16_t((closeX - Pad - whenWidth) & ~uint32_t(Align - 1));
                (void)painter.text(tx, uint16_t(y + 2), StringView(n.title[0] != '\0' ? n.title : n.app), font(),
                                   p.color(Role::FaceText), face, uint16_t(whenX > tx + Pad ? whenX - tx - Pad : 0));
                (void)painter.text(whenX, uint16_t(y + 2), StringView(when), font(), p.color(Role::DisabledText), face,
                                   whenWidth);
                (void)painter.text(closeX, uint16_t(y + 2), StringView(CloseGlyph), Fonts::icons16,
                                   p.color(Role::FaceText), face, CloseSide);
                (void)painter.text(tx, uint16_t(y + 2 + textHeight()), firstLine(n.body), font(),
                                   p.color(Role::DisabledText), face, uint16_t(right - tx));
        }
        return;
}

void NotificationCentre::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || _count == 0 || event.y < rowsTop()) return;
        const size_t r = size_t(event.y - rowsTop()) / rowHeight();
        if (r >= visibleRows() || _first + r >= _count) return;
        const uint32_t id = _ids[_first + r];
        const Rect    &g = geometry();
        const uint16_t right = uint16_t(g.right() - Bevel::width(Bevel::Style::Window) - Pad / 2);
        const uint16_t closeX = uint16_t((right - CloseSide) & ~uint32_t(Align - 1));
        if (event.x >= closeX) {
                _server.remove(id); // the list follows (the change)
                return;
        }
        closeCentre();
        _server.activate(id, 0, false);
        return;
}

void NotificationCentre::closeCentre() {
        close();
        _server.centreClosed();
        return;
}

void NotificationCentre::closedByManager() {
        GuiWindow::closedByManager(); // a press outside it
        _server.centreClosed();
        return;
}

bool NotificationCentre::wheelTurned(int notches) {
        const size_t rows = visibleRows();
        if (_count <= rows) return true;
        const int    next = int(_first) - notches;
        const size_t last = _count - rows;
        _first = next < 0 ? 0 : size_t(next) > last ? last : size_t(next);
        update();
        return true;
}

bool NotificationCentre::keyUnused(const KeyEvent &event) {
        if (event.usage != HidKey::Escape) return false;
        closeCentre();
        return true;
}

void NotificationCentre::clearClicked(void *arg, Button &) {
        static_cast<NotificationCentre *>(arg)->_server.clearAll();
        return;
}

// ---- NotificationServer

NotificationServer::NotificationServer(GuiApplication &app, const Targets &targets)
    : _app(app), _targets(targets), _icon(app), _menu(app), _timeMenu(app), _quietMenu(app) {
        _icon.setCategory(TrayIcon::Category::System);
        _icon.setActivated(trayActivated, this);
        _icon.setContextMenu(&_menu);
        _menu.setTriggered(chosen, this); // the submenus' choices too
}

NotificationServer::~NotificationServer() {
        if (_started) Notifications::clearServer(this);
        _app.cancelPosts(this);
        if (_quietTimer >= 0) _app.stopTimer(_quietTimer);
        for (Shown &s : _shown) {
                stopTimer(s);
                if (s.balloon == nullptr) continue;
                s.balloon->~NotificationBalloon();
                Memory::free(s.balloon);
        }
        if (_centre != nullptr) {
                _centre->~NotificationCentre();
                Memory::free(_centre);
        }
}

Error NotificationServer::start() {
        settingsChanged();
        const Error err = Notifications::setServer(changedFromKernel, this);
        if (err.isError()) return err;
        _started = true;
        sync(); // those posted before there was a server
        return Error::Ok;
}

void NotificationServer::settingsChanged() {
        const Result<bool> sound = SysConf::getBool(StringView(bmdsConf_notifySound.key));
        _sound = !sound.isOk() || sound.value();
        const Result<bool> balloons = SysConf::getBool(StringView(bmdsConf_notifyBalloons.key));
        _balloons = !balloons.isOk() || balloons.value();
        const Result<int32_t> timeout = SysConf::getInt(StringView(bmdsConf_notifyTimeout.key));
        _timeoutMs = timeout.isOk() ? timeout.value() : 6000;
        const Result<bool> quiet = SysConf::getBool(StringView(bmdsConf_notifyQuiet.key));
        _quietForever = quiet.isOk() && quiet.value();
        fillMenu();
        _unread = SIZE_MAX; // the icon drawn again (quiet or not)
        if (_started) sync();
        return;
}

// ---- The centre's menu: the settings and Do Not Disturb (devplan/notifications.md §3.4)

bool NotificationServer::isQuiet() const {
        return _quietForever || (_quietUntilMs != 0 && monotonicMs() < _quietUntilMs);
}

void NotificationServer::fillMenu() {
        if (_menu.isShown() || _timeMenu.isShown() || _quietMenu.isShown()) return; // open: made again next time
        const auto checked = [](bool on) {
                return on ? uint8_t(MenuLayout::Checked) : uint8_t(0);
        };
        _timeMenu.clear();
        for (size_t i = 0; i < sizeof(BalloonSeconds) / sizeof(BalloonSeconds[0]); ++i) {
                char label[24];
                std::snprintf(label, sizeof(label), "&%u Seconds", unsigned(BalloonSeconds[i]));
                (void)_timeMenu.addItem(StringView(label), ChooseTime3 + int(i),
                                        checked(uint32_t(_timeoutMs) == BalloonSeconds[i] * 1000));
        }
        const bool timed = !_quietForever && isQuiet();
        _quietMenu.clear();
        (void)_quietMenu.addItem("For &10 Minutes", ChooseQuiet10m);
        (void)_quietMenu.addItem("For &1 Hour", ChooseQuiet1h);
        (void)_quietMenu.addItem("For &8 Hours", ChooseQuiet8h);
        (void)_quietMenu.addItem("For &24 Hours", ChooseQuiet24h);
        (void)_quietMenu.addItem("&Until Turned Off", ChooseQuietOn, checked(_quietForever));
        (void)_quietMenu.addSeparator();
        (void)_quietMenu.addItem("&Turn Off", ChooseQuietOff, _quietForever || timed ? 0 : MenuLayout::Disabled);
        _menu.clear();
        (void)_menu.addItem("Show &Balloons", ChooseBalloons, checked(_balloons));
        (void)_menu.addItem("Play &Sounds", ChooseSound, checked(_sound));
        (void)_menu.addSubmenu("Balloon &Time", _timeMenu);
        (void)_menu.addSeparator();
        (void)_menu.addSubmenu(isQuiet() ? "&Do Not Disturb (on)" : "&Do Not Disturb", _quietMenu);
        (void)_menu.addSeparator();
        (void)_menu.addItem("C&lear All", ChooseClear,
                            Notifications::ids(Span<uint32_t>()) > 0 ? 0 : MenuLayout::Disabled);
        return;
}

// Set and kept across a restart; the panel's watch on notify/... brings it back here.
void NotificationServer::save(const char *key, StringView value) {
        if (SysConf::set(StringView(key), value).isOk()) (void)SysConf::save(StringView(key));
        return;
}

// Do Not Disturb for @p minutes (counted here: the real-time clock may not be set), or off.
void NotificationServer::quietFor(uint32_t minutes) {
        if (_quietTimer >= 0) _app.stopTimer(_quietTimer);
        _quietTimer = -1;
        _quietUntilMs = 0;
        if (minutes == 0) {
                if (_quietForever) save(QuietKey, "false"); // settingsChanged() follows
        } else {
                _quietUntilMs = monotonicMs() + int64_t(minutes) * 60000;
                const Result<int> timer = _app.startTimer(minutes * 60000u, false, quietEnded, this);
                _quietTimer = timer.isOk() ? timer.value() : -1;
                if (_quietForever) save(QuietKey, "false"); // timed instead
        }
        fillMenu();
        _unread = SIZE_MAX;
        showCentreIcon();
        return;
}

void NotificationServer::quietEnded(void *arg) {
        auto *self = static_cast<NotificationServer *>(arg);
        self->_quietTimer = -1;
        self->_quietUntilMs = 0;
        self->fillMenu();
        self->_unread = SIZE_MAX;
        self->showCentreIcon();
        return;
}

void NotificationServer::chosen(void *arg, int item) {
        auto *self = static_cast<NotificationServer *>(arg);
        switch (item) {
                case ChooseBalloons: self->save(BalloonsKey, self->_balloons ? "false" : "true"); break;
                case ChooseSound: self->save(SoundKey, self->_sound ? "false" : "true"); break;
                case ChooseTime3:
                case ChooseTime6:
                case ChooseTime10:
                case ChooseTime20: {
                        char ms[16];
                        std::snprintf(ms, sizeof(ms), "%u", unsigned(BalloonSeconds[item - ChooseTime3] * 1000));
                        self->save(TimeoutKey, StringView(ms));
                        break;
                }
                case ChooseQuiet10m: self->quietFor(10); break;
                case ChooseQuiet1h: self->quietFor(60); break;
                case ChooseQuiet8h: self->quietFor(8 * 60); break;
                case ChooseQuiet24h: self->quietFor(24 * 60); break;
                case ChooseQuietOn:
                        self->quietFor(0);
                        self->save(QuietKey, "true");
                        break;
                case ChooseQuietOff: self->quietFor(0); break;
                case ChooseClear: self->clearAll(); break;
                default: break;
        }
        return;
}

void NotificationServer::setPanel(bool bottom, uint16_t panelHeight) {
        _bottom = bottom;
        _panelHeight = panelHeight;
        layout();
        return;
}

void NotificationServer::setCentreShown(bool shown) {
        _centreShown = shown;
        if (!shown) {
                _icon.hide();
                if (_centre != nullptr && _centre->isShown()) _centre->closeCentre();
                return;
        }
        _unread = SIZE_MAX; // drawn again
        showCentreIcon();
        (void)_icon.show();
        return;
}

void NotificationServer::changedFromKernel(void *arg) {
        auto *self = static_cast<NotificationServer *>(arg);
        if (self->_syncPosted.exchange(true)) return;
        if (self->_app.post(syncDue, self).isError()) self->_syncPosted = false; // the next change tries again
        return;
}

void NotificationServer::syncDue(void *arg) {
        auto *self = static_cast<NotificationServer *>(arg);
        self->_syncPosted = false;
        self->sync();
        return;
}

void NotificationServer::sync() {
        uint32_t     ids[NotificationLog::Capacity];
        const size_t total = Notifications::ids(Span<uint32_t>(ids, NotificationLog::Capacity));
        const size_t count = total < NotificationLog::Capacity ? total : NotificationLog::Capacity;
        // Do Not Disturb holds back all but the critical ones.
        const bool quiet = isQuiet();
        const auto held = [&](const Notification &n) {
                return quiet && n.urgency != Notification::Urgency::Critical;
        };
        const auto sound = [&](const Notification &n) {
                if (_sound && !held(n) && (n.flags & Notification::Silent) == 0 &&
                    n.urgency != Notification::Urgency::Low)
                        (void)SystemSounds::play(SystemSound::Notify);
        };
        // Balloons whose notification went, ended, or changed (replaced: shown again).
        for (Shown &s : _shown) {
                if (s.balloon == nullptr) continue;
                Notification n;
                if (Notifications::get(s.balloon->id(), n).isError() || !n.balloon) {
                        stopTimer(s);
                        s.balloon->~NotificationBalloon();
                        Memory::free(s.balloon);
                        s.balloon = nullptr;
                        continue;
                }
                if (n.changed == s.balloon->changed()) continue;
                const bool     popped = !n.read; // replaced (unread again), not just read
                const Rect     r = s.balloon->geometry();
                const uint16_t h = s.balloon->setNotification(n);
                if (h != r.height && s.balloon->isShown()) s.balloon->close(); // shown again by layout()
                if (popped) {
                        s.timeoutMs = n.timeoutMs;
                        startTimer(s);
                        sound(n);
                }
        }
        // New balloons, the oldest first, while there's room (the rest wait).
        for (size_t k = count; k-- > 0;) {
                Notification n;
                if (Notifications::get(ids[k], n).isError() || !n.balloon) continue;
                bool shown = false;
                for (const Shown &s : _shown) shown = shown || (s.balloon != nullptr && s.balloon->id() == n.id);
                if (shown) continue;
                if (!_balloons || held(n)) {
                        Notifications::endBalloon(n.id, NotificationLog::Ended::Expired); // straight to the centre
                        continue;
                }
                Shown *slot = nullptr;
                for (Shown &s : _shown)
                        if (s.balloon == nullptr && slot == nullptr) slot = &s;
                if (slot == nullptr) break;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(NotificationBalloon));
                if (mem == nullptr) break;
                slot->balloon = ::new (mem) NotificationBalloon(_app, *this);
                (void)slot->balloon->setNotification(n);
                slot->timeoutMs = n.timeoutMs;
                startTimer(*slot);
                sound(n);
        }
        layout();
        if (_centre != nullptr && _centre->isShown()) _centre->refresh();
        showCentreIcon();
        fillMenu(); // Clear All's state
        return;
}

// The balloons stacked in the corner by the tray, the newest nearest the panel.
void NotificationServer::layout() {
        uint32_t       ids[NotificationLog::Capacity];
        const size_t   total = Notifications::ids(Span<uint32_t>(ids, NotificationLog::Capacity));
        const size_t   count = total < NotificationLog::Capacity ? total : NotificationLog::Capacity;
        const Rect     screen = _app.windowManager().screen();
        const uint16_t x = uint16_t((screen.right() - NotificationBalloon::Width - Gap) & ~uint32_t(Align - 1));
        int            edge = _bottom ? int(screen.bottom()) - _panelHeight - Gap : int(screen.y) + _panelHeight + Gap;
        for (size_t k = 0; k < count; ++k) {
                for (Shown &s : _shown) {
                        if (s.balloon == nullptr || s.balloon->id() != ids[k]) continue;
                        NotificationBalloon &b = *s.balloon;
                        Notification         n;
                        if (Notifications::get(b.id(), n).isError()) continue;
                        const uint16_t h = b.isShown() ? b.geometry().height : b.setNotification(n);
                        const int      y = _bottom ? edge - h : edge;
                        edge = _bottom ? y - Gap : y + h + Gap;
                        if (y < 0) continue; // no room: it waits off the screen's edge for now
                        if (!b.isShown()) {
                                (void)b.show(Rect{x, uint16_t(y), NotificationBalloon::Width, h});
                        } else if (b.geometry().x != x || b.geometry().y != y) {
                                (void)b.move(x, uint16_t(y));
                        }
                }
        }
        return;
}

void NotificationServer::startTimer(Shown &s) {
        stopTimer(s);
        if (s.balloon == nullptr) return;
        Notification n;
        if (Notifications::get(s.balloon->id(), n).isError()) return;
        // A critical one with the default time stays, as one asking for no timeout.
        if (s.timeoutMs == Notification::NoTimeout ||
            (s.timeoutMs == Notification::DefaultTimeout && n.urgency == Notification::Urgency::Critical))
                return;
        const uint32_t    ms = s.timeoutMs > 0 ? uint32_t(s.timeoutMs) : uint32_t(_timeoutMs);
        const Result<int> timer = _app.startTimer(ms, false, expired, &s);
        s.timer = timer.isOk() ? timer.value() : -1;
        return;
}

void NotificationServer::stopTimer(Shown &s) {
        if (s.timer >= 0) _app.stopTimer(s.timer);
        s.timer = -1;
        return;
}

void NotificationServer::expired(void *arg) {
        auto *s = static_cast<Shown *>(arg);
        s->timer = -1;
        if (s->balloon != nullptr) Notifications::endBalloon(s->balloon->id(), NotificationLog::Ended::Expired);
        return; // the change closes it (sync())
}

void NotificationServer::hoverChanged(NotificationBalloon &balloon, bool over) {
        for (Shown &s : _shown) {
                if (s.balloon != &balloon) continue;
                if (over)
                        stopTimer(s);
                else
                        startTimer(s);
        }
        return;
}

void NotificationServer::activate(uint32_t id, uint32_t action, bool fromBalloon) {
        Notification n;
        if (Notifications::get(id, n).isError()) return;
        if (fromBalloon)
                Notifications::endBalloon(id, NotificationLog::Ended::Activated, action);
        else
                Notifications::activate(id, action);
        // A button is the program's alone unless it says it runs the target (a question's Yes).
        if (action == 0 || (action <= Notification::MaxActions && (n.actionTargets & (1u << (action - 1))) != 0))
                runTarget(n);
        return;
}

void NotificationServer::dismiss(uint32_t id) {
        Notifications::endBalloon(id, NotificationLog::Ended::Dismissed);
        return;
}

void NotificationServer::remove(uint32_t id) {
        Notifications::remove(id);
        return;
}

void NotificationServer::clearAll() {
        Notifications::clear();
        return;
}

void NotificationServer::runTarget(const Notification &n) {
        const StringView target(n.targetText);
        if (target.isEmpty()) return;
        switch (n.target) {
                case Notification::Target::Open:
                        if (_targets.open != nullptr) _targets.open(_targets.arg, target);
                        break;
                case Notification::Target::Show:
                        if (_targets.show != nullptr) _targets.show(_targets.arg, target);
                        break;
                case Notification::Target::Launch:
                        if (_targets.launch != nullptr) _targets.launch(_targets.arg, target);
                        break;
                case Notification::Target::Service: {
                        const StringView message(n.message);
                        (void)_app.windowManager().sendToService(
                                target, ByteSpan(reinterpret_cast<const uint8_t *>(message.data()), message.size()));
                        break;
                }
                case Notification::Target::None: break;
        }
        return;
}

const Icon *NotificationServer::iconOf(const Notification &n, uint16_t side) {
        IconTheme  &icons = _app.icons();
        const Icon *icon = n.icon[0] != '\0' ? icons.find(StringView(n.icon), side) : nullptr;
        if (icon == nullptr)
                icon = icons.find(n.urgency == Notification::Urgency::Critical ? IconId::DialogWarning
                                                                               : IconId::DialogInformation,
                                  side);
        return icon;
}

// The centre's icon: the theme's information sign, a red dot on it while some are unread.
void NotificationServer::showCentreIcon() {
        if (!_centreShown) return;
        const size_t unread = Notifications::unread();
        const bool   quiet = isQuiet();
        if (unread == _unread && quiet == _iconQuiet) return;
        _unread = unread;
        _iconQuiet = quiet;
        const Icon *base = _app.icons().find(IconId::DialogInformation, SmallIcon);
        for (uint32_t &px : _iconPixels) px = 0;
        if (base != nullptr && base->width == SmallIcon && base->height == SmallIcon)
                std::memcpy(_iconPixels, base->pixels, sizeof(_iconPixels));
        if (unread > 0) {
                // A dot at the bottom right, ringed in white so it shows on any icon.
                for (int y = 9; y < 16; ++y)
                        for (int x = 9; x < 16; ++x) {
                                const int dx = x - 12, dy = y - 12, d = dx * dx + dy * dy;
                                if (d <= 12) _iconPixels[y * 16 + x] = d <= 6 ? 0xff000000u | UnreadColor : 0xffffffffu;
                        }
        }
        if (quiet) // Do Not Disturb: the sign faded, the dot too
                for (uint32_t &px : _iconPixels) px = halfAlpha(px);
        (void)_icon.setIcon(Icon{SmallIcon, SmallIcon, _iconPixels});
        int n = unread == 0 ? std::snprintf(_tip, sizeof(_tip), "Notifications: %s", "none new")
                            : std::snprintf(_tip, sizeof(_tip), "Notifications: %u new", unsigned(unread));
        if (quiet && n > 0 && size_t(n) < sizeof(_tip)) {
                if (_quietForever || _quietUntilMs == 0) {
                        std::snprintf(_tip + n, sizeof(_tip) - size_t(n), "\n%s", "Do Not Disturb");
                } else {
                        // Until when, on the clock the panel shows.
                        String          until(MemSpace::Psram);
                        const int64_t   leftNs = (_quietUntilMs - monotonicMs()) * 1000000;
                        const LocalTime end = Clock::localTime(Timestamp{Clock::now().ns + leftNs});
                        (void)formatTime(until, StringView("%-l:%M %p"), end);
                        std::snprintf(_tip + n, sizeof(_tip) - size_t(n), "\nDo Not Disturb until %s", until.data());
                }
        }
        _icon.setToolTip(StringView(_tip));
        return;
}

void NotificationServer::trayActivated(void *arg, TrayIcon &icon, TrayIcon::Reason reason) {
        auto *self = static_cast<NotificationServer *>(arg);
        if (reason != TrayIcon::Reason::Trigger) return;
        if (self->_centre == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(NotificationCentre));
                if (mem == nullptr) return;
                self->_centre = ::new (mem) NotificationCentre(self->_app, *self);
        }
        if (self->_centre->isShown()) {
                self->_centre->closeCentre(); // a second click closes it
                return;
        }
        const Rect at = icon.geometry();
        if (at.width == 0) return; // the tray hasn't said where it is yet
        self->_centre->open(at);   // read once it closes: the unread marked meanwhile
        return;
}

BMDS_NAMESPACE_END
