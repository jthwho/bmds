/**
 * @file      synthfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Synth fds and the fonts they share (devplan/audio.md §22).
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/synthfile.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>
#include <bmds/mididecoder.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // devplan/audio.md §22: the key is an interface once shipped.
        BMDS_SYSCONF(synthFont, "synth/font", Config::Type::String, SynthFile::DefaultFont,
                     "The SoundFont (SF2 or SF3) synths play when a program names none", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(synthGain, "synth/gain", Config::Type::Int, "-10",
                     "The synths' gain in dB (-40 to 12): headroom, so that many notes at once stay below full scale",
                     static_cast<int32_t>(Synth::MinGainDb), static_cast<int32_t>(Synth::MaxGainDb), nullptr, nullptr)
        BMDS_SYSCONF(synthKeepFonts, "synth/keep-fonts", Config::Type::Bool, "true",
                     "Keep SoundFonts loaded after their last synth, until memory runs short", 0, 0, nullptr, nullptr)

        constexpr size_t MaxFonts = 4;        // fonts loaded at once
        constexpr size_t ConvertFrames = 128; // S16 reads render through this many frames at a time

        struct Loaded {
                        String     path;
                        SoundFont *font = nullptr;
                        uint32_t   users = 0;
                        uint64_t   size = 0; // the file's, when loaded: a changed file loads again
                        int64_t    modified = 0;
                        uint64_t   idleSince = 0; // Fonts::clock when its last synth closed
        };

        // The fonts loaded. One no synth plays stays (synth/keep-fonts) until memory runs short:
        // the memory's reclaimer frees the longest idle first.
        class Fonts final : public Memory::Reclaimer {
                public:
                        size_t reclaim(MemSpace space, size_t bytes) override;
                        Usage  usage() const override;

                        // With the lock held.
                        void unload(Loaded &l) {
                                l.font->destroy();
                                l.font = nullptr;
                                l.path.clear();
                                l.users = 0;
                                return;
                        }

                        mutable Mutex lock;
                        Loaded        loaded[MaxFonts];
                        uint64_t      clock = 0;
                        uint32_t      hits = 0;
                        uint32_t      misses = 0;
                        uint32_t      reclaims = 0;
        };

        // Called by a failed allocation, perhaps on a thread loading a font with the lock held: so
        // the lock is only tried, and nothing here allocates.
        size_t Fonts::reclaim(MemSpace space, size_t bytes) {
                if (space != MemSpace::Psram && space != MemSpace::PsramDma) return 0;
                if (lock.tryLock().isError()) return 0;
                size_t freed = 0;
                while (freed < bytes) {
                        Loaded *oldest = nullptr;
                        for (Loaded &l : loaded)
                                if (l.font != nullptr && l.users == 0 &&
                                    (oldest == nullptr || l.idleSince < oldest->idleSince))
                                        oldest = &l;
                        if (oldest == nullptr) break;
                        freed += oldest->font->bytes();
                        unload(*oldest);
                }
                if (freed != 0) ++reclaims;
                lock.unlock();
                return freed;
        }

        Memory::Reclaimer::Usage Fonts::usage() const {
                Mutex::Locker locker(lock, WaitForever);
                Usage         u;
                u.items = "fonts";
                for (const Loaded &l : loaded) {
                        if (l.font == nullptr) continue;
                        ++u.count;
                        u.bytes += l.font->bytes();
                        if (l.users == 0) u.reclaimable += l.font->bytes();
                }
                u.hits = hits;
                u.misses = misses;
                u.reclaims = reclaims;
                return u;
        }

        bool keepFonts() {
                if (!SysConf::isReady()) return true;
                auto [keep, err] = SysConf::getBool(bmdsConf_synthKeepFonts.key);
                return err.isError() || keep;
        }

        constinit StaticObject<Fonts> fonts;

        Fonts &theFonts() {
                return *fonts;
        }

        constexpr size_t ReadBufferBytes = 4096; // a font's small reads come from this

        // A font file read in order. TinySoundFont reads its preset tables a few bytes at a
        // time (92,340 reads for TimGM6mb, 0.9 s from the card one by one), so small reads come
        // from a buffer; a large one (the samples) goes straight into its destination, from the
        // file's position, so that an aligned destination is read by DMA without a bounce.
        class FileReader final : public SoundFont::Reader {
                public:
                        FileReader(const FileHandle::Ptr &file, uint8_t *buffer) : _file(file), _buffer(buffer) {}

                        Error read(void *out, size_t bytes) override {
                                auto *p = static_cast<uint8_t *>(out);
                                if (bytes >= ReadBufferBytes) {
                                        Error err = discard();
                                        if (err.isOk()) err = readFile(p, bytes);
                                        _position += bytes;
                                        SoundFont::setLargeAllocationPhase(_position);
                                        return err;
                                }
                                _position += bytes;
                                while (bytes > 0) {
                                        if (_at == _have) {
                                                auto [n, err] = _file->read(WritableByteSpan(_buffer, ReadBufferBytes),
                                                                            WaitForever);
                                                if (err.isError()) return err;
                                                if (n == 0) return Error::EndOfFile;
                                                _at = 0;
                                                _have = n;
                                        }
                                        const size_t n = bytes < _have - _at ? bytes : _have - _at;
                                        std::memcpy(p, _buffer + _at, n);
                                        _at += n;
                                        p += n;
                                        bytes -= n;
                                }
                                SoundFont::setLargeAllocationPhase(_position);
                                return Error::Ok;
                        }

                        Error skip(size_t bytes) override {
                                _position += bytes;
                                SoundFont::setLargeAllocationPhase(_position);
                                if (bytes <= _have - _at) {
                                        _at += bytes;
                                        return Error::Ok;
                                }
                                bytes -= _have - _at;
                                _at = _have = 0;
                                return _file->stream()
                                        .seek(static_cast<int64_t>(bytes), Stream::SeekFrom::Current)
                                        .error();
                        }

                private:
                        // The file's position back to where the reader is, the buffer emptied.
                        Error discard() {
                                const size_t ahead = _have - _at;
                                _at = _have = 0;
                                if (ahead == 0) return Error::Ok;
                                return _file->stream()
                                        .seek(-static_cast<int64_t>(ahead), Stream::SeekFrom::Current)
                                        .error();
                        }

                        Error readFile(uint8_t *p, size_t bytes) {
                                while (bytes > 0) {
                                        auto [n, err] = _file->read(WritableByteSpan(p, bytes), WaitForever);
                                        if (err.isError()) return err;
                                        if (n == 0) return Error::EndOfFile;
                                        p += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }

                        const FileHandle::Ptr &_file;
                        uint8_t               *_buffer;
                        size_t                 _at = 0;
                        size_t                 _have = 0;
                        size_t                 _position = 0; // the reader's, in the file
        };

        Result<SoundFont *> loadFont(StringView path) {
                auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context());
                if (err.isError()) return makeError<SoundFont *>(err);
                auto *buffer = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, ReadBufferBytes));
                if (buffer == nullptr) return makeError<SoundFont *>(Error::NoMem);
                FileReader    reader(file, buffer);
                const int64_t start = Clock::monotonicNs();
                auto [font, lerr] = SoundFont::load(reader);
                Memory::free(buffer);
                if (lerr.isError()) {
                        bmdsWarn("synth: %.*s: %s", static_cast<int>(path.size()), path.data(), lerr.desc());
                        return makeError<SoundFont *>(lerr);
                }
                bmdsInfo("synth: %.*s: %u presets, %u KB, loaded in %u ms", static_cast<int>(path.size()), path.data(),
                         static_cast<unsigned>(font->presetCount()), static_cast<unsigned>(font->bytes() / 1024),
                         static_cast<unsigned>((Clock::monotonicNs() - start) / 1000000));
                return makeResult(font);
        }

} // namespace

// ---- The fonts

Error SoundFonts::init() {
        fonts.create();
        if (Memory::addReclaimer(&*fonts).isError())
                bmdsWarn("synth: idle fonts won't be given back when memory runs short");
        // MIDI files play through the synth wherever sound files decode, on synth/font.
        MidiDecoder::setFonts(
                [](const Synth::Config &config, Synth &synth) -> Result<SoundFont *> {
                        String      path;
                        const Error err = defaultFont(path);
                        if (err.isError()) return makeError<SoundFont *>(err);
                        return openSynth(path.view(), config, synth);
                },
                closeSynth);
        const Error err = Decoder::addFormat("MThd", MidiDecoder::open);
        if (err.isError()) bmdsWarn("synth: MIDI files won't decode: %s", err.desc());
        return Error::Ok;
}

Error SoundFonts::defaultFont(String &out) {
        if (SysConf::isReady() && SysConf::get(bmdsConf_synthFont.key, out).isOk() && !out.isEmpty()) return Error::Ok;
        return out.assign(SynthFile::DefaultFont);
}

Result<SoundFont *> SoundFonts::openSynth(StringView path, const Synth::Config &asked, Synth &synth) {
        // Every synth's gain is synth/gain's, read here: play, synth and programs' synth fds alike.
        Synth::Config config = asked;
        if (SysConf::isReady()) {
                auto [gain, gerr] = SysConf::getInt(bmdsConf_synthGain.key);
                if (gerr.isOk()) config.gainDb = static_cast<float>(gain);
        }
        // The file as it is now: a font loaded from it before is used only if it hasn't changed.
        auto [info, serr] = Vfs::stat(path, Vfs::Context());
        if (serr.isError()) return makeError<SoundFont *>(serr);
        Fonts        &f = theFonts();
        Mutex::Locker lock(f.lock, WaitForever);
        Loaded       *entry = nullptr;
        for (Loaded &l : f.loaded) {
                if (l.font == nullptr || l.path.view() != path) continue;
                if (l.size == info.size && l.modified == info.modified.ns) {
                        entry = &l;
                } else if (l.users == 0) {
                        bmdsInfo("synth: %s: changed: loading it again", l.path.cStr());
                        f.unload(l);
                }
        }
        if (entry != nullptr) {
                ++f.hits;
        } else {
                // A free place, else the longest idle font's.
                Loaded *place = nullptr;
                for (Loaded &l : f.loaded) {
                        if (l.font == nullptr) {
                                place = &l;
                                break;
                        }
                        if (l.users == 0 && (place == nullptr || l.idleSince < place->idleSince)) place = &l;
                }
                if (place == nullptr) return makeError<SoundFont *>(Error::Busy);
                if (place->font != nullptr) f.unload(*place);
                ++f.misses;
                // Loaded with the lock held: a second synth on the same font waits for it, not a second copy.
                Result<SoundFont *> loaded = loadFont(path);
                if (loaded.error() == Error::NoMem) {
                        // The reclaimer can't free fonts while this holds the lock: the idle ones now,
                        // and once more.
                        bool freed = false;
                        for (Loaded &l : f.loaded) {
                                if (l.font == nullptr || l.users != 0) continue;
                                f.unload(l);
                                freed = true;
                        }
                        if (freed) loaded = loadFont(path);
                }
                if (loaded.error().isError()) return makeError<SoundFont *>(loaded.error());
                SoundFont  *font = loaded.value();
                const Error aerr = place->path.assign(path);
                if (aerr.isError()) {
                        font->destroy();
                        return makeError<SoundFont *>(aerr);
                }
                place->font = font;
                place->users = 0;
                place->size = info.size;
                place->modified = info.modified.ns;
                place->idleSince = ++f.clock;
                entry = place;
        }
        const Error err = synth.open(*entry->font, config);
        if (err.isError()) {
                if (entry->users == 0 && !keepFonts()) f.unload(*entry);
                return makeError<SoundFont *>(err);
        }
        ++entry->users;
        return makeResult(entry->font);
}

void SoundFonts::closeSynth(Synth &synth, SoundFont *font) {
        const bool    keep = keepFonts(); // read before the lock: the settings take their own
        Fonts        &f = theFonts();
        Mutex::Locker lock(f.lock, WaitForever);
        synth.close();
        for (Loaded &l : f.loaded) {
                if (l.font != font || font == nullptr) continue;
                if (--l.users != 0) continue;
                l.idleSince = ++f.clock;
                if (!keep) {
                        bmdsInfo("synth: %s: unloaded", l.path.cStr());
                        f.unload(l);
                }
        }
        return;
}

// ---- Synth fds

Result<FileHandle::Ptr> SynthFile::open(const Config &config, const Vfs::Context &context) {
        if (config.format != SampleFormat::F32Le && config.format != SampleFormat::S16Le)
                return makeError<FileHandle::Ptr>(Error::Invalid);
        // The font: the path given (made absolute), or synth/font's.
        String path;
        Error  err = Error::Ok;
        if (config.font.isEmpty()) {
                err = SoundFonts::defaultFont(path);
        } else {
                auto [full, nerr] = Vfs::normalize(config.font, context.cwd);
                err = nerr.isOk() ? path.assign(full.view()) : nerr;
        }
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(SynthFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto         *file = ::new (mem) SynthFile();
        Synth::Config sc;
        sc.rateHz = config.rateHz;
        sc.channels = config.channels;
        sc.maxVoices = config.maxVoices;
        auto [font, ferr] = SoundFonts::openSynth(path.view(), sc, file->_synth);
        if (ferr.isError()) {
                file->~SynthFile();
                Memory::free(mem);
                return makeError<FileHandle::Ptr>(ferr);
        }
        file->_font = font;
        file->_fontPath = std::move(path);
        file->_config = config;
        file->_config.font = file->_fontPath.view();
        file->_config.maxVoices = file->_synth.config().maxVoices;
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, FileHandle::ReadWrite);
        if (handle.error().isError()) file->handleClosed(0);
        return handle;
}

void SynthFile::handleClosed(uint8_t) {
        SoundFonts::closeSynth(_synth, _font);
        this->~SynthFile();
        Memory::free(this);
        return;
}

SynthFile::Info SynthFile::info() {
        Mutex::Locker lock(_lock, WaitForever);
        Info          i;
        i.synth = _synth.stats();
        i.config = _config;
        i.presets = _font != nullptr ? _font->presetCount() : 0;
        i.fontBytes = _font != nullptr ? _font->bytes() : 0;
        i.cpuPermille = _cpuPermille;
        i.cpuPermilleMax = _cpuPermilleMax;
        return i;
}

Result<size_t> SynthFile::write(ByteSpan data, unsigned int) {
        if (data.size() % sizeof(Synth::Event) != 0) return Result<size_t>(0, Error::Invalid);
        Mutex::Locker lock(_lock, WaitForever);
        size_t        done = 0;
        while (done < data.size()) {
                Synth::Event e;
                std::memcpy(&e, data.data() + done, sizeof(e));
                const Error err = _synth.queue(e);
                if (err.isError()) return Result<size_t>(done, err == Error::Full ? Error::WouldBlock : err);
                done += sizeof(e);
        }
        return makeResult(done);
}

Result<size_t> SynthFile::read(WritableByteSpan buf, unsigned int) {
        const size_t frameBytes = _config.channels * SampleFormats::bytesPerSample(_config.format);
        const size_t frames = buf.size() / frameBytes;
        if (frames == 0) return makeError<size_t>(Error::Invalid);
        Mutex::Locker lock(_lock, WaitForever);
        const int64_t start = Clock::monotonicNs();
        if (_config.format == SampleFormat::F32Le) {
                // Straight into the buffer when it is aligned for floats, else through the stack.
                if ((reinterpret_cast<uintptr_t>(buf.data()) & 3) == 0) {
                        _synth.render(reinterpret_cast<float *>(buf.data()), frames);
                } else {
                        float block[ConvertFrames * 2];
                        for (size_t done = 0; done < frames;) {
                                const size_t n = frames - done < ConvertFrames ? frames - done : ConvertFrames;
                                _synth.render(block, n);
                                std::memcpy(buf.data() + done * frameBytes, block, n * frameBytes);
                                done += n;
                        }
                }
        } else {
                float block[ConvertFrames * 2];
                for (size_t done = 0; done < frames;) {
                        const size_t n = frames - done < ConvertFrames ? frames - done : ConvertFrames;
                        _synth.render(block, n);
                        for (size_t i = 0; i < n * _config.channels; ++i) {
                                const float   x = block[i] * 32767.0f;
                                const int16_t s =
                                        static_cast<int16_t>(x > 32767.0f ? 32767.0f : (x < -32768.0f ? -32768.0f : x));
                                std::memcpy(buf.data() + done * frameBytes + i * 2, &s, 2);
                        }
                        done += n;
                }
        }
        // Its render time over the frames' time, in thousandths.
        const int64_t took = Clock::monotonicNs() - start;
        const auto permille = static_cast<uint32_t>(took * _config.rateHz / (static_cast<int64_t>(frames) * 1000000));
        _cpuPermille = permille;
        if (permille > _cpuPermilleMax) _cpuPermilleMax = permille;
        return makeResult(frames * frameBytes);
}

BMDS_NAMESPACE_END
