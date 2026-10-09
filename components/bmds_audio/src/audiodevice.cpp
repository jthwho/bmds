/**
 * @file      audiodevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An audio output: I2S TX on the APLL, its codec, and the engine that mixes
 * its streams into the DMA buffers (devplan/audio.md §5, §16, §17).
 */

#include <cstdio>
#include <cstring>
#include <driver/i2s_std.h>
#include <esp_cache.h>
#include <esp_clk_tree.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/board.h>
#include <bmds/deadline.h>
#include <bmds/es8311.h>
#include <bmds/es8311regs.h>
#include <bmds/i2cbus.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   RingSize = AudioDevice::DmaBuffers * 2;
        constexpr size_t   FrameBytes = 2 * sizeof(int32_t); // two 32-bit slots
        constexpr uint32_t MclkMultiple = 256;
        constexpr uint32_t FadeMs = 5;          // a tone's fade
        constexpr uint32_t StopMarginMs = 20;   // scheduling, on top of the fades and the DMA
        constexpr uint32_t DetachWaitMs = 1000; // the engine lets go within a period

        Error checkRate(StringView value) {
                const auto rate = static_cast<uint32_t>(Config::toInt(value));
                return Es8311Regs::clockPlan(rate * MclkMultiple, rate).error();
        }

        // devplan/audio.md §6.4, §17: the keys are an interface once shipped.
        BMDS_SYSCONF(audioDefault, "audio/default", Config::Type::String, "dsp0",
                     "The device /dev/dsp and the system sounds use", 0, 16, nullptr, nullptr)
        BMDS_SYSCONF(audioBell, "audio/bell", Config::Type::Enum, "sound", "The terminal's bell", 0, 0, "sound|off",
                     nullptr)
        BMDS_SYSCONF(audioFeedback, "audio/feedback", Config::Type::Bool, "false",
                     "UI feedback sounds (click, error, notify)", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(audioDsp0Volume, "audio/dsp0/volume", Config::Type::Int, "-9",
                     "dsp0's volume in dB below full scale (-60 to 0; audio/dsp0/mute silences it)", -60, 0, nullptr,
                     nullptr)
        BMDS_SYSCONF(audioDsp0Mute, "audio/dsp0/mute", Config::Type::Bool, "false", "dsp0 muted", 0, 0, nullptr,
                     nullptr)
        BMDS_SYSCONF(audioDsp0Rate, "audio/dsp0/rate", Config::Type::Int, "48000",
                     "dsp0's rate in Hz; streams at other rates are resampled", 8000, 96000, nullptr, checkRate)
        BMDS_SYSCONF(audioDsp0Period, "audio/dsp0/period", Config::Type::Int, "240",
                     "dsp0's period in frames (240: 5 ms at 48 kHz)",
                     static_cast<int32_t>(AudioDevice::MinPeriodFrames),
                     static_cast<int32_t>(AudioDevice::MaxPeriodFrames), nullptr, nullptr)

        constinit StaticObject<Es8311>      codecs[AudioDevice::MaxDevices];
        constinit StaticObject<AudioDevice> devices[AudioDevice::MaxDevices];

        i2s_chan_handle_t handleOf(void *channel) {
                return static_cast<i2s_chan_handle_t>(channel);
        }

        i2s_std_clk_config_t clockFor(uint32_t rateHz) {
                i2s_std_clk_config_t c = {};
                c.sample_rate_hz = rateHz;
                c.clk_src = I2S_CLK_SRC_APLL;
                c.mclk_multiple = I2S_MCLK_MULTIPLE_256;
                return c;
        }

        uint32_t periodMs(uint32_t frames, uint32_t rateHz) {
                return rateHz == 0 ? 0 : (frames * 1000 + rateHz - 1) / rateHz;
        }

} // namespace

// The interrupt: the DMA finished sending a buffer (IDF has already queued
// it to be cleared after this returns). It runs with the cache off while the
// flash is written (I2S_ISR_IRAM_SAFE), so it touches only internal RAM and
// IRAM code: the device lives in BSS.
struct AudioDeviceIsr {
                BMDS_ISR_ATTR static bool sent(i2s_chan_handle_t, i2s_event_data_t *event, void *arg) {
                        auto          &d = *static_cast<AudioDevice *>(arg);
                        const uint32_t sequence = d._sequence.fetch_add(1, std::memory_order_relaxed) + 1;
                        const uint32_t head = d._head.load(std::memory_order_relaxed);
                        d._ring[head % RingSize] = {static_cast<int32_t *>(event->dma_buf), sequence,
                                                    esp_timer_get_time()};
                        d._head.store(head + 1, std::memory_order_release);
                        d._wake.releaseFromIsr();
                        return false; // releaseFromIsr() yields itself
                }
};

Error AudioDevice::init() {
        const BoardAudio &a = board().audio;
        if (a.codec != BoardAudioCodec::Es8311) return Error::Ok;
        I2cBus *bus = I2cBus::bus(static_cast<size_t>(a.i2c));
        if (bus == nullptr) return Error::NotExist;
        codecs[0].create(*bus, a.i2cAddr, a.powerOn);
        devices[0].create(*codecs[0], static_cast<uint8_t>(0));
        AudioDevice &d = *devices[0];
        d._settings = d.readSettings();
        if (SysConf::isReady()) {
                const Error err = d._watcher.start("audio/dsp0", settingsChanged, &d);
                if (err.isError()) bmdsWarn("audio: dsp0 won't follow its settings: %s", err.desc());
        }
        const Error err = AudioFile::addFiles(d);
        if (err.isError()) bmdsWarn("audio: no /dev/dsp0: %s", err.desc());
        return Error::Ok;
}

Result<AudioDevice *> AudioDevice::device(size_t number) {
        if (number >= MaxDevices || !devices[number].isCreated()) return makeError<AudioDevice *>(Error::NotSupported);
        return makeResult(&*devices[number]);
}

Result<AudioDevice *> AudioDevice::defaultDevice() {
        if (!SysConf::isReady()) return device(0);
        String name;
        if (SysConf::get(bmdsConf_audioDefault.key, name).isError())
                return makeError<AudioDevice *>(Error::NotSupported);
        const StringView n(name);
        if (n.size() != 4 || !n.startsWith("dsp") || n[3] < '0' || n[3] > '9')
                return makeError<AudioDevice *>(Error::NotSupported);
        return device(static_cast<size_t>(n[3] - '0'));
}

AudioDevice::AudioDevice(AudioCodec &codec, uint8_t number) : _codec(codec), _wake(1, 0), _number(number) {}

const char *AudioDevice::stateName(State state) {
        switch (state) {
                case State::Stopped: return "stopped";
                case State::Running: return "running";
                case State::Faulted: return "faulted";
        }
        return "?";
}

// ---- Settings

AudioDevice::Settings AudioDevice::readSettings() const {
        Settings s;
        if (!SysConf::isReady()) return s;
        if (auto [v, e] = SysConf::getInt(bmdsConf_audioDsp0Volume.key); e.isOk()) s.volumeDb = v;
        if (auto [v, e] = SysConf::getBool(bmdsConf_audioDsp0Mute.key); e.isOk()) s.muted = v;
        if (auto [v, e] = SysConf::getInt(bmdsConf_audioDsp0Rate.key); e.isOk()) s.rateHz = static_cast<uint32_t>(v);
        if (auto [v, e] = SysConf::getInt(bmdsConf_audioDsp0Period.key); e.isOk())
                s.periodFrames = static_cast<uint32_t>(v);
        return s;
}

// A setting changed (on the task that changed it): the engine's task applies it.
void AudioDevice::settingsChanged(void *arg) {
        auto &d = *static_cast<AudioDevice *>(arg);
        d._settingsChanges.fetch_add(1, std::memory_order_release);
        d._wake.release();
        return;
}

Error AudioDevice::applyVolumeLocked() {
        const int32_t cdb = _settings.muted ? AudioMix::MutedCentiDb : _settings.volumeDb * 100;
        if (cdb == _appliedVolume) return Error::Ok;
        const Error err = _codec.setVolume(cdb);
        if (err.isOk() && _codec.isRunning()) _appliedVolume = cdb;
        return err;
}

void AudioDevice::applySettingsLocked() {
        _settingsSeen = _settingsChanges.load(std::memory_order_acquire);
        _settings = readSettings();
        const Error err = applyVolumeLocked();
        if (err.isError()) bmdsWarn("audio: dsp%u: the volume wasn't set: %s", _number, err.desc());
        if (_state != State::Running) return;
        if (wantedRate() != _rateHz || _settings.periodFrames != _periodFrames) {
                const Error rerr = restartLocked(wantedRate(), _settings.periodFrames);
                if (rerr.isError()) bmdsWarn("audio: dsp%u: the restart failed: %s", _number, rerr.desc());
        }
        return;
}

// ---- Starting and stopping

Error AudioDevice::makeChannel(uint32_t periodFrames) {
        const BoardAudio &a = board().audio;
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.dma_desc_num = DmaBuffers;
        cc.dma_frame_num = periodFrames;
        cc.auto_clear_after_cb = true; // a buffer not refilled in time plays silence
        i2s_chan_handle_t tx = nullptr;
        esp_err_t         err = i2s_new_channel(&cc, &tx, nullptr);
        if (err != ESP_OK) return Error::fromEspErr(err);
        i2s_std_config_t sc = {};
        sc.clk_cfg = clockFor(DefaultRateHz);
        sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
        sc.gpio_cfg.mclk = static_cast<gpio_num_t>(a.mclk);
        sc.gpio_cfg.bclk = static_cast<gpio_num_t>(a.bclk);
        sc.gpio_cfg.ws = static_cast<gpio_num_t>(a.ws);
        sc.gpio_cfg.dout = static_cast<gpio_num_t>(a.dout);
        sc.gpio_cfg.din = I2S_GPIO_UNUSED; // playback only (capture comes last)
        err = i2s_channel_init_std_mode(tx, &sc);
        if (err == ESP_OK) {
                i2s_event_callbacks_t cbs = {};
                cbs.on_sent = AudioDeviceIsr::sent;
                err = i2s_channel_register_event_callback(tx, &cbs, this);
        }
        if (err != ESP_OK) {
                (void)i2s_del_channel(tx);
                return Error::fromEspErr(err);
        }
        _channel = tx;
        _channelPeriod = periodFrames;
        return Error::Ok;
}

Error AudioDevice::startLocked(uint32_t rateHz, uint32_t periodFrames) {
        // Check the rate and period before touching anything.
        if (Es8311Regs::clockPlan(rateHz * MclkMultiple, rateHz).isError()) return Error::NotSupported;
        if (periodFrames < MinPeriodFrames || periodFrames > MaxPeriodFrames) return Error::NotSupported;
        if (_state == State::Running) {
                if (rateHz == _rateHz && periodFrames == _periodFrames) return Error::Ok;
                return restartLocked(rateHz, periodFrames);
        }
        if (!_task.isRunning()) {
                Task::Config tc;
                tc.name = "audio";
                tc.stackBytes = 4096;
                tc.priority = CONFIG_BMDS_AUDIO_PRIORITY;
                const Error err = _task.start(tc, taskEntry, this);
                if (err.isError()) return err;
        }
        if (_mix == nullptr) {
                const size_t bytes = MaxPeriodFrames * 2 * sizeof(float);
                _mix = static_cast<float *>(Memory::alloc(MemSpace::Internal, bytes));
                _scratch = static_cast<float *>(Memory::alloc(MemSpace::Internal, bytes));
                if (_mix == nullptr || _scratch == nullptr) {
                        Memory::free(_mix);
                        Memory::free(_scratch);
                        _mix = _scratch = nullptr;
                        return Error::NoMem;
                }
        }
        Error err = _codec.attach();
        // The DMA buffers are a period each: another period, another channel.
        if (err.isOk() && _channel != nullptr && _channelPeriod != periodFrames) {
                (void)i2s_del_channel(handleOf(_channel));
                _channel = nullptr;
        }
        if (err.isOk() && _channel == nullptr) err = makeChannel(periodFrames);
        if (err.isOk()) {
                const i2s_std_clk_config_t clk = clockFor(rateHz);
                err = Error::fromEspErr(i2s_channel_reconfig_std_clock(handleOf(_channel), &clk));
        }
        // The streams attached follow the rate (the engine isn't running them).
        for (auto &slot : _slots) {
                AudioStream *s = slot.load(std::memory_order_acquire);
                if (err.isOk() && s != nullptr) err = s->prepare(rateHz, periodFrames);
        }
        if (err.isError()) {
                _state = State::Faulted;
                _lastError = err;
                dropAll();
                return err;
        }
        _rateHz = rateHz;
        _periodFrames = periodFrames;
        _mono = _codec.outputChannels() == 1;
        _mclkHz = 0;
        _apllHz = 0;
        _haveFirst = false;
        _idlePeriods = 0;
        _limiter.setRate(rateHz);
        _measuredMilliHz.store(0, std::memory_order_relaxed);
        _measuredMs.store(0, std::memory_order_relaxed);
        _periods.store(0, std::memory_order_relaxed);
        _late.store(0, std::memory_order_relaxed);
        _fillUsLast.store(0, std::memory_order_relaxed);
        _fillUsMax.store(0, std::memory_order_relaxed);
        _limited.store(0, std::memory_order_relaxed);
        _mixedPeriods.store(0, std::memory_order_relaxed);
        _mixedUs.store(0, std::memory_order_relaxed);
        _tail = _head.load(std::memory_order_acquire);
        // The volume goes in with the codec's start (which resets the chip).
        _appliedVolume = INT32_MIN;
        (void)applyVolumeLocked();
        _settled.store(false, std::memory_order_relaxed);
        _active.store(true, std::memory_order_release);
        err = Error::fromEspErr(i2s_channel_enable(handleOf(_channel)));
        // MCLK runs from here: the codec needs it to start.
        if (err.isOk()) err = _codec.start(rateHz * MclkMultiple, rateHz);
        if (err.isError()) {
                _active.store(false, std::memory_order_release);
                (void)i2s_channel_disable(handleOf(_channel));
                _state = State::Faulted;
                _lastError = err;
                {
                        Mutex::Locker filling(_fillLock, WaitForever);
                        dropAll();
                }
                return err;
        }
        _appliedVolume = _settings.muted ? AudioMix::MutedCentiDb : _settings.volumeDb * 100;
        i2s_tuning_info_t info = {};
        if (i2s_channel_tune_rate(handleOf(_channel), nullptr, &info) == ESP_OK)
                _mclkHz = static_cast<uint32_t>(info.curr_mclk_hz);
        uint32_t apll = 0;
        if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_APLL, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &apll) == ESP_OK)
                _apllHz = apll;
        _state = State::Running;
        _lastError = Error::Ok;
        // Periods the DMA played while the codec started (a restart on the engine's own task
        // can't fill them) were silence anyway: lateness counts from here.
        _settled.store(true, std::memory_order_release);
        return Error::Ok;
}

Error AudioDevice::restartLocked(uint32_t rateHz, uint32_t periodFrames) {
        (void)stopLocked(true);
        return startLocked(rateHz, periodFrames);
}

Error AudioDevice::stopLocked(bool keepStreams) {
        if (_state == State::Stopped) return Error::Ok;
        if (_state == State::Running && !keepStreams && attachedCount() > 0 &&
            _task.handle() != xTaskGetCurrentTaskHandle()) {
                // Tones fade out; the engine drops each once the DMA has sent its last period.
                for (auto &slot : _slots) {
                        AudioStream *s = slot.load(std::memory_order_acquire);
                        if (s != nullptr) s->cancel();
                }
                Semaphore       wake(1, 0);
                WaitList::Entry entry(_dropped, wake);
                const Deadline  deadline(FadeMs + (DmaBuffers + 1) * periodMs(_periodFrames, _rateHz) + StopMarginMs);
                while (entry.isAdded() && attachedCount() > 0)
                        if (wake.acquire(deadline).isError()) break;
        }
        const Error err = _state == State::Running ? _codec.stop() : Error::Ok;
        _active.store(false, std::memory_order_release);
        if (_channel != nullptr) (void)i2s_channel_disable(handleOf(_channel));
        {
                // The task isn't filling once it is out of here (and won't again: not active).
                Mutex::Locker filling(_fillLock, WaitForever);
                if (!keepStreams) dropAll();
        }
        _state = State::Stopped;
        return err;
}

Error AudioDevice::stop() {
        Mutex::Locker locker(_lock, WaitForever);
        _held = false;
        _rateOverride = 0;
        return stopLocked(false);
}

Error AudioDevice::hold(uint32_t rateHz) {
        Mutex::Locker locker(_lock, WaitForever);
        if (rateHz != 0 && Es8311Regs::clockPlan(rateHz * MclkMultiple, rateHz).isError()) return Error::NotSupported;
        _rateOverride = rateHz;
        const Error err = startLocked(wantedRate(), _settings.periodFrames);
        if (err.isOk()) _held = true;
        return err;
}

// ---- Streams

size_t AudioDevice::attachedCount() const {
        size_t n = 0;
        for (const auto &slot : _slots) n += slot.load(std::memory_order_acquire) != nullptr;
        return n;
}

Error AudioDevice::attach(AudioStream &stream) {
        Mutex::Locker locker(_lock, WaitForever);
        return attachLocked(stream);
}

Error AudioDevice::attachLocked(AudioStream &stream) {
        if (stream._state.load(std::memory_order_acquire) != AudioStream::Idle) return Error::InvalidState;
        std::atomic<AudioStream *> *free = nullptr;
        for (auto &slot : _slots)
                if (free == nullptr && slot.load(std::memory_order_acquire) == nullptr) free = &slot;
        if (free == nullptr) return Error::Full;
        if (_state != State::Running) {
                const Error err = startLocked(wantedRate(), _settings.periodFrames);
                if (err.isError()) return err;
        }
        const Error err = stream.prepare(_rateHz, _periodFrames);
        if (err.isError()) return err;
        (void)stream._gone.tryAcquire(); // released once per attach, when the engine lets go
        stream._detach.store(false, std::memory_order_relaxed);
        stream._state.store(AudioStream::Attached, std::memory_order_release);
        _idlePeriods = 0;
        free->store(&stream, std::memory_order_release);
        return Error::Ok;
}

void AudioDevice::detach(AudioStream &stream) {
        if (stream._state.load(std::memory_order_acquire) == AudioStream::Idle) return;
        stream._detach.store(true, std::memory_order_release);
        if (stream._gone.acquire(DetachWaitMs).isError()) {
                // The engine isn't running it: take it out here.
                Mutex::Locker locker(_lock, WaitForever);
                Mutex::Locker filling(_fillLock, WaitForever);
                for (auto &slot : _slots)
                        if (slot.load(std::memory_order_acquire) == &stream)
                                slot.store(nullptr, std::memory_order_release);
        }
        stream._state.store(AudioStream::Idle, std::memory_order_release);
        stream._detach.store(false, std::memory_order_relaxed);
        Mutex::Locker locker(_lock, WaitForever); // nobody is describing it now
        return;
}

void AudioDevice::reclaim(AudioStream &stream) {
        if (stream._state.load(std::memory_order_acquire) != AudioStream::Over) return;
        (void)stream._gone.acquire(DetachWaitMs); // released already: the engine let go of it
        stream._state.store(AudioStream::Idle, std::memory_order_release);
        return;
}

// The engine lets go of @p stream (with the fill lock held, or the device stopped).
void AudioDevice::drop(size_t slot, AudioStream &stream) {
        _slots[slot].store(nullptr, std::memory_order_release);
        stream._state.store(AudioStream::Over, std::memory_order_release);
        stream._waiters.notify();
        _dropped.notify();
        stream._gone.release(); // the last touch: its owner may free it now
        return;
}

void AudioDevice::dropAll() {
        for (size_t i = 0; i < MaxStreams; ++i) {
                AudioStream *s = _slots[i].load(std::memory_order_acquire);
                if (s != nullptr) drop(i, *s);
        }
        return;
}

Result<uint32_t> AudioDevice::playTone(uint32_t frequencyHz, uint32_t durationMs, int32_t levelCentiDb,
                                       SineTone::Slots slots) {
        Mutex::Locker  locker(_lock, WaitForever);
        const uint32_t rate = _state == State::Running ? _rateHz : wantedRate();
        if (frequencyHz == 0 || frequencyHz >= rate / 2 || durationMs == 0) return makeError<uint32_t>(Error::Invalid);
        size_t i = 0;
        for (; i < MaxTones; ++i) {
                reclaim(_tones[i]);
                if (_tones[i]._state.load(std::memory_order_acquire) == AudioStream::Idle) break;
        }
        if (i == MaxTones) return makeError<uint32_t>(Error::Busy);
        ToneStream &t = _tones[i];
        t.setTone(frequencyHz, durationMs, levelCentiDb, slots);
        const uint32_t generation = _toneGeneration[i].fetch_add(1, std::memory_order_acq_rel) + 1;
        const Error    err = attachLocked(t);
        if (err.isError()) return makeError<uint32_t>(err);
        return makeResult(generation * MaxTones + static_cast<uint32_t>(i));
}

Error AudioDevice::waitTone(uint32_t id, Interrupt *interrupt) {
        const size_t    i = id % MaxTones;
        const uint32_t  generation = id / MaxTones;
        ToneStream     &t = _tones[i];
        Semaphore       wake(1, 0);
        WaitList::Entry entry(t.waiters(), wake);
        if (!entry.isAdded()) return Error::Busy;
        const Deadline forever(WaitForever);
        while (_toneGeneration[i].load(std::memory_order_acquire) == generation && !t.isOver()) {
                const Error err = Interrupt::wait(interrupt, wake, forever);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

void AudioDevice::cancelTone(uint32_t id) {
        const size_t i = id % MaxTones;
        if (_toneGeneration[i].load(std::memory_order_acquire) == id / MaxTones) _tones[i].cancel();
        return;
}

Error AudioDevice::playSound(SystemSound sound) {
        const auto n = static_cast<size_t>(sound);
        if (n >= SystemSoundCount) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        ToneStream   &s = _sounds[n];
        reclaim(s);
        if (s._state.load(std::memory_order_acquire) != AudioStream::Idle) return Error::Ok; // sounding already
        s.setSound(sound);
        return attachLocked(s);
}

// ---- Status

AudioDevice::Status AudioDevice::status() {
        Mutex::Locker locker(_lock, WaitForever);
        Status        s;
        s.state = _state;
        s.rateHz = _rateHz;
        s.periodFrames = _periodFrames;
        s.mclkAskedHz = _rateHz * MclkMultiple;
        s.mclkHz = _mclkHz;
        s.apllHz = _apllHz;
        s.measuredMilliHz = _measuredMilliHz.load(std::memory_order_relaxed);
        s.measuredMs = _measuredMs.load(std::memory_order_relaxed);
        s.periods = _periods.load(std::memory_order_relaxed);
        s.late = _late.load(std::memory_order_relaxed);
        s.fillUsLast = _fillUsLast.load(std::memory_order_relaxed);
        s.fillUsMax = _fillUsMax.load(std::memory_order_relaxed);
        s.limited = _limited.load(std::memory_order_relaxed);
        s.mixedPeriods = _mixedPeriods.load(std::memory_order_relaxed);
        s.mixedUs = _mixedUs.load(std::memory_order_relaxed);
        s.volumeDb = _settings.volumeDb;
        s.muted = _settings.muted;
        s.held = _held;
        s.streams = attachedCount();
        for (const ToneStream &t : _tones) s.toneActive |= t.isPlaying();
        return s;
}

AudioDevice::Timing AudioDevice::timing() {
        Mutex::Locker locker(_lock, WaitForever);
        if (_state == State::Running) return Timing{_rateHz, _periodFrames};
        return Timing{wantedRate(), _settings.periodFrames};
}

Error AudioDevice::addClient() {
        uint8_t n = _clients.load(std::memory_order_relaxed);
        do {
                if (n >= MaxClients) return Error::Busy;
        } while (!_clients.compare_exchange_weak(n, static_cast<uint8_t>(n + 1), std::memory_order_acq_rel));
        return Error::Ok;
}

void AudioDevice::removeClient() {
        _clients.fetch_sub(1, std::memory_order_acq_rel);
        return;
}

size_t AudioDevice::streamLines(StreamLine *out, size_t max) {
        Mutex::Locker locker(_lock, WaitForever);
        size_t        n = 0;
        for (auto &slot : _slots) {
                AudioStream *s = slot.load(std::memory_order_acquire);
                if (s == nullptr || n >= max) continue;
                char about[96];
                s->describe(about, sizeof(about));
                const char *state = s->_state.load(std::memory_order_acquire) == AudioStream::Ending ? " (ending)" : "";
                std::snprintf(out[n].text, sizeof(out[n].text), "%s: %s%s", s->label(), about, state);
                ++n;
        }
        return n;
}

// ---- The engine

void AudioDevice::taskEntry(void *arg) {
        static_cast<AudioDevice *>(arg)->run();
        return;
}

void AudioDevice::run() {
        for (;;) {
                (void)_wake.acquire(WaitForever);
                {
                        Mutex::Locker filling(_fillLock, WaitForever);
                        for (;;) {
                                const uint32_t head = _head.load(std::memory_order_acquire);
                                if (_tail == head) break;
                                // Behind by more than the ring holds: those buffers have played.
                                if (head - _tail > RingSize) _tail = head - RingSize;
                                const Finished f = _ring[_tail % RingSize];
                                ++_tail;
                                if (_active.load(std::memory_order_acquire)) fill(f);
                        }
                }
                control();
        }
}

// Between periods: settings to apply, and the idle stop. Never waits for the lock (a caller
// holding it may be waiting for this task): tried again next period.
void AudioDevice::control() {
        const bool settings = _settingsChanges.load(std::memory_order_acquire) != _settingsSeen;
        const bool idle = _active.load(std::memory_order_acquire) && !_held &&
                          _idlePeriods * periodMs(_periodFrames, _rateHz) >= IdleMs;
        if (!settings && !idle) return;
        if (_lock.tryLock().isError()) return;
        if (settings) applySettingsLocked();
        if (idle && !_held && _state == State::Running && attachedCount() == 0) (void)stopLocked(false);
        _lock.unlock();
        return;
}

void AudioDevice::fill(const Finished &f) {
        const int64_t  started = esp_timer_get_time();
        const uint32_t frames = _periodFrames;
        const uint32_t now = _sequence.load(std::memory_order_relaxed);
        bool           any = false;
        for (size_t i = 0; i < MaxStreams; ++i) {
                AudioStream *s = _slots[i].load(std::memory_order_acquire);
                if (s == nullptr) continue;
                if (!any) std::memset(_mix, 0, frames * 2 * sizeof(float));
                any = true;
                if (s->_detach.load(std::memory_order_acquire)) {
                        drop(i, *s);
                        continue;
                }
                const uint8_t state = s->_state.load(std::memory_order_relaxed);
                if (state == AudioStream::Attached) {
                        if (!s->render(_mix, frames, _scratch)) {
                                // Its last period is in this buffer: over once the DMA has sent it.
                                s->_endAt = f.sequence + DmaBuffers;
                                s->_state.store(AudioStream::Ending, std::memory_order_release);
                        }
                } else if (state == AudioStream::Ending && static_cast<int32_t>(now - s->_endAt) >= 0) {
                        drop(i, *s);
                }
        }
        // Nothing playing: the buffer was cleared after the DMA sent it (auto_clear).
        if (any) {
                _limiter.process(_mix, frames);
                AudioMix::toSlots(_mix, f.buffer, frames, _mono);
                (void)esp_cache_msync(f.buffer, frames * FrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                _limited.store(_limiter.limitedFrames(), std::memory_order_relaxed);
                _idlePeriods = 0;
        } else {
                ++_idlePeriods;
        }
        // Late: the DMA has come round to this buffer (DmaBuffers - 1 EOFs on).
        if (_sequence.load(std::memory_order_relaxed) - f.sequence >= DmaBuffers - 1 &&
            _settled.load(std::memory_order_acquire))
                _late.fetch_add(1, std::memory_order_relaxed);
        _periods.fetch_add(1, std::memory_order_relaxed);
        // The frame rate, by the EOFs' times since the first (esp_timer runs from the crystal).
        if (!_haveFirst) {
                _firstSequence = f.sequence;
                _firstUs = f.timeUs;
                _haveFirst = true;
        } else if (f.timeUs - _firstUs >= 1'000'000) {
                const uint64_t played = static_cast<uint64_t>(f.sequence - _firstSequence) * frames;
                const uint64_t us = static_cast<uint64_t>(f.timeUs - _firstUs);
                _measuredMilliHz.store(static_cast<uint32_t>(played * 1'000'000'000ull / us),
                                       std::memory_order_relaxed);
                _measuredMs.store(static_cast<uint32_t>(us / 1000), std::memory_order_relaxed);
        }
        const auto took = static_cast<uint32_t>(esp_timer_get_time() - started);
        if (any) {
                _mixedPeriods.fetch_add(1, std::memory_order_relaxed);
                _mixedUs.fetch_add(took, std::memory_order_relaxed);
        }
        _fillUsLast.store(took, std::memory_order_relaxed);
        if (took > _fillUsMax.load(std::memory_order_relaxed)) _fillUsMax.store(took, std::memory_order_relaxed);
        return;
}

// ---- System sounds

namespace SystemSounds {

        Error play(SystemSound sound) {
                if (!SysConf::isReady()) return Error::Ok;
                if (sound == SystemSound::Bell) {
                        String bell;
                        if (SysConf::get(bmdsConf_audioBell.key, bell).isOk() && StringView(bell) == "off")
                                return Error::Ok;
                } else {
                        auto [on, err] = SysConf::getBool(bmdsConf_audioFeedback.key);
                        if (err.isError() || !on) return Error::Ok;
                }
                auto [d, derr] = AudioDevice::defaultDevice();
                if (derr.isError()) return Error::Ok;
                return d->playSound(sound);
        }

} // namespace SystemSounds

BMDS_NAMESPACE_END
