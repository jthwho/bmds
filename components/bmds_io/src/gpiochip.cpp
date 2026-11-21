/**
 * @file      gpiochip.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /dev/gpiochip0 (devplan/io.md §3.9, §12.3): a stream per open; lines as Linux describes them;
 * lines watched, their changes told after every claim, release and setup (the registry's hook,
 * `Change`s) and read as Linux's gpio_v2_line_info_changed records.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/gpiochip.h>
#include <bmds/gpiolines.h>
#include <bmds/interrupt.h>
#include <bmds/io.h>
#include <bmds/iopin.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/vfs.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxLines = 64;    // a watch mask's bits (the P4 has 55 pads)
        constexpr size_t MaxWatchers = 16; // fds watching at once

        // A change waiting to be read: the line's info then (its name is read with it).
        struct Pending {
                        int64_t  timeNs = 0;
                        uint64_t flags = 0;
                        uint32_t debounceUs = 0;
                        uint8_t  offset = 0;
                        uint8_t  type = 0;
                        char     consumer[32] = {};
        };

        // The fds that watch, compared at every change (under the lock: one at a time).
        constinit StaticObject<Mutex> watchersLock;
        constinit GpioChip           *watchers[MaxWatchers] = {};
        constinit std::atomic<int>    changing{0}; // `Change`s alive

        uint64_t hashOf(const GpioChip::LineInfo &l) {
                uint64_t   h = 1469598103934665603ull;
                const auto mix = [&](const void *p, size_t n) {
                        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
                };
                mix(&l.flags, sizeof(l.flags));
                mix(&l.debounceUs, sizeof(l.debounceUs));
                mix(l.consumer, strnlen(l.consumer, sizeof(l.consumer)));
                return h;
        }

        void copyName(char (&out)[32], const char *text) {
                std::snprintf(out, sizeof(out), "%s", text);
                return;
        }

} // namespace

struct GpioChip::Watch {
                Mutex         lock; // the watches and the queue
                Semaphore     queued{1, 0};
                WaitListOf<4> pollers;
                uint64_t      watched = 0;
                uint64_t      used = 0; // each watched line's `Used` as last seen
                uint64_t      hash[MaxLines] = {};
                Pending       queue[MaxEvents];
                size_t        head = 0;
                size_t        count = 0;
                bool          listed = false; // in `watchers`
};

// ---- Lines as Linux describes them

Error GpioChip::lineInfo(unsigned offset, LineInfo &out) {
        if (offset >= ioChip().padCount()) return Error::Invalid;
        IoPinRow row;
        if (Io::readPins(offset, Span<IoPinRow>(&row, 1)) != 1) return Error::Invalid;
        out = LineInfo();
        out.offset = offset;
        const IoPadState &s = row.state;
        using C = IoLineConfig;
        uint64_t f = 0;
        if (row.resourceClass != IoClass::Free) f |= C::Used;
        IoLineSettings r;
        if (GpioLines::settingsOf(offset, r)) {
                // A program's request: what it asked, as Linux reports it.
                using D = IoLineSettings::Direction;
                const bool output = r.direction == D::Output || (r.direction == D::AsIs && s.outputEnabled);
                f |= output ? C::Output : C::Input;
                if (output && r.drive == IoLineSettings::Drive::OpenDrain) f |= C::OpenDrain;
                if (output && r.drive == IoLineSettings::Drive::OpenSource) f |= C::OpenSource;
                if (r.activeLow) f |= C::ActiveLow;
                if (r.edges & IoLineSettings::Rising) f |= C::EdgeRising;
                if (r.edges & IoLineSettings::Falling) f |= C::EdgeFalling;
                if (r.realtime) f |= C::ClockRealtime;
                switch (r.bias) {
                        case IoLineSettings::Bias::PullUp: f |= C::PullUp; break;
                        case IoLineSettings::Bias::PullDown: f |= C::PullDown; break;
                        case IoLineSettings::Bias::Disabled: f |= C::BiasDisabled; break;
                        case IoLineSettings::Bias::AsIs:
                                f |= s.pullUp ? C::PullUp : s.pullDown ? C::PullDown : C::BiasDisabled;
                                break;
                }
                out.debounceUs = r.debounceUs;
        } else {
                // Linux's line is one or the other: an output reads back too.
                if (s.outputEnabled)
                        f |= C::Output;
                else if (s.inputEnabled)
                        f |= C::Input;
                if (s.openDrain) f |= C::OpenDrain;
                if (s.pullUp) f |= C::PullUp;
                if (s.pullDown) f |= C::PullDown;
                if (!s.pullUp && !s.pullDown) f |= C::BiasDisabled;
        }
        out.flags = f;
        // Linux's line names are the board's (its device tree's gpio-line-names).
        const IoPinBoard board = Io::boardOf(offset);
        copyName(out.name, board.label[0] != '\0' ? board.label : row.name);
        copyName(out.consumer, row.owner);
        return Error::Ok;
}

// ---- Changes told

GpioChip::Change::Change() {
        changing.fetch_add(1);
}

GpioChip::Change::~Change() {
        // The last one out tells (a change in it, or elsewhere meanwhile).
        if (changing.fetch_sub(1) == 1) linesChanged();
}

void GpioChip::linesChanged() {
        if (changing.load() > 0 || !watchersLock.isCreated()) return; // told at the batch's end
        Mutex::Locker lock(*watchersLock, WaitForever);
        for (GpioChip *c : watchers)
                if (c != nullptr) c->compare();
        return;
}

// Its watched lines against what it last saw; what moved queued. `watchersLock` held.
void GpioChip::compare() {
        Watch        &w = *_watch;
        Mutex::Locker lock(w.lock, WaitForever);
        bool          told = false;
        for (unsigned o = 0; o < MaxLines; ++o) {
                if ((w.watched & (1ull << o)) == 0) continue;
                LineInfo l;
                if (lineInfo(o, l).isError()) continue;
                const uint64_t h = hashOf(l);
                if (h == w.hash[o]) continue;
                w.hash[o] = h;
                const bool was = (w.used & (1ull << o)) != 0;
                const bool now = (l.flags & IoLineConfig::Used) != 0;
                w.used = now ? w.used | (1ull << o) : w.used & ~(1ull << o);
                if (w.count == MaxEvents) continue; // full: dropped, as Linux's
                Pending &p = w.queue[(w.head + w.count) % MaxEvents];
                ++w.count;
                p.timeNs = Clock::monotonicNs();
                p.flags = l.flags;
                p.debounceUs = l.debounceUs;
                p.offset = uint8_t(o);
                p.type = uint8_t(!was && now ? Requested : was && !now ? Released : Reconfigured);
                std::memcpy(p.consumer, l.consumer, sizeof(p.consumer));
                told = true;
        }
        if (told) {
                w.queued.release();
                w.pollers.notify();
        }
        return;
}

// ---- Watching

GpioChip::Watch *GpioChip::watchState() {
        if (_watch != nullptr) return _watch;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Watch));
        if (mem == nullptr) return nullptr;
        _watch = ::new (mem) Watch();
        return _watch;
}

Error GpioChip::watch(unsigned offset, LineInfo &out) {
        if (offset >= ioChip().padCount() || offset >= MaxLines) return Error::Invalid;
        if (!watchersLock.isCreated()) return Error::NotReady;
        Mutex::Locker list(*watchersLock, WaitForever);
        Watch        *w = watchState();
        if (w == nullptr) return Error::NoMem;
        if (!w->listed) {
                bool listed = false;
                for (GpioChip *&slot : watchers)
                        if (slot == nullptr && !listed) {
                                slot = this;
                                listed = true;
                        }
                if (!listed) return Error::Full;
                w->listed = true;
        }
        Mutex::Locker lock(w->lock, WaitForever);
        if (w->watched & (1ull << offset)) return Error::Busy;
        if (const Error err = lineInfo(offset, out); err.isError()) return err;
        w->watched |= 1ull << offset;
        w->hash[offset] = hashOf(out);
        w->used = (out.flags & IoLineConfig::Used) ? w->used | (1ull << offset) : w->used & ~(1ull << offset);
        return Error::Ok;
}

Error GpioChip::unwatch(unsigned offset) {
        if (offset >= ioChip().padCount() || offset >= MaxLines) return Error::Invalid;
        if (_watch == nullptr) return Error::Busy;
        Mutex::Locker lock(_watch->lock, WaitForever);
        if ((_watch->watched & (1ull << offset)) == 0) return Error::Busy;
        _watch->watched &= ~(1ull << offset);
        return Error::Ok;
}

Result<size_t> GpioChip::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(Changed)) return makeError<size_t>(Error::Invalid);
        Watch *w = watchState(); // nothing watched: a read waits, as Linux's
        if (w == nullptr) return makeError<size_t>(Error::NoMem);
        Deadline deadline(timeoutMs);
        for (;;) {
                size_t n = 0;
                {
                        Mutex::Locker lock(w->lock, WaitForever);
                        while (w->count > 0 && n + sizeof(Changed) <= buf.size()) {
                                const Pending &p = w->queue[w->head];
                                Changed        c;
                                std::memset(&c, 0, sizeof(c));
                                LineInfo now;
                                if (lineInfo(p.offset, now).isOk()) std::memcpy(c.name, now.name, sizeof(c.name));
                                std::memcpy(c.consumer, p.consumer, sizeof(c.consumer));
                                c.offset = p.offset;
                                c.flags = p.flags;
                                if (p.debounceUs != 0) {
                                        c.numAttrs = 1;
                                        c.attrs[0].id = IoLineConfig::AttrDebounce;
                                        c.attrs[0].value = p.debounceUs;
                                }
                                c.timestampNs = uint64_t(p.timeNs);
                                c.eventType = p.type;
                                std::memcpy(buf.data() + n, &c, sizeof(c));
                                n += sizeof(c);
                                w->head = (w->head + 1) % MaxEvents;
                                --w->count;
                        }
                }
                if (n > 0) return makeResult(n);
                const Error err = Interrupt::waitCurrent(w->queued, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

uint8_t GpioChip::readiness() {
        if (_watch == nullptr) return Writable;
        Mutex::Locker lock(_watch->lock, WaitForever);
        return _watch->count > 0 ? uint8_t(Readable | Writable) : uint8_t(Writable);
}

WaitList *GpioChip::readinessWaiters() {
        Watch *w = watchState();
        return w != nullptr ? &w->pollers : nullptr;
}

void GpioChip::handleClosed(uint8_t mode) {
        (void)mode;
        // Its last handle: off the watchers' list, then gone.
        if (_watch != nullptr) {
                if (_watch->listed && watchersLock.isCreated()) {
                        Mutex::Locker lock(*watchersLock, WaitForever);
                        for (GpioChip *&slot : watchers)
                                if (slot == this) slot = nullptr;
                }
                _watch->~Watch();
                Memory::free(_watch);
        }
        this->~GpioChip();
        Memory::free(this);
        return;
}

// ---- The device file

Error GpioChip::addFiles() {
        if (!Io::isReady()) return Error::NotReady;
        if (watchersLock.isCreated()) return Error::Exists;
        watchersLock.create();
        Vfs::DeviceFile file;
        file.name = "gpiochip0";
        file.arg = nullptr;
        file.isDevice = true;
        // A chip per open: its watches are its own (Linux's are per file).
        file.open = [](void *, uint8_t flags) -> Result<FileHandle::Ptr> {
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(GpioChip));
                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *chip = ::new (mem) GpioChip(0);
                auto [handle, err] = FileHandle::open(*chip, mode);
                if (err.isError()) {
                        chip->~GpioChip();
                        Memory::free(mem);
                }
                return Result<FileHandle::Ptr>(std::move(handle), err);
        };
        return Vfs::addDeviceFile(file);
}

BMDS_NAMESPACE_END
