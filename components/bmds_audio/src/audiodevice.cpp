/**
 * @file      audiodevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An audio output: I2S TX on the APLL, its codec, and the task that fills
 * the DMA buffers (devplan/audio.md §5, §16).
 */

#include <cstring>
#include <driver/i2s_std.h>
#include <esp_cache.h>
#include <esp_clk_tree.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <bmds/audiodevice.h>
#include <bmds/board.h>
#include <bmds/es8311.h>
#include <bmds/es8311regs.h>
#include <bmds/i2cbus.h>
#include <bmds/interrupt.h>
#include <bmds/placement.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   RingSize = AudioDevice::DmaBuffers * 2;
        constexpr size_t   FrameBytes = 2 * sizeof(int32_t); // two 32-bit slots
        constexpr uint32_t MclkMultiple = 256;
        constexpr uint32_t FadeMs = 5;        // SineTone's fade
        constexpr uint32_t FadeMarginMs = 20; // scheduling, on top

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
        return Error::Ok;
}

Result<AudioDevice *> AudioDevice::device(size_t number) {
        if (number >= MaxDevices || !devices[number].isCreated()) return makeError<AudioDevice *>(Error::NotSupported);
        return makeResult(&*devices[number]);
}

AudioDevice::AudioDevice(AudioCodec &codec, uint8_t number)
    : _codec(codec), _wake(1, 0), _number(number), _toneDone(1, 0) {}

const char *AudioDevice::stateName(State state) {
        switch (state) {
                case State::Stopped: return "stopped";
                case State::Running: return "running";
                case State::Faulted: return "faulted";
        }
        return "?";
}

Error AudioDevice::makeChannel() {
        const BoardAudio &a = board().audio;
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        cc.dma_desc_num = DmaBuffers;
        cc.dma_frame_num = PeriodFrames;
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
        return Error::Ok;
}

Error AudioDevice::start(uint32_t rateHz) {
        Mutex::Locker locker(_lock, WaitForever);
        return startLocked(rateHz);
}

Error AudioDevice::startLocked(uint32_t rateHz) {
        // Check the rate before touching anything.
        auto [plan, perr] = Es8311Regs::clockPlan(rateHz * MclkMultiple, rateHz);
        (void)plan;
        if (perr.isError()) return perr;
        if (_state == State::Running) {
                if (rateHz == _rateHz) return Error::Ok;
                (void)stopLocked();
        }
        if (!_task.isRunning()) {
                Task::Config tc;
                tc.name = "audio";
                tc.stackBytes = 4096;
                tc.priority = CONFIG_BMDS_AUDIO_PRIORITY;
                const Error err = _task.start(tc, taskEntry, this);
                if (err.isError()) return err;
        }
        Error err = _codec.attach();
        if (err.isOk() && _channel == nullptr) err = makeChannel();
        if (err.isOk()) {
                const i2s_std_clk_config_t clk = clockFor(rateHz);
                err = Error::fromEspErr(i2s_channel_reconfig_std_clock(handleOf(_channel), &clk));
        }
        if (err.isError()) {
                _state = State::Faulted;
                _lastError = err;
                return err;
        }
        _rateHz = rateHz;
        _mclkHz = 0;
        _apllHz = 0;
        _haveFirst = false;
        _measuredMilliHz.store(0, std::memory_order_relaxed);
        _measuredMs.store(0, std::memory_order_relaxed);
        _periods.store(0, std::memory_order_relaxed);
        _late.store(0, std::memory_order_relaxed);
        _fillUsLast.store(0, std::memory_order_relaxed);
        _fillUsMax.store(0, std::memory_order_relaxed);
        _tail = _head.load(std::memory_order_acquire);
        _active.store(true, std::memory_order_release);
        err = Error::fromEspErr(i2s_channel_enable(handleOf(_channel)));
        // MCLK runs from here: the codec needs it to start.
        if (err.isOk()) err = _codec.start(rateHz * MclkMultiple, rateHz);
        if (err.isError()) {
                _active.store(false, std::memory_order_release);
                (void)i2s_channel_disable(handleOf(_channel));
                _state = State::Faulted;
                _lastError = err;
                return err;
        }
        i2s_tuning_info_t info = {};
        if (i2s_channel_tune_rate(handleOf(_channel), nullptr, &info) == ESP_OK)
                _mclkHz = static_cast<uint32_t>(info.curr_mclk_hz);
        uint32_t apll = 0;
        if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_APLL, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &apll) == ESP_OK)
                _apllHz = apll;
        _state = State::Running;
        _lastError = Error::Ok;
        return Error::Ok;
}

Error AudioDevice::stop() {
        Mutex::Locker locker(_lock, WaitForever);
        return stopLocked();
}

Error AudioDevice::stopLocked() {
        if (_state == State::Stopped) return Error::Ok;
        if (_toneActive.load(std::memory_order_acquire) && _state == State::Running) {
                (void)_toneDone.tryAcquire();
                _toneCancel.store(true, std::memory_order_release);
                // The fade, then the buffers queued behind it, then a margin.
                const uint32_t queuedMs = (DmaBuffers + 1) * PeriodFrames * 1000 / _rateHz;
                (void)_toneDone.acquire(FadeMs + queuedMs + FadeMarginMs);
        }
        const Error err = _codec.stop();
        _active.store(false, std::memory_order_release);
        _toneActive.store(false, std::memory_order_release);
        _toneWaiters.notify();
        if (_channel != nullptr) (void)i2s_channel_disable(handleOf(_channel));
        _state = State::Stopped;
        return err;
}

Result<uint32_t> AudioDevice::playTone(uint32_t frequencyHz, uint32_t durationMs, int32_t levelCentiDb,
                                       SineTone::Slots slots) {
        Mutex::Locker  locker(_lock, WaitForever);
        const uint32_t rate = _state == State::Running ? _rateHz : (_rateHz != 0 ? _rateHz : DefaultRateHz);
        if (frequencyHz == 0 || frequencyHz >= rate / 2 || durationMs == 0) return makeError<uint32_t>(Error::Invalid);
        const bool startHere = _state != State::Running;
        if (startHere) {
                const Error err = startLocked(rate);
                if (err.isError()) return makeError<uint32_t>(err);
        }
        const bool stops = startHere ||
                           (_toneStops.load(std::memory_order_relaxed) && _toneActive.load(std::memory_order_relaxed));
        _toneFrequency = frequencyHz;
        _toneLevel = levelCentiDb;
        _toneSlots = slots;
        _toneDurationMs = durationMs;
        _toneStops.store(stops, std::memory_order_relaxed);
        _toneCancel.store(false, std::memory_order_relaxed);
        _toneActive.store(true, std::memory_order_relaxed);
        const uint32_t id = _toneRequest.fetch_add(1, std::memory_order_release) + 1;
        _toneWaiters.notify(); // a tone it replaced is over
        return makeResult(id);
}

bool AudioDevice::isToneOver(uint32_t id) const {
        return _toneRequest.load(std::memory_order_acquire) != id || !_toneActive.load(std::memory_order_acquire);
}

Error AudioDevice::waitTone(uint32_t id, Interrupt *interrupt) {
        Semaphore       wake(1, 0);
        WaitList::Entry entry(_toneWaiters, wake);
        if (!entry.isAdded()) return Error::Busy;
        const Deadline forever(WaitForever);
        while (!isToneOver(id)) {
                const Error err = Interrupt::wait(interrupt, wake, forever);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

void AudioDevice::cancelTone(uint32_t id) {
        Mutex::Locker locker(_lock, WaitForever);
        if (!isToneOver(id)) _toneCancel.store(true, std::memory_order_release); // fades, then ends
        return;
}

AudioDevice::Status AudioDevice::status() {
        Mutex::Locker locker(_lock, WaitForever);
        Status        s;
        s.state = _state;
        s.rateHz = _rateHz;
        s.mclkAskedHz = _rateHz * MclkMultiple;
        s.mclkHz = _mclkHz;
        s.apllHz = _apllHz;
        s.measuredMilliHz = _measuredMilliHz.load(std::memory_order_relaxed);
        s.measuredMs = _measuredMs.load(std::memory_order_relaxed);
        s.periods = _periods.load(std::memory_order_relaxed);
        s.late = _late.load(std::memory_order_relaxed);
        s.fillUsLast = _fillUsLast.load(std::memory_order_relaxed);
        s.fillUsMax = _fillUsMax.load(std::memory_order_relaxed);
        s.toneActive = _toneActive.load(std::memory_order_relaxed);
        return s;
}

void AudioDevice::taskEntry(void *arg) {
        static_cast<AudioDevice *>(arg)->run();
        return;
}

void AudioDevice::run() {
        for (;;) {
                (void)_wake.acquire(WaitForever);
                bool toneEnded = false;
                for (;;) {
                        const uint32_t head = _head.load(std::memory_order_acquire);
                        if (_tail == head) break;
                        // Behind by more than the ring holds: those buffers have played.
                        if (head - _tail > RingSize) _tail = head - RingSize;
                        const Finished f = _ring[_tail % RingSize];
                        ++_tail;
                        if (_active.load(std::memory_order_acquire)) fill(f);
                }
                // A faded tone ends once the DMA has sent the buffer that holds its
                // fade: stopping sooner cuts the fade off, which clicks.
                if (_draining && !_toneActive.load(std::memory_order_acquire)) _draining = false; // stopped
                if (_draining && static_cast<int32_t>(_sequence.load(std::memory_order_relaxed) - _drainAt) >= 0) {
                        _draining = false;
                        toneEnded = _toneStops.load(std::memory_order_relaxed);
                        _toneActive.store(false, std::memory_order_release);
                        _toneDone.release();
                        _toneWaiters.notify();
                }
                if (toneEnded) stopAfterTone();
        }
}

void AudioDevice::fill(const Finished &f) {
        const int64_t started = esp_timer_get_time();
        // A new tone?
        const uint32_t request = _toneRequest.load(std::memory_order_acquire);
        if (request != _toneSeen) {
                _toneSeen = request;
                _tone.start(_toneFrequency, _rateHz, _toneLevel, _toneSlots);
                _toneFramesLeft = static_cast<uint32_t>(static_cast<uint64_t>(_toneDurationMs) * _rateHz / 1000);
                _draining = false;
        }
        if (_toneActive.load(std::memory_order_acquire) && !_draining) {
                if (_toneCancel.load(std::memory_order_acquire) || _toneFramesLeft == 0) _tone.fadeOut();
                _tone.fill(f.buffer, PeriodFrames);
                _toneFramesLeft = _toneFramesLeft > PeriodFrames ? _toneFramesLeft - PeriodFrames : 0;
                if (_tone.isFinished()) {
                        // This buffer plays from DmaBuffers - 1 EOFs on, and is sent by the next.
                        _draining = true;
                        _drainAt = f.sequence + DmaBuffers;
                }
                (void)esp_cache_msync(f.buffer, PeriodFrames * FrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        // Late: the DMA has come round to this buffer (DmaBuffers - 1 EOFs on).
        const uint32_t now = _sequence.load(std::memory_order_relaxed);
        if (now - f.sequence >= DmaBuffers - 1) _late.fetch_add(1, std::memory_order_relaxed);
        _periods.fetch_add(1, std::memory_order_relaxed);
        // The frame rate, by the EOFs' times since the first (esp_timer runs from the crystal).
        if (!_haveFirst) {
                _firstSequence = f.sequence;
                _firstUs = f.timeUs;
                _haveFirst = true;
        } else if (f.timeUs - _firstUs >= 1'000'000) {
                const uint64_t frames = static_cast<uint64_t>(f.sequence - _firstSequence) * PeriodFrames;
                const uint64_t us = static_cast<uint64_t>(f.timeUs - _firstUs);
                _measuredMilliHz.store(static_cast<uint32_t>(frames * 1'000'000'000ull / us),
                                       std::memory_order_relaxed);
                _measuredMs.store(static_cast<uint32_t>(us / 1000), std::memory_order_relaxed);
        }
        const auto took = static_cast<uint32_t>(esp_timer_get_time() - started);
        _fillUsLast.store(took, std::memory_order_relaxed);
        if (took > _fillUsMax.load(std::memory_order_relaxed)) _fillUsMax.store(took, std::memory_order_relaxed);
        return;
}

void AudioDevice::stopAfterTone() {
        Mutex::Locker locker(_lock, WaitForever);
        // Unless another tone began meanwhile.
        if (_toneActive.load(std::memory_order_acquire) || _toneRequest.load(std::memory_order_acquire) != _toneSeen)
                return;
        (void)stopLocked();
        return;
}

BMDS_NAMESPACE_END
