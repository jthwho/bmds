/**
 * @file      player.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The media player's playback thread (devplan/mediaplayer.md §6b).
 */

#include "player.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <sdkconfig.h>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midievent.h>
#include <bmds/midifile.h>
#include <bmds/sampleformat.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView DevicePath("/dev/dsp");
        constexpr StringView MidiPath("/dev/midi");   // the system port (devplan/mediaplayer.md §12)
        constexpr int64_t    MidiLeadNs = 50'000'000; // a MIDI file's first message this far ahead
        constexpr unsigned   MidiLongestWaitMs = 500; // the thread looks again at least this often

        // A gain (centi-dB) as a scale of the channels' volumes (Q15): its amplitude's cube root, as
        // the synths cube controller 7 (devplan/audio.md §26).
        int32_t volumeScale(int32_t centiDb) {
                if (centiDb <= AudioFile::MutedCentiDb) return 0;
                if (centiDb >= 0) return 32768;
                return int32_t(std::lround(32768.0f * std::exp2f(float(centiDb) / 6000.0f * 3.3219281f)));
        }
        constexpr size_t     BufferBytes = 16384; // a period of 96 kHz stereo float

        // Copies @p from into @p to (MaxTag + 1 bytes), cut on a character.
        void copyTag(StringView from, char *to) {
                size_t n = from.size() < Player::MaxTag ? from.size() : Player::MaxTag;
                if (n < from.size())
                        while (n > 0 && (static_cast<uint8_t>(from[n]) & 0xc0) == 0x80) --n;
                std::memcpy(to, from.data(), n);
                to[n] = '\0';
                return;
        }

} // namespace

Error Player::start(Notify notify, void *arg, StringView cwd) {
        if (_started) return Error::InvalidState;
        _notify = notify;
        _notifyArg = arg;
        if (_cwd.assign(cwd).isError()) return Error::NoMem;
        _reports = static_cast<Report *>(Memory::alloc(MemSpace::Psram, MaxReports * sizeof(Report)));
        _buffer = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, BufferBytes));
        if (_reports == nullptr || _buffer == nullptr) {
                Memory::free(_reports);
                Memory::free(_buffer);
                _reports = nullptr;
                _buffer = nullptr;
                return Error::NoMem;
        }
        for (size_t i = 0; i < MaxReports; ++i) ::new (&_reports[i]) Report();
        _bufferBytes = BufferBytes;
        (void)_systemWatcher.start(StringView("synth/system"), systemChanged, this); // a MIDI file follows it
        Thread::Config config;
        config.name = "mplayer";
        config.stackBytes = 8192;
        config.priority = CONFIG_BMDS_SHELL_PRIORITY + 1; // above the loop: the window never starves the sound
        const Error err = _thread.start(config, entry, this);
        if (err.isError()) {
                Memory::free(_reports);
                Memory::free(_buffer);
                _reports = nullptr;
                _buffer = nullptr;
                return err;
        }
        _started = true;
        return Error::Ok;
}

void Player::quit() {
        if (!_started) return;
        Command c;
        c.kind = Kind::Quit;
        while (send(std::move(c), true) == Error::Full) Thread::sleep(PeriodMs); // never: Quit replaces
        _thread.join();
        _systemWatcher.stop();
        Memory::free(_reports); // trivially destructible
        Memory::free(_buffer);
        _reports = nullptr;
        _buffer = nullptr;
        _started = false;
        return;
}

// ---- The loop's side

Error Player::send(Command &&command, bool replaces) {
        {
                Mutex::Locker lock(_commandLock, WaitForever);
                if (replaces) _commandCount = 0; // superseded, not taken yet
                if (_commandCount == MaxCommands) return Error::Full;
                _commands[(_commandHead + _commandCount) % MaxCommands] = std::move(command);
                ++_commandCount;
        }
        if (replaces) {
                // What is queued goes now, and a write waiting for room or a drain wakes.
                Mutex::Locker lock(_deviceLock, WaitForever);
                if (_audio != nullptr) (void)_audio->drop();
        }
        _wake.release();
        return Error::Ok;
}

Error Player::play(uint32_t track, StringView path, uint64_t startMs, Start how) {
        Command c;
        c.kind = Kind::Play;
        c.how = how;
        c.track = track;
        c.startMs = startMs;
        if (c.path.assign(path).isError()) return Error::NoMem;
        return send(std::move(c), how == Start::Now);
}

Error Player::pause(bool paused) {
        Command c;
        c.kind = paused ? Kind::Pause : Kind::Resume;
        return send(std::move(c), false);
}

Error Player::stop(uint32_t track) {
        Command c;
        c.kind = Kind::Stop;
        c.track = track;
        return send(std::move(c), true);
}

Error Player::finish(uint32_t track) {
        Command c;
        c.kind = Kind::Finish;
        c.track = track;
        return send(std::move(c), false);
}

void Player::setGain(int32_t centiDb) {
        _gain.store(centiDb, std::memory_order_relaxed);
        if (_midiOn.load(std::memory_order_acquire)) _wake.release(); // the volumes sent again
        Mutex::Locker lock(_deviceLock, WaitForever);
        if (_audio != nullptr) (void)_audio->setGain(centiDb);
        return;
}

AudioFile::Status Player::status() {
        if (_midiOn.load(std::memory_order_acquire)) {
                // A MIDI file: the player's own clock, as a stream's played frames.
                AudioFile::Status s;
                const uint64_t    ms = midiMs();
                s.state = _heldMs.load(std::memory_order_acquire) >= 0 ? AudioFile::State::Paused
                                                                       : AudioFile::State::Playing;
                s.playedFrames = ms * MidiRateHz / 1000;
                s.writtenFrames = s.playedFrames;
                s.timestampNs = Clock::monotonicNs();
                s.gainCentiDb = _gain.load(std::memory_order_relaxed);
                return s;
        }
        Mutex::Locker lock(_deviceLock, WaitForever);
        return _audio != nullptr ? _audio->status() : AudioFile::Status();
}

bool Player::take(Report &out) {
        Mutex::Locker lock(_reportLock, WaitForever);
        if (_reportCount == 0) return false;
        out = _reports[_reportHead];
        _reportHead = (_reportHead + 1) % MaxReports;
        --_reportCount;
        return true;
}

// ---- The playback thread

void Player::entry(void *arg) {
        static_cast<Player *>(arg)->run();
        return;
}

void Player::run() {
        while (!_quit) {
                bool have = false;
                {
                        Mutex::Locker lock(_commandLock, WaitForever);
                        if (_commandCount > 0) {
                                _taken = std::move(_commands[_commandHead]);
                                _commandHead = (_commandHead + 1) % MaxCommands;
                                --_commandCount;
                                have = true;
                        }
                }
                if (_systemChanged.exchange(false) && _midiHandle) {
                        // Another system synth: the state at this time to it (as a seek), and the
                        // window told which synth plays.
                        SysConf::Id id;
                        while (_systemWatcher.next(id)) {}
                        (void)_systemWatcher.overflowed();
                        if (_state == State::Decoding) {
                                Midi::applySettings(); // the route moved first, so the state reaches the new one
                                seekMidi(midiMs());
                                reportStarted(true);
                        }
                }
                if (have) {
                        handle(_taken);
                } else if (_state == State::Decoding && !_paused && _midiHandle) {
                        const unsigned waitMs = sendSome();
                        if (waitMs > 0) (void)_wake.acquire(waitMs); // a command wakes it sooner
                } else if (_state == State::Decoding && !_paused) {
                        decodeSome();
                } else {
                        (void)_wake.acquire(WaitForever);
                }
        }
        dropQueued();
        closeDecoder();
        closeDevice();
        return;
}

void Player::handle(Command &command) {
        switch (command.kind) {
                case Kind::Play:
                        if (command.how == Start::Now) dropQueued();
                        if (command.how == Start::Now && command.track == _track &&
                            (_decoder != nullptr || _midiHandle)) {
                                seekTrack(command);
                        } else {
                                startTrack(command);
                        }
                        break;
                case Kind::Pause:
                case Kind::Resume:
                        _paused = command.kind == Kind::Pause;
                        if (_midiHandle) pauseMidi(_paused);
                        if (_audio != nullptr) (void)_audio->setPaused(_paused);
                        break;
                case Kind::Stop:
                        dropQueued();
                        closeDecoder();
                        closeDevice();
                        _paused = false;
                        reportStopped(command.track);
                        break;
                case Kind::Finish:
                        closeDecoder();
                        drainAndClose();
                        _paused = false;
                        reportStopped(command.track);
                        break;
                case Kind::Quit: _quit = true; break;
        }
        return;
}

Error Player::readAt(void *arg, uint64_t offset, WritableByteSpan buf) {
        FileHandle &file = *static_cast<Player *>(arg)->_file;
        auto [at, serr] = file.stream().seek(static_cast<int64_t>(offset), Stream::SeekFrom::Start);
        if (serr.isError()) return serr;
        size_t done = 0;
        while (done < buf.size()) {
                auto [n, err] = file.read(buf.mid(done), WaitForever);
                if (err == Error::EndOfFile || (err.isOk() && n == 0)) return Error::EndOfFile;
                if (err.isError()) return err;
                done += n;
        }
        return Error::Ok;
}

void Player::startTrack(const Command &command) {
        closeDecoder();
        _track = command.track;
        auto [file, oerr] = Vfs::open(command.path.view(), Vfs::Read, Vfs::Context{_cwd.view(), nullptr});
        Error err = oerr;
        bool  isMidi = false;
        if (err.isOk()) {
                _file = std::move(file);
                auto [size, zerr] = _file->stream().seek(0, Stream::SeekFrom::End);
                err = zerr;
                _fileSize = size;
        }
        if (err.isOk()) {
                uint8_t magic[4] = {};
                isMidi = readAt(this, 0, WritableByteSpan(magic, sizeof(magic))).isOk() &&
                         std::memcmp(magic, "MThd", 4) == 0;
        }
        if (err.isOk() && isMidi) {
                // Its messages to the system port, not decoded.
                err = startMidi(command);
                if (err.isError()) {
                        const uint32_t track = _track;
                        closeDecoder();
                        _state = State::Idle;
                        reportFailed(track, err, 0, true);
                }
                return;
        }
        if (err.isOk()) {
                auto [d, derr] = Decoder::open({readAt, this, _fileSize});
                err = derr;
                if (err.isOk()) _decoder = d;
        }
        if (err.isOk()) err = _decoder->setFormat(SampleFormat::F32Le);
        if (err.isError()) {
                const uint32_t track = _track;
                closeDecoder();
                _state = State::Idle;
                reportFailed(track, err, 0, isMidi);
                return;
        }
        _info = _decoder->info();
        // Another rate or channel count: what is queued plays out before the stream changes.
        const bool same = _audio != nullptr && _config.format == SampleFormat::F32Le &&
                          _config.channels == _info.channels && _config.rateHz == _info.rateHz;
        if (command.how == Start::Follow && _audio != nullptr && !same) (void)_audio->drain();
        bool fresh = command.how == Start::Now || !same;
        err = openDevice(_info);
        if (err.isError()) {
                const uint32_t track = _track;
                closeDecoder();
                closeDevice();
                _state = State::Idle;
                reportFailed(track, err, 0, false);
                return;
        }
        if (command.startMs > 0) {
                uint64_t frame = command.startMs * _info.rateHz / 1000;
                if (_info.frames != Decoder::UnknownFrames && frame >= _info.frames) frame = 0;
                if (frame > 0 && _decoder->seek(frame).isError()) (void)_decoder->seek(0);
        }
        _paused = false;
        (void)_audio->setPaused(false);
        _state = State::Decoding;
        reportStarted(fresh);
        return;
}

void Player::seekTrack(const Command &command) {
        if (_midiHandle) {
                const uint64_t length = _sequence.lengthMs() + MidiTailMs;
                seekMidi(command.startMs < length ? command.startMs : length);
                _state = State::Decoding;
                reportStarted(true);
                return;
        }
        uint64_t frame = command.startMs * _info.rateHz / 1000;
        if (_info.frames != Decoder::UnknownFrames && frame >= _info.frames)
                frame = _info.frames > 0 ? _info.frames - 1 : 0;
        const Error err = _decoder->seek(frame);
        if (err.isError()) {
                const uint32_t track = _track;
                closeDecoder();
                _state = State::Idle;
                reportFailed(track, err, frame, false);
                return;
        }
        // Paused, it stays so: the sound from there is written when it resumes.
        _state = State::Decoding;
        reportStarted(true);
        return;
}

void Player::decodeSome() {
        const size_t frameBytes = _decoder->frameBytes();
        size_t       frames = size_t(_info.rateHz) * PeriodMs / 1000;
        if (frames * frameBytes > _bufferBytes) frames = _bufferBytes / frameBytes;
        auto [n, err] = _decoder->decode(WritableByteSpan(_buffer, frames * frameBytes));
        if (n > 0) {
                auto [w, werr] = _audio->write(ByteSpan(_buffer, n), WaitForever);
                _written += w / frameBytes;
                if (werr == Error::InvalidState) return; // thrown away under it: a command follows
                if (werr == Error::Interrupted) {
                        // The program is being stopped.
                        const uint32_t track = _track;
                        dropQueued();
                        closeDecoder();
                        closeDevice();
                        reportStopped(track);
                        return;
                }
                if (werr.isError()) {
                        const uint32_t track = _track;
                        const uint64_t at = _decoder->position();
                        closeDecoder();
                        _state = State::Idle;
                        reportFailed(track, werr, at, false);
                        return;
                }
        }
        if (err == Error::EndOfFile) {
                // Kept open (a seek back); the loop says what follows.
                _state = State::Ended;
                Report r;
                r.event = Event::Ended;
                r.track = _track;
                report(r);
        } else if (err.isError()) {
                const uint32_t track = _track;
                const uint64_t at = _decoder->position();
                closeDecoder();
                _state = State::Idle;
                reportFailed(track, err, at, false);
        }
        return;
}

Error Player::openDevice(const Decoder::Info &info) {
        if (_audio == nullptr) {
                auto [handle, err] = Vfs::open(DevicePath, Vfs::Write, Vfs::Context{_cwd.view(), nullptr});
                if (err.isError()) return err;
                AudioFile *audio = handle->stream().audioFile();
                if (audio == nullptr) return Error::NotSupported;
                (void)audio->setGain(_gain.load(std::memory_order_relaxed));
                Mutex::Locker lock(_deviceLock, WaitForever);
                _device = std::move(handle);
                _audio = audio;
                _config = AudioFile::Config();
        }
        if (_config.format == SampleFormat::F32Le && _config.channels == info.channels && _config.rateHz == info.rateHz)
                return Error::Ok;
        AudioFile::Config c;
        c.format = SampleFormat::F32Le;
        c.channels = info.channels;
        c.rateHz = info.rateHz;
        c.ringFrames = info.rateHz * RingMs / 1000;
        const Error err = _audio->configure(c);
        if (err.isError()) return err;
        _config = c;
        _written = 0; // a configured stream counts from 0
        return Error::Ok;
}

void Player::dropQueued() {
        if (_audio == nullptr) return;
        (void)_audio->drop();
        (void)_audio->setPaused(_paused);
        _written = _audio->status().playedFrames; // the next write plays from where it stopped
        return;
}

void Player::drainAndClose() {
        // Cut short by a drop() from the loop (a stop, a new track).
        if (_audio != nullptr) (void)_audio->drain();
        closeDevice();
        return;
}

void Player::closeDecoder() {
        closeMidi();
        Decoder::close(_decoder);
        _decoder = nullptr;
        _file.reset();
        _track = 0;
        _state = State::Idle;
        return;
}

void Player::closeDevice() {
        FileHandle::Ptr gone;
        {
                Mutex::Locker lock(_deviceLock, WaitForever);
                gone = std::move(_device);
                _audio = nullptr;
        }
        _config = AudioFile::Config();
        _written = 0;
        return; // closed here, as `gone` goes: nothing is queued
}

void Player::report(const Report &r) {
        {
                Mutex::Locker lock(_reportLock, WaitForever);
                if (_reportCount == MaxReports) {
                        // The loop is far behind: the oldest goes.
                        _reportHead = (_reportHead + 1) % MaxReports;
                        --_reportCount;
                }
                _reports[(_reportHead + _reportCount) % MaxReports] = r;
                ++_reportCount;
        }
        if (_notify != nullptr) _notify(_notifyArg);
        return;
}

void Player::reportStarted(bool fresh) {
        Report r;
        r.event = Event::Started;
        r.track = _track;
        r.fresh = fresh;
        r.info = _info;
        if (_midiHandle) {
                // The player's clock is the file's: a frame of each at the same place.
                r.streamFrame = midiMs() * MidiRateHz / 1000;
                r.frame = r.streamFrame;
                String synth;
                (void)SysConf::get(StringView("synth/system"), synth);
                std::snprintf(r.synth, sizeof(r.synth), "%.*s", int(synth.size()), synth.data());
                report(r);
                return;
        }
        r.streamFrame = _written;
        r.frame = _decoder->position();
        copyTag(_decoder->tag("TITLE"), r.title);
        copyTag(_decoder->tag("ARTIST"), r.artist);
        copyTag(_decoder->tag("ALBUM"), r.album);
        report(r);
        return;
}

void Player::reportFailed(uint32_t track, Error error, uint64_t frame, bool isMidi) {
        Report r;
        r.event = Event::Failed;
        r.track = track;
        r.error = error;
        r.frame = frame;
        r.isMidi = isMidi;
        if (frame > 0) r.info = _info; // its rate, for where
        report(r);
        return;
}

void Player::reportStopped(uint32_t track) {
        Report r;
        r.event = Event::Stopped;
        r.track = track;
        report(r);
        return;
}

// ---- A MIDI file, through the system port (devplan/mediaplayer.md §12)

Error Player::startMidi(const Command &command) {
        // The whole file, its messages kept in time order.
        if (_fileSize > MidiSequence::MaxFileBytes) return Error::Invalid;
        auto *data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size_t(_fileSize)));
        if (data == nullptr) return Error::NoMem;
        Error err = readAt(this, 0, WritableByteSpan(data, size_t(_fileSize)));
        if (err.isOk()) err = _sequence.load(ByteSpan(data, size_t(_fileSize)));
        Memory::free(data);
        if (err.isError()) return err;
        // No system synth: nothing would sound.
        String synth;
        if (SysConf::get(StringView("synth/system"), synth).isError() || synth.isEmpty()) return Error::NotConnected;
        // A sound file's stream before it plays out (the next in the list), or goes (a new choice).
        if (_audio != nullptr && command.how == Start::Follow) (void)_audio->drain();
        closeDevice();
        auto [handle, oerr] = Vfs::open(MidiPath, Vfs::Write, Vfs::Context{_cwd.view(), nullptr});
        if (oerr.isError()) return oerr;
        MidiFile *midi = handle->stream().midiFile();
        if (midi == nullptr) return Error::NotSupported;
        MidiFile::Config c;
        MidiFile::Config got;
        c.flags = MidiFile::Events;
        c.eventSize = sizeof(MidiEvent);
        if (Error cerr = midi->configure(c, got); cerr.isError()) return cerr;
        _midiHandle = std::move(handle);
        _next = 0;
        for (uint8_t &v : _volume) v = 100; // GM's default
        _heldMs.store(-1, std::memory_order_release);
        _midiOn.store(true, std::memory_order_release);
        // Another program's controllers and notes gone, then the volumes at the player's gain.
        sendToAll(121, 0);
        sendToAll(123, 0);
        _scaleGain = _gain.load(std::memory_order_relaxed);
        _scale = volumeScale(_scaleGain);
        sendVolumes();
        const uint64_t length = _sequence.lengthMs();
        const uint64_t start = command.startMs < length ? command.startMs : 0;
        if (start > 0) {
                seekMidi(start);
        } else {
                _zeroNs.store(Clock::monotonicNs() + MidiLeadNs, std::memory_order_release);
        }
        _info = Decoder::Info();
        _info.codec = Codec::Midi;
        _info.format = SampleFormat::F32Le;
        _info.channels = 2;
        _info.rateHz = MidiRateHz;
        _info.frames = (length + MidiTailMs) * MidiRateHz / 1000;
        _paused = false;
        _state = State::Decoding;
        reportStarted(true);
        return Error::Ok;
}

void Player::systemChanged(void *arg) {
        auto *self = static_cast<Player *>(arg);
        self->_systemChanged.store(true);
        self->_wake.release();
        return;
}

uint64_t Player::midiMs() const {
        const int64_t held = _heldMs.load(std::memory_order_acquire);
        if (held >= 0) return uint64_t(held);
        const int64_t since = Clock::monotonicNs() - _zeroNs.load(std::memory_order_acquire);
        return since > 0 ? uint64_t(since / 1'000'000) : 0;
}

uint8_t Player::volumeOut(uint8_t channel) const {
        return uint8_t((int32_t(_volume[channel & 0x0f]) * _scale) >> 15);
}

void Player::sendNow(const uint8_t *data, uint8_t length) {
        (void)sendAt(0, data, length);
        return;
}

// A message as sent: a volume (7) as the file set it is kept, and scaled by the gain.
bool Player::sendAt(int64_t timeNs, const uint8_t *data, uint8_t length) {
        if (!_midiHandle) return true;
        MidiEvent e;
        e.timeNs = timeNs; // 0: now
        e.size = length;
        std::memcpy(e.data, data, length);
        const bool volume = length == 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 7;
        if (volume) {
                _volume[data[0] & 0x0f] = data[2] & 0x7f;
                e.data[2] = volumeOut(data[0]);
        }
        auto [n, err] = _midiHandle->write(ByteSpan(reinterpret_cast<const uint8_t *>(&e), sizeof(e)), WaitForever);
        (void)n;
        return err != Error::WouldBlock; // the fd's schedule full: again later
}

void Player::sendToAll(uint8_t controller, uint8_t value) {
        for (uint8_t ch = 0; ch < 16; ++ch) {
                const uint8_t m[3] = {uint8_t(0xb0 | ch), controller, value};
                sendNow(m, 3);
        }
        return;
}

void Player::sendVolumes() {
        for (uint8_t ch = 0; ch < 16; ++ch) {
                const uint8_t m[3] = {uint8_t(0xb0 | ch), 7, _volume[ch]};
                sendNow(m, 3);
        }
        return;
}

void Player::seekMidi(uint64_t ms) {
        if (MidiFile *midi = _midiHandle ? _midiHandle->stream().midiFile() : nullptr; midi != nullptr)
                midi->cancelScheduled();
        sendToAll(120, 0); // all sound off
        sendToAll(121, 0); // reset the controllers
        for (uint8_t &v : _volume) v = 100;
        sendVolumes();
        MidiSequence::Message state[MidiSequence::MaxStateMessages];
        const size_t          n = _sequence.stateAt(uint32_t(ms), Span<MidiSequence::Message>(state));
        for (size_t i = 0; i < n; ++i) sendNow(state[i].data, state[i].length);
        _next = _sequence.indexAt(uint32_t(ms));
        if (_heldMs.load(std::memory_order_acquire) >= 0) {
                _heldMs.store(int64_t(ms), std::memory_order_release); // paused: it stays so, there
        } else {
                _zeroNs.store(Clock::monotonicNs() + MidiLeadNs - int64_t(ms) * 1'000'000, std::memory_order_release);
        }
        return;
}

void Player::pauseMidi(bool paused) {
        if (paused) {
                if (_heldMs.load(std::memory_order_acquire) >= 0) return;
                const uint64_t at = midiMs();
                if (MidiFile *midi = _midiHandle->stream().midiFile(); midi != nullptr) midi->cancelScheduled();
                sendToAll(123, 0); // all notes off: their releases sound
                _heldMs.store(int64_t(at), std::memory_order_release);
                return;
        }
        const int64_t held = _heldMs.load(std::memory_order_acquire);
        if (held < 0) return;
        // On from there: what was cancelled is written again.
        _next = _sequence.indexAt(uint32_t(held));
        _zeroNs.store(Clock::monotonicNs() + MidiLeadNs - held * 1'000'000, std::memory_order_release);
        _heldMs.store(-1, std::memory_order_release);
        return;
}

unsigned Player::sendSome() {
        // The gain changed: the volumes again.
        const int32_t cdb = _gain.load(std::memory_order_relaxed);
        if (cdb != _scaleGain) {
                _scaleGain = cdb;
                _scale = volumeScale(cdb);
                sendVolumes();
        }
        // Each message due within MidiAheadMs, written with its time.
        const uint64_t pos = midiMs();
        const int64_t  zero = _zeroNs.load(std::memory_order_acquire);
        while (_next < _sequence.count()) {
                const MidiSequence::Message &m = _sequence.message(_next);
                if (m.ms > pos + MidiAheadMs) break;
                if (!sendAt(zero + int64_t(m.ms) * 1'000'000, m.data, m.length)) return PeriodMs;
                ++_next;
        }
        if (_next >= _sequence.count()) {
                // All written: the end once its last notes have had their release.
                const uint64_t end = uint64_t(_sequence.lengthMs()) + MidiTailMs;
                if (pos >= end) {
                        _state = State::Ended; // kept open (a seek back); the loop says what follows
                        Report r;
                        r.event = Event::Ended;
                        r.track = _track;
                        report(r);
                        return 0;
                }
                return unsigned(end - pos < MidiLongestWaitMs ? end - pos : MidiLongestWaitMs);
        }
        const uint64_t due = _sequence.message(_next).ms; // written MidiAheadMs before this
        const uint64_t wait = due > pos + MidiAheadMs ? due - pos - MidiAheadMs : 1;
        return unsigned(wait < MidiLongestWaitMs ? wait : MidiLongestWaitMs);
}

void Player::closeMidi() {
        if (!_midiOn.load(std::memory_order_acquire)) return;
        if (MidiFile *midi = _midiHandle ? _midiHandle->stream().midiFile() : nullptr; midi != nullptr) {
                midi->cancelScheduled();
                sendToAll(120, 0); // all sound off; the close turns off what it left on too
        }
        _midiHandle.reset();
        _sequence.clear();
        _midiOn.store(false, std::memory_order_release);
        _heldMs.store(-1, std::memory_order_release);
        return;
}

BMDS_NAMESPACE_END
