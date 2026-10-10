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

#include <cstring>
#include <new>
#include <utility>
#include <sdkconfig.h>
#include <bmds/memory.h>
#include <bmds/sampleformat.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView DevicePath("/dev/dsp");
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
        Mutex::Locker lock(_deviceLock, WaitForever);
        if (_audio != nullptr) (void)_audio->setGain(centiDb);
        return;
}

AudioFile::Status Player::status() {
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
                if (have) {
                        handle(_taken);
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
                        if (command.how == Start::Now && command.track == _track && _decoder != nullptr) {
                                seekTrack(command);
                        } else {
                                startTrack(command);
                        }
                        break;
                case Kind::Pause:
                case Kind::Resume:
                        _paused = command.kind == Kind::Pause;
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
                // A MIDI file that won't open is most likely missing its SoundFont: said so.
                uint8_t magic[4] = {};
                isMidi = readAt(this, 0, WritableByteSpan(magic, sizeof(magic))).isOk() &&
                         std::memcmp(magic, "MThd", 4) == 0;
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
        r.streamFrame = _written;
        r.frame = _decoder->position();
        r.info = _info;
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

BMDS_NAMESPACE_END
