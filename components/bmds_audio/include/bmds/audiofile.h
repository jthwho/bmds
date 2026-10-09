/**
 * @file      audiofile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/audiodevice.h>
#include <bmds/audiostream.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What an fd of `/dev/dsp*`, `/dev/audio` or `/dev/mixer*` refers to
 *        (`devplan/audio.md` §18): a `PcmStream` on a device, written by a
 *        program and mixed with everything else. The system calls
 *        (`audio_config`, `audio_status`, `audio_control`) reach it through
 *        `Stream::audioFile()`.
 *
 * Opened at OSS's defaults (8 kHz, `u8`, mono; `/dev/audio` µ-law). The ring
 * is allocated at the first write or configure, and the stream attached to
 * the device at the first write. Writes block until all is in the ring.
 * Closing the last handle waits until what is queued has played; a signal
 * (a killed program) drops it instead.
 *
 * A mixer file has no stream: only its device's status.
 *
 * Configured `Realtime` (devplan/audio.md §20), it holds an `RtStream`
 * instead: attached at once, and played by a thread looping in `cycle()`.
 * `Exclusive` also gives it the device.
 *
 * @par Thread Safety
 * Thread-safe: the calls serialize on its lock (a write holds it while it
 * waits for room; `drop()` wakes such a writer first).
 */
class AudioFile final : public Stream {
        public:
                /** @brief What it was opened as. */
                enum class Kind : uint8_t {
                        Dsp,   ///< `/dev/dsp`, `/dev/dspN`.
                        Audio, ///< `/dev/audio`: as `Dsp`, but µ-law at first.
                        Mixer, ///< `/dev/mixer`, `/dev/mixerN`.
                };

                /** @brief `Config::flags` (`bmds_audio_config_t::flags`). */
                static constexpr uint32_t Realtime = 0x1;  ///< A real-time client (`cycle()`).
                static constexpr uint32_t Exclusive = 0x2; ///< With `Realtime`: it owns the device.

                /**
                 * @brief How it plays (`bmds_audio_config_t`; 0: the default). A
                 *        real-time client takes `F32Le`, no ring (`ringFrames` and
                 *        `startFrames` 0), the device's rate (0) unless exclusive, and
                 *        asks for the period in `wakeFrames` (0: the device's).
                 */
                struct Config {
                                SampleFormat format = SampleFormat::U8;
                                uint8_t      channels = 1;
                                uint32_t     rateHz = 8000;
                                uint32_t     ringFrames = 0;
                                uint32_t     startFrames = 0;
                                uint32_t     wakeFrames = 0;
                                uint32_t     flags = 0;
                };

                /** @brief Its state (`bmds_audio_status_t::state`'s numbers). */
                enum class State : uint8_t {
                        Idle,     ///< Not attached: nothing queued and not playing.
                        Waiting,  ///< Attached, below the start threshold.
                        Playing,  ///< Playing.
                        Paused,   ///< Paused, its ring kept.
                        Draining, ///< Playing what is queued, then idle.
                        Mixer,    ///< A mixer file.
                };

                /** @brief `bmds_audio_status_t`, in the kernel's types. */
                struct Status {
                                State    state = State::Idle;
                                uint8_t  device = 0;
                                uint32_t deviceRate = 0;
                                uint32_t periodFrames = 0;
                                uint32_t ringFrames = 0;
                                uint64_t writtenFrames = 0;
                                uint64_t playedFrames = 0;
                                int64_t  timestampNs = 0;
                                uint32_t queuedFrames = 0;
                                uint32_t delayFrames = 0;
                                uint32_t underruns = 0;
                                int32_t  gainCentiDb = 0;
                                uint32_t latePeriods = 0;
                                uint32_t limitedFrames = 0;
                };

                /** @brief The longest ring taken, in seconds of the stream's rate. */
                static constexpr uint32_t MaxRingSeconds = 10;

                /** @brief The quietest gain (silent), centi-dB. */
                static constexpr int32_t MutedCentiDb = AudioMix::MutedCentiDb;

                /**
                 * @brief Adds device @p device's files to `/dev` (`dspN`, `mixerN`),
                 *        and for device 0 also `dsp`, `audio` and `mixer`, which open
                 *        the device `audio/default` names.
                 * @return `Ok`, or `Vfs::addDeviceFile()`'s error.
                 */
                static Error addFiles(AudioDevice &device);

                /** @brief Returns the kind. */
                Kind kind() const { return _kind; }

                /** @brief Returns the device. */
                AudioDevice &device() { return _device; }

                /** @brief Returns the configuration as chosen (sizes rounded once allocated). */
                Config config();

                /**
                 * @brief Sets the configuration: what is queued plays out first.
                 * @return `Ok`, `Invalid` (a format, channel count, rate, size or flag
                 *         not taken), `NotSupported` (a mixer), `Busy` (exclusive mode
                 *         not free, or `AudioDevice::MaxRealtime` clients), `NoMem`,
                 *         `Interrupted`. On an error it plays as it was set before.
                 * @blocking While what is queued plays.
                 */
                Error configure(const Config &config);

                /** @brief Returns the status. */
                Status status();

                /**
                 * @brief A real-time client's thread: ends its period and, unless
                 *        @p next is null, waits for the next (`RtStream::cycle()`). The
                 *        thread runs at the client priority from here
                 *        (`AudioDevice::raiseCaller()`). After its device dropped the
                 *        stream (`BrokenPipe`), the next call joins again.
                 * @return `Ok`, `Invalid` (not real-time), `NotSupported` (a mixer),
                 *         `Interrupted`, `BrokenPipe`, or a rejoin's error.
                 * @blocking When @p next isn't null: a period.
                 */
                Error cycle(RtStream::Period *next);

                /** @brief Plays now, below the start threshold. @return `Ok`, or `NotSupported` (a mixer). */
                Error start();

                /** @brief Pauses or resumes. @return `Ok`, or `NotSupported` (a mixer). */
                Error setPaused(bool paused);

                /**
                 * @brief Waits until what is queued has played.
                 * @return `Ok`, `Interrupted` (it plays on), `NotSupported` (a mixer).
                 * @blocking
                 */
                Error drain();

                /** @brief Throws away what is queued and stops. @return `Ok`, or `NotSupported` (a mixer). */
                Error drop();

                /**
                 * @brief Sets the stream's gain to @p centiDb (`MutedCentiDb` to 0).
                 * @return `Ok`, `Invalid`, or `NotSupported` (a mixer).
                 */
                Error setGain(int32_t centiDb);

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                uint8_t    readiness() override;
                WaitList  *readinessWaiters() override;
                AudioFile *audioFile() override { return this; }
                void       handleClosed(uint8_t mode) override;

        private:
                AudioFile(AudioDevice &device, Kind kind);
                ~AudioFile() = default;

                // Opens one on @p device (`Vfs::DeviceFile::open`).
                static Result<FileHandle::Ptr> open(AudioDevice &device, Kind kind, uint8_t flags);

                Error    checkLocked(const Config &config) const;
                Error    openLocked();
                Error    openRealtimeLocked();
                void     closeLocked();
                bool     isRealtime() const { return (_config.flags & Realtime) != 0; }
                Error    attachLocked();
                void     settleLocked();
                Error    drainLocked(unsigned int timeoutMs);
                uint32_t wakeFramesLocked();
                uint32_t drainTimeoutMs() const;

                AudioDevice &_device;
                Kind         _kind;
                Mutex        _lock;
                Config       _config;
                int32_t      _gainCentiDb = 0;
                uint32_t     _wakeFrames = 1; // the ring's writable mark, set when it opens
                PcmStream    _pcm;
                RtStream          _rt;
                std::atomic<bool> _realtimeOn{false}; // as isRealtime(), for cycle() without the lock
};

BMDS_NAMESPACE_END
