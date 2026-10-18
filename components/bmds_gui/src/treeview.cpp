/**
 * @file      treeview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A tree of folders (devplan/filebrowser.md §4g).
 */

#include <cstdio>
#include <bmds/font.h>
#include <bmds/guiwindow.h>
#include <bmds/treeview.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        constexpr uint8_t  KeypadMinus = 0x56;
        constexpr uint8_t  KeypadPlus = 0x57;
        constexpr uint16_t BoxSide = 9;

        Rect cut(int x, int y, int w, int h, const Rect &clip) {
                const int l = x > clip.x ? x : clip.x, t = y > clip.y ? y : clip.y;
                const int r = x + w < int(clip.right()) ? x + w : int(clip.right());
                const int b = y + h < int(clip.bottom()) ? y + h : int(clip.bottom());
                if (r <= l || b <= t) return Rect();
                return Rect{uint16_t(l), uint16_t(t), uint16_t(r - l), uint16_t(b - t)};
        }

} // namespace

TreeView::TreeView(Widget *parent) : ItemView(parent) {
        setSingleSelection(true);
        setLineSteps(16, RowHeight);
        setModel(&_model);
}

void TreeView::setIcons(const Icon *root, const Icon *folder, const Icon *open) {
        _rootIcon = root;
        _folderIcon = folder;
        _openIcon = open;
        update();
        return;
}

void TreeView::setChildIcon(StringView name, const Icon *icon) {
        std::snprintf(_childName, sizeof(_childName), "%.*s", int(name.size()), name.data());
        _childIcon = icon;
        update();
        return;
}

void TreeView::changedRows(int keep) {
        resetSelecting(keep, false);
        if (keep >= 0) showRow(size_t(keep));
        return;
}

Error TreeView::addRoot(StringView path) {
        const Error err = _rows.addRoot(path);
        if (err.isOk()) {
                changedRows(current());
                tellRowsChanged();
        }
        return err;
}

Error TreeView::setChildren(size_t row, Span<const StringView> names) {
        // The selected row found again by its path (it may be under this one).
        String    path(MemSpace::Psram);
        const int was = current();
        if (was >= 0 && _rows.pathOf(size_t(was), path).isError()) path.clear();
        const Error err = _rows.setChildren(row, names);
        if (err.isError()) return err;
        int keep = was >= 0 ? _rows.find(path.view()) : -1;
        if (was >= 0 && keep < 0) keep = int(row); // it went: the row it was under
        changedRows(keep);
        tellRowsChanged();
        return Error::Ok;
}

void TreeView::expand(size_t row) {
        if (row >= _rows.count()) return;
        const uint8_t flags = _rows.row(row).flags;
        if ((flags & TreeRows::Expanded) != 0 || (flags & TreeRows::HasChildren) == 0) return;
        if (_expanding != nullptr) _expanding(_expandingArg, *this, row);
        return;
}

void TreeView::collapse(size_t row) {
        if (row >= _rows.count() || (_rows.row(row).flags & TreeRows::Expanded) == 0) return;
        const size_t end = _rows.endOf(row);
        const int    was = current();
        _rows.collapse(row);
        if (was > int(row) && size_t(was) < end) {
                // It was under the row collapsed: the selection moves up to it.
                resetSelecting(int(row), true);
                showRow(row);
                tellRowsChanged();
                return;
        }
        changedRows(was > int(row) ? was - int(end - row - 1) : was);
        tellRowsChanged();
        return;
}

void TreeView::clear() {
        _rows.clear();
        modelReset();
        tellRowsChanged();
        return;
}

int TreeView::selectPath(StringView path) {
        const int row = _rows.find(path);
        if (row >= 0 && row != current()) selectOnly(row);
        return row;
}

void TreeView::relayout() {
        uint32_t width = 0;
        for (size_t r = 0; r < _rows.count(); ++r) {
                const uint32_t right = textX(_rows.row(r).depth) + Fonts::unscii16.textWidth(_rows.name(r)) + 4;
                if (right > width) width = right;
        }
        setContentSize(width, uint32_t(_rows.count()) * RowHeight);
        return;
}

ContentRect TreeView::rowRect(size_t row) const {
        return ContentRect{0, uint32_t(row) * RowHeight, contentWidth() > 0 ? contentWidth() : 1, RowHeight};
}

int TreeView::rowAt(uint32_t x, uint32_t y) const {
        (void)x;
        const size_t row = y / RowHeight;
        return row < _rows.count() ? int(row) : -1;
}

bool TreeView::onBox(size_t row, uint32_t x) const {
        if ((_rows.row(row).flags & TreeRows::HasChildren) == 0) return false;
        const uint32_t left = uint32_t(_rows.row(row).depth) * Indent;
        return x >= left && x < left + Indent;
}

bool TreeView::onName(size_t row, uint32_t x, uint32_t y) const {
        (void)y;
        return x >= textX(_rows.row(row).depth);
}

size_t TreeView::neighbour(size_t row, int dx, int dy) const {
        (void)dx;
        if (dy < 0) return row > 0 ? row - 1 : row;
        if (dy > 0) return row + 1 < _rows.count() ? row + 1 : row;
        return row;
}

void TreeView::activate(size_t row) {
        if (row < _rows.count()) {
                if ((_rows.row(row).flags & TreeRows::Expanded) != 0) {
                        collapse(row);
                } else {
                        expand(row);
                }
        }
        ItemView::activate(row);
        return;
}

void TreeView::mousePressed(const WindowManager::MouseEvent &event) {
        const Rect v = viewport();
        if (event.button == PointerEvent::Left && event.x >= v.x && event.y >= v.y && event.x < v.right() &&
            event.y < v.bottom()) {
                const int row = rowAt(contentX(event.x), contentY(event.y));
                if (row >= 0 && onBox(size_t(row), contentX(event.x))) {
                        if ((_rows.row(size_t(row)).flags & TreeRows::Expanded) != 0) {
                                collapse(size_t(row));
                        } else {
                                expand(size_t(row));
                        }
                        return;
                }
        }
        ItemView::mousePressed(event);
        return;
}

bool TreeView::keyPressed(const KeyEvent &event) {
        const int row = current();
        if (row >= 0 && !event.hasCtrl() && !event.hasAlt()) {
                const uint8_t flags = _rows.row(size_t(row)).flags;
                const bool    open = (flags & TreeRows::Expanded) != 0;
                switch (event.usage) {
                        case HidKey::Right:
                                if (!open) {
                                        expand(size_t(row));
                                } else if (size_t(row) + 1 < _rows.count()) {
                                        selectOnly(row + 1);
                                }
                                return true;
                        case HidKey::Left:
                                if (open) {
                                        collapse(size_t(row));
                                } else if (_rows.parent(size_t(row)) >= 0) {
                                        selectOnly(_rows.parent(size_t(row)));
                                }
                                return true;
                        case KeypadPlus: expand(size_t(row)); return true;
                        case KeypadMinus: collapse(size_t(row)); return true;
                        default: break;
                }
        }
        return ItemView::keyPressed(event);
}

Rect TreeView::nameRect(size_t row) const {
        GuiWindow *w = window();
        if (row >= _rows.count() || w == nullptr) return Rect();
        const Rect s = w->screenRect();
        const Rect v = viewport();
        const int  x = int(v.x) + int(textX(_rows.row(row).depth)) - int(scrollX()) - 4;
        const int  y = int(v.y) + int(row) * RowHeight - int(scrollY()) + 1;
        return Rect{uint16_t(s.x + (x > 0 ? x : 0)), uint16_t(s.y + y), 160, uint16_t(RowHeight - 2)};
}

void TreeView::paintRows(Painter &painter, const Rect &viewport) {
        const WindowPalette &p = painter.palette();
        const uint32_t       background = p.color(Role::FaceHighlight);
        (void)painter.fill(viewport, background);
        const size_t first = scrollY() / RowHeight;
        const size_t last = (scrollY() + viewport.height) / RowHeight;
        const bool   focus = showsFocus();
        for (size_t row = first; row <= last && row < _rows.count(); ++row) {
                const TreeRows::Row &r = _rows.row(row);
                const int            y = int(viewport.y) + int(row) * RowHeight - int(scrollY());
                const int            x = int(viewport.x) - int(scrollX()) + int(r.depth) * Indent;
                const bool           open = (r.flags & TreeRows::Expanded) != 0;
                if ((r.flags & TreeRows::HasChildren) != 0) {
                        // The box: an outline, a minus, and a plus's upright while closed.
                        const int      bx = x + (Indent - BoxSide) / 2, by = y + (RowHeight - BoxSide) / 2;
                        const uint32_t edge = p.color(Role::FaceShadow), mark = p.color(Role::FaceText);
                        const Rect     parts[] = {cut(bx, by, BoxSide, 1, viewport),
                                                  cut(bx, by + BoxSide - 1, BoxSide, 1, viewport),
                                                  cut(bx, by + 1, 1, BoxSide - 2, viewport),
                                                  cut(bx + BoxSide - 1, by + 1, 1, BoxSide - 2, viewport)};
                        for (const Rect &e : parts)
                                if (!e.isEmpty()) (void)painter.fill(e, edge);
                        const Rect minus = cut(bx + 2, by + BoxSide / 2, BoxSide - 4, 1, viewport);
                        if (!minus.isEmpty()) (void)painter.fill(minus, mark);
                        const Rect upright = cut(bx + BoxSide / 2, by + 2, 1, BoxSide - 4, viewport);
                        if (!open && !upright.isEmpty()) (void)painter.fill(upright, mark);
                }
                const Icon *icon = r.depth == 0 && _rootIcon != nullptr ? _rootIcon
                                   : open && _openIcon != nullptr       ? _openIcon
                                                                        : _folderIcon;
                if (r.depth == 1 && _childIcon != nullptr && _rows.name(row) == StringView(_childName))
                        icon = _childIcon;
                if (icon != nullptr && icon->isValid() && icon->height <= 16)
                        (void)painter.image(x + Indent, y + (RowHeight - icon->height) / 2, *icon, background);
                if (int(row) == editing()) continue;
                const StringView name = _rows.name(row);
                const int        tx = int(viewport.x) - int(scrollX()) + int(textX(r.depth));
                const int        tw = int(Fonts::unscii16.textWidth(name));
                const bool       selected = selection().isSelected(row);
                const bool       current = int(row) == this->current();
                const uint32_t   bg =
                        drawLabel(painter, viewport, tx - 2, y, tw + 4, selected, focus, current, background);
                const uint32_t fg = p.color(selected && focus ? Role::HighlightText : Role::FaceText);
                (void)painter.text(tx, y + (RowHeight - GuiStyle::TextHeight) / 2, name, Fonts::unscii16, fg, bg,
                                   uint16_t(tw > 0 ? tw : 1));
                if (!selected && focus && current) focusRect(painter, viewport, tx - 2, y, tw + 4);
        }
        return;
}

BMDS_NAMESPACE_END
