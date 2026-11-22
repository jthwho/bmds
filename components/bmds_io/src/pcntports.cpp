/**
 * @file      pcntports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pulse counters (devplan/io.md §15.3): `io/pcnt0` .. `io/pcnt3`, a PCNT unit each, and
 * `/dev/counter0`, Linux's counter character device over them. PCNT on its LL layer with one
 * interrupt of our own: the 16-bit counter runs between ±32767, each limit it reaches added to the
 * count's 64-bit base; a threshold is armed in the hardware whenever it falls inside that window.
 */

#include <climits>
#include <cstdio>
#include <cstring>
#include <new>
#include <esp_intr_alloc.h>
#include <esp_private/esp_clk.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <hal/pcnt_ll.h>
#include <soc/gpio_pins.h>
#include <hal/pcnt_periph.h>
#include <bmds/counterchip.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/flashcache.h>
#include <bmds/gpiochip.h>
#include <bmds/interrupt.h>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/placement.h>
#include <bmds/psramobject.h>
#include <bmds/spinlock.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace PcntPorts {

        constexpr unsigned Units = CounterChip::Counts;
        constexpr int      Limit = 32767; // the counter's window: ±Limit, reset to 0 at either
        static_assert(Units <= PCNT_LL_UNITS_PER_INST, "a unit each");

        using Function = IoPcntSetup::Function;
        using Action = IoPcntSetup::Action;

        // A channel's four actions (edge rising, falling; level high, low).
        struct Actions {
                        pcnt_channel_edge_action_t  rise = PCNT_CHANNEL_EDGE_ACTION_HOLD;
                        pcnt_channel_edge_action_t  fall = PCNT_CHANNEL_EDGE_ACTION_HOLD;
                        pcnt_channel_level_action_t high = PCNT_CHANNEL_LEVEL_ACTION_KEEP;
                        pcnt_channel_level_action_t low = PCNT_CHANNEL_LEVEL_ACTION_KEEP;
        };

        struct Port {
                        unsigned    number = 0;
                        char        owner[8] = {}; // `pcnt0`
                        char        key[10] = {};  // `io/pcnt0`
                        IoPcntSetup setup;         // as running
                        bool        running = false;
                        IoClaim     claim;
                        SysConf::Id id = SysConf::NoId;
                        // The pads (not a monitor's: those are their owners') and the input
                        // signals' selectors, put back at the stop.
                        IoChipOps::Saved savedA, savedB;
                        bool             padSavedA = false, padSavedB = false;
                        uint16_t         signals[4] = {};
                        uint32_t         signalWords[4] = {};
                        unsigned         signalCount = 0;
        };

        constexpr uint32_t MaxPausedHits = 4; // a pause's threshold hits kept with their times

        // What a count's interrupt keeps, in internal RAM: it runs while the flash is busy too
        // (devplan/io.md §15.3). Under the counts' lock, but for the interrupt during a pause (no
        // task runs then).
        struct UnitCount {
                        int64_t  base = 0;      // the count when the hardware's reads 0
                        bool     armed = false; // a threshold is set
                        int64_t  threshold = 0;
                        bool     hwThres = false;                 // threshold 0 armed in the hardware
                        bool     hwZero = false;                  // the zero event armed (the threshold is the base)
                        uint32_t pausedHits = 0;                  // threshold hits while the cache was off,
                        uint64_t pausedTimes[MaxPausedHits] = {}; // the first ones' times
        };

        DRAM_ATTR UnitCount counts[Units] = {};
        DRAM_ATTR bool      paused = false; // hits wait for the flash's end

        struct State {
                        SysConf::Watcher watcher;
                        Mutex            lock; // the ports' starts and stops, the device's opening
                        Port             ports[Units];
                        SpinLock         countLock; // the counts, the open device's watches and queue
                        unsigned         running = 0;
                        intr_handle_t    interrupt = nullptr;
                        uint32_t         apbHz = 0;
                        CounterChip     *open = nullptr; // the device's one open, or null
                        uint64_t         eventsLost = 0;
        };

        constinit PsramObject<State> state;

        pcnt_dev_t *hw() {
                return PCNT_LL_GET_HW(0);
        }

        // ---- The registers the interrupt touches, inlined into IRAM code (ESP-IDF's LL
        // functions are plain `inline`: one left out of line would be in flash).

        constexpr uint32_t ThresEvent = BIT(PCNT_LL_WATCH_EVENT_THRES0);
        constexpr uint32_t ZeroEvent = BIT(PCNT_LL_WATCH_EVENT_ZERO_CROSS);
        constexpr uint32_t HighEvent = BIT(PCNT_LL_WATCH_EVENT_HIGH_LIMIT);
        constexpr uint32_t LowEvent = BIT(PCNT_LL_WATCH_EVENT_LOW_LIMIT);

        [[gnu::always_inline]] inline uint32_t pendingRaw() {
                return PCNT.int_st.val;
        }
        [[gnu::always_inline]] inline uint32_t eventsRaw(unsigned u) {
                return PCNT.status_unit[u].val >> 2;
        }
        [[gnu::always_inline]] inline void clearRaw(unsigned u) {
                PCNT.int_clr.val = 1u << u;
                return;
        }
        [[gnu::always_inline]] inline int countRaw(unsigned u) {
                return int16_t(PCNT.cnt_unit[u].pulse_cnt);
        }

        // The threshold armed in the hardware if it falls inside the window from the base (the
        // zero event when it is the base). The counts' lock held, or the cache off.
        [[gnu::always_inline]] inline void rearm(unsigned u, UnitCount &c) {
                const int64_t t = c.threshold - c.base;
                const bool    zero = c.armed && t == 0;
                const bool    inside = c.armed && t != 0 && t > -Limit && t < Limit;
                if (inside) {
                        pcnt_un_conf1_reg_t conf1;
                        conf1.val = PCNT.conf_unit[u].conf1.val;
                        conf1.cnt_thres0 = int16_t(t);
                        PCNT.conf_unit[u].conf1.val = conf1.val;
                }
                if (inside != c.hwThres) PCNT.conf_unit[u].conf0.thr_thres0_en = inside;
                if (zero != c.hwZero) PCNT.conf_unit[u].conf0.thr_zero_en = zero;
                c.hwThres = inside;
                c.hwZero = zero;
                return;
        }

        // Unit @p u's interrupt: its limits added to the base (the threshold re-armed), and true
        // if its threshold was reached.
        [[gnu::always_inline]] inline bool takeRaw(unsigned u, UnitCount &c) {
                const uint32_t ev = eventsRaw(u);
                clearRaw(u);
                // The zero event comes with a limit's reset too: only one without is the count back at 0.
                bool hit = (c.hwThres && (ev & ThresEvent)) ||
                           (c.hwZero && (ev & ZeroEvent) && !(ev & (HighEvent | LowEvent)));
                if (ev & HighEvent) c.base += Limit;
                if (ev & LowEvent) c.base -= Limit;
                if (ev & (HighEvent | LowEvent)) {
                        // A threshold on the limit itself: reached as the counter resets to 0 (its
                        // comparator never sees it there).
                        if (c.armed && c.threshold == c.base) hit = true;
                        rearm(u, c);
                }
                return hit;
        }

        // The count's value: its base plus the hardware's, a limit the interrupt hasn't taken yet
        // added (ESP-IDF's check: the counter just reset, so near 0). The counts' lock held.
        int64_t valueLocked(unsigned u) {
                int v = countRaw(u);
                if (pendingRaw() & (1u << u)) {
                        const uint32_t ev = eventsRaw(u);
                        if ((ev & LowEvent) && v >= -Limit / 2)
                                v -= Limit;
                        else if ((ev & HighEvent) && v <= Limit / 2)
                                v += Limit;
                }
                return counts[u].base + v;
        }

        // Hits a pause kept, to the open device's queue (their own times). The counts' lock held.
        CounterChip *deliverPausedLocked() {
                State       &s = *state;
                CounterChip *wake = nullptr;
                paused = false;
                for (unsigned u = 0; u < Units; ++u) {
                        UnitCount &c = counts[u];
                        for (uint32_t i = 0; i < c.pausedHits; ++i) {
                                if (i >= MaxPausedHits) {
                                        ++s.eventsLost; // their times not kept
                                        continue;
                                }
                                if (s.open == nullptr) continue;
                                s.open->eventFromIsr(u, CounterChip::EventThreshold, c.pausedTimes[i]);
                                wake = s.open;
                        }
                        c.pausedHits = 0;
                }
                return wake;
        }

        // The counters' interrupt, with the cache: what a pause left, then the units'.
        void handleInterrupt() {
                State         &s = *state;
                const uint64_t now = uint64_t(Detail::monotonicUs()) * 1000;
                s.countLock.lockFromIsr();
                CounterChip   *wake = paused ? deliverPausedLocked() : nullptr;
                const uint32_t status = pendingRaw();
                for (unsigned u = 0; u < Units; ++u) {
                        if ((status & (1u << u)) == 0) continue;
                        if (takeRaw(u, counts[u]) && s.open != nullptr) {
                                s.open->eventFromIsr(u, CounterChip::EventThreshold, now);
                                wake = s.open;
                        }
                }
                s.countLock.unlockFromIsr();
                if (wake != nullptr) wake->wakeFromIsr();
                return;
        }

        // In IRAM, cache-safe: while the flash is busy (a sector erase holds the cache off some
        // 45 ms, 400 at most) the hardware's limits are still taken, so no wrap is lost; a
        // threshold's hits keep their times for the flash's end (afterFlash()).
        BMDS_ISR_ATTR void isr(void *) {
                if (FlashCache::enterFromIsr()) {
                        handleInterrupt();
                        FlashCache::leaveFromIsr();
                        return;
                }
                const uint64_t now = uint64_t(Detail::monotonicUs()) * 1000;
                const uint32_t status = pendingRaw();
                for (unsigned u = 0; u < Units; ++u) {
                        if ((status & (1u << u)) == 0) continue;
                        UnitCount &c = counts[u];
                        if (takeRaw(u, c)) {
                                if (c.pausedHits < MaxPausedHits) c.pausedTimes[c.pausedHits] = now;
                                ++c.pausedHits;
                                __atomic_store_n(&paused, true, __ATOMIC_RELEASE);
                        }
                }
                return;
        }

        // The end of a flash operation (in its task, the cache and the scheduler back): hits it
        // held delivered.
        void afterFlash(void *) {
                if (!__atomic_load_n(&paused, __ATOMIC_ACQUIRE) || !state.isCreated()) return;
                CounterChip *wake = nullptr;
                {
                        SpinLock::Locker lock(state->countLock);
                        wake = deliverPausedLocked();
                }
                if (wake != nullptr) wake->wakeFromIsr();
                return;
        }

        // ---- Starting and stopping

        // How each channel acts for a function (channel 0: A's edges, B's level; channel 1: B's
        // edges, A's level). Forward is A leading B; pulse-direction counts up while B is high.
        void actionsOf(const IoPcntSetup &s, Actions &c0, Actions &c1) {
                constexpr auto Inc = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
                constexpr auto Dec = PCNT_CHANNEL_EDGE_ACTION_DECREASE;
                constexpr auto Hold = PCNT_CHANNEL_EDGE_ACTION_HOLD;
                constexpr auto Keep = PCNT_CHANNEL_LEVEL_ACTION_KEEP;
                constexpr auto Inverse = PCNT_CHANNEL_LEVEL_ACTION_INVERSE;
                c0 = Actions();
                c1 = Actions();
                switch (s.function) {
                        case Function::Increase:
                        case Function::Decrease: {
                                const auto d = s.function == Function::Increase ? Inc : Dec;
                                c0.rise = s.action == Action::Rising || s.action == Action::Both ? d : Hold;
                                c0.fall = s.action == Action::Falling || s.action == Action::Both ? d : Hold;
                                break;
                        }
                        case Function::PulseDirection: c0 = {Inc, Hold, Keep, Inverse}; break;
                        case Function::QuadratureX1A: c0 = {Inc, Hold, Inverse, Keep}; break;
                        case Function::QuadratureX2A: c0 = {Inc, Dec, Inverse, Keep}; break;
                        case Function::QuadratureX1B: c1 = {Inc, Hold, Keep, Inverse}; break;
                        case Function::QuadratureX2B: c1 = {Inc, Dec, Keep, Inverse}; break;
                        case Function::QuadratureX4:
                                c0 = {Inc, Dec, Inverse, Keep};
                                c1 = {Inc, Dec, Keep, Inverse};
                                break;
                }
                return;
        }

        // Input signal @p signal from @p pad (or a constant high, -1), its selector kept to put back.
        void route(Port &p, unsigned signal, int pad) {
                p.signals[p.signalCount] = uint16_t(signal);
                p.signalWords[p.signalCount++] = IoChipOps::inputWord(signal);
                if (pad >= 0)
                        IoChipOps::routeInput(signal, unsigned(pad));
                else
                        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, signal, false);
                return;
        }

        // A pad as the counter's input: a monitor's only read, else set up with its pulls.
        void takePad(const Port &p, int pad, IoChipOps::Saved &saved, bool &savedFlag) {
                if (p.setup.monitor) {
                        IoChipOps::enableInput(unsigned(pad));
                        return;
                }
                IoChipOps::savePad(unsigned(pad), saved);
                savedFlag = true;
                IoPinSetup in;
                in.pad = uint8_t(pad);
                in.mode = IoPinSetup::Mode::Input;
                in.pullUp = p.setup.pullUp;
                in.pullDown = p.setup.pullDown;
                IoChipOps::configurePad(unsigned(pad), in);
                return;
        }

        // PCNT's clock and interrupt with the first unit. The lock held.
        Error unitsOn() {
                State &s = *state;
                if (s.running++ > 0) return Error::Ok;
                PERIPH_RCC_ATOMIC() {
                        pcnt_ll_enable_bus_clock(0, true);
                        pcnt_ll_reset_register(0);
                }
                s.apbHz = uint32_t(esp_clk_apb_freq());
                for (unsigned u = 0; u < Units; ++u) {
                        pcnt_ll_stop_count(hw(), u);
                        pcnt_ll_disable_all_events(hw(), u);
                }
                pcnt_ll_enable_intr(hw(), (1u << Units) - 1, false);
                pcnt_ll_clear_intr_status(hw(), (1u << Units) - 1);
                // Cache-safe: it takes the limits while the flash is busy (isr()).
                if (esp_intr_alloc(soc_pcnt_signals[0].irq_id, ESP_INTR_FLAG_IRAM, isr, nullptr, &s.interrupt) !=
                    ESP_OK) {
                        s.interrupt = nullptr;
                        PERIPH_RCC_ATOMIC() {
                                pcnt_ll_enable_bus_clock(0, false);
                        }
                        --s.running;
                        return Error::NoMem;
                }
                return Error::Ok;
        }

        // Off after the last. The lock held.
        void unitsOff() {
                State &s = *state;
                if (s.running == 0 || --s.running > 0) return;
                if (s.interrupt != nullptr) (void)esp_intr_free(s.interrupt);
                s.interrupt = nullptr;
                PERIPH_RCC_ATOMIC() {
                        pcnt_ll_enable_bus_clock(0, false);
                }
                return;
        }

        void stop(Port &p) {
                if (!p.running) return;
                State &s = *state;
                {
                        SpinLock::Locker lock(s.countLock);
                        pcnt_ll_enable_intr(hw(), PCNT_LL_UNIT_WATCH_EVENT(p.number), false);
                        pcnt_ll_stop_count(hw(), p.number);
                        pcnt_ll_disable_all_events(hw(), p.number);
                        pcnt_ll_clear_intr_status(hw(), PCNT_LL_UNIT_WATCH_EVENT(p.number));
                        p.running = false;
                        counts[p.number].hwThres = counts[p.number].hwZero = false;
                        counts[p.number].armed = false;
                }
                while (p.signalCount > 0) {
                        --p.signalCount;
                        IoChipOps::setInputWord(p.signals[p.signalCount], p.signalWords[p.signalCount]);
                }
                if (p.padSavedA) IoChipOps::restorePad(unsigned(p.setup.a), p.savedA);
                if (p.padSavedB) IoChipOps::restorePad(unsigned(p.setup.b), p.savedB);
                p.padSavedA = p.padSavedB = false;
                p.claim.reset();
                unitsOff();
                bmdsInfo("io: %s stopped", p.owner);
                return;
        }

        void start(Port &p, const IoPcntSetup &s) {
                State &st = *state;
                // The pads (a monitor's only read: not taken) and the unit.
                IoUse  uses[3];
                size_t n = 0;
                if (!s.monitor) {
                        uses[n++] = IoUse{IoResource(s.a), "counter A"};
                        if (s.b >= 0) uses[n++] = IoUse{IoResource(s.b), "counter B"};
                }
                uses[n++] = IoUse{Io::instance(IoKind::Pcnt, p.number), "the count"};
                IoResource blocked = NoIoResource;
                auto [claim, cerr] =
                        Io::registry().claim(StringView(p.owner), IoOwnerKind::User, Span<const IoUse>(uses, n),
                                             s.force ? IoRegistry::Force : 0u, &blocked);
                if (cerr.isError()) {
                        char busy[160];
                        (void)Io::busyText(blocked, busy, sizeof(busy));
                        bmdsWarn("io: %s not started: %s", p.key, busy);
                        return;
                }
                if (s.monitor) {
                        for (const int pad : {int(s.a), int(s.b)}) {
                                if (pad < 0) continue;
                                IoPadState state_;
                                IoChipOps::readPad(unsigned(pad), state_);
                                if (state_.function != ioChip().gpioFunction) {
                                        bmdsWarn("io: %s not started: GPIO%d runs an IO_MUX function; monitor reads "
                                                 "through the matrix",
                                                 p.key, pad);
                                        return;
                                }
                        }
                }
                if (const Error err = unitsOn(); err.isError()) {
                        bmdsWarn("io: %s not started: PCNT's interrupt (%s)", p.key, err.desc());
                        return;
                }
                const uint64_t cycles = uint64_t(s.filterNs) * st.apbHz / 1000000000u;
                if (cycles > PCNT_LL_MAX_GLITCH_WIDTH) {
                        bmdsWarn("io: %s not started: filter is at most %u ns (1023 cycles of %u Hz)", p.key,
                                 unsigned(uint64_t(PCNT_LL_MAX_GLITCH_WIDTH) * 1000000000u / st.apbHz),
                                 unsigned(st.apbHz));
                        unitsOff();
                        return;
                }
                p.setup = s;
                const unsigned u = p.number;
                pcnt_ll_stop_count(hw(), u);
                pcnt_ll_disable_all_events(hw(), u);
                pcnt_ll_set_high_limit_value(hw(), u, Limit);
                pcnt_ll_set_low_limit_value(hw(), u, -Limit);
                pcnt_ll_enable_high_limit_event(hw(), u, true);
                pcnt_ll_enable_low_limit_event(hw(), u, true);
                Actions c0, c1;
                actionsOf(s, c0, c1);
                pcnt_ll_set_edge_action(hw(), u, 0, c0.rise, c0.fall);
                pcnt_ll_set_level_action(hw(), u, 0, c0.high, c0.low);
                pcnt_ll_set_edge_action(hw(), u, 1, c1.rise, c1.fall);
                pcnt_ll_set_level_action(hw(), u, 1, c1.high, c1.low);
                pcnt_ll_set_glitch_filter_thres(hw(), u, uint32_t(cycles));
                pcnt_ll_enable_glitch_filter(hw(), u, cycles > 0);
                // The pads, then the signals: channel 0's edge A and level B, channel 1's the other
                // way (a signal unused reads high).
                takePad(p, s.a, p.savedA, p.padSavedA);
                if (s.b >= 0) takePad(p, s.b, p.savedB, p.padSavedB);
                const auto &sig = soc_pcnt_signals[0].units[u];
                p.signalCount = 0;
                route(p, unsigned(sig.channels[0].pulse_sig_id_matrix), s.a);
                route(p, unsigned(sig.channels[0].ctl_sig_id_matrix), s.b);
                route(p, unsigned(sig.channels[1].pulse_sig_id_matrix), s.b >= 0 ? int(s.b) : -1);
                route(p, unsigned(sig.channels[1].ctl_sig_id_matrix), s.b >= 0 ? int(s.a) : -1);
                {
                        SpinLock::Locker lock(st.countLock);
                        counts[u] = UnitCount();
                        pcnt_ll_clear_count(hw(), u);
                        pcnt_ll_clear_intr_status(hw(), PCNT_LL_UNIT_WATCH_EVENT(u));
                        pcnt_ll_enable_intr(hw(), PCNT_LL_UNIT_WATCH_EVENT(u), true);
                        pcnt_ll_start_count(hw(), u);
                        p.running = true;
                }
                p.claim = std::move(claim);
                bmdsInfo("io: %s: %s on GPIO%d%s%s", p.key, IoPcntSetup::functionName(s.function), int(s.a),
                         s.b >= 0 ? " and B" : "", s.monitor ? " (monitor)" : "");
                return;
        }

        // io/pcntN's check: the value parses, and without `force` (or as a monitor) takes no pin
        // another owner holds.
        template <unsigned N> Error checkPcnt(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                if (!Io::isReady()) return Error::NotReady;
                IoPcntSetup s;
                char        why[IoPcntSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[8];
                std::snprintf(owner, sizeof(owner), "pcnt%u", N);
                if (!s.force && !s.monitor) {
                        for (const int pad : {int(s.a), int(s.b)})
                                if (pad >= 0 && IoSetups::takenElsewhere(IoResource(pad), owner))
                                        return IoSetups::refuseTaken(IoResource(pad), nullptr);
                }
                return Error::Ok;
        }

        constexpr const char *PcntHelp = "A pulse counter on /dev/counter0: 'a=46 b=47 function=quadrature-x4'; "
                                         "increase or decrease with action=rising|falling|both, pulse-direction, "
                                         "quadrature-x1-a|x1-b|x2-a|x2-b|x4; filter=, pull-up, pull-down, monitor, "
                                         "force (devplan/io.md)";
        BMDS_SYSCONF(ioPcnt0, "io/pcnt0", Config::Type::String, "", PcntHelp, 0, 0, nullptr, checkPcnt<0>);
        BMDS_SYSCONF(ioPcnt1, "io/pcnt1", Config::Type::String, "", PcntHelp, 0, 0, nullptr, checkPcnt<1>);
        BMDS_SYSCONF(ioPcnt2, "io/pcnt2", Config::Type::String, "", PcntHelp, 0, 0, nullptr, checkPcnt<2>);
        BMDS_SYSCONF(ioPcnt3, "io/pcnt3", Config::Type::String, "", PcntHelp, 0, 0, nullptr, checkPcnt<3>);

        void applySetting(Port &p) {
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(state->lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                IoPcntSetup s;
                char        why[IoPcntSetup::ReasonBytes];
                if (!value.isEmpty() && s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (!value.isEmpty() && p.running && s == p.setup) return;
                stop(p);
                if (!value.isEmpty()) start(p, s);
                return;
        }

        void changed(void *) {
                SysConf::Id id;
                const bool  all = state->watcher.overflowed();
                bool        which[Units] = {};
                while (state->watcher.next(id))
                        for (unsigned n = 0; n < Units; ++n)
                                if (state->ports[n].id == id) which[n] = true;
                for (unsigned n = 0; n < Units; ++n)
                        if (all || which[n]) applySetting(state->ports[n]);
                return;
        }

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4).
        void putBackAtRestart() {
                if (!state.isCreated()) return;
                for (Port &p : state->ports) {
                        if (!p.running) continue;
                        for (unsigned i = p.signalCount; i-- > 0;)
                                IoChipOps::setInputWord(p.signals[i], p.signalWords[i]);
                        if (p.padSavedA) IoChipOps::restorePad(unsigned(p.setup.a), p.savedA);
                        if (p.padSavedB) IoChipOps::restorePad(unsigned(p.setup.b), p.savedB);
                }
                return;
        }

} // namespace PcntPorts

// ---- /dev/counter0

using PcntPorts::Port;

Error CounterChip::addWatch(const Watch &w) {
        // A threshold event on a count, reading a count's value or nothing.
        if (w.event != EventThreshold || w.channel >= Counts || w.id != 0) return Error::Invalid;
        if (w.type == ComponentCount) {
                if (w.scope != ScopeCount || w.parent >= Counts) return Error::Invalid;
        } else if (w.type != ComponentNone) {
                return Error::Invalid;
        }
        SpinLock::Locker lock(PcntPorts::state->countLock);
        if (_nextCount == MaxWatches) return Error::NoMem;
        _next[_nextCount++] = w;
        return Error::Ok;
}

Error CounterChip::enableEvents() {
        SpinLock::Locker lock(PcntPorts::state->countLock);
        for (size_t i = 0; i < _nextCount; ++i) _active[i] = _next[i];
        _activeCount = _nextCount;
        _nextCount = 0;
        _enabled = true;
        return Error::Ok;
}

Error CounterChip::disableEvents() {
        SpinLock::Locker lock(PcntPorts::state->countLock);
        _enabled = false;
        _activeCount = 0;
        _nextCount = 0;
        return Error::Ok;
}

void CounterChip::eventFromIsr(unsigned count, uint8_t event, uint64_t timestamp) {
        if (!_enabled) return;
        PcntPorts::State &s = *PcntPorts::state;
        for (size_t i = 0; i < _activeCount; ++i) {
                const Watch &w = _active[i];
                if (w.event != event || w.channel != count) continue;
                if (_head - _tail == MaxEvents) {
                        ++s.eventsLost;
                        continue;
                }
                Event &e = _events[_head++ % MaxEvents];
                e = Event();
                e.timestamp = timestamp;
                if (w.type == ComponentCount) {
                        const Port &p = s.ports[w.parent];
                        // Its own count was the threshold when the event fired; another's is
                        // read now.
                        e.value = uint64_t(w.parent == count ? PcntPorts::counts[count].threshold
                                                             : (p.running ? PcntPorts::valueLocked(w.parent) : 0));
                        e.status = p.running ? 0 : 19; // Linux's ENODEV: a count not set up
                }
                e.type = w.type;
                e.scope = w.scope;
                e.parent = w.parent;
                e.id = w.id;
                e.event = w.event;
                e.channel = w.channel;
        }
        return;
}

void CounterChip::wakeFromIsr() {
        _ready.releaseFromIsr();
        _pollers.notifyFromIsr();
        return;
}

Result<size_t> CounterChip::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(Event)) return makeError<size_t>(Error::Invalid);
        PcntPorts::State &s = *PcntPorts::state;
        Deadline          deadline(timeoutMs);
        for (;;) {
                size_t n = 0;
                {
                        SpinLock::Locker lock(s.countLock);
                        while (_tail != _head && n + sizeof(Event) <= buf.size()) {
                                std::memcpy(buf.data() + n, &_events[_tail++ % MaxEvents], sizeof(Event));
                                n += sizeof(Event);
                        }
                }
                if (n > 0) return makeResult(n);
                const Error err = Interrupt::waitCurrent(_ready, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

uint8_t CounterChip::readiness() {
        SpinLock::Locker lock(PcntPorts::state->countLock);
        return _tail != _head ? Readable : 0;
}

Error CounterChip::value(unsigned count, int64_t &out, int64_t &timeNs) {
        if (count >= Counts) return Error::Invalid;
        PcntPorts::State &s = *PcntPorts::state;
        SpinLock::Locker  lock(s.countLock);
        const Port       &p = s.ports[count];
        if (!p.running) return Error::NotExist;
        timeNs = Detail::monotonicUs() * 1000;
        out = PcntPorts::valueLocked(count);
        return Error::Ok;
}

Error CounterChip::preset(unsigned count, int64_t v) {
        if (count >= Counts) return Error::Invalid;
        PcntPorts::State &s = *PcntPorts::state;
        SpinLock::Locker  lock(s.countLock);
        Port             &p = s.ports[count];
        if (!p.running) return Error::NotExist;
        // The hardware's 0 is the new value; a limit pending from before goes with the old.
        pcnt_ll_clear_count(PcntPorts::hw(), count);
        pcnt_ll_clear_intr_status(PcntPorts::hw(), PCNT_LL_UNIT_WATCH_EVENT(count));
        PcntPorts::counts[count].base = v;
        PcntPorts::rearm(count, PcntPorts::counts[count]);
        return Error::Ok;
}

Error CounterChip::setThreshold(unsigned count, bool armed, int64_t v) {
        if (count >= Counts) return Error::Invalid;
        PcntPorts::State &s = *PcntPorts::state;
        SpinLock::Locker  lock(s.countLock);
        Port             &p = s.ports[count];
        if (!p.running) return Error::NotExist;
        PcntPorts::UnitCount &c = PcntPorts::counts[count];
        c.armed = armed;
        c.threshold = armed ? v : 0;
        PcntPorts::rearm(count, c);
        return Error::Ok;
}

Error CounterChip::info(unsigned count, Info &out) {
        if (count >= Counts) return Error::Invalid;
        PcntPorts::State &s = *PcntPorts::state;
        Mutex::Locker     lock(s.lock, WaitForever);
        const Port       &p = s.ports[count];
        out = Info();
        out.configured = p.running;
        if (p.running) {
                out.monitor = p.setup.monitor;
                out.function = uint32_t(p.setup.function);
                out.pinA = p.setup.a;
                out.pinB = p.setup.b;
                out.filterNs = p.setup.filterNs;
        }
        SpinLock::Locker counts(s.countLock);
        out.hasThreshold = p.running && PcntPorts::counts[count].armed;
        out.threshold = PcntPorts::counts[count].threshold;
        out.eventsLost = s.eventsLost;
        return Error::Ok;
}

void CounterChip::handleClosed(uint8_t mode) {
        (void)mode;
        // Its last handle: the device free for another open, then gone.
        PcntPorts::State &s = *PcntPorts::state;
        {
                Mutex::Locker    lock(s.lock, WaitForever);
                SpinLock::Locker counts(s.countLock);
                if (s.open == this) s.open = nullptr;
        }
        this->~CounterChip();
        Memory::free(this);
        return;
}

Error CounterChip::addFiles() {
        Vfs::DeviceFile file;
        file.name = "counter0";
        file.arg = nullptr;
        file.isDevice = true;
        // One open at a time, as Linux's.
        file.open = [](void *, uint8_t flags) -> Result<FileHandle::Ptr> {
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                PcntPorts::State &s = *PcntPorts::state;
                Mutex::Locker     lock(s.lock, WaitForever);
                if (s.open != nullptr) return makeError<FileHandle::Ptr>(Error::Busy);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(CounterChip));
                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *chip = ::new (mem) CounterChip();
                auto [handle, err] = FileHandle::open(*chip, mode);
                if (err.isError()) {
                        chip->~CounterChip();
                        Memory::free(mem);
                } else {
                        SpinLock::Locker counts(s.countLock);
                        s.open = chip;
                }
                return Result<FileHandle::Ptr>(std::move(handle), err);
        };
        return Vfs::addDeviceFile(file);
}

namespace IoSetups {

        Error startPcntPorts() {
                using namespace PcntPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < Units; ++n) {
                        Port &p = s->ports[n];
                        p.number = n;
                        std::snprintf(p.owner, sizeof(p.owner), "pcnt%u", n);
                        std::snprintf(p.key, sizeof(p.key), "io/pcnt%u", n);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: the counters' pins will stay set up across a restart");
                if (FlashCache::addAfterHook(afterFlash, nullptr).isError())
                        bmdsWarn("io: no flash hook: a counter's threshold events in a flash write wait for more");
                if (const Error err = CounterChip::addFiles(); err.isError())
                        bmdsWarn("io: /dev/counter0: %s", err.desc());
                for (Port &p : s->ports) applySetting(p);
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
