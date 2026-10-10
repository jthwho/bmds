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
#include <bmds/seqlocked.h>
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
                        Realtime, ///< Made each period by a client's thread (`RtStream`).
                };

                AudioStream(const AudioStream &) = delete;
                AudioStream &operator=(const AudioStream &) = delete;

                /** @brief Returns the kind. */
                Kind kind() const { return _kind; }

                /** @brief Returns the label `audio status` shows (`tone`, `bell`, `sweep`). */
                const char *label() const { return _label; }

                /** @brief The problems its device counts across its streams (`AudioDevice::Status`). */
                enum class Tally : uint8_t {
                        Underrun,        ///< A written stream ran dry while playing.
                        ClientLate,      ///< A real-time client didn't finish a period in time.
                        ClientSkipped,   ///< A real-time client missed periods altogether.
                        ClientThrottled, ///< A real-time client's thread was put back to its own priority.
                };

                /** @brief `Tally`'s values. */
                static constexpr size_t TallyCount = 4;

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

                /** @brief True once its device has dropped it (played out, cancelled, a stop). */
                bool isDropped() const { return _state.load(std::memory_order_acquire) == Over; }
                virtual ~AudioStream() = default;

                /** @brief Adds @p n to its device's count of @p what. @hotpath */
                void tally(Tally what, uint32_t n = 1) {
                        if (_tallies != nullptr) _tallies[size_t(what)].fetch_add(n, std::memory_order_relaxed);
                        return;
                }

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
                std::atomic<uint32_t> *_tallies = nullptr; // its device's (set when attached)
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
 * Paused, it mixes nothing and keeps its ring (no underruns counted).
 * Once over (drained, cancelled, dropped), `reset()` empties it for another
 * attach.
 *
 * @par Thread Safety
 * One writer task (`write()`, `drain()`) and the engine; `pause()`,
 * `startNow()` and the counters from any task. `open()`, `close()` and
 * `reset()` while not attached.
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

                /** @brief True once `open()` has allocated its ring. */
                bool isOpen() const { return _ring != nullptr; }

                /**
                 * @brief Empties the ring and forgets the start, a drain and a
                 *        cancel, for another attach (the counters and a pause stay).
                 */
                void reset();

                /** @brief Sets the start threshold (at least 1, at most the ring), while not attached. */
                void setStartFrames(size_t frames);

                /** @brief Returns the format. */
                const Format &format() const { return _format; }

                /** @brief Returns the bytes a frame takes. */
                size_t frameBytes() const { return _frameBytes; }

                /**
                 * @brief Copies @p data into the ring, waiting for room while it is
                 *        full (or not at all if @p nonBlocking).
                 * @return The bytes taken: all of them, or (over or non-blocking) what
                 *         fit; interrupted, those taken with `Interrupted`. Nothing
                 *         taken: `WouldBlock` (non-blocking and full) or `InvalidState`
                 *         (over).
                 * @blocking Unless @p nonBlocking.
                 */
                Result<size_t> write(ByteSpan data, bool nonBlocking, Interrupt *interrupt);

                /**
                 * @brief Plays what is in the ring, then ends: waits until it is over,
                 *        up to @p timeoutMs.
                 * @return `Ok`, `Interrupted` (it plays on), or `Timeout`.
                 * @blocking
                 */
                Error drain(Interrupt *interrupt, unsigned int timeoutMs);

                /** @brief Plays from now on, without waiting for the start threshold. @nonblocking */
                void startNow();

                /** @brief Pauses (mixes nothing, keeps the ring) or resumes. @nonblocking */
                void setPaused(bool paused);

                /** @brief True while paused. */
                bool isPaused() const { return _paused.load(std::memory_order_acquire); }

                /** @brief True once the start threshold was reached (or `startNow()`). */
                bool hasStarted() const { return _started.load(std::memory_order_acquire); }

                /** @brief True while a drain is asked. */
                bool isDraining() const { return _draining.load(std::memory_order_acquire); }

                /** @brief Returns the frames in the ring. */
                size_t queuedFrames() const;

                /** @brief Returns the frames the ring holds in all. */
                size_t capacityFrames() const;

                /** @brief Returns the start threshold in frames. */
                size_t startFrames() const { return _startFrames; }

                /** @brief Returns the frames written since it opened. */
                uint64_t writtenFrames() const {
                        return _frameBytes == 0 ? 0 : _written.load(std::memory_order_relaxed) / _frameBytes;
                }

                /**
                 * @brief Returns the resampler's delay in the stream's frames (0
                 *        when it plays at the device's rate).
                 */
                size_t resamplerDelayFrames() const { return _resampling ? _resampler.delayFrames() : 0; }

                /** @brief Where playing has got to: frames taken from the ring, and when. */
                struct Position {
                                uint64_t frames = 0;
                                int64_t  ns = 0; ///< `Clock::monotonicNs()` when they were taken (0: none yet).
                };

                /** @brief Returns the position. @nonblocking */
                Position position() const { return _position.load(); }

                /** @brief Returns the underruns since it opened. */
                uint32_t underruns() const { return _underruns.load(std::memory_order_relaxed); }

                /** @brief Returns the frames it has played (at its own rate). */
                uint64_t playedFrames() const { return _position.load().frames; }

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
                size_t                _flushLeft = 0;  // silence to feed the resampler at the end
                std::atomic<bool>     _started{false}; // the start threshold was reached (or startNow())
                std::atomic<bool>     _paused{false};
                std::atomic<bool>     _draining{false};
                std::atomic<bool>     _cancelled{false};
                std::atomic<uint32_t> _underruns{0};
                std::atomic<uint64_t> _written{0}; // bytes
                SeqLocked<Position>   _position;
                bool                  _starved = false; // the last period ran short (one underrun each time)
};

/**
 * @brief A real-time client's stream (`devplan/audio.md` §20): a program's
 *        thread makes each period's output as the engine asks for it, with
 *        no ring between them.
 *
 * Each period the engine `offer()`s the period to make, mixes its other
 * streams, then waits (until the next end-of-frame, a period on) for the
 * client to end it (`isDone()`), and `render()` adds the output. The
 * client's thread loops in `cycle()`: it ends the period it had and waits
 * for the next. A period not ended by then is late: counted, and nothing
 * added. A client more than a period behind gets the newest period, told
 * how many it skipped.
 *
 * Its output buffer holds `MaxFrames` frames of float in the client's
 * channels, so a change of the device's period never reallocates it.
 *
 * @par Thread Safety
 * One client thread (`cycle()`) and the engine (`offer()`, `isDone()`,
 * `render()`, `isStalled()`); `cancel()` and the counters from any task.
 * `open()` and `close()` while not attached.
 */
class RtStream final : public AudioStream {
        public:
                /** @brief The most frames a period has (the device's `MaxPeriodFrames`). */
                static constexpr uint32_t MaxFrames = 480;

                /** @brief A period for the client to make (`cycle()`). */
                struct Period {
                                float       *out = nullptr; ///< `frames` × `channels` floats, interleaved.
                                const float *in = nullptr;  ///< The input's (capture, later): null.
                                uint32_t     frames = 0;
                                uint8_t      channels = 0;
                                uint32_t     rateHz = 0;
                                uint64_t     position = 0;   ///< The device's frame count at `out[0]`.
                                int64_t      deadlineNs = 0; ///< `Clock::monotonicNs()` by which to end it.
                                int64_t      playNs = 0;     ///< When `out[0]` leaves the DMA.
                                uint32_t     skipped = 0;    ///< Periods skipped since the last call.
                                uint32_t     late = 0;       ///< Periods late in all.
                                uint32_t     throttled = 0;  ///< Times its thread was put back to its priority.
                };

                explicit RtStream(const char *label = "rt") : AudioStream(Kind::Realtime, label), _ready(1, 0) {}
                ~RtStream();

                /**
                 * @brief Allocates the output for @p channels (1 or 2) in @p space;
                 *        @p periodFrames is the period asked for (0: the device's).
                 * @return `Ok`, `Invalid`, or `NoMem`.
                 */
                Error open(uint8_t channels, uint32_t periodFrames, MemSpace space);

                /** @brief Frees the output. */
                void close();

                /** @brief True once `open()` has allocated its output. */
                bool isOpen() const { return _out != nullptr; }

                /** @brief Returns the channels. */
                uint8_t channels() const { return _channels; }

                /** @brief Returns the period asked for (0: the device's). */
                uint32_t periodFrames() const { return _periodFrames; }

                /** @brief Sets what `cycle()` releases when it ends a period: the engine's wake. */
                void setEngineWake(Semaphore *wake) { _engineWake = wake; }

                /**
                 * @brief Forgets the periods handed out and ended, for another attach
                 *        (the counters stay).
                 */
                void reset();

                /**
                 * @brief The client: ends the period it was given (if any), then, if
                 *        @p next isn't null, waits for the next and fills it in.
                 * @return `Ok`, `Interrupted` (@p interrupt raised while waiting),
                 *         `BrokenPipe` (over: cancelled, or dropped by its device's
                 *         stop), or `Busy` (too many waiters).
                 * @blocking When @p next isn't null.
                 */
                Error cycle(Period *next, Interrupt *interrupt);

                /**
                 * @brief The engine: hands out the next period, of @p frames at
                 *        @p position, to end by @p deadlineNs and heard from @p playNs.
                 * @hotpath
                 */
                void offer(uint64_t position, uint32_t frames, int64_t deadlineNs, int64_t playNs);

                /**
                 * @brief The engine: true once the period offered is ended, or there
                 *        is nothing to wait for (no client has cycled yet; over).
                 */
                bool isDone() const;

                /**
                 * @brief The engine: true if the client has been out of `cycle()`
                 *        for @p periods periods or more since it was last given one.
                 */
                bool isStalled(uint32_t periods) const;

                /** @brief The engine: its thread was put back to its own priority. */
                void noteThrottled() {
                        _throttled.fetch_add(1, std::memory_order_relaxed);
                        tally(Tally::ClientThrottled);
                        return;
                }

                /** @brief Returns the periods late in all. */
                uint32_t latePeriods() const { return _late.load(std::memory_order_relaxed); }

                /** @brief Returns the periods skipped in all. */
                uint32_t skippedPeriods() const { return _skipped.load(std::memory_order_relaxed); }

                /** @brief Returns the throttles in all. */
                uint32_t throttles() const { return _throttled.load(std::memory_order_relaxed); }

                /** @brief Returns the periods ended in time and mixed. */
                uint64_t donePeriods() const { return _done.load(std::memory_order_relaxed); }

                /** @brief Returns the frames ended in time and mixed. */
                uint64_t doneFrames() const { return _doneFrames.load(std::memory_order_relaxed); }

                /** @brief Where playing has got to: the last period mixed's end, and when. */
                PcmStream::Position position() const { return _position.load(); }

                void  cancel() override;
                Error prepare(uint32_t deviceRate, size_t maxFrames) override;
                bool  render(float *mix, size_t frames, float *scratch) override;
                void  describe(char *out, size_t size) const override;

        private:
                friend class AudioDevice;

                // What the engine offered last.
                struct Offer {
                                uint64_t number = 0; // from 1
                                uint64_t position = 0;
                                int64_t  deadlineNs = 0;
                                int64_t  playNs = 0;
                                uint32_t frames = 0;
                                uint32_t spare = 0; // no padding (SeqLocked)
                };

                float                         *_out = nullptr;
                MemSpace                       _space = MemSpace::Internal;
                uint8_t                        _channels = 0;
                uint32_t                       _periodFrames = 0;
                std::atomic<uint32_t>          _rateHz{0};
                Semaphore                      _ready; // the client waits on it for a period
                Semaphore                     *_engineWake = nullptr;
                SeqLocked<Offer>               _offer;
                std::atomic<uint64_t>          _offered{0}; // the period last offered
                std::atomic<uint64_t>          _given{0};   // the period the client was last given (0: none yet)
                std::atomic<uint64_t>          _ended{0};   // the period the client last ended
                std::atomic<bool>              _inCycle{false};
                std::atomic<bool>              _cancelled{false};
                std::atomic<uint32_t>          _late{0};
                std::atomic<uint32_t>          _skipped{0};
                std::atomic<uint32_t>          _throttled{0};
                std::atomic<uint64_t>          _done{0};
                std::atomic<uint64_t>          _doneFrames{0};
                SeqLocked<PcmStream::Position> _position;

                // Its thread's priority: the device's, changed under its real-time lock (the
                // client reads its own entry without it, once a period).
                std::atomic<void *> _task{nullptr};       // the thread raised for it (a TaskHandle_t)
                int                 _basePriority = 0;    // what to put it back to
                std::atomic<bool>   _throttledNow{false}; // put back by the engine: raise it again
};

BMDS_NAMESPACE_END
