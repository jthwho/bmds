/**
 * @file      adcports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ADC (devplan/io.md §15.7): `io/adc0`'s pins, `/dev/adc0`. ESP-IDF's drivers: a one-shot unit
 * per ADC unit and a curve-fitting calibration per pin for readings; the continuous driver for a
 * stream, which runs while its reader has `/dev/adc0` open (its pool the ring, `flush_pool` off so
 * a full pool drops a whole new frame and says so: losses counted exactly).
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_continuous.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_private/periph_ctrl.h>
#include <soc/lpperi_struct.h>
#include <bmds/adcdevice.h>
#include <bmds/board.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/flashwrites.h>
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
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace AdcPorts {

        constexpr size_t   MaxPins = IoAdcSetup::MaxPins;
        constexpr uint32_t ConvBytes = SOC_ADC_DIGI_DATA_BYTES_PER_CONV; // a conversion in the pool
        constexpr size_t   ReadConversions = 128;                        // a read's piece
        constexpr uint32_t CacheLine = 64;                               // the L1's
        constexpr uint32_t MaxFrame = 4032;                              // a DMA descriptor's, whole lines

        struct State {
                        SysConf::Watcher          watcher;
                        SysConf::Id               id = SysConf::NoId;
                        Mutex                     lock; // the ADC: its setting, readings, the stream's start and stop
                        IoAdcSetup                setup;
                        bool                      running = false;
                        IoClaim                   claim;
                        IoChipOps::Saved          saved[MaxPins];
                        adc_oneshot_unit_handle_t oneshot[2] = {};
                        adc_cali_handle_t         cali[MaxPins] = {};
                        // The stream.
                        adc_continuous_handle_t stream = nullptr;
                        AdcDevice              *streamer = nullptr; // the open whose reads take it
                        bool                    unitStreams[2] = {};
                        uint32_t                frameBytes = 0;
                        uint32_t                poolBytes = 0;
                        int64_t                 startNs = 0;
                        std::atomic<uint64_t>   frames{0}; // done, those dropped counted
                        std::atomic<uint64_t>   dropped{0};
                        uint64_t                taken = 0; // bytes read from the pool
        };

        constinit PsramObject<State> state;

        // The driver wants its callbacks' argument in internal RAM.
        DRAM_ATTR State *stateRef = nullptr;

        bool frameDone(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *arg) {
                State &s = **static_cast<State **>(arg);
                s.frames.fetch_add(1, std::memory_order_release);
                if (AdcDevice *d = s.streamer; d != nullptr) d->wakeFromIsr();
                return false;
        }

        bool poolFull(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *arg) {
                State &s = **static_cast<State **>(arg);
                s.dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
        }

        adc_atten_t attenOf(IoAdcSetup::Atten a) {
                return adc_atten_t(a); // the same order (ESP-IDF's adc_atten_t)
        }

        // The one-shot units for the setting's pins (a unit's one-shot and continuous modes can't
        // share it: these are closed while a stream runs). The lock held.
        esp_err_t openOneshot() {
                State            &s = *state;
                const IoAdcSetup &a = s.setup;
                esp_err_t         err = ESP_OK;
                for (size_t i = 0; i < a.pinCount && err == ESP_OK; ++i) {
                        const uint8_t u = a.unit[i];
                        if (s.oneshot[u] == nullptr) {
                                adc_oneshot_unit_init_cfg_t init = {};
                                init.unit_id = adc_unit_t(u);
                                err = adc_oneshot_new_unit(&init, &s.oneshot[u]);
                        }
                        adc_oneshot_chan_cfg_t ch = {};
                        ch.atten = attenOf(a.atten[i]);
                        ch.bitwidth = ADC_BITWIDTH_12;
                        if (err == ESP_OK)
                                err = adc_oneshot_config_channel(s.oneshot[u], adc_channel_t(a.channel[i]), &ch);
                }
                return err;
        }

        void closeOneshot() {
                for (auto &u : state->oneshot) {
                        if (u != nullptr) (void)adc_oneshot_del_unit(u);
                        u = nullptr;
                }
                return;
        }

        // The stream stopped and given back, the one-shot units back. The lock held.
        void stopStream() {
                State &s = *state;
                if (s.stream == nullptr) return;
                (void)adc_continuous_stop(s.stream);
                (void)adc_continuous_deinit(s.stream);
                s.stream = nullptr;
                s.streamer = nullptr;
                s.unitStreams[0] = s.unitStreams[1] = false;
                FlashWrites::release();
                if (s.running && openOneshot() != ESP_OK) bmdsWarn("io: adc0: no readings after the stream");
                return;
        }

        // A stream of every pin in turn at the setting's rate, for @p reader. The lock held.
        Error startStream(AdcDevice *reader) {
                State            &s = *state;
                const IoAdcSetup &a = s.setup;
                // Frames of 10 ms, a pool of 100 ms unless given. A frame is whole rounds of the pins
                // and whole cache lines: ESP-IDF's interrupt syncs each from memory, and on the P4
                // (internal RAM behind the L1) a sync not of whole lines fails its assert.
                const uint32_t round = a.pinCount * ConvBytes;
                uint32_t       unit = round;
                while (unit % CacheLine != 0) unit += round;
                uint32_t frame = a.rate * ConvBytes / 100;
                frame -= frame % unit;
                if (frame < unit) frame = unit;
                while (frame > MaxFrame && frame > unit) frame -= unit;
                uint32_t pool = a.buffer != 0 ? a.buffer : a.rate * ConvBytes / 10;
                pool = pool < 2 * frame ? 2 * frame : pool > IoAdcSetup::MaxBuffer ? IoAdcSetup::MaxBuffer : pool;
                pool -= pool % ConvBytes;
                closeOneshot();
                adc_continuous_handle_cfg_t c = {};
                c.max_store_buf_size = pool;
                c.conv_frame_size = frame;
                c.flags.flush_pool = 0; // a full pool drops the new frame, and says so
                esp_err_t                 err = adc_continuous_new_handle(&c, &s.stream);
                adc_digi_pattern_config_t pattern[MaxPins] = {};
                bool                      units[2] = {};
                for (size_t i = 0; i < a.pinCount; ++i) {
                        pattern[i].atten = uint8_t(attenOf(a.atten[i]));
                        pattern[i].channel = a.channel[i];
                        pattern[i].unit = a.unit[i];
                        pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
                        units[a.unit[i]] = true;
                }
                adc_continuous_config_t cfg = {};
                cfg.pattern_num = a.pinCount;
                cfg.adc_pattern = pattern;
                cfg.sample_freq_hz = a.rate;
                cfg.conv_mode = units[0] && units[1] ? ADC_CONV_ALTER_UNIT
                                : units[0]           ? ADC_CONV_SINGLE_UNIT_1
                                                     : ADC_CONV_SINGLE_UNIT_2;
                if (err == ESP_OK) err = adc_continuous_config(s.stream, &cfg);
                adc_continuous_evt_cbs_t cbs = {};
                cbs.on_conv_done = frameDone;
                cbs.on_pool_ovf = poolFull;
                if (err == ESP_OK) err = adc_continuous_register_event_callbacks(s.stream, &cbs, &stateRef);
                s.frames.store(0);
                s.dropped.store(0);
                s.taken = 0;
                s.streamer = reader;
                s.frameBytes = frame;
                s.poolBytes = pool;
                // ESP-IDF's start gives the units to the digital controller (LP_ADC's measure mux)
                // before it turns on LP_ADC's register clock (with the units' power, later in the
                // start): with that clock off, as a deleted one-shot unit leaves it, the write is
                // lost and the controller's conversions all read 0 (devplan/io.md §15.8). On first.
                PERIPH_RCC_ATOMIC() {
                        LPPERI.clk_en.ck_en_lp_adc = 1;
                }
                if (err == ESP_OK) err = adc_continuous_start(s.stream);
                s.startNs = Detail::monotonicUs() * 1000;
                if (err != ESP_OK) {
                        bmdsWarn("io: adc0: no stream (%s)", esp_err_to_name(err));
                        if (s.stream != nullptr) (void)adc_continuous_deinit(s.stream);
                        s.stream = nullptr;
                        s.streamer = nullptr;
                        (void)openOneshot();
                        return Error::IoError;
                }
                s.unitStreams[0] = units[0];
                s.unitStreams[1] = units[1];
                // /flash's writes wait while it streams: the pool can't cover an erase at speed.
                FlashWrites::hold();
                return Error::Ok;
        }

        // ---- The setting

        void stop() {
                State &s = *state;
                if (!s.running) return;
                stopStream();
                for (size_t i = 0; i < MaxPins; ++i) {
                        if (s.cali[i] != nullptr) (void)adc_cali_delete_scheme_curve_fitting(s.cali[i]);
                        s.cali[i] = nullptr;
                }
                closeOneshot();
                for (size_t i = s.setup.pinCount; i-- > 0;)
                        IoChipOps::restorePad(unsigned(s.setup.pins[i]), s.saved[i]);
                s.claim.reset();
                s.running = false;
                bmdsInfo("io: adc0 stopped");
                return;
        }

        void start(const IoAdcSetup &a) {
                State &s = *state;
                IoUse  uses[MaxPins];
                for (size_t i = 0; i < a.pinCount; ++i) uses[i] = IoUse{IoResource(a.pins[i]), "ADC input"};
                IoResource blocked = NoIoResource;
                auto [claim, cerr] =
                        Io::registry().claim(StringView("adc0"), IoOwnerKind::User, Span<const IoUse>(uses, a.pinCount),
                                             a.force ? IoRegistry::Force : 0u, &blocked);
                if (cerr.isError()) {
                        char busy[160];
                        (void)Io::busyText(blocked, busy, sizeof(busy));
                        bmdsWarn("io: io/adc0 not started: %s", busy);
                        return;
                }
                s.setup = a;
                for (size_t i = 0; i < a.pinCount; ++i) IoChipOps::savePad(unsigned(a.pins[i]), s.saved[i]);
                esp_err_t err = openOneshot();
                for (size_t i = 0; i < a.pinCount && err == ESP_OK; ++i) {
                        const uint8_t                   u = a.unit[i];
                        adc_cali_curve_fitting_config_t cal = {};
                        cal.unit_id = adc_unit_t(u);
                        cal.chan = adc_channel_t(a.channel[i]);
                        cal.atten = attenOf(a.atten[i]);
                        cal.bitwidth = ADC_BITWIDTH_12;
                        if (err == ESP_OK && adc_cali_create_scheme_curve_fitting(&cal, &s.cali[i]) != ESP_OK) {
                                s.cali[i] = nullptr; // no calibration in eFuse: raw only (mv 0)
                                bmdsWarn("io: adc0: GPIO%d uncalibrated (no eFuse data)", int(a.pins[i]));
                        }
                }
                s.claim = std::move(claim);
                s.running = true;
                if (err != ESP_OK) {
                        bmdsWarn("io: io/adc0 not started: the ADC (%s)", esp_err_to_name(err));
                        stop();
                        return;
                }
                bmdsInfo("io: io/adc0: /dev/adc0, %u pins%s", unsigned(a.pinCount),
                         a.rate ? ", streaming on reading" : "");
                return;
        }

        // io/adc0's check: the value parses, and without `force` takes no pin another owner holds.
        Error check(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                if (!Io::isReady()) return Error::NotReady;
                IoAdcSetup a;
                char       why[IoAdcSetup::ReasonBytes];
                if (const Error err = a.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                if (!a.force)
                        for (size_t i = 0; i < a.pinCount; ++i)
                                if (IoSetups::takenElsewhere(IoResource(a.pins[i]), "adc0"))
                                        return IoSetups::refuseTaken(IoResource(a.pins[i]), nullptr);
                return Error::Ok;
        }

        constexpr const char *AdcHelp = "The ADC, /dev/adc0: 'pins=20,16:6 atten=12' (pads with an ADC channel, "
                                        "each :ATTEN in dB: 0, 2.5, 6, 12); rate= (conversions a second: streaming), "
                                        "buffer=, force (devplan/io.md)";
        BMDS_SYSCONF(ioAdc0, "io/adc0", Config::Type::String, "", AdcHelp, 0, 0, nullptr, check);

        void apply() {
                const GpioChip::Change change; // line watchers told once, after the lock
                State                 &s = *state;
                Mutex::Locker          lock(s.lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(s.id, value).isError()) return;
                IoAdcSetup a;
                char       why[IoAdcSetup::ReasonBytes];
                if (!value.isEmpty() && a.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: io/adc0: %s: not changed", why);
                        return;
                }
                if (!value.isEmpty() && s.running && a == s.setup) return;
                AdcDevice *reader = s.streamer;
                stop();
                if (reader != nullptr) reader->wakeFromIsr(); // its read sees the stream gone
                if (!value.isEmpty()) start(a);
                return;
        }

        void changed(void *) {
                SysConf::Id id;
                const bool  all = state->watcher.overflowed();
                bool        mine = false;
                while (state->watcher.next(id))
                        if (id == state->id) mine = true;
                if (all || mine) apply();
                return;
        }

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4).
        void putBackAtRestart() {
                if (!state.isCreated() || !state->running) return;
                for (size_t i = state->setup.pinCount; i-- > 0;)
                        IoChipOps::restorePad(unsigned(state->setup.pins[i]), state->saved[i]);
                return;
        }

} // namespace AdcPorts

// ---- /dev/adc0

using AdcPorts::State;

Error AdcDevice::status(Info &out, const AdcDevice *self) {
        State        &s = *AdcPorts::state;
        Mutex::Locker lock(s.lock, WaitForever);
        out = Info();
        out.configured = s.running;
        out.channels = s.running ? s.setup.pinCount : 0;
        out.rateHz = s.running ? s.setup.rate : 0;
        out.streaming = s.stream != nullptr;
        out.mine = out.streaming && self != nullptr && s.streamer == self;
        if (out.streaming) {
                const uint64_t conv = s.frameBytes / AdcPorts::ConvBytes;
                out.startNs = s.startNs;
                out.samples = s.frames.load(std::memory_order_acquire) * conv;
                out.lost = s.dropped.load(std::memory_order_relaxed) * conv;
                out.buffer = s.poolBytes;
        }
        return Error::Ok;
}

Error AdcDevice::channel(unsigned index, Channel &out) {
        State        &s = *AdcPorts::state;
        Mutex::Locker lock(s.lock, WaitForever);
        if (!s.running) return Error::NotExist;
        if (index >= s.setup.pinCount) return Error::Invalid;
        const IoAdcSetup &a = s.setup;
        out = Channel();
        out.pin = a.pins[index];
        out.unit = a.unit[index];
        out.channel = a.channel[index];
        out.attenCdb = IoAdcSetup::centiDb(a.atten[index]);
        for (const BoardPin &p : board().pins)
                if (p.gpio == a.pins[index]) out.divider = p.analogDivider;
        return Error::Ok;
}

Error AdcDevice::reading(int pin, Reading &out) {
        State        &s = *AdcPorts::state;
        Mutex::Locker lock(s.lock, WaitForever);
        if (!s.running) return Error::NotExist;
        const int i = s.setup.indexOf(pin);
        if (i < 0) return Error::NotExist;
        const uint8_t u = s.setup.unit[i];
        if (s.unitStreams[u]) return Error::Busy;
        int raw = 0;
        if (adc_oneshot_read(s.oneshot[u], adc_channel_t(s.setup.channel[i]), &raw) != ESP_OK) return Error::IoError;
        out.timeNs = Detail::monotonicUs() * 1000;
        out.raw = uint32_t(raw);
        int mv = 0;
        if (s.cali[i] != nullptr) (void)adc_cali_raw_to_voltage(s.cali[i], raw, &mv);
        out.mv = mv;
        return Error::Ok;
}

void AdcDevice::wakeFromIsr() {
        _ready.releaseFromIsr();
        _pollers.notifyFromIsr();
        return;
}

Result<size_t> AdcDevice::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(Sample)) return makeError<size_t>(Error::Invalid);
        State   &s = *AdcPorts::state;
        Deadline deadline(timeoutMs);
        for (;;) {
                {
                        Mutex::Locker lock(s.lock, WaitForever);
                        if (!s.running) return makeError<size_t>(Error::NotExist);
                        if (s.setup.rate == 0) return makeError<size_t>(Error::Invalid); // readings only
                        if (s.stream == nullptr)
                                if (const Error err = AdcPorts::startStream(this); err.isError())
                                        return makeError<size_t>(err);
                        if (s.streamer != this) return makeError<size_t>(Error::Busy);
                        // What the pool has, a piece at a time, as calibrated records.
                        uint8_t        raw[AdcPorts::ReadConversions * AdcPorts::ConvBytes];
                        const size_t   want = buf.size() / sizeof(Sample);
                        const uint32_t ask =
                                uint32_t((want < AdcPorts::ReadConversions ? want : AdcPorts::ReadConversions) *
                                         AdcPorts::ConvBytes);
                        uint32_t got = 0;
                        if (adc_continuous_read(s.stream, raw, ask, &got, 0) == ESP_OK && got > 0) {
                                s.taken += got;
                                adc_continuous_data_t parsed[AdcPorts::ReadConversions];
                                uint32_t              n = 0;
                                (void)adc_continuous_parse_data(s.stream, raw, got, parsed, &n);
                                size_t out = 0;
                                for (uint32_t k = 0; k < n; ++k) {
                                        if (!parsed[k].valid) continue;
                                        int i = -1;
                                        for (int j = 0; j < s.setup.pinCount; ++j)
                                                if (s.setup.unit[j] == uint8_t(parsed[k].unit) &&
                                                    s.setup.channel[j] == uint8_t(parsed[k].channel))
                                                        i = j;
                                        if (i < 0) continue;
                                        Sample r;
                                        r.pin = uint8_t(s.setup.pins[i]);
                                        r.raw = uint16_t(parsed[k].raw_data);
                                        int mv = 0;
                                        if (s.cali[i] != nullptr)
                                                (void)adc_cali_raw_to_voltage(s.cali[i], int(r.raw), &mv);
                                        r.mv = uint16_t(mv < 0 ? 0 : mv);
                                        std::memcpy(buf.data() + out * sizeof(Sample), &r, sizeof(r));
                                        ++out;
                                }
                                if (out > 0) return makeResult(out * sizeof(Sample));
                                continue;
                        }
                }
                const Error err = Interrupt::waitCurrent(_ready, deadline);
                if (err.isError()) return makeError<size_t>(err);
        }
}

uint8_t AdcDevice::readiness() {
        State &s = *AdcPorts::state;
        if (s.stream == nullptr || s.streamer != this) return 0;
        const uint64_t in =
                (s.frames.load(std::memory_order_acquire) - s.dropped.load(std::memory_order_relaxed)) * s.frameBytes;
        return in > s.taken ? Readable : 0;
}

void AdcDevice::handleClosed(uint8_t mode) {
        (void)mode;
        // Its last handle: its stream stopped, then gone.
        {
                State        &s = *AdcPorts::state;
                Mutex::Locker lock(s.lock, WaitForever);
                if (s.streamer == this) AdcPorts::stopStream();
        }
        this->~AdcDevice();
        Memory::free(this);
        return;
}

Error AdcDevice::addFiles() {
        Vfs::DeviceFile file;
        file.name = "adc0";
        file.arg = nullptr;
        file.isDevice = true;
        file.open = [](void *, uint8_t flags) -> Result<FileHandle::Ptr> {
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(AdcDevice));
                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *dev = ::new (mem) AdcDevice();
                auto [handle, err] = FileHandle::open(*dev, mode);
                if (err.isError()) {
                        dev->~AdcDevice();
                        Memory::free(mem);
                }
                return Result<FileHandle::Ptr>(std::move(handle), err);
        };
        return Vfs::addDeviceFile(file);
}

namespace IoSetups {

        Error startAdc() {
                using namespace AdcPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                stateRef = s;
                auto [id, err] = SysConf::find(StringView("io/adc0"));
                if (err.isOk()) s->id = id;
                if (System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: the ADC's pins will stay set up across a restart");
                if (const Error e = AdcDevice::addFiles(); e.isError()) bmdsWarn("io: /dev/adc0: %s", e.desc());
                apply();
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
