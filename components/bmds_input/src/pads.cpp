/**
 * @file      pads.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The gamepads attached, as standard pads (devplan/controls.md §4, §7a).
 */

#include <cstring>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/pads.h>
#include <bmds/psramobject.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  MaxListeners = 4;
        constexpr int32_t AxisStep = 65536 / 16; // an axis moved less than this isn't news

        struct Slot {
                        bool       attached = false;
                        bool       used = false; // once attached: its pad may come back
                        uint16_t   location = 0;
                        Pads::Info info;
                        PadLayout  layout;
                        PadMapping mapping;
                        RawPad     raw;   // as last reported
                        PadState   state; // as last reported
                        uint16_t   descriptorLength = 0;
                        uint8_t    descriptor[Pads::MaxDescriptor] = {};
        };

        struct Table {
                        Mutex           lock;
                        Slot            slots[Pads::MaxPads];
                        Pads::Listener *listeners[MaxListeners] = {};
                        size_t          listening = 0;
        };

        constinit PsramObject<Table> table;

        void put(Table &t, Pads::Event::Type type, uint8_t pad, uint8_t index, int32_t value) {
                const Pads::Event e{type, pad, index, value};
                for (Pads::Listener *l : t.listeners)
                        if (l != nullptr) l->put(e);
                return;
        }

        // An axis's news: moved a step, or reached its centre or an end.
        bool moved(int32_t was, int32_t now) {
                if (was == now) return false;
                const int32_t d = now > was ? now - was : was - now;
                return d >= AxisStep || now == 0 || now <= -32767 || now >= 32767;
        }

        // The raw and standard changes of @p s to @p raw and @p state, told and kept.
        void changed(Table &t, Slot &s, const RawPad &raw, const PadState &state) {
                const uint8_t n = s.info.number;
                using Type = Pads::Event::Type;
                if (t.listening != 0) {
                        for (uint8_t i = 0; i < s.layout.axisCount(); ++i)
                                if (moved(s.raw.axes[i], raw.axes[i])) {
                                        s.raw.axes[i] = raw.axes[i];
                                        put(t, Type::RawAxis, n, i, raw.axes[i]);
                                }
                        for (uint8_t i = 0; i < s.layout.hatCount(); ++i)
                                if (s.raw.hats[i] != raw.hats[i]) {
                                        s.raw.hats[i] = raw.hats[i];
                                        put(t, Type::RawHat, n, i, raw.hats[i]);
                                }
                        for (uint32_t d = s.raw.buttons ^ raw.buttons; d != 0; d &= d - 1) {
                                const auto b = static_cast<uint8_t>(__builtin_ctz(d));
                                put(t, Type::RawButton, n, b, int32_t(raw.buttons >> b & 1u));
                        }
                        s.raw.buttons = raw.buttons;
                } else {
                        s.raw = raw;
                }
                for (uint32_t d = s.state.buttons ^ state.buttons; d != 0; d &= d - 1) {
                        const auto b = static_cast<uint8_t>(__builtin_ctz(d));
                        put(t, Type::Button, n, b, int32_t(state.buttons >> b & 1u));
                }
                s.state.buttons = state.buttons;
                for (uint8_t i = 0; i < PadState::AxisCount; ++i)
                        if (moved(s.state.axes[i], state.axes[i])) {
                                s.state.axes[i] = state.axes[i];
                                put(t, Type::Axis, n, i, state.axes[i]);
                        }
                return;
        }

        Slot *attachedSlot(Table &t, uint8_t number) {
                return number < Pads::MaxPads && t.slots[number].attached ? &t.slots[number] : nullptr;
        }

} // namespace

bool Pads::Listener::take(Event &event) {
        if (!table.isCreated()) return false;
        Mutex::Locker lock(table->lock, WaitForever);
        if (_count == 0) return false;
        event = _events[_head];
        _head = (_head + 1) % Capacity;
        --_count;
        return true;
}

void Pads::Listener::put(const Event &event) {
        if (_count == Capacity) {
                ++_dropped;
        } else {
                _events[(_head + _count) % Capacity] = event;
                ++_count;
        }
        _ready.release();
        return;
}

Error Pads::init() {
        if (table.isCreated()) return Error::Ok;
        if (table.create() == nullptr) return Error::NoMem;
        for (uint8_t i = 0; i < MaxPads; ++i) table->slots[i].info.number = i;
        return Error::Ok;
}

Result<uint8_t> Pads::attachHid(uint16_t vendor, uint16_t product, uint16_t location, Span<const uint8_t> descriptor) {
        if (!table.isCreated()) return makeError<uint8_t>(Error::NotReady);
        const PadLayout layout = PadLayout::parse(descriptor);
        if (!layout.isValid()) return makeError<uint8_t>(Error::NotSupported);
        // Its mapping: the database's line, else the usual order. The ROM is read in place.
        PadMapping mapping = PadMapping::standard(layout);
        Origin     origin = Origin::Standard;
        StringView name; // the line's, until the device's own arrives (if it has one)
        if (auto [bytes, err] = Vfs::map(RomDatabase, Vfs::Context()); err.isOk()) {
                const StringView text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                if (PadMapping::find(text, vendor, product, mapping, &name)) origin = Origin::Database;
        }

        Table        &t = *table;
        Mutex::Locker lock(t.lock, WaitForever);
        // The slot its pad had (here, else anywhere), else one never used, else any free.
        Slot *slot = nullptr;
        int   best = 0;
        for (Slot &s : t.slots) {
                if (s.attached) continue;
                const bool same = s.used && s.info.vendor == vendor && s.info.product == product;
                const int  score = same && s.location == location ? 4 : same ? 3 : !s.used ? 2 : 1;
                if (score > best) {
                        best = score;
                        slot = &s;
                }
        }
        if (slot == nullptr) return makeError<uint8_t>(Error::Full);
        Slot &s = *slot;
        s.attached = true;
        s.used = true;
        s.location = location;
        s.layout = layout;
        s.mapping = mapping;
        s.raw = RawPad();
        s.state = PadState();
        const uint8_t number = s.info.number;
        s.info = Info();
        s.info.number = number;
        s.info.kind = Kind::Hid;
        s.info.origin = origin;
        s.info.vendor = vendor;
        s.info.product = product;
        s.info.axes = layout.axisCount();
        s.info.hats = layout.hatCount();
        s.info.buttons = layout.buttonCount();
        const size_t length = name.size() < MaxName - 1 ? name.size() : MaxName - 1;
        std::memcpy(s.info.name, name.data(), length);
        s.info.name[length] = '\0';
        s.descriptorLength =
                static_cast<uint16_t>(descriptor.size() < MaxDescriptor ? descriptor.size() : MaxDescriptor);
        std::memcpy(s.descriptor, descriptor.data(), s.descriptorLength);
        // The first report is news only where it isn't at rest: an axis's rest is its centre.
        for (uint8_t i = 0; i < layout.axisCount(); ++i) s.raw.axes[i] = 0;
        put(t, Event::Type::Attached, number, 0, 0);
        bmdsInfo("Pads: pad%u: %04x:%04x, %u axes, %u hats, %u buttons, mapping: %s", number, vendor, product,
                 layout.axisCount(), layout.hatCount(), layout.buttonCount(),
                 origin == Origin::Database ? "database" : "standard");
        return makeResult(number);
}

void Pads::setName(uint8_t number, StringView name) {
        if (!table.isCreated()) return;
        Mutex::Locker lock(table->lock, WaitForever);
        Slot         *s = attachedSlot(*table, number);
        if (s == nullptr || name.isEmpty()) return;
        const size_t n = name.size() < MaxName - 1 ? name.size() : MaxName - 1;
        std::memcpy(s->info.name, name.data(), n);
        s->info.name[n] = '\0';
        return;
}

void Pads::reportHid(uint8_t number, Span<const uint8_t> report) {
        if (!table.isCreated()) return;
        Table        &t = *table;
        Mutex::Locker lock(t.lock, WaitForever);
        Slot         *s = attachedSlot(t, number);
        RawPad        raw;
        if (s == nullptr || !s->layout.decode(report, raw)) return; // another report ID's
        PadState state;
        s->mapping.apply(raw, state);
        changed(t, *s, raw, state);
        return;
}

void Pads::detach(uint8_t number) {
        if (!table.isCreated()) return;
        Table        &t = *table;
        Mutex::Locker lock(t.lock, WaitForever);
        Slot         *s = attachedSlot(t, number);
        if (s == nullptr) return;
        changed(t, *s, RawPad(), PadState()); // its buttons up, its axes centred
        s->attached = false;
        put(t, Event::Type::Detached, number, 0, 0);
        bmdsInfo("Pads: pad%u: gone", number);
        return;
}

size_t Pads::list(Span<Info> out) {
        if (!table.isCreated()) return 0;
        Mutex::Locker lock(table->lock, WaitForever);
        size_t        n = 0;
        for (const Slot &s : table->slots)
                if (s.attached && n < out.size()) out[n++] = s.info;
        return n;
}

Error Pads::info(uint8_t number, Info &out) {
        if (!table.isCreated()) return Error::NotFound;
        Mutex::Locker lock(table->lock, WaitForever);
        const Slot   *s = attachedSlot(*table, number);
        if (s == nullptr) return Error::NotFound;
        out = s->info;
        return Error::Ok;
}

Error Pads::mapping(uint8_t number, PadMapping &out) {
        if (!table.isCreated()) return Error::NotFound;
        Mutex::Locker lock(table->lock, WaitForever);
        const Slot   *s = attachedSlot(*table, number);
        if (s == nullptr) return Error::NotFound;
        out = s->mapping;
        return Error::Ok;
}

Result<size_t> Pads::descriptor(uint8_t number, Span<uint8_t> out) {
        if (!table.isCreated()) return makeError<size_t>(Error::NotFound);
        Mutex::Locker lock(table->lock, WaitForever);
        const Slot   *s = attachedSlot(*table, number);
        if (s == nullptr) return makeError<size_t>(Error::NotFound);
        const size_t n = s->descriptorLength < out.size() ? s->descriptorLength : out.size();
        std::memcpy(out.data(), s->descriptor, n);
        return makeResult(n);
}

Error Pads::state(uint8_t number, PadState &standard, RawPad &raw) {
        if (!table.isCreated()) return Error::NotFound;
        Mutex::Locker lock(table->lock, WaitForever);
        const Slot   *s = attachedSlot(*table, number);
        if (s == nullptr) return Error::NotFound;
        standard = s->state;
        raw = s->raw;
        return Error::Ok;
}

Error Pads::listen(Listener &listener) {
        if (!table.isCreated()) return Error::NotReady;
        Mutex::Locker lock(table->lock, WaitForever);
        for (Listener *&l : table->listeners) {
                if (l != nullptr) continue;
                l = &listener;
                ++table->listening;
                return Error::Ok;
        }
        return Error::Full;
}

void Pads::unlisten(Listener &listener) {
        if (!table.isCreated()) return;
        Mutex::Locker lock(table->lock, WaitForever);
        for (Listener *&l : table->listeners) {
                if (l != &listener) continue;
                l = nullptr;
                --table->listening;
        }
        return;
}

BMDS_NAMESPACE_END
