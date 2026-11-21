/**
 * @file      gpiolines.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' line requests on /dev/gpiochip0 (devplan/io.md §12.1): Linux's GPIO v2 line fd. The
 * pads are claimed for the consumer and set up as the config says, their registers saved and
 * put back when the fd goes; an input's edges come from its pad's interrupt into the request's
 * ring, waking the reader and its pollers.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <bmds/clock.h>
#include <bmds/gpiochip.h>
#include <bmds/deadline.h>
#include <bmds/gpiolines.h>
#include <bmds/interrupt.h>
#include <bmds/io.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>
#include <bmds/system.h>
#include <bmds/waitlist.h>

#include "iochipops.h"

BMDS_NAMESPACE_BEGIN

// An edge as the interrupt writes it.
struct EdgeRecord {
                std::atomic<uint32_t> seq{0}; // the request's number; 0 while the rest is written
                int64_t               timeNs = 0;
                uint32_t              lineSeq = 0;
                uint8_t               index = 0;
                bool                  rising = false;
};

struct GpioLines::State {
                Mutex                 lock;         // the lines' settings and values, a reader at a time
                IoClaim               claim;        // the pads, for the consumer
                Semaphore             events{1, 0}; // released at each edge
                WaitListOf<4>         pollers;      // poll()s waiting for an edge
                std::atomic<uint32_t> written{0};   // the last record's number (the request's seqno)
                uint32_t              readSeq = 0;  // the last one read
                uint32_t              capacity = 0; // records in the ring
                EdgeRecord           *ring = nullptr;
};

struct GpioLines::Line {
                State           *state = nullptr; // its request's (the interrupt's way to the ring)
                uint8_t          pad = 0;
                uint8_t          index = 0;
                IoLineSettings   settings;
                IoChipOps::Saved saved;
                bool             interrupt = false; // its handler is added
                bool             high = false;      // an output's physical level now
                uint32_t         lineSeq = 0;       // its edges seen (the interrupt's)
                int64_t          lastUs = 0;        // its last edge taken (debounce)
};

namespace {

        // A pad's interrupt: an edge into its request's ring, the reader and pollers woken. ESP-IDF's
        // GPIO ISR service runs the pads' handlers one after another: one writer at a time.
        void lineIsr(void *arg) {
                auto *const           l = static_cast<GpioLines::Line *>(arg);
                GpioLines::State     &st = *l->state;
                const int64_t         us = Detail::monotonicUs();
                const IoLineSettings &s = l->settings;
                if (s.debounceUs != 0 && l->lineSeq != 0 && us - l->lastUs < int64_t(s.debounceUs)) return;
                l->lastUs = us;
                const bool logical = IoChipOps::level(l->pad) != s.activeLow;
                // One way only: that way (the level may have moved on already); both: the level says.
                const bool     rising = s.edges == IoLineSettings::Rising    ? true
                                        : s.edges == IoLineSettings::Falling ? false
                                                                             : logical;
                const uint32_t seq = st.written.load(std::memory_order_relaxed) + 1;
                EdgeRecord    &e = st.ring[seq % st.capacity];
                e.seq.store(0, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                e.timeNs = us * 1000;
                e.lineSeq = ++l->lineSeq;
                e.index = l->index;
                e.rising = rising;
                e.seq.store(seq, std::memory_order_release);
                st.written.store(seq, std::memory_order_release);
                st.events.releaseFromIsr();
                st.pollers.notifyFromIsr();
                return;
        }

        // The physical edges for logical @p edges (active low swaps them).
        gpio_int_type_t interruptType(const IoLineSettings &s) {
                uint8_t e = s.edges;
                if (s.activeLow)
                        e = uint8_t(((e & IoLineSettings::Rising) ? IoLineSettings::Falling : 0) |
                                    ((e & IoLineSettings::Falling) ? IoLineSettings::Rising : 0));
                return e == IoLineSettings::Rising    ? GPIO_INTR_POSEDGE
                       : e == IoLineSettings::Falling ? GPIO_INTR_NEGEDGE
                                                      : GPIO_INTR_ANYEDGE;
        }

        void stopInterrupt(GpioLines::Line &l) {
                if (!l.interrupt) return;
                const gpio_num_t gpio = gpio_num_t(l.pad);
                (void)gpio_intr_disable(gpio);
                (void)gpio_isr_handler_remove(gpio);
                (void)gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
                l.interrupt = false;
                return;
        }

        Error startInterrupt(GpioLines::Line &l) {
                const gpio_num_t gpio = gpio_num_t(l.pad);
                const esp_err_t  installed = gpio_install_isr_service(0); // the SD card's may have
                if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) return Error::IoError;
                if (gpio_set_intr_type(gpio, interruptType(l.settings)) != ESP_OK ||
                    gpio_isr_handler_add(gpio, lineIsr, &l) != ESP_OK || gpio_intr_enable(gpio) != ESP_OK) {
                        (void)gpio_isr_handler_remove(gpio);
                        return Error::IoError;
                }
                l.interrupt = true;
                return Error::Ok;
        }

        // Sets line @p l's pad up as its settings say (an as-is direction or bias: as the pad is).
        void setUp(GpioLines::Line &l) {
                const IoLineSettings &s = l.settings;
                IoPadState            now;
                IoChipOps::readPad(l.pad, now);
                IoPinSetup p;
                p.pad = l.pad;
                p.drive = now.drive;
                using D = IoLineSettings::Direction;
                const bool output =
                        s.direction == D::Output || (s.direction == D::AsIs && now.outputEnabled && !now.lpIo);
                bool high = s.direction == D::AsIs ? now.outputLevel : s.value != s.activeLow;
                if (!output) {
                        p.mode = IoPinSetup::Mode::Input;
                } else if (s.drive == IoLineSettings::Drive::OpenDrain) {
                        p.mode = IoPinSetup::Mode::OpenDrain;
                } else if (s.drive == IoLineSettings::Drive::OpenSource) {
                        // No open source on the chip: driven high, let go (an input) for low.
                        p.mode = high ? IoPinSetup::Mode::Output : IoPinSetup::Mode::Input;
                } else {
                        p.mode = IoPinSetup::Mode::Output;
                }
                p.high = high;
                switch (s.bias) {
                        case IoLineSettings::Bias::AsIs:
                                p.pullUp = now.pullUp;
                                p.pullDown = now.pullDown;
                                break;
                        case IoLineSettings::Bias::PullUp: p.pullUp = true; break;
                        case IoLineSettings::Bias::PullDown: p.pullDown = true; break;
                        case IoLineSettings::Bias::Disabled: break;
                }
                IoChipOps::configurePad(l.pad, p);
                l.high = output && high;
                return;
        }

        bool isOutput(const GpioLines::Line &l) {
                return l.settings.direction == IoLineSettings::Direction::Output;
        }

} // namespace

namespace {

        // The live requests, for the restart hook (a restart leaves IO_MUX and the matrix as they
        // are: devplan/io.md §10.4).
        // What each held pad's request asked, for line info (a spinlock: a copy's moment). In
        // PSRAM, made by the first request and kept: 1 KB nobody pays for until a program asks.
        constexpr size_t                        MaxPads = 64;
        constinit SpinLock                      publishedLock;
        constinit std::atomic<IoLineSettings *> published{nullptr};
        constinit uint64_t                      publishedMask = 0;

        bool makePublished() {
                if (published.load() != nullptr) return true;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(IoLineSettings) * MaxPads);
                if (mem == nullptr) return false;
                auto *table = static_cast<IoLineSettings *>(mem);
                for (size_t i = 0; i < MaxPads; ++i) ::new (&table[i]) IoLineSettings();
                IoLineSettings *none = nullptr;
                if (!published.compare_exchange_strong(none, table)) Memory::free(mem); // another made it first
                return true;
        }

        void publish(unsigned pad, const IoLineSettings &s) {
                if (pad >= MaxPads || published.load() == nullptr) return;
                SpinLock::Locker lock(publishedLock);
                published.load()[pad] = s;
                publishedMask |= 1ull << pad;
                return;
        }

        void unpublish(unsigned pad) {
                if (pad >= MaxPads) return;
                SpinLock::Locker lock(publishedLock);
                publishedMask &= ~(1ull << pad);
                return;
        }

        // Lock-free: a slot is taken and given back by compare-and-swap.
        constinit std::atomic<GpioLines *> live[32] = {};
        constinit std::atomic<bool>        hooked{false};

        void putBackAtRestart();

        void addLive(GpioLines *g) {
                if (!hooked.exchange(true) && System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: programs' lines will stay set up across a restart");
                for (std::atomic<GpioLines *> &slot : live) {
                        GpioLines *none = nullptr;
                        if (slot.compare_exchange_strong(none, g)) return;
                }
                return; // more than 32 at once: not put back at a restart, only when closed
        }

        void removeLive(GpioLines *g) {
                for (std::atomic<GpioLines *> &slot : live) {
                        GpioLines *mine = g;
                        if (slot.compare_exchange_strong(mine, nullptr)) return;
                }
                return;
        }

} // namespace

bool GpioLines::settingsOf(unsigned pad, IoLineSettings &out) {
        if (pad >= MaxPads) return false;
        SpinLock::Locker lock(publishedLock);
        if ((publishedMask & (1ull << pad)) == 0) return false;
        out = published.load()[pad];
        return true;
}

void GpioLines::putBack() {
        for (size_t i = _count; i-- > 0;) IoChipOps::restorePad(_lines[i].pad, _lines[i].saved);
        return;
}

namespace {

        // No lock: the restart's task, the others about to stop.
        void putBackAtRestart() {
                for (std::atomic<GpioLines *> &slot : live)
                        if (GpioLines *g = slot.load()) g->putBack();
                return;
        }

} // namespace

Result<FileHandle::Ptr> GpioLines::request(Span<const uint32_t> offsets, StringView consumer,
                                           const IoLineConfig &config, uint32_t eventBufferSize) {
        if (!Io::isReady()) return makeError<FileHandle::Ptr>(Error::NotReady);
        const size_t n = offsets.size();
        if (n == 0 || n > IoLineConfig::MaxLines) return makeError<FileHandle::Ptr>(Error::Invalid);
        const size_t pads = ioChip().padCount();
        for (size_t i = 0; i < n; ++i) {
                if (offsets[i] >= pads) return makeError<FileHandle::Ptr>(Error::Invalid);
                for (size_t k = i + 1; k < n; ++k)
                        if (offsets[i] == offsets[k]) return makeError<FileHandle::Ptr>(Error::Invalid);
        }
        IoLineSettings settings[IoLineConfig::MaxLines];
        if (const Error err = IoLine::resolve(config, n, settings); err.isError())
                return makeError<FileHandle::Ptr>(err);
        if (!makePublished()) return makeError<FileHandle::Ptr>(Error::NoMem);
        uint32_t capacity = eventBufferSize == 0 ? uint32_t(n) * EventsPerLine : eventBufferSize;
        if (capacity > MaxEvents) capacity = MaxEvents;
        if (capacity < 2) capacity = 2;

        // One block: the stream, its state, its lines, its ring.
        const size_t bytes = sizeof(GpioLines) + sizeof(State) + n * sizeof(Line) + capacity * sizeof(EdgeRecord);
        auto        *mem = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *self = ::new (mem) GpioLines();
        self->_state = ::new (mem + sizeof(GpioLines)) State();
        self->_lines = reinterpret_cast<Line *>(mem + sizeof(GpioLines) + sizeof(State));
        auto *ring = reinterpret_cast<EdgeRecord *>(mem + sizeof(GpioLines) + sizeof(State) + n * sizeof(Line));
        for (uint32_t i = 0; i < capacity; ++i) ::new (&ring[i]) EdgeRecord();
        self->_state->ring = ring;
        self->_state->capacity = capacity;
        self->_count = n;
        const auto destroy = [&]() {
                for (uint32_t i = 0; i < capacity; ++i) ring[i].~EdgeRecord();
                for (size_t i = 0; i < n; ++i) self->_lines[i].~Line();
                self->_state->~State();
                self->~GpioLines();
                Memory::free(mem);
        };

        // Watchers are told once, when the lines are claimed, set up and published.
        const GpioChip::Change change;
        // The pads, all or none, for the consumer (a program's name when it gave none).
        IoUse uses[IoLineConfig::MaxLines];
        for (size_t i = 0; i < n; ++i) uses[i] = IoUse{IoResource(offsets[i]), "line"};
        char         owner[IoHolder::NameBytes];
        const size_t ownerLength = consumer.size() < sizeof(owner) - 1 ? consumer.size() : sizeof(owner) - 1;
        std::memcpy(owner, consumer.data(), ownerLength);
        owner[ownerLength] = '\0';
        if (ownerLength == 0) std::strcpy(owner, "gpio");
        IoResource blocked = NoIoResource;
        auto [claim, cerr] =
                Io::registry().claim(StringView(owner), IoOwnerKind::User, Span<const IoUse>(uses, n), 0u, &blocked);
        for (size_t i = 0; i < n; ++i) ::new (&self->_lines[i]) Line();
        if (cerr.isError()) {
                destroy();
                return makeError<FileHandle::Ptr>(cerr == Error::Busy ? Error::Busy : cerr);
        }
        self->_state->claim = std::move(claim);
        for (size_t i = 0; i < n; ++i) {
                Line &l = self->_lines[i];
                l.state = self->_state;
                l.pad = uint8_t(offsets[i]);
                l.index = uint8_t(i);
                l.settings = settings[i];
                IoChipOps::savePad(l.pad, l.saved);
                setUp(l);
                publish(l.pad, l.settings);
                if (l.settings.edges != IoLineSettings::NoEdges && startInterrupt(l).isError())
                        bmdsWarn("io: %s: GPIO%u's edges not watched: its interrupt can't be set", owner,
                                 unsigned(l.pad));
        }
        auto [handle, herr] = FileHandle::open(*self, FileHandle::Read | FileHandle::Write);
        if (herr.isOk()) addLive(self);
        if (herr.isError()) {
                for (size_t i = n; i-- > 0;) {
                        stopInterrupt(self->_lines[i]);
                        unpublish(self->_lines[i].pad);
                        IoChipOps::restorePad(self->_lines[i].pad, self->_lines[i].saved);
                }
                self->_state->claim.reset();
                destroy();
                return makeError<FileHandle::Ptr>(herr);
        }
        return makeResult(std::move(handle));
}

Error GpioLines::setConfig(const IoLineConfig &config) {
        IoLineSettings settings[IoLineConfig::MaxLines];
        if (const Error err = IoLine::resolve(config, _count, settings); err.isError()) return err;
        const GpioChip::Change change; // before the lock: told after it
        Mutex::Locker          lock(_state->lock, WaitForever);
        for (size_t i = 0; i < _count; ++i) {
                Line &l = _lines[i];
                // A line that keeps watching keeps its interrupt through the change: an edge the
                // change itself makes (a pull flipped) is seen, as Linux sees it.
                const bool watch = settings[i].edges != IoLineSettings::NoEdges;
                if (!watch) stopInterrupt(l);
                l.settings = settings[i];
                setUp(l);
                publish(l.pad, l.settings);
                if (watch && l.interrupt)
                        (void)gpio_set_intr_type(gpio_num_t(l.pad), interruptType(l.settings));
                else if (watch && startInterrupt(l).isError())
                        bmdsWarn("io: GPIO%u's edges not watched: its interrupt can't be set", unsigned(l.pad));
        }
        return Error::Ok;
}

Error GpioLines::values(uint64_t mask, uint64_t &bits) {
        if (mask == 0 || (_count < 64 && (mask >> _count) != 0)) return Error::Invalid;
        Mutex::Locker lock(_state->lock, WaitForever);
        bits = 0;
        for (size_t i = 0; i < _count; ++i) {
                if ((mask & (1ull << i)) == 0) continue;
                const Line &l = _lines[i];
                // A push-pull output: what it drives; anything else: the pad read (an open drain
                // or source line reads back what the wire does).
                const bool physical = isOutput(l) && l.settings.drive == IoLineSettings::Drive::PushPull
                                              ? l.high
                                              : IoChipOps::level(l.pad);
                if (physical != l.settings.activeLow) bits |= 1ull << i;
        }
        return Error::Ok;
}

Error GpioLines::setValues(uint64_t mask, uint64_t bits) {
        if (mask == 0 || (_count < 64 && (mask >> _count) != 0)) return Error::Invalid;
        Mutex::Locker lock(_state->lock, WaitForever);
        for (size_t i = 0; i < _count; ++i)
                if ((mask & (1ull << i)) != 0 && !isOutput(_lines[i])) return Error::PermissionDenied;
        for (size_t i = 0; i < _count; ++i) {
                if ((mask & (1ull << i)) == 0) continue;
                Line &l = _lines[i];
                l.settings.value = (bits & (1ull << i)) != 0;
                const bool high = l.settings.value != l.settings.activeLow;
                if (l.settings.drive == IoLineSettings::Drive::OpenSource) {
                        setUp(l); // driven high, or let go
                } else {
                        IoChipOps::setLevel(l.pad, high);
                        l.high = high;
                }
        }
        return Error::Ok;
}

Result<size_t> GpioLines::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(Event)) return makeError<size_t>(Error::Invalid);
        Deadline      deadline(timeoutMs);
        Mutex::Locker lock(_state->lock, WaitForever);
        State        &st = *_state;
        for (;;) {
                size_t n = 0;
                while (n + sizeof(Event) <= buf.size()) {
                        const uint32_t written = st.written.load(std::memory_order_acquire);
                        if (written == st.readSeq) break;
                        // Fallen more than the ring behind: the oldest are gone (the numbers show it).
                        if (written - st.readSeq > st.capacity) st.readSeq = written - st.capacity;
                        const uint32_t    seq = st.readSeq + 1;
                        const EdgeRecord &r = st.ring[seq % st.capacity];
                        Event             e;
                        e.timestampNs = uint64_t(r.timeNs);
                        e.lineSeqno = r.lineSeq;
                        const uint8_t index = r.index;
                        e.id = r.rising ? Rising : Falling;
                        std::atomic_thread_fence(std::memory_order_acquire);
                        st.readSeq = seq;
                        if (r.seq.load(std::memory_order_acquire) != seq) continue; // overwritten while copied
                        if (index >= _count) continue;
                        const Line &l = _lines[index];
                        e.offset = l.pad;
                        e.seqno = seq;
                        if (l.settings.realtime) // the wall clock's offset from the monotonic one now
                                e.timestampNs =
                                        uint64_t(int64_t(e.timestampNs) + Clock::now().ns - Clock::monotonicNs());
                        std::memcpy(buf.data() + n, &e, sizeof(e));
                        n += sizeof(e);
                }
                if (n > 0) return makeResult(n);
                const Error err = Interrupt::waitCurrent(st.events, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

uint8_t GpioLines::readiness() {
        return _state->written.load(std::memory_order_acquire) != _state->readSeq ? uint8_t(Readable | Writable)
                                                                                  : uint8_t(Writable);
}

WaitList *GpioLines::readinessWaiters() {
        return &_state->pollers;
}

void GpioLines::handleClosed(uint8_t mode) {
        (void)mode;
        // Its last handle: the interrupts stopped, the pads as they were, the claim ended; watchers
        // told once, at the end of this block.
        removeLive(this);
        {
                const GpioChip::Change change;
                for (size_t i = _count; i-- > 0;) {
                        stopInterrupt(_lines[i]);
                        unpublish(_lines[i].pad);
                        IoChipOps::restorePad(_lines[i].pad, _lines[i].saved);
                }
                _state->claim.reset();
        }
        const uint32_t capacity = _state->capacity;
        for (uint32_t i = 0; i < capacity; ++i) _state->ring[i].~EdgeRecord();
        for (size_t i = 0; i < _count; ++i) _lines[i].~Line();
        _state->~State();
        this->~GpioLines();
        Memory::free(this);
        return;
}

BMDS_NAMESPACE_END
