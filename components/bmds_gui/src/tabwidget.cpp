/**
 * @file      tabwidget.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Tabs over stacked pages (devplan/mediaplayer.md §6d).
 */

#include <cstring>
#include <bmds/bevel.h>
#include <bmds/bitmapfont.h>
#include <bmds/guiwindow.h>
#include <bmds/inputevent.h>
#include <bmds/tabrow.h>
#include <bmds/tabwidget.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint16_t Raise = 2; // the current tab above the others
        constexpr uint16_t Start = 2; // the first tab from the left edge

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

} // namespace

TabWidget::TabWidget(Widget *parent) : Widget(parent) {}

Error TabWidget::addTab(Widget *page, StringView title) {
        if (_count == MaxTabs) return Error::Full;
        if (page == nullptr || page->parent() != this) return Error::Invalid;
        _tabs[_count].page = page;
        page->setVisible(_count == 0);
        ++_count;
        setTitle(_count - 1, title);
        return Error::Ok;
}

void TabWidget::removeTab(size_t index) {
        if (index >= _count) return;
        Widget    *gone = _tabs[index].page;
        GuiWindow *w = window();
        const bool focusIn = w != nullptr && contains(gone, w->focusWidget());
        gone->setVisible(false);
        for (size_t i = index; i + 1 < _count; ++i) _tabs[i] = _tabs[i + 1];
        _tabs[--_count] = Tab{};
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

void TabWidget::setTitle(size_t index, StringView title) {
        if (index >= _count) return;
        size_t n = title.size() < MaxTitle ? title.size() : MaxTitle;
        while (n > 0 && n < title.size() && (uint8_t(title[n]) & 0xc0) == 0x80) --n;
        if (StringView(_tabs[index].title) == title.left(n)) return; // unchanged: no layout, no redraw
        std::memcpy(_tabs[index].title, title.data(), n);
        _tabs[index].title[n] = '\0';
        layout();
        update();
        return;
}

void TabWidget::setCurrent(size_t index) {
        if (index >= _count || index == _current) return;
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
        return;
}

void TabWidget::userSet(size_t index) {
        if (index == _current) return;
        setCurrent(index);
        if (_changed != nullptr) _changed(_changedArg, *this, _current);
        return;
}

Rect TabWidget::pageRect() const {
        const Rect &g = geometry();
        if (g.width < 2 * Inset || g.height < rowHeight() + 2 * Inset) return Rect{g.x, g.y, 0, 0};
        return Rect{uint16_t(g.x + Inset), uint16_t(g.y + rowHeight() + Inset), uint16_t(g.width - 2 * Inset),
                    uint16_t(g.height - rowHeight() - 2 * Inset)};
}

void TabWidget::layout() {
        uint16_t texts[MaxTabs], lefts[MaxTabs], widths[MaxTabs];
        for (size_t i = 0; i < _count; ++i)
                texts[i] = static_cast<uint16_t>(font().textWidth(StringView(_tabs[i].title)));
        (void)TabRow::place(Span<const uint16_t>(texts, _count), uint16_t(geometry().x + Start),
                            Span<uint16_t>(lefts, _count), Span<uint16_t>(widths, _count));
        for (size_t i = 0; i < _count; ++i) {
                _tabs[i].left = lefts[i];
                _tabs[i].width = widths[i];
        }
        if (_count > 0) _tabs[_current].page->setGeometry(pageRect());
        return;
}

void TabWidget::resized() {
        layout();
        return;
}

GuiSize TabWidget::sizeHint() const {
        GuiSize s;
        for (size_t i = 0; i < _count; ++i) {
                const GuiSize p = _tabs[i].page->sizeHint();
                if (p.width > s.width) s.width = p.width;
                if (p.height > s.height) s.height = p.height;
        }
        s.width = uint16_t(s.width + 2 * Inset);
        s.height = uint16_t(s.height + rowHeight() + 2 * Inset);
        return s;
}

void TabWidget::paint(Painter &painter) {
        const WindowPalette &p = painter.palette();
        const Rect          &g = geometry();
        const uint32_t       face = p.color(Role::Face);
        (void)painter.fill(g, face);
        if (g.height <= rowHeight()) return;
        // The pages' frame, under the row.
        const Rect frame{g.x, uint16_t(g.y + rowHeight()), g.width, uint16_t(g.height - rowHeight())};
        drawBevel(painter, frame, Bevel::Style::Raised);
        const uint32_t light = p.color(Role::FaceHighlight), shadow = p.color(Role::FaceShadow),
                       dark = p.color(Role::FaceDark), text = p.color(Role::FaceText);
        // The current tab last: it overlaps its neighbours' edges.
        for (size_t k = 0; k < _count; ++k) {
                const size_t i = k + 1 < _count ? (k < _current ? k : k + 1) : _current;
                const Tab   &t = _tabs[i];
                const bool   on = i == _current;
                // The current one wider by 2 a side, taller, its foot open into the frame.
                const int x = on ? int(t.left) - 2 : t.left;
                const int w = on ? t.width + 4 : t.width;
                const int top = g.y + (on ? 0 : Raise);
                const int h = int(g.y) + rowHeight() - top + (on ? 2 : 0);
                if (x < g.x || x + w > g.right() || w < 4) continue;
                (void)painter.fill(Rect{uint16_t(x + 1), uint16_t(top + 1), uint16_t(w - 2), uint16_t(h - 1)}, face);
                const Rect lit[] = {{uint16_t(x), uint16_t(top + 2), 1, uint16_t(h - 2)},
                                    {uint16_t(x + 1), uint16_t(top + 1), 1, 1},
                                    {uint16_t(x + 2), uint16_t(top), uint16_t(w - 4), 1}};
                (void)painter.fill(Span<const Rect>(lit), light);
                const Rect darkEdge[] = {{uint16_t(x + w - 1), uint16_t(top + 2), 1, uint16_t(h - 2)},
                                         {uint16_t(x + w - 2), uint16_t(top + 1), 1, 1}};
                (void)painter.fill(Span<const Rect>(darkEdge), dark);
                (void)painter.fill(Rect{uint16_t(x + w - 2), uint16_t(top + 2), 1, uint16_t(h - 2)}, shadow);
                // The text on a column, a pixel higher on the current tab's.
                const int tx = (t.left + TabRow::Pad) & ~3;
                const int room = t.left + t.width - TabRow::Pad / 2 - tx;
                if (room > 0)
                        (void)painter.text(tx, top + (on ? 4 : 3), StringView(t.title), font(), text, face,
                                           uint16_t(room));
        }
        return;
}

void TabWidget::mousePressed(const WindowManager::MouseEvent &event) {
        if (event.button != PointerEvent::Left || event.y >= geometry().y + rowHeight()) return;
        uint16_t lefts[MaxTabs], widths[MaxTabs];
        for (size_t i = 0; i < _count; ++i) {
                lefts[i] = _tabs[i].left;
                widths[i] = _tabs[i].width;
        }
        const int tab = TabRow::at(Span<const uint16_t>(lefts, _count), Span<const uint16_t>(widths, _count), event.x);
        if (tab >= 0) userSet(size_t(tab));
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
