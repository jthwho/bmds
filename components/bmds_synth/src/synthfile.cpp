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

        constexpr size_t MaxFonts = 4;        // fonts loaded at once
        constexpr size_t ConvertFrames = 128; // S16 reads render through this many frames at a time

        struct Loaded {
                        String     path;
                        SoundFont *font = nullptr;
                        uint32_t   users = 0;
        };

        struct Fonts {
                        Mutex  lock;
                        Loaded loaded[MaxFonts];
        };

        constinit StaticObject<Fonts> fonts;

        Fonts &theFonts() {
                return *fonts;
        }

        // A font file read in order, skips by seeking.
        class FileReader final : public SoundFont::Reader {
                public:
                        explicit FileReader(const FileHandle::Ptr &file) : _file(file) {}
                        Error read(void *out, size_t bytes) override {
                                auto *p = static_cast<uint8_t *>(out);
                                while (bytes > 0) {
                                        auto [n, err] = _file->read(WritableByteSpan(p, bytes), WaitForever);
                                        if (err.isError()) return err;
                                        if (n == 0) return Error::EndOfFile;
                                        p += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }
                        Error skip(size_t bytes) override {
                                return _file->stream()
                                        .seek(static_cast<int64_t>(bytes), Stream::SeekFrom::Current)
                                        .error();
                        }

                private:
                        const FileHandle::Ptr &_file;
        };

        Result<SoundFont *> loadFont(StringView path) {
                auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context());
                if (err.isError()) return makeError<SoundFont *>(err);
                FileReader    reader(file);
                const int64_t start = Clock::monotonicNs();
                auto [font, lerr] = SoundFont::load(reader);
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

Result<SoundFont *> SoundFonts::openSynth(StringView path, const Synth::Config &config, Synth &synth) {
        Fonts        &f = theFonts();
        Mutex::Locker lock(f.lock, WaitForever);
        Loaded       *entry = nullptr;
        Loaded       *free = nullptr;
        for (Loaded &l : f.loaded) {
                if (l.font != nullptr && l.path.view() == path) entry = &l;
                if (l.font == nullptr && free == nullptr) free = &l;
        }
        if (entry == nullptr) {
                if (free == nullptr) return makeError<SoundFont *>(Error::Busy);
                // Loaded with the lock held: a second synth on the same font waits for it, not a second copy.
                auto [font, err] = loadFont(path);
                if (err.isError()) return makeError<SoundFont *>(err);
                const Error aerr = free->path.assign(path);
                if (aerr.isError()) {
                        font->destroy();
                        return makeError<SoundFont *>(aerr);
                }
                free->font = font;
                free->users = 0;
                entry = free;
        }
        const Error err = synth.open(*entry->font, config);
        if (err.isError()) {
                if (entry->users == 0) {
                        entry->font->destroy();
                        entry->font = nullptr;
                        entry->path.clear();
                }
                return makeError<SoundFont *>(err);
        }
        ++entry->users;
        return makeResult(entry->font);
}

void SoundFonts::closeSynth(Synth &synth, SoundFont *font) {
        Fonts        &f = theFonts();
        Mutex::Locker lock(f.lock, WaitForever);
        synth.close();
        for (Loaded &l : f.loaded) {
                if (l.font != font || font == nullptr) continue;
                if (--l.users == 0) {
                        bmdsInfo("synth: %s: unloaded", l.path.cStr());
                        l.font->destroy();
                        l.font = nullptr;
                        l.path.clear();
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
