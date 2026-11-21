/**
 * @file      iogpio.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The pins the user sets up (devplan/io.md §10): `io/gpio`, checked, applied as it changes, held
 * for the owner `io`; every pad's registers saved when it is taken and put back when it is let go;
 * and `io pulse`.
 */

#include <cstdio>
#include <atomic>
#include <cstring>
#include <new>
#include <utility>
#include <driver/gpio.h>
#include <esp_err.h>
#include <bmds/detail/platform.h>
#include <bmds/gpiochip.h>
#include <bmds/io.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/thread.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *GpioKey = "io/gpio";
        constexpr const char *Owner = "io";

        BMDS_SYSCONF(ioGpio, "io/gpio", Config::Type::String, "",
                     "The pins set up by hand: '14=out,high 15=in,pull-up 2=func=MTCK 38=in:UART1_RXD'; "
                     "'force' takes a pin from its holder (devplan/io.md)",
                     0, 0, nullptr, Io::checkSetups);

        // ---- Edges (devplan/io.md §10.5)

        constexpr size_t RingSize = 64;  // records a watched pad keeps
        constexpr size_t MaxWatched = 8; // pads watched at once

        struct EdgeRecord {
                        std::atomic<uint32_t> seq{0}; // last written: 0 while the rest is written
                        int64_t               timeNs = 0;
                        bool                  rising = false;
                        bool                  level = false;
        };

        // One pad's edges: written by its interrupt only; read under the lock by tasks.
        struct EdgeRing {
                        std::atomic<uint32_t> written{0}; // the last record's number
                        uint32_t              base = 0;   // `written` when this pad's watch began
                        int64_t               lastUs = 0; // the last edge taken (debounce)
                        uint32_t              debounceUs = 0;
                        uint8_t               pad = 0;
                        uint8_t               edges = 0;
                        bool                  inUse = false;
                        EdgeRecord            records[RingSize];
        };

        // A pad's interrupt (ESP-IDF's GPIO ISR service, not an IRAM one): the level and the time
        // into its ring.
        void edgeIsr(void *arg) {
                auto *const    r = static_cast<EdgeRing *>(arg);
                const int64_t  us = Detail::monotonicUs();
                const uint32_t n = r->written.load(std::memory_order_relaxed);
                if (r->debounceUs != 0 && n != r->base && us - r->lastUs < int64_t(r->debounceUs)) return;
                r->lastUs = us;
                const bool level = IoChipOps::level(r->pad);
                // One way only: that way (the level may already have moved on); both: the level says.
                const bool     rising = r->edges == IoPinSetup::Rising    ? true
                                        : r->edges == IoPinSetup::Falling ? false
                                                                          : level;
                const uint32_t seq = n + 1;
                EdgeRecord    &e = r->records[seq % RingSize];
                e.seq.store(0, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                e.timeNs = us * 1000;
                e.rising = rising;
                e.level = level;
                e.seq.store(seq, std::memory_order_release);
                r->written.store(seq, std::memory_order_release);
                return;
        }

        // A pad as applied: its setup, and what it was before (put back when it is let go).
        struct Applied {
                        IoPinSetup       setup;
                        IoChipOps::Saved saved;
                        uint32_t         inputWords[IoPinSetup::MaxInputs] = {}; // the input signals' old selectors
                        char             lender[IoHolder::NameBytes] = {};       // forced from, or ""
                        EdgeRing        *ring = nullptr;                         // its edges, if watched
        };

        struct State {
                        Mutex            lock; // the applied pads, a pulse
                        SysConf::Watcher watcher;
                        IoClaim          claim; // the owner `io`
                        Applied          pins[IoGpioSetup::MaxPins];
                        size_t           count = 0;
                        IoGpioSetup      wanted;                 // the value being applied (the lock's)
                        EdgeRing        *rings[MaxWatched] = {}; // made when first wanted, kept
        };

        constinit PsramObject<State> state;

        // The words a pad is held for, in the registry (rodata: kept).
        const char *roleOf(const IoPinSetup &s) {
                if (s.function >= 0) return "IO_MUX function";
                if (s.outSignal != IoPinSetup::NoSignal) return "matrix output";
                switch (s.mode) {
                        case IoPinSetup::Mode::Input: return s.inputCount > 0 ? "matrix input" : "GPIO input";
                        case IoPinSetup::Mode::Output: return "GPIO output";
                        case IoPinSetup::Mode::OpenDrain: return "GPIO open drain";
                        case IoPinSetup::Mode::Off: return "off";
                }
                return "set up";
        }

        bool takenElsewhere(IoResource r) {
                return IoSetups::takenElsewhere(r, Owner);
        }

        Error refuseTaken(IoResource r, const char *signal) {
                return IoSetups::refuseTaken(r, signal);
        }

        IoResource instanceOf(const IoChip &chip, uint16_t signal, bool input) {
                return IoSetups::instanceOf(chip, signal, input);
        }

        void lenderOf(unsigned pad, char (&out)[IoHolder::NameBytes], const char *&role) {
                IoHolder h;
                out[0] = '\0';
                role = "";
                if (Io::registry().holder(IoResource(pad), h).isError() || !h.isHeld()) return;
                if (std::strcmp(h.owner, Owner) == 0) return;
                std::snprintf(out, sizeof(out), "%s", h.owner);
                role = h.role;
                return;
        }

        // Starts watching pad @p a's edges. The lock held.
        void watch(Applied &a) {
                const IoPinSetup &s = a.setup;
                EdgeRing         *r = nullptr;
                for (EdgeRing *&slot : state->rings) {
                        if (slot == nullptr) {
                                void *mem = Memory::alloc(MemSpace::Psram, sizeof(EdgeRing));
                                if (mem == nullptr) break;
                                slot = ::new (mem) EdgeRing();
                        }
                        if (!slot->inUse) {
                                r = slot;
                                break;
                        }
                }
                if (r == nullptr) {
                        bmdsWarn("io: io/gpio: GPIO%u's edges not watched: %u pads at most", unsigned(s.pad),
                                 unsigned(MaxWatched));
                        return;
                }
                r->inUse = true;
                r->pad = s.pad;
                r->edges = s.edges;
                r->debounceUs = s.debounceUs;
                r->base = r->written.load(std::memory_order_relaxed); // the numbers go on from there
                r->lastUs = 0;
                const gpio_int_type_t type = s.edges == IoPinSetup::Rising    ? GPIO_INTR_POSEDGE
                                             : s.edges == IoPinSetup::Falling ? GPIO_INTR_NEGEDGE
                                                                              : GPIO_INTR_ANYEDGE;
                const gpio_num_t      gpio = gpio_num_t(s.pad);
                if (gpio_set_intr_type(gpio, type) != ESP_OK || gpio_isr_handler_add(gpio, edgeIsr, r) != ESP_OK ||
                    gpio_intr_enable(gpio) != ESP_OK) {
                        bmdsWarn("io: io/gpio: GPIO%u's edges not watched: its interrupt can't be set",
                                 unsigned(s.pad));
                        (void)gpio_isr_handler_remove(gpio);
                        r->inUse = false;
                        return;
                }
                a.ring = r;
                return;
        }

        // Stops watching pad @p a's edges. The lock held.
        void unwatch(Applied &a) {
                if (a.ring == nullptr) return;
                const gpio_num_t gpio = gpio_num_t(a.setup.pad);
                (void)gpio_intr_disable(gpio);
                (void)gpio_isr_handler_remove(gpio);
                a.ring->inUse = false;
                a.ring = nullptr;
                return;
        }

        // Lets pad @p a go: its input signals' old sources, its registers, then the claim (a lender
        // holds it again). The lock held.
        void letGo(Applied &a) {
                const IoPinSetup &s = a.setup;
                unwatch(a);
                for (size_t i = s.inputCount; i-- > 0;) IoChipOps::setInputWord(s.inputs[i], a.inputWords[i]);
                IoChipOps::restorePad(s.pad, a.saved);
                (void)state->claim.release(IoResource(s.pad));
                if (a.lender[0] != '\0') bmdsInfo("io: GPIO%u given back to %s", unsigned(s.pad), a.lender);
                return;
        }

        // Sets pad @p a up as its setup says, its input signals' old sources kept. The lock held.
        void setUp(Applied &a) {
                const IoPinSetup &s = a.setup;
                IoChipOps::configurePad(s.pad, s);
                for (size_t i = 0; i < s.inputCount; ++i) {
                        a.inputWords[i] = IoChipOps::inputWord(s.inputs[i]);
                        IoChipOps::routeInput(s.inputs[i], s.pad);
                }
                if (s.edges != IoPinSetup::NoEdges) watch(a);
                return;
        }

        // Applies io/gpio's value now. The lock held.
        void applyLocked() {
                String value(MemSpace::Psram);
                if (SysConf::get(StringView(GpioKey), value).isError()) return;
                char why[IoGpioSetup::ReasonBytes];
                if (state->wanted.parse(ioChip(), value.view(), why).isError()) {
                        // The check refuses such a value; a store edited by hand may still hold one.
                        bmdsWarn("io: io/gpio: %s: nothing changed", why);
                        return;
                }
                const IoGpioSetup &wanted = state->wanted;
                // Pads that left, or whose setup changed: let go (a changed one is taken again below).
                for (size_t i = state->count; i-- > 0;) {
                        Applied          &a = state->pins[i];
                        const IoPinSetup *w = wanted.find(a.setup.pad);
                        if (w != nullptr && *w == a.setup) continue;
                        letGo(a);
                        state->pins[i] = state->pins[--state->count];
                }
                // Pads that came (or came back changed): taken, saved, set up.
                for (size_t i = 0; i < wanted.count(); ++i) {
                        const IoPinSetup &w = wanted.at(i);
                        bool              have = false;
                        for (size_t k = 0; k < state->count && !have; ++k) have = state->pins[k].setup.pad == w.pad;
                        if (have || state->count == IoGpioSetup::MaxPins) continue;
                        Applied &a = state->pins[state->count];
                        a = Applied();
                        a.setup = w;
                        const char *lenderRole = "";
                        lenderOf(w.pad, a.lender, lenderRole);
                        const IoUse use{IoResource(w.pad), roleOf(w)};
                        IoResource  blocked = NoIoResource;
                        const Error err = state->claim.add(Span<const IoUse>(&use, 1), w.force ? IoRegistry::Force : 0u,
                                                           &blocked);
                        if (err.isError()) {
                                // The holders changed since the check (a driver started on it).
                                char busy[160];
                                (void)Io::busyText(IoResource(w.pad), busy, sizeof(busy));
                                bmdsWarn("io: io/gpio: GPIO%u not set up: %s", unsigned(w.pad), busy);
                                continue;
                        }
                        IoChipOps::savePad(w.pad, a.saved);
                        setUp(a);
                        ++state->count;
                        if (a.lender[0] != '\0')
                                bmdsInfo("io: GPIO%u taken from %s%s%s%s", unsigned(w.pad), a.lender,
                                         lenderRole[0] != '\0' ? " (" : "", lenderRole,
                                         lenderRole[0] != '\0' ? ")" : "");
                }
                return;
        }

        // A restart (System::restart(): a reboot, an update) resets the CPUs, not IO_MUX or
        // the matrix, so pins set up now would still be set up in the next boot, and saved there
        // as "what it was before". Every pad io/gpio has goes back first. Not on a panic: the
        // pads then stay as they were (devplan/io.md §10.4).
        void letGoAtShutdown() {
                if (!state.isCreated()) return;
                // The restart's own task, the others about to stop: no lock (one held by a stopped
                // task would never come back).
                for (size_t i = state->count; i-- > 0;) {
                        const Applied    &a = state->pins[i];
                        const IoPinSetup &s = a.setup;
                        for (size_t k = s.inputCount; k-- > 0;) IoChipOps::setInputWord(s.inputs[k], a.inputWords[k]);
                        IoChipOps::restorePad(s.pad, a.saved);
                }
                return;
        }

        void changed(void *arg) {
                (void)arg;
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(state->lock, WaitForever);
                SysConf::Id   id;
                while (state->watcher.next(id)) {}
                (void)state->watcher.overflowed();
                applyLocked();
                return;
        }

} // namespace

namespace Io {

        Error checkSetups(StringView value) {
                if (!isReady()) return Error::NotReady;
                auto *setup = static_cast<IoGpioSetup *>(Memory::alloc(MemSpace::Psram, sizeof(IoGpioSetup)));
                if (setup == nullptr) return Error::NoMem;
                ::new (setup) IoGpioSetup();
                char  why[IoGpioSetup::ReasonBytes];
                Error err = setup->parse(ioChip(), value, why);
                if (err.isError()) SysConf::refuse(StringView(why));
                const IoChip &chip = ioChip();
                for (size_t i = 0; err.isOk() && i < setup->count(); ++i) {
                        const IoPinSetup &p = setup->at(i);
                        if (p.force) continue;
                        if (takenElsewhere(IoResource(p.pad))) {
                                err = refuseTaken(IoResource(p.pad), nullptr);
                                break;
                        }
                        // A matrix signal whose peripheral another owner holds.
                        if (p.outSignal != IoPinSetup::NoSignal) {
                                const IoResource r = instanceOf(chip, p.outSignal, false);
                                if (r != NoIoResource && takenElsewhere(r))
                                        err = refuseTaken(r, chip.outputName(p.outSignal));
                        }
                        for (size_t k = 0; err.isOk() && k < p.inputCount; ++k) {
                                const IoResource r = instanceOf(chip, p.inputs[k], true);
                                if (r != NoIoResource && takenElsewhere(r))
                                        err = refuseTaken(r, chip.inputName(p.inputs[k]));
                        }
                }
                setup->~IoGpioSetup();
                Memory::free(setup);
                return err;
        }

        Error startSetups() {
                if (!isReady()) return Error::NotReady;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                auto [claim, err] =
                        registry().claim(StringView(Owner), IoOwnerKind::User, Span<const IoUse>(), 0, nullptr);
                if (err.isError()) return err;
                s->claim = std::move(claim);
                // The GPIO interrupt service, for edges (the SD card's detect installs it too).
                const esp_err_t isr = gpio_install_isr_service(0);
                if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE)
                        bmdsWarn("io: no GPIO interrupt service (%s): no edges", esp_err_to_name(isr));
                if (System::addRestartHook(letGoAtShutdown).isError())
                        bmdsWarn("io: no restart hook: io/gpio's pins will stay set up across a restart");
                {
                        Mutex::Locker lock(s->lock, WaitForever);
                        applyLocked();
                }
                if (const Error watching = s->watcher.start(StringView(GpioKey), changed, nullptr); watching.isError())
                        return watching;
                // The ports after io/gpio: a port's pins can't be io/gpio's too (the registry says so).
                return IoSetups::startPorts();
        }

        Error pulse(unsigned pad, unsigned ms, bool force, char *why, size_t size) {
                if (why != nullptr && size > 0) why[0] = '\0';
                if (!isReady() || !state.isCreated()) return Error::NotReady;
                const IoChip &chip = ioChip();
                if (pad >= chip.padCount() || ms == 0 || ms > MaxPulseMs) return Error::Invalid;
                Mutex::Locker lock(state->lock, WaitForever);
                // A pad io/gpio has as a GPIO output: its latch inverted, then put back.
                for (size_t i = 0; i < state->count; ++i) {
                        const IoPinSetup &s = state->pins[i].setup;
                        if (s.pad != pad) continue;
                        if (!s.isOutput() || s.function >= 0 || s.outSignal != IoPinSetup::NoSignal) {
                                if (why != nullptr)
                                        std::snprintf(why, size, "GPIO%u isn't a GPIO output in io/gpio", pad);
                                return Error::InvalidState;
                        }
                        IoChipOps::setLevel(pad, !s.high);
                        Thread::sleep(ms);
                        IoChipOps::setLevel(pad, s.high);
                        return Error::Ok;
                }
                // Any other pad: a plain GPIO output only (a peripheral's line has no latch to pulse).
                IoPadState st;
                (void)readPad(pad, st);
                if (st.lpIo || st.function != chip.gpioFunction || !st.outputEnabled ||
                    st.outputSignal != chip.gpioOutputSignal) {
                        if (why != nullptr)
                                std::snprintf(why, size, "GPIO%u isn't a GPIO output: nothing to pulse", pad);
                        return Error::InvalidState;
                }
                if (takenElsewhere(IoResource(pad)) && !force) {
                        char busy[160];
                        (void)busyText(IoResource(pad), busy, sizeof(busy));
                        if (why != nullptr)
                                std::snprintf(why, size,
                                              "%s Pulsing it will probably disturb that hardware: use --force.", busy);
                        return Error::Busy;
                }
                // Borrowed for the pulse: its registers saved, the holder's claim given back after.
                const IoUse use{IoResource(pad), "pulse"};
                auto [claim, err] = registry().claim(StringView("io pulse"), IoOwnerKind::User,
                                                     Span<const IoUse>(&use, 1), IoRegistry::Force, nullptr);
                if (err.isError()) return err;
                IoChipOps::Saved saved;
                IoChipOps::savePad(pad, saved);
                IoChipOps::setLevel(pad, !st.outputLevel);
                Thread::sleep(ms);
                IoChipOps::restorePad(pad, saved);
                claim.reset();
                return Error::Ok;
        }

        Error edgeInfo(unsigned pad, EdgeInfo &out) {
                out = EdgeInfo();
                if (!state.isCreated()) return Error::NotExist;
                Mutex::Locker lock(state->lock, WaitForever);
                for (size_t i = 0; i < state->count; ++i) {
                        const Applied &a = state->pins[i];
                        if (a.setup.pad != pad || a.ring == nullptr) continue;
                        out.edges = a.ring->edges;
                        out.debounceUs = a.ring->debounceUs;
                        out.seen = a.ring->written.load(std::memory_order_acquire) - a.ring->base;
                        return Error::Ok;
                }
                return Error::NotExist;
        }

        Result<size_t> readEdges(unsigned pad, uint32_t after, Span<Edge> out, uint32_t &next, uint32_t &lost) {
                next = after;
                lost = 0;
                if (!state.isCreated()) return makeError<size_t>(Error::NotExist);
                Mutex::Locker   lock(state->lock, WaitForever);
                const EdgeRing *r = nullptr;
                for (size_t i = 0; i < state->count && r == nullptr; ++i)
                        if (state->pins[i].setup.pad == pad) r = state->pins[i].ring;
                if (r == nullptr) return makeError<size_t>(Error::NotExist);
                // Numbers are this watch's: 1 is the first edge since it was set up.
                const uint32_t written = r->written.load(std::memory_order_acquire) - r->base;
                uint32_t       from = after + 1;
                if (after > written) from = 1; // a number from an earlier watch: from the start
                if (written >= RingSize && from + RingSize <= written) {
                        lost += written - RingSize + 1 - from; // overwritten before they were read
                        from = written - RingSize + 1;
                }
                size_t n = 0;
                for (uint32_t seq = from; seq <= written && n < out.size(); ++seq) {
                        const uint32_t    abs = seq + r->base;
                        const EdgeRecord &e = r->records[abs % RingSize];
                        Edge              edge;
                        if (e.seq.load(std::memory_order_acquire) == abs) {
                                edge.timeNs = e.timeNs;
                                edge.rising = e.rising;
                                edge.level = e.level;
                                std::atomic_thread_fence(std::memory_order_acquire);
                        }
                        next = seq;
                        // Written again while it was copied (or before): lost, not shown wrong.
                        if (e.seq.load(std::memory_order_relaxed) != abs) {
                                ++lost;
                                continue;
                        }
                        edge.seq = seq;
                        out[n++] = edge;
                }
                return makeResult(n);
        }

} // namespace Io

BMDS_NAMESPACE_END
