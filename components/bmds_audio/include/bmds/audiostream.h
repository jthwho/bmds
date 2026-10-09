/**
 * @file      audiostream.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/memspace.h>
#include <bmds/resampler.h>
#include <bmds/result.h>
#include <bmds/sampleformat.h>
#include <bmds/semaphore.h>
#include <bmds/sinetone.h>
#include <bmds/span.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class AudioDevice;

/**
 * @brief Something an audio device mixes: PCM from a writer, or a tone
 *        (`devplan/audio.md` §17).
 *
 * The device's engine calls `prepare()` when it is attached (and when the
 * device's rate changes, while it isn't being rendered), then `render()` each
 * period on the engine's task. A stream's last period returns false; the
 * device then keeps it until the DMA has sent that period, and it is over
 * (`isOver()`, the waiters told).
 *
 * Every attach is paired with one `AudioDevice::detach()`, which returns
 * once the engine has let go of it: only then may it be freed or reused.
 *
 * @par Thread Safety
 * The engine's calls run on the device's task; `cancel()`, `isOver()` and
 * the waiters from any task; the rest as each kind says.
 */
class AudioStream {
        public:
                /** @brief What it is. */
                enum class Kind : uint8_t {
                        Pcm,  ///< Written by a client, converted and resampled.
                        Tone, ///< A tone or a system sound, made at the device's rate.
                };

                AudioStream(const AudioStream &) = delete;
                AudioStream &operator=(const AudioStream &) = delete;

                /** @brief Returns the kind. */
                Kind kind() const { return _kind; }

                /** @brief Returns the label `audio status` shows (`tone`, `bell`, `sweep`). */
                const char *label() const { return _label; }

                /** @brief Sets the gain applied in the mix (1: as written). */
                void setGain(float gain);

                /** @brief Returns the gain. */
                float gain() const;

                /** @brief True while attached to a device and not over. @nonblocking */
                bool isPlaying() const { return _state.load(std::memory_order_acquire) == Attached; }

                /** @brief True once over (played out, cancelled, dropped) or never attached. @nonblocking */
                bool isOver() const;

                /** @brief Told when it is over (and, for PCM, when room frees). */
                WaitList &waiters() { return _waiters; }

                /**
                 * @brief Ends it soon: a tone fades out; PCM stops at once. Then it is
                 *        over as if it had ended. @nonblocking
                 */
                virtual void cancel() = 0;

                /**
                 * @brief Gets ready to play at @p deviceRate, @p maxFrames a period at
                 *        most (the engine isn't rendering it).
                 * @return `Ok`, or `NoMem`.
                 */
                virtual Error prepare(uint32_t deviceRate, size_t maxFrames) = 0;

                /**
                 * @brief Adds @p frames frames (stereo float) into @p mix, using
                 *        @p scratch (as big) if it needs to.
                 * @return False if this was its last period (it may have added
                 *         frames to it): the device then lets the DMA send it.
                 * @hotpath On the engine's task.
                 */
                virtual bool render(float *mix, size_t frames, float *scratch) = 0;

                /** @brief Writes a line about it for `audio status` into @p out. */
                virtual void describe(char *out, size_t size) const = 0;

        protected:
                AudioStream(Kind kind, const char *label) : _kind(kind), _label(label), _gone(1, 0) {}
                virtual ~AudioStream() = default;

        private:
                friend class AudioDevice;

                // Where it is in a device (set by the device).
                enum State : uint8_t {
                        Idle,     // not attached
                        Attached, // rendered each period
                        Ending,   // rendered its last: waiting for the DMA to send it
                        Over,     // dropped by the engine (`_gone` released)
                };

                Kind                  _kind;
                const char           *_label;
                std::atomic<uint8_t>  _state{Idle};
                std::atomic<bool>     _detach{false};    // its owner asks the engine to drop it
                std::atomic<uint32_t> _gain{0x3f800000}; // 1.0f's bits
                uint32_t              _endAt = 0;        // the EOF count by which its last period is sent
                Semaphore             _gone;             // released when the engine lets go of it
                WaitListOf<4>         _waiters;
};

/**
 * @brief A tone, or a system sound: a `ToneSequence` made at the device's
 *        rate (`devplan/audio.md` §17).
 *
 * @par Thread Safety
 * Set it up (`setTone()`, `setSound()`) before it is attached.
 */
class ToneStream final : public AudioStream {
        public:
                explicit ToneStream(const char *label = "tone") : AudioStream(Kind::Tone, label) {}

                /** @brief Plays one tone (as `audio tone`). */
                void setTone(uint32_t frequencyHz, uint32_t ms, int32_t centiDb, SineTone::Slots slots);

                /** @brief Plays @p sound. */
                void setSound(SystemSound sound);

                void  cancel() override { _cancel.store(true, std::memory_order_release); }
                Error prepare(uint32_t deviceRate, size_t maxFrames) override;
                bool  render(float *mix, size_t frames, float *scratch) override;
                void  describe(char *out, size_t size) const override;

        private:
                ToneSequence      _sequence;
                std::atomic<bool> _cancel{false};
                bool              _cancelled = false; // the engine's: cancel() seen
                bool              _isSound = false;
                SystemSound       _sound = SystemSound::Bell;
                uint32_t          _frequencyHz = 0;
                uint32_t          _ms = 0;
                int32_t           _centiDb = 0;
                SineTone::Slots   _slots = SineTone::Both;
};

/**
 * @brief PCM written by a client into a ring, converted to float and
 *        resampled to the device's rate as the engine plays it
 *        (`devplan/audio.md` §17). The stream under step 3's `/dev/dsp`.
 *
 * It plays once the ring holds the start threshold (or a drain is asked).
 * A ring that runs dry while playing is an underrun: counted, and silence
 * mixed until data comes.
 *
 * @par Thread Safety
 * One writer task (`write()`, `drain()`) and the engine. `open()` and
 * `close()` while not attached.
 */
class PcmStream final : public AudioStream {
        public:
                /** @brief A stream's format. */
                struct Format {
                                SampleFormat format = SampleFormat::S16Le;
                                uint8_t      channels = 2;
                                uint32_t     rateHz = 48000;
                };

                /** @brief The lowest and highest rates taken. */
                static constexpr uint32_t MinRateHz = 4000;
                static constexpr uint32_t MaxRateHz = 192000;

                explicit PcmStream(const char *label = "pcm") : AudioStream(Kind::Pcm, label) {}
                ~PcmStream();

                /**
                 * @brief Sets the format and allocates a ring of at least @p ringFrames
                 *        in @p space (rounded up to a power of two in bytes), to play
                 *        once @p startFrames are in it.
                 * @return `Ok`, `Invalid` (a format, channel count or rate not taken),
                 *         or `NoMem`.
                 */
                Error open(const Format &format, size_t ringFrames, size_t startFrames, MemSpace space);

                /** @brief Frees the ring and the resampler. */
                void close();

                /** @brief Returns the format. */
                const Format &format() const { return _format; }

                /** @brief Returns the bytes a frame takes. */
                size_t frameBytes() const { return _frameBytes; }

                /**
                 * @brief Copies @p data into the ring, waiting for room while it is
                 *        full (or not at all if @p nonBlocking).
                 * @return The bytes taken (all of them, unless interrupted, over or
                 *         non-blocking); `WouldBlock` (non-blocking and full),
                 *         `Interrupted` (nothing taken), or `InvalidState` (over).
                 * @blocking Unless @p nonBlocking.
                 */
                Result<size_t> write(ByteSpan data, bool nonBlocking, Interrupt *interrupt);

                /**
                 * @brief Plays what is in the ring, then ends: waits until it is over.
                 * @return `Ok`, or `Interrupted` (it plays on).
                 * @blocking
                 */
                Error drain(Interrupt *interrupt);

                /** @brief Returns the frames in the ring. */
                size_t queuedFrames() const;

                /** @brief Returns the underruns since it opened. */
                uint32_t underruns() const { return _underruns.load(std::memory_order_relaxed); }

                /** @brief Returns the frames it has played (at its own rate). */
                uint64_t playedFrames() const { return _played.load(std::memory_order_relaxed); }

                void  cancel() override;
                Error prepare(uint32_t deviceRate, size_t maxFrames) override;
                bool  render(float *mix, size_t frames, float *scratch) override;
                void  describe(char *out, size_t size) const override;

        private:
                // Converts up to @p frames whole frames from the ring into @p stereo.
                size_t convert(float *stereo, size_t frames);

                Format                _format;
                size_t                _frameBytes = 0;
                size_t                _startFrames = 0;
                uint8_t              *_storage = nullptr;
                MemSpace              _space = MemSpace::Psram;
                ByteRing             *_ring = nullptr; // in _storage
                Resampler             _resampler;
                bool                  _resampling = false;
                bool                  _playing = false; // the start threshold was reached
                size_t                _flushLeft = 0;   // silence to feed the resampler at the end
                std::atomic<bool>     _draining{false};
                std::atomic<bool>     _cancelled{false};
                std::atomic<uint32_t> _underruns{0};
                std::atomic<uint64_t> _played{0};
                bool                  _starved = false; // the last period ran short (one underrun each time)
};

BMDS_NAMESPACE_END
