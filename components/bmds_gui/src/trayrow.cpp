/**
 * @file      trayrow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system tray's icons and their order (devplan/mediaplayer.md §6e).
 */

#include <cstring>
#include <bmds/trayrow.h>

BMDS_NAMESPACE_BEGIN

int TrayRow::indexOf(StringView reply, uint32_t id) const {
        for (size_t i = 0; i < _entries.size(); ++i)
                if (_entries[i].id == id && TrayProtocol::view(_entries[i].reply) == reply) return int(i);
        return -1;
}

bool TrayRow::before(const Entry &a, const Entry &b) const {
        if (a.category != b.category) return uint32_t(a.category) < uint32_t(b.category);
        return a.order < b.order;
}

Error TrayRow::update(const TrayProtocol::Update &message, bool *moved) {
        const StringView reply = TrayProtocol::view(message.reply);
        if (reply.isEmpty()) return Error::Invalid;
        int at = indexOf(reply, message.id);
        if (at < 0) {
                if (_entries.size() >= MaxIcons) return Error::Full;
                Entry e;
                TrayProtocol::copy(e.reply, reply);
                e.id = message.id;
                e.order = _nextOrder++;
                if (_entries.append(e).isError()) return Error::NoMem;
                at = int(_entries.size() - 1);
                if (moved != nullptr) *moved = true; // a new one is shown or not: the row's order changed
        } else if (moved != nullptr) {
                const Entry &was = _entries[size_t(at)];
                *moved = was.isVisible() != ((message.flags & TrayProtocol::Visible) != 0) ||
                         was.category != message.category;
        }
        Entry &e = _entries[size_t(at)];
        e.category = uint32_t(message.category) <= uint32_t(TrayProtocol::Category::Hardware)
                             ? message.category
                             : TrayProtocol::Category::Application;
        e.flags = message.flags;
        TrayProtocol::copy(e.icon, TrayProtocol::view(message.icon));
        TrayProtocol::toolTipOf(message, e.toolTip);
        if ((e.flags & TrayProtocol::HasPixels) == 0) e.hasPixels = false;
        return Error::Ok;
}

Error TrayRow::setPixels(const TrayProtocol::Pixels &message) {
        if (message.width != TrayProtocol::IconSide || message.height != TrayProtocol::IconSide) return Error::Invalid;
        const int at = indexOf(TrayProtocol::view(message.reply), message.id);
        if (at < 0) return Error::NotFound;
        Entry &e = _entries[size_t(at)];
        std::memcpy(e.argb, message.argb, sizeof(e.argb));
        e.hasPixels = true;
        return Error::Ok;
}

bool TrayRow::remove(StringView reply, uint32_t id) {
        const int at = indexOf(reply, id);
        if (at < 0) return false;
        _entries.removeAt(size_t(at));
        return true;
}

size_t TrayRow::dropGone(Gone gone, void *arg) {
        size_t dropped = 0;
        for (size_t i = _entries.size(); i-- > 0;) {
                if (!gone(arg, TrayProtocol::view(_entries[i].reply))) continue;
                _entries.removeAt(i);
                ++dropped;
        }
        return dropped;
}

size_t TrayRow::shownCount() const {
        size_t n = 0;
        for (const Entry &e : _entries) n += e.isVisible() ? 1 : 0;
        return n;
}

// The one with @p position shown ones left of it (a few icons: counted each time).
const TrayRow::Entry *TrayRow::shown(size_t position) const {
        for (const Entry &e : _entries) {
                if (!e.isVisible()) continue;
                size_t left = 0;
                for (const Entry &o : _entries) left += o.isVisible() && &o != &e && before(o, e) ? 1 : 0;
                if (left == position) return &e;
        }
        return nullptr;
}

int TrayRow::positionAt(int x) const {
        if (x < 0) return -1;
        const size_t p = size_t(x) / CellWidth;
        return p < shownCount() ? int(p) : -1;
}

int TrayRow::positionOf(StringView reply, uint32_t id) const {
        const int at = indexOf(reply, id);
        if (at < 0 || !_entries[size_t(at)].isVisible()) return -1;
        const Entry &e = _entries[size_t(at)];
        size_t       left = 0;
        for (const Entry &o : _entries) left += o.isVisible() && &o != &e && before(o, e) ? 1 : 0;
        return int(left);
}

BMDS_NAMESPACE_END
