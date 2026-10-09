/**
 * @file      audiofile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /dev/dsp, /dev/audio, /dev/mixer: a program's stream on an audio device
 * (devplan/audio.md §18).
 */

#include <new>
#include <bmds/audiofile.h>
#include <bmds/deadline.h>
#include <bmds/filehandle.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t DrainMarginMs = 1000; // a closing drain's slack beyond the ring's length

        // A file in /dev: its device (-1: the one audio/default names) and kind.
        struct FileArg {
                        const char     *name;
                        int8_t          device;
                        AudioFile::Kind kind;
        };

        static_assert(AudioDevice::MaxDevices == 1, "the names below are device 0's");
        constexpr FileArg Device0Files[] = {{"dsp0", 0, AudioFile::Kind::Dsp},
                                            {"mixer0", 0, AudioFile::Kind::Mixer},
                                            {"dsp", -1, AudioFile::Kind::Dsp},
                                            {"audio", -1, AudioFile::Kind::Audio},
                                            {"mixer", -1, AudioFile::Kind::Mixer}};

        uint32_t framesAt(uint64_t frames, uint32_t fromRate, uint32_t toRate) {
                return fromRate == 0 ? 0 : static_cast<uint32_t>((frames * toRate + fromRate - 1) / fromRate);
        }

} // namespace

// ---- Opening

Error AudioFile::addFiles(AudioDevice &device) {
        if (device.number() != 0) return Error::NotSupported;
        for (const FileArg &a : Device0Files) {
                Vfs::DeviceFile file;
                file.name = a.name;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        const auto *fa = static_cast<const FileArg *>(arg);
                        auto [d, err] = fa->device < 0 ? AudioDevice::defaultDevice()
                                                       : AudioDevice::device(static_cast<size_t>(fa->device));
                        if (err.isError()) return makeError<FileHandle::Ptr>(Error::NotExist);
                        return open(*d, fa->kind, flags);
                };
                file.arg = const_cast<FileArg *>(&a);
                const Error err = Vfs::addDeviceFile(file);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

Result<FileHandle::Ptr> AudioFile::open(AudioDevice &device, Kind kind, uint8_t flags) {
        uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
        if (kind != Kind::Mixer) {
                // Capture is step 8: a stream is written.
                if ((flags & Vfs::Read) != 0 || (flags & Vfs::Write) == 0)
                        return makeError<FileHandle::Ptr>(Error::NotSupported);
                const Error err = device.addClient();
                if (err.isError()) return makeError<FileHandle::Ptr>(err);
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(AudioFile));
        if (mem == nullptr) {
                if (kind != Kind::Mixer) device.removeClient();
                return makeError<FileHandle::Ptr>(Error::NoMem);
        }
        auto *file = ::new (mem) AudioFile(device, kind);
        Error err = Error::Ok;
        if (kind != Kind::Mixer) {
                Mutex::Locker locker(file->_lock, WaitForever);
                err = file->openLocked();
        }
        Result<FileHandle::Ptr> handle = err.isOk() ? FileHandle::open(*file, mode) : makeError<FileHandle::Ptr>(err);
        if (handle.error().isError()) file->handleClosed(0); // frees it
        return handle;
}

AudioFile::AudioFile(AudioDevice &device, Kind kind) : _device(device), _kind(kind), _pcm("dsp"), _rt("rt") {
        if (kind == Kind::Audio) _config.format = SampleFormat::MuLaw;
}

void AudioFile::handleClosed(uint8_t) {
        if (_kind != Kind::Mixer) {
                {
                        Mutex::Locker locker(_lock, WaitForever);
                        // What is queued plays out; a signal (a killed program) or a pause drops it.
                        if (!isRealtime() && !_pcm.isOver() &&
                            (_pcm.isPaused() || drainLocked(drainTimeoutMs()).isError()))
                                _pcm.cancel();
                        closeLocked();
                }
                _device.removeClient();
        }
        this->~AudioFile();
        Memory::free(this);
        return;
}

// ---- Configuring

Error AudioFile::checkLocked(const Config &c) const {
        if ((c.flags & ~(Realtime | Exclusive)) != 0 || c.flags == Exclusive) return Error::Invalid;
        if ((c.flags & Realtime) != 0) {
                // Float at the device's rate (or, exclusive, any), no ring, a period it can run.
                const bool exclusive = (c.flags & Exclusive) != 0;
                if (c.format != SampleFormat::F32Le || c.channels < 1 || c.channels > 2 || c.ringFrames != 0 ||
                    c.startFrames != 0 ||
                    (c.wakeFrames != 0 && (c.wakeFrames < AudioDevice::MinClientPeriodFrames ||
                                           c.wakeFrames > AudioDevice::MaxPeriodFrames)))
                        return Error::Invalid;
                if (exclusive && c.rateHz != 0 && (c.rateHz < PcmStream::MinRateHz || c.rateHz > PcmStream::MaxRateHz))
                        return Error::Invalid;
                if (!exclusive && c.rateHz != 0 && c.rateHz != _device.timing().rateHz) return Error::Invalid;
                return Error::Ok;
        }
        if (static_cast<size_t>(c.format) >= SampleFormats::Count || c.channels < 1 || c.channels > 2 ||
            c.rateHz < PcmStream::MinRateHz || c.rateHz > PcmStream::MaxRateHz ||
            c.ringFrames > c.rateHz * MaxRingSeconds)
                return Error::Invalid;
        return Error::Ok;
}

// Allocates the ring for _config (closed: not attached), or for a real-time client its
// output, attached at once.
Error AudioFile::openLocked() {
        if (isRealtime()) return openRealtimeLocked();
        const uint32_t          ring = _config.ringFrames != 0 ? _config.ringFrames : _config.rateHz / 10;
        const PcmStream::Format format{_config.format, _config.channels, _config.rateHz};
        Error                   err = _pcm.open(format, ring, 0, MemSpace::Psram);
        if (err.isError()) return err;
        const size_t capacity = _pcm.capacityFrames();
        _pcm.setStartFrames(_config.startFrames != 0 ? _config.startFrames : capacity / 2);
        _wakeFrames = wakeFramesLocked();
        _pcm.setGain(AudioMix::gainOf(_gainCentiDb));
        return Error::Ok;
}

Error AudioFile::openRealtimeLocked() {
        Error err = _rt.open(_config.channels, _config.wakeFrames, MemSpace::Internal);
        if (err.isError()) return err;
        _rt.setGain(AudioMix::gainOf(_gainCentiDb));
        err = _device.addRealtime(_rt);
        if (err.isOk()) {
                const bool exclusive = (_config.flags & Exclusive) != 0;
                err = _device.attachRealtime(_rt, exclusive, exclusive ? _config.rateHz : 0);
                if (err.isError()) _device.removeRealtime(_rt);
        }
        if (err.isError()) {
                _rt.close();
                return err;
        }
        _realtimeOn.store(true, std::memory_order_release);
        return Error::Ok;
}

// Takes the stream out and frees it (_config still says which).
void AudioFile::closeLocked() {
        if (isRealtime()) {
                _realtimeOn.store(false, std::memory_order_release);
                _rt.cancel(); // a thread waiting in cycle() returns
                _device.detachRealtime(_rt);
                _device.removeRealtime(_rt);
                _rt.close();
                return;
        }
        _device.detach(_pcm);
        _pcm.close();
        return;
}

uint32_t AudioFile::wakeFramesLocked() {
        uint32_t wake = _config.wakeFrames;
        if (wake == 0) {
                const AudioDevice::Timing t = _device.timing();
                wake = framesAt(t.periodFrames, t.rateHz, _config.rateHz);
        }
        const auto capacity = static_cast<uint32_t>(_pcm.capacityFrames());
        if (wake > capacity) wake = capacity;
        return wake == 0 ? 1 : wake;
}

AudioFile::Config AudioFile::config() {
        Mutex::Locker locker(_lock, WaitForever);
        Config        c = _config;
        if (isRealtime()) {
                const AudioDevice::Timing t = _device.timing();
                c.rateHz = t.rateHz;
                c.wakeFrames = t.periodFrames;
                return c;
        }
        if (_pcm.isOpen()) {
                c.ringFrames = static_cast<uint32_t>(_pcm.capacityFrames());
                c.startFrames = static_cast<uint32_t>(_pcm.startFrames());
                c.wakeFrames = _wakeFrames;
        }
        return c;
}

Error AudioFile::configure(const Config &config) {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        Mutex::Locker locker(_lock, WaitForever);
        Error         err = checkLocked(config);
        if (err.isError()) return err;
        if (!isRealtime() && !_pcm.isOver()) {
                err = drainLocked(WaitForever);
                if (err.isError()) return err;
        }
        closeLocked();
        const Config old = _config;
        _config = config;
        err = openLocked();
        if (err.isError()) {
                // As it was (exclusive mode refused, say).
                _config = old;
                (void)openLocked();
        }
        return err;
}

// ---- Playing

// A stream that is over (drained, dropped, or its device stopped) is taken back, emptied.
void AudioFile::settleLocked() {
        if (!_pcm.isOver()) return;
        _device.detach(_pcm);
        _pcm.reset();
        return;
}

Error AudioFile::attachLocked() {
        if (!_pcm.isOpen()) return Error::InvalidState;
        if (!_pcm.isOver()) return Error::Ok;
        settleLocked();
        return _device.attach(_pcm);
}

Result<size_t> AudioFile::read(WritableByteSpan, unsigned int) {
        return makeError<size_t>(Error::NotSupported);
}

Result<size_t> AudioFile::write(ByteSpan data, unsigned int timeoutMs) {
        if (_kind == Kind::Mixer) return makeError<size_t>(Error::NotSupported);
        if (_realtimeOn.load(std::memory_order_acquire)) return makeError<size_t>(Error::Invalid);
        Semaphore       wake(1, 0);
        WaitList::Entry entry(_pcm.waiters(), wake);
        if (!entry.isAdded()) return makeError<size_t>(Error::Busy);
        const Deadline deadline(timeoutMs);
        size_t         done = 0;
        while (done < data.size()) {
                {
                        // Not held while waiting for room: status and drop() go on meanwhile.
                        Mutex::Locker locker(_lock, WaitForever);
                        Error         err = attachLocked();
                        if (err.isOk()) {
                                const Result<size_t> r = _pcm.write(data.mid(done), true, nullptr);
                                if (r.error().isOk()) done += r.value();
                                err = r.error() == Error::WouldBlock ? Error::Ok : r.error();
                        }
                        // Part written, then an error: the count with it (Stream's contract).
                        if (err.isError()) return Result<size_t>(done, err);
                }
                if (done == data.size()) break;
                const Error err = Interrupt::wait(Interrupt::current(), wake, deadline);
                if (err.isError()) return Result<size_t>(done, err);
        }
        return makeResult(done);
}

uint8_t AudioFile::readiness() {
        if (_kind == Kind::Mixer) return Readable | Writable;
        Mutex::Locker locker(_lock, WaitForever);
        if (isRealtime()) return Writable; // a write fails at once (not written: cycled)
        const size_t  free = _pcm.capacityFrames() - _pcm.queuedFrames();
        return free >= _wakeFrames ? Writable : 0;
}

WaitList *AudioFile::readinessWaiters() {
        return _kind == Kind::Mixer ? nullptr : &_pcm.waiters();
}

Error AudioFile::start() {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (_realtimeOn.load(std::memory_order_acquire)) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        if (!_pcm.isOver()) _pcm.startNow();
        return Error::Ok;
}

Error AudioFile::setPaused(bool paused) {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (_realtimeOn.load(std::memory_order_acquire)) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        _pcm.setPaused(paused);
        return Error::Ok;
}

uint32_t AudioFile::drainTimeoutMs() const {
        const uint64_t ms = static_cast<uint64_t>(_pcm.capacityFrames()) * 1000 / _config.rateHz;
        return static_cast<uint32_t>(ms) + DrainMarginMs;
}

Error AudioFile::drainLocked(unsigned int timeoutMs) {
        if (_pcm.isOver()) return Error::Ok;
        _pcm.setPaused(false); // a drain plays
        return _pcm.drain(Interrupt::current(), timeoutMs);
}

Error AudioFile::drain() {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (_realtimeOn.load(std::memory_order_acquire)) return Error::Invalid;
        // Not under the lock: the drain doesn't touch the ring, and status goes on meanwhile.
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_pcm.isOver()) return Error::Ok;
                _pcm.setPaused(false);
        }
        return _pcm.drain(Interrupt::current(), WaitForever);
}

Error AudioFile::drop() {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (_realtimeOn.load(std::memory_order_acquire)) return Error::Invalid;
        _pcm.cancel(); // wakes a writer waiting for room, which then takes the lock
        Mutex::Locker locker(_lock, WaitForever);
        _device.detach(_pcm);
        _pcm.reset();
        return Error::Ok;
}

Error AudioFile::setGain(int32_t centiDb) {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (centiDb < MutedCentiDb || centiDb > 0) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        _gainCentiDb = centiDb;
        _pcm.setGain(AudioMix::gainOf(centiDb));
        _rt.setGain(AudioMix::gainOf(centiDb));
        return Error::Ok;
}

// ---- Status

AudioFile::Status AudioFile::status() {
        const AudioDevice::Status d = _device.status();
        const AudioDevice::Timing t = _device.timing();
        Status                    s;
        s.device = _device.number();
        s.deviceRate = t.rateHz;
        s.periodFrames = t.periodFrames;
        s.latePeriods = d.late;
        s.limitedFrames = d.limited;
        if (_kind == Kind::Mixer) {
                s.state = State::Mixer;
                return s;
        }
        Mutex::Locker locker(_lock, WaitForever);
        if (isRealtime()) {
                // No ring: what was mixed, and the client's own late periods.
                const PcmStream::Position p = _rt.position();
                s.state = _rt.isOver() ? State::Idle : State::Playing;
                s.writtenFrames = _rt.doneFrames();
                s.playedFrames = p.frames;
                s.timestampNs = p.ns;
                s.delayFrames = (AudioDevice::DmaBuffers - 1) * t.periodFrames;
                s.underruns = _rt.latePeriods();
                s.latePeriods = _rt.latePeriods();
                s.gainCentiDb = _gainCentiDb;
                return s;
        }
        const bool    attached = !_pcm.isOver();
        if (!attached) {
                s.state = State::Idle;
        } else if (_pcm.isPaused()) {
                s.state = State::Paused;
        } else if (_pcm.isDraining()) {
                s.state = State::Draining;
        } else if (!_pcm.hasStarted()) {
                s.state = State::Waiting;
        } else {
                s.state = State::Playing;
        }
        const PcmStream::Position p = _pcm.position();
        s.ringFrames = static_cast<uint32_t>(_pcm.capacityFrames());
        s.writtenFrames = _pcm.writtenFrames();
        s.playedFrames = p.frames;
        s.timestampNs = p.ns;
        s.queuedFrames = static_cast<uint32_t>(_pcm.queuedFrames());
        s.delayFrames = s.queuedFrames;
        if (attached && _pcm.hasStarted())
                s.delayFrames +=
                        framesAt((AudioDevice::DmaBuffers - 1) * uint64_t(t.periodFrames), t.rateHz, _config.rateHz) +
                        static_cast<uint32_t>(_pcm.resamplerDelayFrames());
        s.underruns = _pcm.underruns();
        s.gainCentiDb = _gainCentiDb;
        return s;
}

// ---- Real-time clients

Error AudioFile::cycle(RtStream::Period *next) {
        if (_kind == Kind::Mixer) return Error::NotSupported;
        if (!_realtimeOn.load(std::memory_order_acquire)) return Error::Invalid;
        // Dropped by its device (a stop, a fault): joins again.
        if (next != nullptr && !_rt.isPlaying()) {
                Mutex::Locker locker(_lock, WaitForever);
                if (!isRealtime()) return Error::Invalid;
                if (!_rt.isPlaying()) {
                        const bool exclusive = (_config.flags & Exclusive) != 0;
                        _device.detachRealtime(_rt);
                        _rt.reset();
                        const Error err = _device.attachRealtime(_rt, exclusive, exclusive ? _config.rateHz : 0);
                        if (err.isError()) return err;
                }
        }
        if (next != nullptr) _device.raiseCaller(_rt);
        return _rt.cycle(next, Interrupt::current());
}

BMDS_NAMESPACE_END
