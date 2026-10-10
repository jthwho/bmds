/**
 * @file      notificationlog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The notifications kept (devplan/notifications.md §3.2).
 */

#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/notificationlog.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Ends @p text (@p size bytes) with a NUL, at a character's end if it was full.
        void cut(char *text, size_t size) {
                if (size == 0) return;
                size_t n = 0;
                while (n + 1 < size && text[n] != '\0') ++n;
                if (n + 1 == size)
                        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xc0u) == 0x80u) --n;
                text[n] = '\0';
                return;
        }

        template <size_t N> void cut(char (&text)[N]) {
                cut(text, N);
        }

} // namespace

NotificationLog::~NotificationLog() {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Capacity; ++i) _slots[i].~Slot();
        Memory::free(_slots);
}

NotificationLog::Slot *NotificationLog::find(uint32_t id) const {
        if (_slots == nullptr || id == 0) return nullptr;
        for (size_t i = 0; i < Capacity; ++i)
                if (_slots[i].used && _slots[i].n.id == id) return &_slots[i];
        return nullptr;
}

// A free slot, or the oldest one without a balloon due (else the oldest) made free.
NotificationLog::Slot *NotificationLog::freeSlot() {
        Slot *oldest = nullptr;
        Slot *oldestQuiet = nullptr;
        for (size_t i = 0; i < Capacity; ++i) {
                Slot &s = _slots[i];
                if (!s.used) return &s;
                if (oldest == nullptr || s.order < oldest->order) oldest = &s;
                if (!s.n.balloon && (oldestQuiet == nullptr || s.order < oldestQuiet->order)) oldestQuiet = &s;
        }
        return oldestQuiet != nullptr ? oldestQuiet : oldest;
}

Result<uint32_t> NotificationLog::post(const Notification &n, int32_t owner, uint32_t listener, int64_t nowNs) {
        if (_slots == nullptr) {
                _slots = static_cast<Slot *>(Memory::alloc(_space, Capacity * sizeof(Slot)));
                if (_slots == nullptr) return makeError<uint32_t>(Error::NoMem);
                for (size_t i = 0; i < Capacity; ++i) ::new (&_slots[i]) Slot();
        }
        Slot *s = nullptr;
        if (n.replaces != 0) {
                s = find(n.replaces);
                if (s != nullptr && s->n.owner != owner) return makeError<uint32_t>(Error::PermissionDenied);
        }
        uint32_t id = s != nullptr ? s->n.id : 0;
        if (s == nullptr) {
                s = freeSlot();
                if (++_lastId == 0) ++_lastId; // 0 is "none"
                id = _lastId;
        }
        s->n = n;
        cut(s->n.app);
        cut(s->n.icon);
        cut(s->n.title);
        cut(s->n.body);
        cut(s->n.targetText);
        cut(s->n.message);
        if (s->n.actionCount > Notification::MaxActions) s->n.actionCount = Notification::MaxActions;
        for (auto &label : s->n.actions) cut(label);
        if (uint8_t(s->n.urgency) > uint8_t(Notification::Urgency::Critical))
                s->n.urgency = Notification::Urgency::Normal;
        if (uint8_t(s->n.target) > uint8_t(Notification::Target::Service)) s->n.target = Notification::Target::None;
        s->n.replaces = 0;
        s->n.id = id;
        s->n.owner = owner;
        s->n.listener = listener;
        s->n.postedNs = nowNs;
        s->n.balloon = true;
        s->n.read = false;
        s->order = ++_order;
        s->used = true;
        touch(*s);
        return makeResult(id);
}

Error NotificationLog::endBalloon(uint32_t id, Ended how) {
        Slot *s = find(id);
        if (s == nullptr) return Error::NotExist;
        if (!s->n.balloon) return Error::Ok;
        s->n.balloon = false;
        if (how == Ended::Activated || how == Ended::Dismissed) s->n.read = true; // the user saw it
        if ((s->n.flags & Notification::Transient) != 0) s->used = false;
        ++_change;
        if (s->used) s->n.changed = _change;
        return Error::Ok;
}

Error NotificationLog::close(uint32_t id, int32_t owner) {
        Slot *s = find(id);
        if (s == nullptr) return Error::NotExist;
        if (owner != -1 && s->n.owner != owner) return Error::PermissionDenied;
        s->used = false;
        ++_change;
        return Error::Ok;
}

void NotificationLog::clear() {
        if (_slots == nullptr) return;
        bool any = false;
        for (size_t i = 0; i < Capacity; ++i) {
                Slot &s = _slots[i];
                if (!s.used || s.n.balloon) continue;
                s.used = false;
                any = true;
        }
        if (any) ++_change;
        return;
}

void NotificationLog::markRead(uint32_t id) {
        if (_slots == nullptr) return;
        for (size_t i = 0; i < Capacity; ++i) {
                Slot &s = _slots[i];
                if (!s.used || s.n.read || (id != 0 && s.n.id != id)) continue;
                s.n.read = true;
                touch(s);
        }
        return;
}

void NotificationLog::dropListener(uint32_t listener) {
        if (_slots == nullptr || listener == 0) return;
        for (size_t i = 0; i < Capacity; ++i)
                if (_slots[i].used && _slots[i].n.listener == listener) _slots[i].n.listener = 0;
        return;
}

size_t NotificationLog::unread() const {
        size_t n = 0;
        if (_slots == nullptr) return 0;
        for (size_t i = 0; i < Capacity; ++i) n += _slots[i].used && !_slots[i].n.read ? 1 : 0;
        return n;
}

size_t NotificationLog::count() const {
        size_t n = 0;
        if (_slots == nullptr) return 0;
        for (size_t i = 0; i < Capacity; ++i) n += _slots[i].used ? 1 : 0;
        return n;
}

Error NotificationLog::get(uint32_t id, Notification &out) const {
        const Slot *s = find(id);
        if (s == nullptr) return Error::NotExist;
        out = s->n;
        return Error::Ok;
}

size_t NotificationLog::ids(Span<uint32_t> out) const {
        // Few (Capacity): a selection by order, no sorting space.
        const size_t total = count();
        uint64_t     below = UINT64_MAX;
        for (size_t k = 0; k < total && k < out.size(); ++k) {
                const Slot *next = nullptr;
                for (size_t i = 0; i < Capacity; ++i) {
                        const Slot &s = _slots[i];
                        if (s.used && s.order < below && (next == nullptr || s.order > next->order)) next = &s;
                }
                if (next == nullptr) break;
                out[k] = next->n.id;
                below = next->order;
        }
        return total;
}

BMDS_NAMESPACE_END
