/**
 * @file      tabwidget.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Tabs over stacked pages (devplan/mediaplayer.md §6d, devplan/tabs.md).
 */

#include <cstring>
#include <new>
#include <bmds/bevel.h>
#include <bmds/bitmapfont.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/tabrow.h>
#include <bmds/tabwidget.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;
        using Kind = TabRow::Hit::Kind;

        constexpr uint16_t Raise = 2; // the current tab above the others
        constexpr uint16_t Start = 2; // the first tab from the left edge (the current one's 2 wider)
        constexpr size_t   Grow = 16; // tabs added to the arrays at a time
        constexpr size_t   ListMost = MenuLayout::MaxItems;

        // The first visible, enabled, focusable widget in @p w's tree, or null.
        Widget *firstFocusable(Widget *w) {
                if (!w->isVisible() || !w->isEnabled()) return nullptr;
                if (w->isFocusable()) return w;
                for (Widget *c = w->firstChild(); c != nullptr; c = c->next())
                        if (Widget *f = firstFocusable(c); f != nullptr) return f;
                return nullptr;
        }

        bool contains(const Widget *tree, const Widget *w) {
                for (; w != nullptr; w = w->parent())
                        if (w == tree) return true;
                return false;
        }

        // @p text cut at @p most bytes on a character, into @p out (`most` + 1 bytes).
        bool copyCut(char *out, size_t most, StringView text) {
                size_t n = text.size() < most ? text.size() : most;
                while (n > 0 && n < text.size() && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
                if (StringView(out) == text.left(n)) return false;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return true;
        }

        uint16_t clamp16(uint32_t v) {
                return v > UINT16_MAX ? UINT16_MAX : uint16_t(v);
        }

} // namespace

TabWidget::TabWidget(Widget *parent) : Widget(parent) {}

TabWidget::~TabWidget() {
        if (_list != nullptr) {
                _list->~Menu();
                Memory::free(_list);
        }
        Memory::free(_tabs);
        Memory::free(_lefts);
        Memory::free(_widths);
        Memory::free(_closes);
}

Error TabWidget::grow() {
        if (_capacity >= MaxTabs) return Error::Full;
        const size_t cap = _capacity + Grow;
        auto        *tabs = static_cast<Tab *>(Memory::alloc(MemSpace::Internal, cap * sizeof(Tab)));
        auto        *lefts = static_cast<uint32_t *>(Memory::alloc(MemSpace::Internal, cap * sizeof(uint32_t)));
        auto        *widths = static_cast<uint16_t *>(Memory::alloc(MemSpace::Internal, cap * sizeof(uint16_t)));
        auto        *closes = static_cast<uint16_t *>(Memory::alloc(MemSpace::Internal, cap * sizeof(uint16_t)));
        if (tabs == nullptr || lefts == nullptr || widths == nullptr || closes == nullptr) {
                Memory::free(tabs);
                Memory::free(lefts);
                Memory::free(widths);
                Memory::free(closes);
                return Error::NoMem;
        }
        for (size_t i = 0; i < cap; ++i) ::new (&tabs[i]) Tab(i < _count ? _tabs[i] : Tab{});
        if (_count > 0) {
                std::memcpy(lefts, _lefts, _count * sizeof(uint32_t));
                std::memcpy(widths, _widths, _count * sizeof(uint16_t));
                std::memcpy(closes, _closes, _count * sizeof(uint16_t));
        }
        Memory::free(_tabs);
        Memory::free(_lefts);
        Memory::free(_widths);
        Memory::free(_closes);
        _tabs = tabs;
        _lefts = lefts;
        _widths = widths;
        _closes = closes;
        _capacity = cap;
        return Error::Ok;
}

Error TabWidget::addTab(Widget *page, StringView title) {
        if (page == nullptr || page->parent() != this) return Error::Invalid;
        if (_count == _capacity) {
                const Error err = grow();
                if (err.isError()) return err;
        }
        _tabs[_count] = Tab{};
        _tabs[_count].page = page;
        (void)copyCut(_tabs[_count].title, MaxTitle, title);
        page->setVisible(_count == 0);
        ++_count;
        layout();
        update();
        return Error::Ok;
}

int TabWidget::indexOf(const Widget *page) const {
        for (size_t i = 0; i < _count; ++i)
                if (_tabs[i].page == page) return int(i);
        return -1;
}

void TabWidget::removeTab(size_t index) {
        if (index >= _count) return;
        Widget    *gone = _tabs[index].page;
        GuiWindow *w = window();
        const bool focusIn = w != nullptr && contains(gone, w->focusWidget());
        gone->setVisible(false);
        for (size_t i = index; i + 1 < _count; ++i) _tabs[i] = _tabs[i + 1];
        _tabs[--_count] = Tab{};
        if (_first > index) --_first;
        _hoverTab = -1;
        _hoverKind = Kind::None;
        _pressedClose = -1;
        if (_count == 0) {
                _current = 0;
        } else {
                if (_current > index || _current == _count) --_current;
                if (_current >= _count) _current = _count - 1;
                Widget *page = _tabs[_current].page;
                page->setGeometry(pageRect());
                page->setVisible(true);
                if (focusIn) w->setFocusWidget(firstFocusable(page));
        }
        layout();
        update();
        return;
}

void TabWidget::moveTab(size_t from, size_t to) {
        if (from >= _count) return;
        if (to >= _count) to = _count - 1;
        if (from == to) return;
        Widget   *current = _tabs[_current].page;
        const Tab moving = _tabs[from];
        if (from < to)
                for (size_t i = from; i < to; ++i) _tabs[i] = _tabs[i + 1];
        else
                for (size_t i = from; i > to; --i) _tabs[i] = _tabs[i - 1];
        _tabs[to] = moving;
        _current = size_t(indexOf(current));
        _hoverTab = -1;
        _pressedClose = -1;
        layout();
        update();
        return;
}

void TabWidget::setTitle(size_t index, StringView title) {
        if (index >= _count || !copyCut(_tabs[index].title, MaxTitle, title)) return; // unchanged: no redraw
        layout();
        update();
        return;
}

void TabWidget::setBadge(size_t index, StringView badge) {
        if (index >= _count || !copyCut(_tabs[index].badge, MaxBadge, badge)) return;
        layout();
        update();
        return;
}

void TabWidget::setAttention(size_t index, uint8_t level) {
        if (level > Alert) level = Alert;
        if (index >= _count || _tabs[index].attention == level) return;
        _tabs[index].attention = level;
        update();
        return;
}

void TabWidget::setClosable(size_t index, bool closable) {
        if (index >= _count || _tabs[index].closable == closable) return;
        _tabs[index].closable = closable;
        layout();
        update();
        return;
}

void TabWidget::setBarVisible(bool visible) {
        if (visible == _barVisible) return;
        _barVisible = visible;
        _hoverRow = false;
        _hoverTab = -1;
        _pressedClose = -1;
        layout();
        update();
        return;
}

void TabWidget::setCurrent(size_t index) {
        if (index >= _count) return;
        if (index != _current) {
                Widget    *old = _tabs[_current].page;
                GuiWindow *w = window();
                const bool focusIn = w != nullptr && contains(old, w->focusWidget());
                old->setVisible(false);
                _current = index;
                Widget *page = _tabs[index].page;
                page->setGeometry(pageRect());
                page->setVisible(true);
                if (focusIn) w->setFocusWidget(firstFocusable(page));
                update();
        }
        showCurrent();
        return;
}

void TabWidget::userSet(size_t index) {
        const bool changed = index != _current;
        setCurrent(index);
        if (_tabs[_current].attention != Quiet) {
                _tabs[_current].attention = Quiet;
                update();
        }
        if (changed && _changed != nullptr) _changed(_changedArg, *this, _current);
        return;
}

Rect TabWidget::rowRect() const {
        const Rect &g = geometry();
        if (g.width < 2 * Start) return Rect{g.x, g.y, 0, rowHeight()};
        return Rect{uint16_t(g.x + Start), g.y, uint16_t(g.width - 2 * Start), rowHeight()};
}

Rect TabWidget::pageRect() const {
        const Rect &g = geometry();
        if (!_barVisible) return g;
        if (g.width < 2 * Inset || g.height < rowHeight() + 2 * Inset) return Rect{g.x, g.y, 0, 0};
        return Rect{uint16_t(g.x + Inset), uint16_t(g.y + rowHeight() + Inset), uint16_t(g.width - 2 * Inset),
                    uint16_t(g.height - rowHeight() - 2 * Inset)};
}

void TabWidget::layout() {
        for (size_t i = 0; i < _count; ++i) {
                Tab &t = _tabs[i];
                t.titleWidth = clamp16(font().textWidth(StringView(t.title)));
                const uint16_t badge = t.badge[0] != '\0' ? clamp16(font().textWidth(StringView(t.badge))) : 0;
                t.parts = TabRow::parts(t.titleWidth, badge, t.closable);
                _widths[i] = t.parts.width;
                _closes[i] = t.parts.close;
        }
        _content = TabRow::placeAll(Span<const uint16_t>(_widths, _count), Span<uint32_t>(_lefts, _count));
        _bar = TabRow::bar(_content, rowRect().width);
        // The row still starts with the tab it did (widths change with titles and badges).
        const uint32_t most = TabRow::maxScroll(Span<const uint32_t>(_lefts, _count), _content, _bar);
        _scroll = _first < _count ? _lefts[_first] : 0;
        if (_scroll > most) _scroll = most;
        syncFirst();
        if (_count > 0) _tabs[_current].page->setGeometry(pageRect());
        return;
}

void TabWidget::showCurrent() {
        if (_count == 0) return;
        scrollTo(TabRow::scrollToShow(_scroll, Span<const uint32_t>(_lefts, _count), _current, _widths[_current],
                                      _content, _bar));
        return;
}

void TabWidget::syncFirst() {
        _first = 0;
        while (_first + 1 < _count && _lefts[_first + 1] <= _scroll) ++_first;
        return;
}

void TabWidget::scrollTo(uint32_t scroll) {
        if (scroll == _scroll) return;
        _scroll = scroll;
        syncFirst();
        GuiWindow  *w = window();
        const Rect &g = geometry();
        if (w != nullptr && _barVisible) w->invalidate(Rect{g.x, g.y, g.width, uint16_t(rowHeight() + 2)});
        return;
}

void TabWidget::resized() {
        layout();
        showCurrent();
        return;
}

GuiSize TabWidget::sizeHint() const {
        GuiSize s;
        for (size_t i = 0; i < _count; ++i) {
                const GuiSize p = _tabs[i].page->sizeHint();
                if (p.width > s.width) s.width = p.width;
                if (p.height > s.height) s.height = p.height;
        }
        if (!_barVisible) return s;
        s.width = uint16_t(s.width + 2 * Inset);
        s.height = uint16_t(s.height + rowHeight() + 2 * Inset);
        return s;
}

TabRow::Hit TabWidget::hitAt(int x, int y) const {
        const Rect &g = geometry();
        if (!_barVisible || y < int(g.y) || y >= int(g.y) + rowHeight()) return TabRow::Hit{};
        return TabRow::hit(Span<const uint32_t>(_lefts, _count), Span<const uint16_t>(_widths, _count),
                           Span<const uint16_t>(_closes, _count), _scroll, _bar, x - int(rowRect().x));
}

void TabWidget::paintTab(Painter &painter, size_t i, int x0, bool on) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        const Tab           &t = _tabs[i];
        const uint32_t       face = p.color(Role::Face);
        const uint32_t       light = p.color(Role::FaceHighlight), shadow = p.color(Role::FaceShadow),
                       dark = p.color(Role::FaceDark);
        // The current one wider by 2 a side, taller, its foot open into the frame.
        const int x = on ? x0 - 2 : x0;
        const int w = on ? t.parts.width + 4 : t.parts.width;
        const int top = g.y + (on ? 0 : Raise);
        const int h = int(g.y) + rowHeight() - top + (on ? 2 : 0);
        if (x < 0 || w < 4) return;
        (void)painter.fill(Rect{uint16_t(x + 1), uint16_t(top + 1), uint16_t(w - 2), uint16_t(h - 1)}, face);
        const Rect lit[] = {{uint16_t(x), uint16_t(top + 2), 1, uint16_t(h - 2)},
                            {uint16_t(x + 1), uint16_t(top + 1), 1, 1},
                            {uint16_t(x + 2), uint16_t(top), uint16_t(w - 4), 1}};
        (void)painter.fill(Span<const Rect>(lit), light);
        const Rect darkEdge[] = {{uint16_t(x + w - 1), uint16_t(top + 2), 1, uint16_t(h - 2)},
                                 {uint16_t(x + w - 2), uint16_t(top + 1), 1, 1}};
        (void)painter.fill(Span<const Rect>(darkEdge), dark);
        (void)painter.fill(Rect{uint16_t(x + w - 2), uint16_t(top + 2), 1, uint16_t(h - 2)}, shadow);

        // The title on a column, a pixel higher on the current tab's; in the attention's color.
        const uint32_t text = t.attention == Alert      ? p.color(Role::CloseBox)
                              : t.attention == Activity ? p.color(Role::Highlight)
                                                        : p.color(Role::FaceText);
        const int      ty = top + (on ? 4 : 3);
        const int      tx = (x0 + TabRow::Pad) & ~3;
        const int      end = t.parts.badge != 0   ? x0 + t.parts.badge - TabRow::BadgeGap / 2
                             : t.parts.close != 0 ? x0 + t.parts.close - TabRow::CloseGap / 2
                                                  : x0 + t.parts.width - TabRow::Pad / 2;
        if (end > tx) (void)painter.text(tx, ty, StringView(t.title), font(), text, face, uint16_t(end - tx));

        // The badge: its text on a column, the pill around it.
        if (t.parts.badge != 0) {
                const int      bx = (x0 + t.parts.badge + TabRow::BadgePad) & ~3;
                const int      bw = t.parts.badgeWidth - 2 * TabRow::BadgePad;
                const uint32_t bg = t.attention == Alert      ? p.color(Role::CloseBox)
                                    : t.attention == Activity ? p.color(Role::Highlight)
                                                              : shadow;
                const uint32_t fg = t.attention == Alert ? p.color(Role::CloseMark) : p.color(Role::HighlightText);
                const Rect pill{uint16_t(bx - TabRow::BadgePad), uint16_t(ty - 1), uint16_t(bw + 2 * TabRow::BadgePad),
                                uint16_t(textHeight() + 2)};
                (void)painter.fill(pill, bg);
                // Its corners rounded off by a pixel.
                const Rect corners[] = {{pill.x, pill.y, 1, 1},
                                        {uint16_t(pill.right() - 1), pill.y, 1, 1},
                                        {pill.x, uint16_t(pill.bottom() - 1), 1, 1},
                                        {uint16_t(pill.right() - 1), uint16_t(pill.bottom() - 1), 1, 1}};
                (void)painter.fill(Span<const Rect>(corners), face);
                (void)painter.text(bx, ty, StringView(t.badge), font(), fg, bg, uint16_t(bw + TabRow::BadgePad));
        }

        // The close button: on the current tab and the one under the pointer; lit under the pointer.
        if (t.closable && (on || int(i) == _hoverTab)) {
                const int  cx = x0 + t.parts.close;
                const int  cy = int(g.y) + (rowHeight() - TabRow::CloseSize) / 2 + (on ? 0 : 1);
                const bool hot = (int(i) == _hoverTab && _hoverKind == Kind::Close) || int(i) == _pressedClose;
                if (hot)
                        (void)painter.fill(Rect{uint16_t(cx), uint16_t(cy), TabRow::CloseSize, TabRow::CloseSize},
                                           p.color(Role::CloseBox));
                const uint32_t mark = hot ? p.color(Role::CloseMark) : p.color(Role::FaceText);
                Rect           cross[16];
                for (int k = 0; k < 8; ++k) {
                        cross[2 * k] = Rect{uint16_t(cx + 2 + k), uint16_t(cy + 2 + k), 1, 1};
                        cross[2 * k + 1] = Rect{uint16_t(cx + 9 - k), uint16_t(cy + 2 + k), 1, 1};
                }
                (void)painter.fill(Span<const Rect>(cross), mark);
                for (int k = 0; k < 8; ++k) {
                        cross[2 * k] = Rect{uint16_t(cx + 3 + k), uint16_t(cy + 2 + k), 1, 1};
                        cross[2 * k + 1] = Rect{uint16_t(cx + 8 - k), uint16_t(cy + 2 + k), 1, 1};
                }
                (void)painter.fill(Span<const Rect>(cross), mark);
        }
        return;
}

void TabWidget::paintButtons(Painter &painter, const Rect &row) {
        if (!_bar.buttons) return;
        const WindowPalette  &p = painter.palette();
        const Rect           &g = geometry();
        const uint32_t        most = TabRow::maxScroll(Span<const uint32_t>(_lefts, _count), _content, _bar);
        const GuiStyle::Arrow ways[] = {GuiStyle::Arrow::Left, GuiStyle::Arrow::Right, GuiStyle::Arrow::Down};
        const bool            usable[] = {_scroll > 0, _scroll < most, true};
        for (int b = 0; b < 3; ++b) {
                const Rect r{uint16_t(row.x + _bar.view + uint32_t(b) * TabRow::ButtonWidth), uint16_t(g.y + Raise),
                             TabRow::ButtonWidth, uint16_t(rowHeight() - Raise)};
                if (r.right() > g.right()) break;
                (void)painter.fill(r, p.color(Role::Face));
                drawBevel(painter, r, Bevel::Style::Raised);
                GuiStyle::drawArrow(painter, r, ways[b],
                                    usable[b] ? p.color(Role::FaceText) : p.color(Role::DisabledText));
        }
        return;
}

void TabWidget::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        (void)painter.fill(g, p.color(Role::Face));
        if (!_barVisible || g.height <= rowHeight()) return;
        // The pages' frame, under the row.
        const Rect frame{g.x, uint16_t(g.y + rowHeight()), g.width, uint16_t(g.height - rowHeight())};
        drawBevel(painter, frame, Bevel::Style::Raised);
        const Rect row = rowRect();
        auto       tabs = [&](Painter &into) {
                // The current tab last: it overlaps its neighbours' edges.
                const int64_t viewLeft = int64_t(_scroll), viewRight = int64_t(_scroll) + _bar.view;
                for (size_t k = 0; k < _count; ++k) {
                        const size_t i = k + 1 < _count ? (k < _current ? k : k + 1) : _current;
                        if (int64_t(_lefts[i]) + _widths[i] <= viewLeft || int64_t(_lefts[i]) >= viewRight) continue;
                        paintTab(into, i, int(row.x) + int(_lefts[i]) - int(_scroll), i == _current);
                }
        };
        if (!_bar.buttons) {
                tabs(painter);
        } else {
                // The tabs cut at the view's right edge, the buttons past it.
                Rect    storage[32];
                Painter cut(painter, Rect{g.x, g.y, uint16_t(row.x + _bar.view - g.x), uint16_t(rowHeight() + 2)},
                            Span<Rect>(storage));
                tabs(cut);
        }
        paintButtons(painter, row);
        return;
}

void TabWidget::mousePressed(const WindowManager::MouseEvent &event) {
        const TabRow::Hit hit = hitAt(event.x, event.y);
        if (hit.kind == Kind::None) return;
        const bool onTab = hit.kind == Kind::Tab || hit.kind == Kind::Close;
        if (event.button == PointerEvent::Left) {
                switch (hit.kind) {
                        case Kind::Tab: userSet(size_t(hit.tab)); break;
                        case Kind::Close:
                                _pressedClose = hit.tab;
                                update();
                                break;
                        case Kind::Back:
                        case Kind::Forward: {
                                const int by = hit.kind == Kind::Back ? -1 : 1;
                                scrollTo(TabRow::scrollBy(_scroll, Span<const uint32_t>(_lefts, _count), _content, _bar,
                                                          by));
                                break;
                        }
                        case Kind::List: openList(); break;
                        case Kind::None: break;
                }
        } else if (event.button == PointerEvent::Middle && onTab) {
                if (_tabs[hit.tab].closable && _closeRequested != nullptr)
                        _closeRequested(_closeRequestedArg, *this, size_t(hit.tab));
        } else if (event.button == PointerEvent::Right && onTab) {
                if (_menuRequested != nullptr)
                        _menuRequested(_menuRequestedArg, *this, size_t(hit.tab), event.x, event.y);
        }
        return;
}

void TabWidget::mouseReleased(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || _pressedClose < 0) return;
        const int pressed = _pressedClose;
        _pressedClose = -1;
        update();
        const TabRow::Hit hit = hitAt(event.x, event.y);
        if (hit.kind == Kind::Close && hit.tab == pressed && _closeRequested != nullptr)
                _closeRequested(_closeRequestedArg, *this, size_t(pressed));
        return;
}

void TabWidget::mouseMoved(const WindowManager::MouseEvent &event) {
        const Rect &g = geometry();
        _hoverRow = _barVisible && event.y >= int(g.y) && event.y < int(g.y) + rowHeight();
        const TabRow::Hit hit = hitAt(event.x, event.y);
        // The close button shows on the tab under the pointer, lit on itself: redrawn as either changes.
        if (hit.tab == _hoverTab && hit.kind == _hoverKind) return;
        const bool redraw = hit.tab != _hoverTab || hit.kind == Kind::Close || _hoverKind == Kind::Close;
        _hoverTab = hit.tab;
        _hoverKind = hit.kind;
        GuiWindow *w = window();
        if (redraw && w != nullptr) w->invalidate(Rect{g.x, g.y, g.width, uint16_t(rowHeight() + 2)});
        return;
}

void TabWidget::mouseLeft() {
        _hoverRow = false;
        if (_hoverTab < 0) return;
        _hoverTab = -1;
        _hoverKind = Kind::None;
        const Rect &g = geometry();
        if (GuiWindow *w = window(); w != nullptr) w->invalidate(Rect{g.x, g.y, g.width, uint16_t(rowHeight() + 2)});
        return;
}

bool TabWidget::wheelTurned(int notches) {
        if (!_hoverRow || !_bar.buttons) return false;
        scrollTo(TabRow::scrollBy(_scroll, Span<const uint32_t>(_lefts, _count), _content, _bar, -notches));
        return true;
}

void TabWidget::openList() {
        GuiWindow *w = window();
        if (w == nullptr || !w->isShown() || _count == 0) return;
        if (_list == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) return;
                _list = ::new (mem) Menu(w->application());
                _list->setTriggered(listChosen, this);
        }
        _list->clear();
        // The most a menu holds, around the current tab.
        size_t first = 0;
        if (_count > ListMost) {
                first = _current > ListMost / 2 ? _current - ListMost / 2 : 0;
                if (first + ListMost > _count) first = _count - ListMost;
        }
        for (size_t i = first; i < _count && i < first + ListMost; ++i) {
                const Tab &t = _tabs[i];
                // An alert marked, the badge after the title; `&` doubled to show as itself.
                char   text[Menu::MaxText + 1];
                size_t n = 0;
                auto   put = [&](StringView s) {
                        for (size_t k = 0; k < s.size() && n + 2 < sizeof(text); ++k) {
                                if (s[k] == '&') text[n++] = '&';
                                text[n++] = s[k];
                        }
                        return;
                };
                if (t.attention == Alert) put("* ");
                put(StringView(t.title));
                if (t.badge[0] != '\0') {
                        put(" (");
                        put(StringView(t.badge));
                        put(")");
                }
                (void)_list->addItem(StringView(text, n), int(i), i == _current ? MenuLayout::Checked : 0);
        }
        const Rect &g = geometry();
        const Rect  s = w->screenRect();
        const Rect  row = rowRect();
        const Rect  button{uint16_t(s.x + row.x + _bar.view + 2 * TabRow::ButtonWidth), uint16_t(s.y + g.y),
                          TabRow::ButtonWidth, rowHeight()};
        if (_list->popup(button).isOk()) _list->highlightFirst();
        return;
}

void TabWidget::listChosen(void *arg, int item) {
        auto *self = static_cast<TabWidget *>(arg);
        if (item >= 0 && size_t(item) < self->_count) self->userSet(size_t(item));
        return;
}

bool TabWidget::keyBeforeChild(const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release || !event.hasCtrl() || event.hasAlt() || _count < 2) return false;
        int by = 0;
        if (event.usage == HidKey::Tab) by = event.hasShift() ? -1 : 1;
        if (event.usage == HidKey::PageDown && !event.hasShift()) by = 1;
        if (event.usage == HidKey::PageUp && !event.hasShift()) by = -1;
        if (by == 0) return false;
        userSet(TabRow::step(_current, _count, by));
        return true;
}

BMDS_NAMESPACE_END
