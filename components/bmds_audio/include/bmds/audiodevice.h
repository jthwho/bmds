/**
 * @file      audiodevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/audiocodec.h>
#include <bmds/audiomix.h>
#include <bmds/audiostream.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/sinetone.h>
#include <bmds/sysconf.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An audio output: an I²S port, the codec on it, and the engine that
 *        mixes its streams into the DMA buffers (`devplan/audio.md` §5, §16,
 *        §17). Numbered: `dsp0` is the board's codec.
 *
 * Each DMA buffer is one period. The engine's task fills the buffer the DMA
 * just finished, which plays `DmaBuffers - 1` periods later: the attached
 * streams are added into a float mix, limited, and written as 32-bit slots
 * (both slots alike for a mono codec). A period filled after the DMA reached
 * it is counted late and plays silence.
 *
 * The device starts when a stream is attached and stops after `IdleMs`
 * without one, unless held (`hold()`). Its volume, mute, rate and period
 * are settings (`audio/dspN/...`), applied as they change.
 *
 * @par Thread Safety
 * Thread-safe: the public calls serialize on the device's lock; the engine
 * reads the stream slots without it.
 */
class AudioDevice {
        public:
                /** @brief Devices a board can have. */
                static constexpr size_t MaxDevices = 1;

                /** @brief DMA buffers (periods in flight). */
                static constexpr size_t DmaBuffers = 3;

                /** @brief The period's bounds and default, in frames (devplan/audio.md §17). */
                static constexpr uint32_t MinPeriodFrames = 48;
                static constexpr uint32_t MaxPeriodFrames = 480;
                static constexpr uint32_t DefaultPeriodFrames = 240;

                /** @brief The rate without a setting. */
                static constexpr uint32_t DefaultRateHz = 48000;

                /** @brief Streams mixed at once: its tones and sounds, programs' and the `audio` command's. */
                static constexpr size_t MaxStreams = 16;

                /** @brief Programs' streams (`/dev/dsp` opens) at once. */
                static constexpr size_t MaxClients = 6;

                /** @brief Tones (`playTone()`) at once. */
                static constexpr size_t MaxTones = 4;

                /** @brief A device with no streams stops after this long. */
                static constexpr uint32_t IdleMs = 3000;

                /** @brief What the device is doing. */
                enum class State : uint8_t {
                        Stopped, ///< Not clocked; the codec muted, its converters stopped.
                        Running, ///< Clocked and playing (silence, or its streams).
                        Faulted, ///< A start or a control failed: see `lastError()`.
                };

                /** @brief What `audio status` shows (devplan/audio.md §3). */
                struct Status {
                                State    state = State::Stopped;
                                uint32_t rateHz = 0;          ///< The rate running (or last run).
                                uint32_t periodFrames = 0;    ///< The period running (or last run).
                                uint32_t mclkAskedHz = 0;     ///< MCLK asked for (256 × rate).
                                uint32_t mclkHz = 0;          ///< MCLK as the I²S driver set it.
                                uint32_t apllHz = 0;          ///< The APLL's frequency.
                                uint32_t measuredMilliHz = 0; ///< Frames per second as counted (0: not yet).
                                uint32_t measuredMs = 0;      ///< Over this long.
                                uint32_t periods = 0;         ///< Periods filled since the start.
                                uint32_t late = 0;            ///< Of those, filled after the DMA reached them.
                                uint32_t fillUsLast = 0;      ///< The last fill's time.
                                uint32_t fillUsMax = 0;       ///< The longest fill since the start.
                                uint32_t limited = 0;         ///< Frames the limiter reduced since the start.
                                uint32_t mixedPeriods = 0;    ///< Periods with streams to mix.
                                uint64_t mixedUs = 0;         ///< Their fills' time in all.
                                int32_t  volumeDb = 0;        ///< The volume setting (dB, at most 0).
                                bool     muted = false;       ///< The mute setting.
                                bool     held = false;        ///< Held running (`hold()`).
                                size_t   streams = 0;         ///< Streams attached.
                                bool     toneActive = false;  ///< A tone (`playTone()`) among them.
                };

                /** @brief The rate and period running, or the settings' while stopped. */
                struct Timing {
                                uint32_t rateHz = 0;
                                uint32_t periodFrames = 0;
                };

                /** @brief One line about a stream, for `audio status`. */
                struct StreamLine {
                                char text[112];
                };

                /**
                 * @brief Boot stage: makes the board's devices and watches their
                 *        settings (no hardware is touched until a device first starts).
                 * @return `Ok` (also on a board without audio), or `NotExist` (the
                 *         codec's I²C bus is missing).
                 */
                static Error init();

                /**
                 * @brief Returns device @p number (`dspN`).
                 * @return The device, or `NotSupported` (no such device on this board).
                 */
                static Result<AudioDevice *> device(size_t number);

                /**
                 * @brief Returns the device `audio/default` names.
                 * @return The device, or `NotSupported` (none, or not a `dspN`).
                 */
                static Result<AudioDevice *> defaultDevice();

                AudioDevice(AudioCodec &codec, uint8_t number);
                AudioDevice(const AudioDevice &) = delete;
                AudioDevice &operator=(const AudioDevice &) = delete;

                /** @brief Returns the number (`dspN`). */
                uint8_t number() const { return _number; }

                /** @brief Returns the codec. Lock with `lock()` around its calls. */
                AudioCodec &codec() { return _codec; }

                /** @brief Returns the device's lock (for codec calls from outside). */
                Mutex &lock() { return _lock; }

                /**
                 * @brief Starts @p stream playing (`prepare()`d at the device's rate),
                 *        starting the device if it is stopped. Pair with `detach()`.
                 * @return `Ok`, `InvalidState` (attached already), `Full`
                 *         (`MaxStreams`), the stream's `prepare()` error, or the
                 *         start's.
                 * @blocking A start: the codec's set-up over I²C.
                 */
                Error attach(AudioStream &stream);

                /**
                 * @brief Takes @p stream out (if it isn't over already) and returns
                 *        once the engine has let go of it: it may then be freed.
                 * @blocking Up to a period.
                 */
                void detach(AudioStream &stream);

                /**
                 * @brief Holds the device running (no idle stop) at @p rateHz (0: the
                 *        setting's), restarting it at another rate.
                 * @return `Ok`, `NotSupported` (a rate no setting fits), or the
                 *         start's error.
                 * @blocking
                 */
                Error hold(uint32_t rateHz);

                /**
                 * @brief Ends the hold and every stream (tones fade), and stops the
                 *        device. Idempotent.
                 * @blocking
                 */
                Error stop();

                /**
                 * @brief Plays a tone of @p frequencyHz for @p durationMs at
                 *        @p levelCentiDb in @p slots, mixed with whatever plays.
                 * @return The tone's number, for `waitTone()` and `cancelTone()`;
                 *         `Invalid` (a frequency of half the rate or more), `Busy`
                 *         (`MaxTones` playing), or `attach()`'s error.
                 * @blocking A start; otherwise returns once the tone is attached.
                 */
                Result<uint32_t> playTone(uint32_t frequencyHz, uint32_t durationMs, int32_t levelCentiDb,
                                          SineTone::Slots slots);

                /**
                 * @brief Waits until tone @p id is over: played to the end of its fade,
                 *        cancelled, or stopped.
                 * @return `Ok`, `Interrupted` (@p interrupt raised: the tone plays on),
                 *         or `Busy` (too many waiters).
                 * @blocking
                 */
                Error waitTone(uint32_t id, Interrupt *interrupt);

                /**
                 * @brief Fades tone @p id out now, if it is still playing; it then ends
                 *        as if it had run its length.
                 * @nonblocking
                 */
                void cancelTone(uint32_t id);

                /**
                 * @brief Plays system sound @p sound, whatever the settings say; one
                 *        already sounding isn't started again.
                 * @return `Ok`, or `attach()`'s error.
                 * @blocking A start.
                 */
                Error playSound(SystemSound sound);

                /** @brief Returns the status. */
                Status status();

                /** @brief Returns the rate and period. */
                Timing timing();

                /**
                 * @brief Counts a program's stream in (`MaxClients`).
                 * @return `Ok`, or `Busy` (as many as it takes).
                 * @nonblocking
                 */
                Error addClient();

                /** @brief Counts one out. @nonblocking */
                void removeClient();

                /** @brief Writes a line about each stream into @p out. @return The lines written. */
                size_t streamLines(StreamLine *out, size_t max);

                /** @brief Returns the error that faulted the device (`Ok` if none). */
                Error lastError() const { return _lastError; }

                /** @brief Returns the state's name: `stopped`, `running`, `faulted`. */
                static const char *stateName(State state);

        private:
                friend struct AudioDeviceIsr;

                struct Finished {
                                int32_t *buffer;
                                uint32_t sequence; // the EOF count when it finished
                                int64_t  timeUs;
                };

                // The settings, as read (devplan/audio.md §17).
                struct Settings {
                                int32_t  volumeDb = -9;
                                bool     muted = false;
                                uint32_t rateHz = DefaultRateHz;
                                uint32_t periodFrames = DefaultPeriodFrames;
                };

                static void taskEntry(void *arg);
                static void settingsChanged(void *arg);
                void        run();
                void        fill(const Finished &f);
                void        drop(size_t slot, AudioStream &stream);
                void        dropAll();
                void        control();
                Settings    readSettings() const;
                void        applySettingsLocked();
                Error       applyVolumeLocked();
                Error       makeChannel(uint32_t periodFrames);
                Error       startLocked(uint32_t rateHz, uint32_t periodFrames);
                Error       stopLocked(bool keepStreams);
                Error       restartLocked(uint32_t rateHz, uint32_t periodFrames);
                Error       attachLocked(AudioStream &stream);
                void        reclaim(AudioStream &stream); // a pool stream that is over, for reuse
                uint32_t    wantedRate() const { return _rateOverride != 0 ? _rateOverride : _settings.rateHz; }
                size_t      attachedCount() const;

                AudioCodec &_codec;
                Mutex       _lock;     // the public calls
                Mutex       _fillLock; // held by the task while it fills: a stop waits for it
                Semaphore   _wake;
                Task        _task;
                void       *_channel = nullptr; // i2s_chan_handle_t
                uint32_t    _channelPeriod = 0; // its DMA buffers' frames
                uint8_t     _number;
                State       _state = State::Stopped;
                Error       _lastError = Error::Ok;
                uint32_t    _rateHz = 0;
                uint32_t    _periodFrames = DefaultPeriodFrames;
                uint32_t    _mclkHz = 0;
                uint32_t    _apllHz = 0;
                bool        _held = false;
                uint32_t    _rateOverride = 0; // hold()'s rate

                // The interrupt's: a ring of finished buffers and the EOF count.
                Finished              _ring[DmaBuffers * 2] = {};
                std::atomic<uint32_t> _head{0};
                std::atomic<uint32_t> _sequence{0};

                // The task's.
                uint32_t              _tail = 0;
                std::atomic<bool>     _active{false};     // fill (else leave the cleared silence)
                std::atomic<bool>     _settled{false};    // started: lateness counts (not while the codec starts)
                float                *_mix = nullptr;     // MaxPeriodFrames stereo, internal RAM
                float                *_scratch = nullptr; // as big: a stream's conversions
                AudioLimiter          _limiter;
                bool                  _mono = false; // the codec plays one channel
                uint32_t              _idlePeriods = 0;
                uint32_t              _firstSequence = 0;
                int64_t               _firstUs = 0;
                bool                  _haveFirst = false;
                std::atomic<uint32_t> _measuredMilliHz{0};
                std::atomic<uint32_t> _measuredMs{0};
                std::atomic<uint32_t> _periods{0};
                std::atomic<uint32_t> _late{0};
                std::atomic<uint32_t> _fillUsLast{0};
                std::atomic<uint32_t> _fillUsMax{0};
                std::atomic<uint32_t> _limited{0};
                std::atomic<uint32_t> _mixedPeriods{0};
                std::atomic<uint64_t> _mixedUs{0};

                // The streams: attached under the lock, read by the engine without it.
                std::atomic<AudioStream *> _slots[MaxStreams] = {};
                WaitListOf<2>              _dropped; // told when the engine drops a stream
                std::atomic<uint8_t>       _clients{0};

                // Its own streams: tones and the system sounds.
                ToneStream            _tones[MaxTones];
                std::atomic<uint32_t> _toneGeneration[MaxTones] = {};
                ToneStream            _sounds[SystemSoundCount];

                // The settings: watched; applied by the task.
                SysConf::Watcher      _watcher;
                std::atomic<uint32_t> _settingsChanges{0};
                uint32_t              _settingsSeen = 0;
                Settings              _settings;
                int32_t               _appliedVolume = INT32_MIN; // centi-dB sent to the codec
};

/**
 * @brief The system sounds, on the default device (`audio/default`) and as
 *        the settings allow (`audio/bell`, `audio/feedback`):
 *        `devplan/audio.md` §17.
 */
namespace SystemSounds {

        /**
         * @brief Plays @p sound if the settings allow it.
         * @return `Ok` (also when the settings say no, or there is no audio
         *         device), or the device's error.
         * @blocking A device start (the codec's set-up over I²C).
         */
        Error play(SystemSound sound);

} // namespace SystemSounds

BMDS_NAMESPACE_END
