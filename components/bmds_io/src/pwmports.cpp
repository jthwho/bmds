/**
 * @file      pwmports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PWM channels (devplan/io.md §15.1): `io/pwm0` .. `io/pwm7`, an LEDC channel each on its pin,
 * and `/dev/pwmchip0`, Linux's PWM character device over them. LEDC on its LL layer: the global
 * source PLL_F80M, four timers shared by the channels whose timing matches, a channel's duty and
 * start (hpoint) in counts of its timer's period. Waveforms rounded as Linux's (`LedcTiming`).
 */

#include <cstdio>
#include <new>
#include <esp_clk_tree.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/periph_ctrl.h>
// ESP-IDF's header, C written for C: a designated initializer leaves a field out.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <hal/ledc_ll.h>
#pragma GCC diagnostic pop
#include <soc/gpio_sig_map.h>
#include <bmds/gpiochip.h>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/pwmchip.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace PwmPorts {

        constexpr unsigned         Channels = PwmChip::Channels;
        constexpr unsigned         Timers = SOC_LEDC_TIMER_NUM;
        constexpr ledc_mode_t      Mode = LEDC_LOW_SPEED_MODE; // the P4's only
        constexpr soc_module_clk_t Source = SOC_MOD_CLK_PLL_F80M;
        static_assert(Channels == SOC_LEDC_CHANNEL_NUM, "a channel each");

        struct Port {
                        unsigned         number = 0;
                        char             owner[8] = {}; // `pwm0`
                        char             key[10] = {};  // `io/pwm0`
                        IoPwmSetup       setup;         // as running
                        bool             running = false;
                        IoClaim          claim;
                        SysConf::Id      id = SysConf::NoId;
                        IoChipOps::Saved saved;
                        bool             padSaved = false;
                        LedcTiming       timing;           // as set (ticks 0: off)
                        int              timer = -1;       // its LEDC timer while on
                        PwmChip         *holder = nullptr; // the open holding it, or null
        };

        struct Timer {
                        LedcTiming timing; // its divider and bits
                        unsigned   users = 0;
        };

        struct State {
                        SysConf::Watcher watcher;
                        Mutex            lock; // the ports, the timers, LEDC itself
                        Port             ports[Channels];
                        Timer            timers[Timers];
                        unsigned         running = 0; // ports running: LEDC on while any
                        uint32_t         sourceHz = 0;
                        IoClaim          timerClaim; // LEDC's timers, the chip's while LEDC is on
        };

        constinit PsramObject<State> state;

        ledc_dev_t *hw() {
                return LEDC_LL_GET_HW();
        }

        // ---- LEDC

        // On with the first port: the bus, the global source, the timers claimed. The lock held.
        Error ledcOn() {
                State &s = *state;
                if (s.running++ > 0) return Error::Ok;
                uint32_t hz = 0;
                if (esp_clk_tree_src_get_freq_hz(Source, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) != ESP_OK ||
                    hz == 0 || 1000000000000ull % hz != 0 || esp_clk_tree_enable_src(Source, true) != ESP_OK) {
                        --s.running;
                        return Error::Invalid;
                }
                s.sourceHz = hz;
                PERIPH_RCC_ATOMIC() {
                        ledc_ll_enable_bus_clock(true);
                        ledc_ll_enable_reset_reg(false);
                }
                PERIPH_RCC_ATOMIC() {
                        ledc_ll_enable_clock(hw(), true);
                        ledc_ll_set_slow_clk_sel(hw(), LEDC_SLOW_CLK_PLL_DIV);
                }
                IoUse uses[Timers];
                for (unsigned t = 0; t < Timers; ++t) {
                        uses[t] = IoUse{Io::instance(IoKind::LedcTimer, t), "a PWM period"};
                        s.timers[t] = Timer();
                }
                auto [claim, err] = Io::registry().claim(StringView("pwmchip0"), IoOwnerKind::User,
                                                         Span<const IoUse>(uses, Timers), 0u, nullptr);
                if (err.isOk()) s.timerClaim = std::move(claim);
                return Error::Ok;
        }

        // Off with the last. The lock held.
        void ledcOff() {
                State &s = *state;
                if (s.running == 0 || --s.running > 0) return;
                for (unsigned t = 0; t < Timers; ++t) ledc_ll_timer_pause(hw(), Mode, ledc_timer_t(t));
                PERIPH_RCC_ATOMIC() {
                        ledc_ll_enable_clock(hw(), false);
                }
                PERIPH_RCC_ATOMIC() {
                        ledc_ll_enable_bus_clock(false);
                        ledc_ll_enable_reset_reg(true);
                }
                (void)esp_clk_tree_enable_src(Source, false);
                s.timerClaim.reset();
                return;
        }

        // Timer @p t set up for @p timing. A timer already running takes its new divider and bits
        // at its period's end (alone on it, its channel's change); a stopped one starts from 0.
        void setTimer(unsigned t, const LedcTiming &timing, bool running) {
                const auto timer = ledc_timer_t(t);
                ledc_ll_set_clock_divider(hw(), Mode, timer, timing.div256);
                ledc_ll_set_duty_resolution(hw(), Mode, timer, timing.bits);
                ledc_ll_ls_timer_update(hw(), Mode, timer);
                if (!running) {
                        ledc_ll_timer_rst(hw(), Mode, timer);
                        ledc_ll_timer_resume(hw(), Mode, timer);
                }
                state->timers[t].timing = timing;
                return;
        }

        // A timer for @p timing on port @p p: one running it already, else its own if it is alone
        // there, else a free one; `Busy` without. The lock held.
        Error timerFor(Port &p, const LedcTiming &timing, int &out) {
                State &s = *state;
                for (unsigned t = 0; t < Timers; ++t)
                        if (s.timers[t].users > 0 && s.timers[t].timing.sharesTimer(timing)) {
                                out = int(t);
                                return Error::Ok;
                        }
                if (p.timer >= 0 && s.timers[p.timer].users == 1) {
                        setTimer(unsigned(p.timer), timing, true);
                        out = p.timer;
                        return Error::Ok;
                }
                for (unsigned t = 0; t < Timers; ++t)
                        if (s.timers[t].users == 0) {
                                setTimer(t, timing, false);
                                out = int(t);
                                return Error::Ok;
                        }
                return Error::Busy;
        }

        // The channel's registers for its timing: off (idle low), or bound to its timer with its
        // duty and start, taken at its period's end. ESP-IDF's sequence (ledc.c): one fade range
        // of one step that changes nothing, the gamma RAM's others cleared.
        void program(const Port &p) {
                const auto ch = ledc_channel_t(p.number);
                if (p.timing.ticks == 0) {
                        ledc_ll_set_idle_level(hw(), Mode, ch, 0);
                        ledc_ll_set_sig_out_en(hw(), Mode, ch, false);
                        ledc_ll_ls_channel_update(hw(), Mode, ch);
                        return;
                }
                ledc_ll_bind_channel_timer(hw(), Mode, ch, ledc_timer_t(p.timer));
                ledc_ll_set_hpoint(hw(), Mode, ch, p.timing.hpoint);
                ledc_ll_set_duty_int_part(hw(), Mode, ch, p.timing.duty);
                ledc_ll_set_fade_param_range(hw(), Mode, ch, 0, 1, 1, 0, 1);
                ledc_ll_set_range_number(hw(), Mode, ch, 1);
                for (uint8_t r = 1; r < SOC_LEDC_GAMMA_CURVE_FADE_RANGE_MAX; ++r)
                        ledc_ll_set_fade_param_range(hw(), Mode, ch, r, 0, 0, 0, 0);
                ledc_ll_set_sig_out_en(hw(), Mode, ch, true);
                ledc_ll_set_duty_start(hw(), Mode, ch);
                ledc_ll_ls_channel_update(hw(), Mode, ch);
                return;
        }

        // Port @p p's waveform set to @p timing: a timer found (`Busy` without, nothing changed),
        // the old one let go. The lock held.
        Error apply(Port &p, const LedcTiming &timing) {
                State &s = *state;
                int    timer = -1;
                if (timing.ticks != 0)
                        if (const Error err = timerFor(p, timing, timer); err.isError()) return err;
                if (timer != p.timer) {
                        if (p.timer >= 0) --s.timers[p.timer].users;
                        if (timer >= 0) ++s.timers[timer].users;
                }
                p.timer = timer;
                p.timing = timing;
                program(p);
                return Error::Ok;
        }

        LedcTiming timingOf(const IoPwmSetup &setup) {
                PwmWaveform w;
                w.periodNs = setup.periodNs;
                w.dutyNs = setup.dutyNs();
                w.offsetNs = setup.offsetNs();
                return LedcTiming::round(w, state->sourceHz);
        }

        // ---- The ports

        void stop(Port &p) {
                if (!p.running) return;
                (void)apply(p, LedcTiming()); // off, its timer let go
                if (p.padSaved) IoChipOps::restorePad(unsigned(p.setup.pin), p.saved);
                p.padSaved = false;
                p.claim.reset();
                p.running = false;
                ledcOff();
                bmdsInfo("io: %s stopped", p.owner);
                return;
        }

        void start(Port &p, const IoPwmSetup &s) {
                const IoUse uses[] = {{IoResource(s.pin), "PWM"},
                                      {Io::instance(IoKind::Ledc, p.number), "the channel"}};
                IoResource  blocked = NoIoResource;
                auto [claim, cerr] =
                        Io::registry().claim(StringView(p.owner), IoOwnerKind::User, Span<const IoUse>(uses, 2),
                                             s.force ? IoRegistry::Force : 0u, &blocked);
                if (cerr.isError()) {
                        char busy[160];
                        (void)Io::busyText(blocked, busy, sizeof(busy));
                        bmdsWarn("io: %s not started: %s", p.key, busy);
                        return;
                }
                if (ledcOn().isError()) {
                        bmdsWarn("io: %s not started: LEDC's clock", p.key);
                        return;
                }
                // The channel set first (off, or its waveform), then the pad given its signal.
                p.setup = s;
                p.timing = LedcTiming();
                p.timer = -1;
                if (const Error err = apply(p, timingOf(s)); err.isError())
                        bmdsWarn("io: %s: no LEDC timer free for its period (four periods run): off", p.key);
                IoChipOps::savePad(unsigned(s.pin), p.saved);
                p.padSaved = true;
                IoPinSetup pin;
                pin.pad = uint8_t(s.pin);
                pin.mode = IoPinSetup::Mode::Output;
                pin.outSignal = uint16_t(LEDC_LS_SIG_OUT_PAD_OUT0_IDX + p.number);
                IoChipOps::configurePad(unsigned(s.pin), pin);
                p.claim = std::move(claim);
                p.running = true;
                const PwmWaveform w = p.timing.waveform(state->sourceHz);
                bmdsInfo("io: %s on GPIO%d: period %llu ns, duty %llu ns, offset %llu ns (%u bits)", p.key, int(s.pin),
                         (unsigned long long)w.periodNs, (unsigned long long)w.dutyNs, (unsigned long long)w.offsetNs,
                         unsigned(p.timing.bits));
                return;
        }

        // io/pwmN's check: the value parses, and without `force` takes no pin another owner holds.
        template <unsigned N> Error checkPwm(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                if (!Io::isReady()) return Error::NotReady;
                IoPwmSetup s;
                char       why[IoPwmSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[8];
                std::snprintf(owner, sizeof(owner), "pwm%u", N);
                if (!s.force && IoSetups::takenElsewhere(IoResource(s.pin), owner))
                        return IoSetups::refuseTaken(IoResource(s.pin), nullptr);
                return Error::Ok;
        }

        constexpr const char *PwmHelp = "A PWM channel on /dev/pwmchip0: 'pin=21 period=1ms duty=25% offset=0' "
                                        "(times in ns, us, ms, s; duty and offset also as %); force (devplan/io.md)";
        BMDS_SYSCONF(ioPwm0, "io/pwm0", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<0>);
        BMDS_SYSCONF(ioPwm1, "io/pwm1", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<1>);
        BMDS_SYSCONF(ioPwm2, "io/pwm2", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<2>);
        BMDS_SYSCONF(ioPwm3, "io/pwm3", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<3>);
        BMDS_SYSCONF(ioPwm4, "io/pwm4", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<4>);
        BMDS_SYSCONF(ioPwm5, "io/pwm5", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<5>);
        BMDS_SYSCONF(ioPwm6, "io/pwm6", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<6>);
        BMDS_SYSCONF(ioPwm7, "io/pwm7", Config::Type::String, "", PwmHelp, 0, 0, nullptr, checkPwm<7>);

        // io/pwmN changed: a new pin restarts the channel; else its waveform is the setting's
        // again (a program's change lasts until the setting's next).
        void applySetting(Port &p) {
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(state->lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                IoPwmSetup s;
                char       why[IoPwmSetup::ReasonBytes];
                if (!value.isEmpty() && s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (value.isEmpty()) {
                        stop(p);
                        return;
                }
                if (p.running && s.pin == p.setup.pin && s.force == p.setup.force) {
                        p.setup = s;
                        if (apply(p, timingOf(s)).isError())
                                bmdsWarn("io: %s: no LEDC timer free for its period (four periods run): not changed",
                                         p.key);
                        return;
                }
                stop(p);
                start(p, s);
                return;
        }

        void changed(void *) {
                SysConf::Id id;
                const bool  all = state->watcher.overflowed();
                bool        which[Channels] = {};
                while (state->watcher.next(id))
                        for (unsigned n = 0; n < Channels; ++n)
                                if (state->ports[n].id == id) which[n] = true;
                for (unsigned n = 0; n < Channels; ++n)
                        if (all || which[n]) applySetting(state->ports[n]);
                return;
        }

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4).
        void putBackAtRestart() {
                if (!state.isCreated()) return;
                for (Port &p : state->ports)
                        if (p.running && p.padSaved) IoChipOps::restorePad(unsigned(p.setup.pin), p.saved);
                return;
        }

} // namespace PwmPorts

// ---- /dev/pwmchip0

using PwmPorts::Port;

Error PwmChip::held(unsigned hw) {
        if (hw >= Channels) return Error::Invalid;
        Port &p = PwmPorts::state->ports[hw];
        if (p.holder == nullptr) p.holder = this; // Linux's: an operation requests the channel
        if (p.holder != this) return Error::Busy;
        return p.running ? Error::Ok : Error::NotExist;
}

Error PwmChip::request(unsigned hw) {
        if (hw >= Channels) return Error::Invalid;
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        Port         &p = PwmPorts::state->ports[hw];
        if (p.holder != nullptr) return Error::Busy; // Linux's: this open's second request too
        if (!p.running) return Error::NotExist;
        p.holder = this;
        return Error::Ok;
}

Error PwmChip::release(unsigned hw) {
        if (hw >= Channels) return Error::Invalid;
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        Port         &p = PwmPorts::state->ports[hw];
        if (p.holder != this) return Error::Invalid;
        p.holder = nullptr;
        return Error::Ok;
}

Error PwmChip::round(unsigned hw, PwmWaveform &wf) {
        if (!wf.isValid()) return Error::Invalid;
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        if (const Error err = held(hw); err.isError()) return err;
        wf = LedcTiming::round(wf, PwmPorts::state->sourceHz).waveform(PwmPorts::state->sourceHz);
        return Error::Ok;
}

Error PwmChip::waveform(unsigned hw, PwmWaveform &out) {
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        if (const Error err = held(hw); err.isError()) return err;
        out = PwmPorts::state->ports[hw].timing.waveform(PwmPorts::state->sourceHz);
        return Error::Ok;
}

Error PwmChip::setWaveform(unsigned hw, const PwmWaveform &wf, bool exact) {
        if (!wf.isValid()) return Error::Invalid;
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        if (const Error err = held(hw); err.isError()) return err;
        const LedcTiming t = LedcTiming::round(wf, PwmPorts::state->sourceHz);
        if (exact && !(t.waveform(PwmPorts::state->sourceHz) == wf)) return Error::OutOfRange;
        return PwmPorts::apply(PwmPorts::state->ports[hw], t);
}

Error PwmChip::info(unsigned hw, Info &out) {
        if (hw >= Channels) return Error::Invalid;
        Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
        const Port   &p = PwmPorts::state->ports[hw];
        out = Info();
        out.hasPin = p.running;
        out.requested = p.holder != nullptr;
        out.mine = p.holder == this;
        out.on = p.running && p.timing.ticks != 0;
        out.pin = p.running ? p.setup.pin : -1;
        out.timer = out.on ? p.timer : -1;
        out.bits = out.on ? p.timing.bits : 0;
        out.sourceHz = PwmPorts::state->sourceHz != 0 ? PwmPorts::state->sourceHz : 80000000;
        return Error::Ok;
}

void PwmChip::handleClosed(uint8_t mode) {
        (void)mode;
        // Its last handle: its channels let go (their waveforms stay, as Linux's), then gone.
        {
                Mutex::Locker lock(PwmPorts::state->lock, WaitForever);
                for (Port &p : PwmPorts::state->ports)
                        if (p.holder == this) p.holder = nullptr;
        }
        this->~PwmChip();
        Memory::free(this);
        return;
}

Error PwmChip::addFiles() {
        Vfs::DeviceFile file;
        file.name = "pwmchip0";
        file.arg = nullptr;
        file.isDevice = true;
        // A chip per open: what it holds is its own (Linux's requests are per file).
        file.open = [](void *, uint8_t flags) -> Result<FileHandle::Ptr> {
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(PwmChip));
                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *chip = ::new (mem) PwmChip();
                auto [handle, err] = FileHandle::open(*chip, mode);
                if (err.isError()) {
                        chip->~PwmChip();
                        Memory::free(mem);
                }
                return Result<FileHandle::Ptr>(std::move(handle), err);
        };
        return Vfs::addDeviceFile(file);
}

namespace IoSetups {

        Error startPwmPorts() {
                using namespace PwmPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < Channels; ++n) {
                        Port &p = s->ports[n];
                        p.number = n;
                        std::snprintf(p.owner, sizeof(p.owner), "pwm%u", n);
                        std::snprintf(p.key, sizeof(p.key), "io/pwm%u", n);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: the PWM channels' pins will stay set up across a restart");
                if (const Error err = PwmChip::addFiles(); err.isError()) bmdsWarn("io: /dev/pwmchip0: %s", err.desc());
                for (Port &p : s->ports) applySetting(p);
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
