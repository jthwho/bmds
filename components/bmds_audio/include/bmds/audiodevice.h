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
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/sinetone.h>
#include <bmds/task.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An audio output: an I²S port, the codec on it, and the task that
 *        fills its DMA buffers (`devplan/audio.md` §5, §16). Numbered: `dsp0`
 *        is the board's codec.
 *
 * Step 1 of the plan: the task fills buffers from a test tone or with
 * silence. The I²S channel is made at the first start and kept (only
 * disabled at a stop), so the task never touches freed memory. Each DMA
 * buffer is one period; the task fills the buffer the DMA just finished,
 * which plays `DmaBuffers - 1` periods later, and counts a period late if
 * the DMA reached it first.
 *
 * @par Thread Safety
 * Thread-safe: the public calls serialize on the device's lock.
 */
class AudioDevice {
        public:
                /** @brief Devices a board can have. */
                static constexpr size_t MaxDevices = 1;

                /** @brief DMA buffers (periods in flight). */
                static constexpr size_t DmaBuffers = 3;

                /** @brief Frames a period (devplan/audio.md §5: 5 ms at 48 kHz). */
                static constexpr uint32_t PeriodFrames = 240;

                /** @brief The rate a start without one uses. */
                static constexpr uint32_t DefaultRateHz = 48000;

                /** @brief What the device is doing. */
                enum class State : uint8_t {
                        Stopped, ///< Not clocked; the codec muted, its converters stopped.
                        Running, ///< Clocked and playing (silence, or the tone).
                        Faulted, ///< A start or a control failed: see `lastError()`.
                };

                /** @brief What `audio status` shows (devplan/audio.md §3). */
                struct Status {
                                State    state = State::Stopped;
                                uint32_t rateHz = 0;          ///< The rate running (or last run).
                                uint32_t mclkAskedHz = 0;     ///< MCLK asked for (256 × rate).
                                uint32_t mclkHz = 0;          ///< MCLK as the I²S driver set it.
                                uint32_t apllHz = 0;          ///< The APLL's frequency.
                                uint32_t measuredMilliHz = 0; ///< Frames per second as counted (0: not yet).
                                uint32_t measuredMs = 0;      ///< Over this long.
                                uint32_t periods = 0;         ///< Periods filled since the start.
                                uint32_t late = 0;            ///< Of those, filled after the DMA reached them.
                                uint32_t fillUsLast = 0;      ///< The last fill's time.
                                uint32_t fillUsMax = 0;       ///< The longest fill since the start.
                                bool     toneActive = false;  ///< A tone is playing.
                };

                /**
                 * @brief Boot stage: makes the board's devices (no hardware is touched
                 *        until a device first starts).
                 * @return `Ok` (also on a board without audio), or `NotExist` (the
                 *         codec's I²C bus is missing).
                 */
                static Error init();

                /**
                 * @brief Returns device @p number (`dspN`).
                 * @return The device, or `NotSupported` (no such device on this board).
                 */
                static Result<AudioDevice *> device(size_t number);

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
                 * @brief Clocks the port at @p rateHz and starts the codec. A running
                 *        device at another rate is restarted.
                 * @return `Ok`, `NotSupported` (a rate no setting fits), `NoMem`, or
                 *         the I²S driver's or codec's error (the device is then
                 *         `Faulted`).
                 * @blocking
                 */
                Error start(uint32_t rateHz);

                /**
                 * @brief Fades out any tone, mutes and stops the codec, and stops the
                 *        port. Idempotent.
                 * @blocking
                 */
                Error stop();

                /**
                 * @brief Plays a tone of @p frequencyHz for @p durationMs at
                 *        @p levelCentiDb in @p slots, starting the device (at its last
                 *        rate, or the default) if it is stopped, and stopping it
                 *        after if so. A tone already playing is replaced.
                 * @return The tone's number, for `waitTone()` and `cancelTone()`;
                 *         `Invalid` (a frequency of half the rate or more), or
                 *         `start()`'s error.
                 * @nonblocking Returns once the tone has begun.
                 */
                Result<uint32_t> playTone(uint32_t frequencyHz, uint32_t durationMs, int32_t levelCentiDb,
                                          SineTone::Slots slots);

                /**
                 * @brief Waits until tone @p id is over: played to the end of its fade,
                 *        replaced, or stopped.
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

                /** @brief Returns the status. */
                Status status();

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

                static void taskEntry(void *arg);
                void        run();
                void        fill(const Finished &f);
                void        stopAfterTone();
                bool        isToneOver(uint32_t id) const;
                Error       makeChannel();
                Error       startLocked(uint32_t rateHz);
                Error       stopLocked();

                AudioCodec &_codec;
                Mutex       _lock;
                Semaphore   _wake;
                Task        _task;
                void       *_channel = nullptr; // i2s_chan_handle_t
                uint8_t     _number;
                State       _state = State::Stopped;
                Error       _lastError = Error::Ok;
                uint32_t    _rateHz = 0;
                uint32_t    _mclkHz = 0;
                uint32_t    _apllHz = 0;

                // The interrupt's: a ring of finished buffers and the EOF count.
                Finished              _ring[DmaBuffers * 2] = {};
                std::atomic<uint32_t> _head{0};
                std::atomic<uint32_t> _sequence{0};

                // The task's.
                uint32_t              _tail = 0;
                std::atomic<bool>     _active{false}; // fill (else leave the cleared silence)
                uint32_t              _firstSequence = 0;
                int64_t               _firstUs = 0;
                bool                  _haveFirst = false;
                std::atomic<uint32_t> _measuredMilliHz{0};
                std::atomic<uint32_t> _measuredMs{0};
                std::atomic<uint32_t> _periods{0};
                std::atomic<uint32_t> _late{0};
                std::atomic<uint32_t> _fillUsLast{0};
                std::atomic<uint32_t> _fillUsMax{0};

                // The tone: set under the lock, the task reads the request.
                SineTone              _tone;
                std::atomic<uint32_t> _toneRequest{0}; // bumped by playTone()
                uint32_t              _toneSeen = 0;
                uint32_t              _toneFrequency = 0;
                int32_t               _toneLevel = 0;
                SineTone::Slots       _toneSlots = SineTone::Both;
                uint32_t              _toneDurationMs = 0;
                uint32_t              _toneFramesLeft = 0;
                bool                  _draining = false; // faded: waiting for the DMA to send it
                uint32_t              _drainAt = 0;      // the EOF count by which it has been sent
                std::atomic<bool>     _toneActive{false};
                std::atomic<bool>     _toneCancel{false}; // fade out now (a stop)
                std::atomic<bool>     _toneStops{false};  // stop the device when the tone ends
                Semaphore             _toneDone;          // released when a tone has faded out
                WaitListOf<4>         _toneWaiters;       // waitTone()'s
};

BMDS_NAMESPACE_END
