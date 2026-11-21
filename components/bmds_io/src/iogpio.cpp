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
#include <cstring>
#include <new>
#include <utility>
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

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *GpioKey = "io/gpio";
        constexpr const char *Owner = "io";

        BMDS_SYSCONF(ioGpio, "io/gpio", Config::Type::String, "",
                     "The pins set up by hand: '14=out,high 15=in,pull-up 2=func=MTCK 38=in:UART1_RXD'; "
                     "'force' takes a pin from its holder (devplan/io.md)",
                     0, 0, nullptr, Io::checkSetups);

        // A pad as applied: its setup, and what it was before (put back when it is let go).
        struct Applied {
                        IoPinSetup       setup;
                        IoChipOps::Saved saved;
                        uint32_t         inputWords[IoPinSetup::MaxInputs] = {}; // the input signals' old selectors
                        char             lender[IoHolder::NameBytes] = {};       // forced from, or ""
        };

        struct State {
                        Mutex            lock; // the applied pads, a pulse
                        SysConf::Watcher watcher;
                        IoClaim          claim; // the owner `io`
                        Applied          pins[IoGpioSetup::MaxPins];
                        size_t           count = 0;
                        IoGpioSetup      wanted; // the value being applied (the lock's)
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

        // The peripheral instance a matrix signal belongs to (`UART1_RXD`: uart1), or `NoIoResource`.
        IoResource instanceOf(const IoChip &chip, uint16_t signal, bool input) {
                const Span<const uint8_t> groups = input ? chip.inputGroups : chip.outputGroups;
                if (signal >= groups.size() || groups[signal] >= chip.groups.size()) return NoIoResource;
                const StringView group(chip.groups[groups[signal]]);
                // The group's name as a resource's (`UART1` → `uart1`), else with a 0 (`EMAC` → `emac0`).
                char   name[24];
                size_t n = 0;
                for (; n < group.size() && n + 2 < sizeof(name); ++n) {
                        const char c = group[n];
                        name[n] = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                }
                name[n] = '\0';
                auto [r, err] = chip.findResource(StringView(name, n));
                if (err.isOk()) return r;
                name[n] = '0';
                auto [r0, err0] = chip.findResource(StringView(name, n + 1));
                return err0.isOk() ? r0 : NoIoResource;
        }

        // True if someone other than `io` holds @p r, or the board wires it (for a pad).
        bool takenElsewhere(IoResource r) {
                IoHolder h;
                if (Io::registry().holder(r, h).isError()) return false;
                if (h.isHeld()) return std::strcmp(h.owner, Owner) != 0;
                return h.resourceClass == IoClass::Fixed || h.resourceClass == IoClass::System;
        }

        // Refuses a value's use of @p r (for @p signal, a matrix signal of its, or null): the
        // holder named, and how to go on.
        Error refuseTaken(IoResource r, const char *signal) {
                char busy[160];
                char text[SysConf::ReasonBytes];
                (void)Io::busyText(r, busy, sizeof(busy));
                if (signal != nullptr) {
                        char name[24];
                        (void)ioChip().resourceName(r, name, sizeof(name));
                        std::snprintf(text, sizeof(text), "%s is %s's: %s Add force to route it anyway.", signal, name,
                                      busy);
                } else {
                        std::snprintf(
                                text, sizeof(text),
                                "%s Changing it will probably break that hardware: add force to change it anyway.",
                                busy);
                }
                SysConf::refuse(StringView(text));
                return Error::Busy;
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

        // Lets pad @p a go: its input signals' old sources, its registers, then the claim (a lender
        // holds it again). The lock held.
        void letGo(Applied &a) {
                const IoPinSetup &s = a.setup;
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
                Mutex::Locker lock(state->lock, WaitForever);
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
                if (System::addRestartHook(letGoAtShutdown).isError())
                        bmdsWarn("io: no restart hook: io/gpio's pins will stay set up across a restart");
                {
                        Mutex::Locker lock(s->lock, WaitForever);
                        applyLocked();
                }
                return s->watcher.start(StringView(GpioKey), changed, nullptr);
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

} // namespace Io

BMDS_NAMESPACE_END
