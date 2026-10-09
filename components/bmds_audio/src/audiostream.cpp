/**
 * @file      audiostream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a device mixes: tones, system sounds, and PCM from a writer
 * (devplan/audio.md §17).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/audiostream.h>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The ring object sits at the start of a PCM stream's storage, its bytes after it.
        constexpr size_t RingHeader = (sizeof(ByteRing) + 15) & ~size_t(15);
        constexpr size_t MinRingBytes = 256;

        size_t powerOfTwoAtLeast(size_t n) {
                size_t p = 1;
                while (p < n) p <<= 1;
                return p;
        }

} // namespace

// ---- AudioStream

void AudioStream::setGain(float gain) {
        uint32_t bits;
        std::memcpy(&bits, &gain, sizeof(bits));
        _gain.store(bits, std::memory_order_relaxed);
        return;
}

float AudioStream::gain() const {
        const uint32_t bits = _gain.load(std::memory_order_relaxed);
        float          g;
        std::memcpy(&g, &bits, sizeof(g));
        return g;
}

bool AudioStream::isOver() const {
        const uint8_t s = _state.load(std::memory_order_acquire);
        return s == Idle || s == Over;
}

// ---- ToneStream

void ToneStream::setTone(uint32_t frequencyHz, uint32_t ms, int32_t centiDb, SineTone::Slots slots) {
        _isSound = false;
        _frequencyHz = frequencyHz;
        _ms = ms;
        _centiDb = centiDb;
        _slots = slots;
        _cancel.store(false, std::memory_order_relaxed);
        return;
}

void ToneStream::setSound(SystemSound sound) {
        _isSound = true;
        _sound = sound;
        _cancel.store(false, std::memory_order_relaxed);
        return;
}

Error ToneStream::prepare(uint32_t deviceRate, size_t) {
        if (_isSound) {
                _sequence.start(systemSoundNotes(_sound), deviceRate, SineTone::Both);
        } else {
                _sequence.startTone(_frequencyHz, _ms, _centiDb, deviceRate, _slots);
        }
        _cancelled = false;
        return Error::Ok;
}

bool ToneStream::render(float *mix, size_t frames, float *) {
        if (!_cancelled && _cancel.load(std::memory_order_acquire)) {
                _sequence.cancel();
                _cancelled = true;
        }
        return _sequence.fill(mix, frames);
}

void ToneStream::describe(char *out, size_t size) const {
        if (_isSound) {
                std::snprintf(out, size, "%s", systemSoundName(_sound));
                return;
        }
        const int32_t     cdb = _centiDb < 0 ? -_centiDb : _centiDb;
        const char *const slots = _slots == SineTone::Left ? "left" : (_slots == SineTone::Right ? "right" : "both");
        std::snprintf(out, size, "%lu Hz, %s%ld.%02ld dBFS, %s", static_cast<unsigned long>(_frequencyHz),
                      _centiDb < 0 ? "-" : "", static_cast<long>(cdb / 100), static_cast<long>(cdb % 100), slots);
        return;
}

// ---- PcmStream

PcmStream::~PcmStream() {
        close();
}

Error PcmStream::open(const Format &format, size_t ringFrames, size_t startFrames, MemSpace space) {
        if (static_cast<size_t>(format.format) >= SampleFormats::Count || format.channels < 1 || format.channels > 2 ||
            format.rateHz < MinRateHz || format.rateHz > MaxRateHz || ringFrames == 0)
                return Error::Invalid;
        close();
        _format = format;
        _frameBytes = SampleFormats::bytesPerSample(format.format) * format.channels;
        size_t bytes = powerOfTwoAtLeast(ringFrames * _frameBytes);
        if (bytes < MinRingBytes) bytes = MinRingBytes;
        _storage = static_cast<uint8_t *>(Memory::alloc(space, RingHeader + bytes));
        if (_storage == nullptr) return Error::NoMem;
        _ring = new (_storage) ByteRing(WritableByteSpan(_storage + RingHeader, bytes));
        const size_t capacity = bytes / _frameBytes;
        _startFrames = startFrames == 0 ? 1 : (startFrames > capacity ? capacity : startFrames);
        _space = space;
        _underruns.store(0, std::memory_order_relaxed);
        _written.store(0, std::memory_order_relaxed);
        _position.store(Position{});
        _paused.store(false, std::memory_order_relaxed);
        reset();
        return Error::Ok;
}

void PcmStream::reset() {
        if (_ring != nullptr) _ring->consume(_ring->available()); // not attached: nobody reads
        _starved = false;
        _flushLeft = 0;
        _started.store(false, std::memory_order_relaxed);
        _draining.store(false, std::memory_order_relaxed);
        _cancelled.store(false, std::memory_order_release);
        return;
}

void PcmStream::close() {
        _resampler.release();
        _resampling = false;
        if (_ring != nullptr) _ring->~ByteRing();
        _ring = nullptr;
        Memory::free(_storage);
        _storage = nullptr;
        return;
}

size_t PcmStream::queuedFrames() const {
        return _ring == nullptr || _frameBytes == 0 ? 0 : _ring->available() / _frameBytes;
}

size_t PcmStream::capacityFrames() const {
        return _ring == nullptr || _frameBytes == 0 ? 0 : _ring->capacity() / _frameBytes;
}

void PcmStream::setStartFrames(size_t frames) {
        const size_t capacity = capacityFrames();
        _startFrames = frames == 0 ? 1 : (frames > capacity ? capacity : frames);
        return;
}

void PcmStream::startNow() {
        _started.store(true, std::memory_order_release);
        return;
}

void PcmStream::setPaused(bool paused) {
        _paused.store(paused, std::memory_order_release);
        return;
}

Result<size_t> PcmStream::write(ByteSpan data, bool nonBlocking, Interrupt *interrupt) {
        if (_ring == nullptr || _cancelled.load(std::memory_order_acquire))
                return makeError<size_t>(Error::InvalidState);
        Semaphore       wake(1, 0);
        WaitList::Entry entry(waiters(), wake);
        const Deadline  forever(WaitForever);
        size_t          done = 0;
        for (;;) {
                const size_t pushed = _ring->push(data.mid(done));
                done += pushed;
                if (pushed > 0) _written.fetch_add(pushed, std::memory_order_relaxed);
                if (done == data.size()) break;
                // Full. Nobody empties a stream that isn't playing: take what fit.
                const bool attached = isPlaying();
                if (nonBlocking || !attached || _cancelled.load(std::memory_order_acquire)) {
                        if (done > 0) break;
                        return makeError<size_t>(nonBlocking && attached ? Error::WouldBlock : Error::InvalidState);
                }
                if (!entry.isAdded()) return makeError<size_t>(Error::Busy);
                const Error err = Interrupt::wait(interrupt, wake, forever);
                if (err.isError()) return Result<size_t>(done, err); // what was taken, and why no more
        }
        return makeResult(done);
}

Error PcmStream::drain(Interrupt *interrupt, unsigned int timeoutMs) {
        Semaphore       wake(1, 0);
        WaitList::Entry entry(waiters(), wake);
        if (!entry.isAdded()) return Error::Busy;
        _draining.store(true, std::memory_order_release);
        const Deadline deadline(timeoutMs);
        while (!isOver()) {
                const Error err = Interrupt::wait(interrupt, wake, deadline);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

void PcmStream::cancel() {
        _cancelled.store(true, std::memory_order_release);
        waiters().notify(); // a writer waiting for room
        return;
}

Error PcmStream::prepare(uint32_t deviceRate, size_t maxFrames) {
        _resampling = _format.rateHz != deviceRate;
        if (!_resampling) {
                _resampler.release();
                _flushLeft = 0;
                return Error::Ok;
        }
        const Error err = _resampler.configure(_format.rateHz, deviceRate, maxFrames, _space);
        if (err.isError()) return err;
        _flushLeft = _resampler.delayFrames(); // its last input comes out after this much more
        return Error::Ok;
}

size_t PcmStream::convert(float *stereo, size_t frames) {
        size_t done = 0;
        while (done < frames) {
                const ByteSpan run = _ring->readable();
                size_t         whole = run.size() / _frameBytes;
                if (whole == 0) {
                        // A frame across the ring's end, or nothing.
                        if (_ring->available() < _frameBytes) break;
                        uint8_t frame[8];
                        (void)_ring->pop(WritableByteSpan(frame, _frameBytes));
                        SampleFormats::toFloat(_format.format, _format.channels, frame, 1, stereo + 2 * done);
                        ++done;
                        continue;
                }
                if (whole > frames - done) whole = frames - done;
                SampleFormats::toFloat(_format.format, _format.channels, run.data(), whole, stereo + 2 * done);
                _ring->consume(whole * _frameBytes);
                done += whole;
        }
        return done;
}

bool PcmStream::render(float *mix, size_t frames, float *scratch) {
        if (_ring == nullptr || _cancelled.load(std::memory_order_acquire)) return false;
        const bool draining = _draining.load(std::memory_order_acquire);
        if (!_started.load(std::memory_order_acquire)) {
                const size_t queued = queuedFrames();
                if (queued == 0 && draining) return false; // nothing was ever enough to start
                if (queued < _startFrames && !draining) return true;
                _started.store(true, std::memory_order_release);
        }
        if (_paused.load(std::memory_order_acquire) && !draining) return true;
        const float g = gain();
        size_t      taken = 0;
        size_t      made = 0;
        if (!_resampling) {
                taken = convert(scratch, frames);
                for (size_t i = 0; i < 2 * taken; ++i) mix[i] += g * scratch[i];
                made = taken;
        } else {
                const size_t need = _resampler.inputNeeded(frames);
                float       *in = _resampler.inputSpace(need);
                if (in != nullptr) {
                        taken = convert(in, need);
                        size_t fed = taken;
                        // Draining and dry: silence to push the filter's last outputs through.
                        if (taken < need && draining && _flushLeft > 0) {
                                size_t zeros = need - taken;
                                if (zeros > _flushLeft) zeros = _flushLeft;
                                std::memset(in + 2 * taken, 0, zeros * 2 * sizeof(float));
                                _flushLeft -= zeros;
                                fed += zeros;
                        }
                        _resampler.commit(fed);
                }
                made = _resampler.process(mix, frames, g);
        }
        if (taken > 0) {
                const int64_t now = Clock::monotonicNs();
                _position.update([taken, now](Position &p) {
                        p.frames += taken;
                        p.ns = now;
                });
                waiters().notify(); // room for a writer
        }
        if (made < frames) {
                if (draining && _flushLeft == 0) return false; // played out
                if (!draining && !_starved) _underruns.fetch_add(1, std::memory_order_relaxed);
                _starved = true;
        } else {
                _starved = false;
        }
        return true;
}

void PcmStream::describe(char *out, size_t size) const {
        const size_t capacity = _ring == nullptr ? 0 : _ring->capacity() / _frameBytes;
        std::snprintf(out, size, "%s, %u ch, %lu Hz%s, %u of %u frames queued, %lu underruns",
                      SampleFormats::name(_format.format), static_cast<unsigned>(_format.channels),
                      static_cast<unsigned long>(_format.rateHz), _resampling ? " (resampled)" : "",
                      static_cast<unsigned>(queuedFrames()), static_cast<unsigned>(capacity),
                      static_cast<unsigned long>(underruns()));
        return;
}

BMDS_NAMESPACE_END
