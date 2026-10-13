/**
 * @file      speechfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Speech fds (devplan/speech.md §4): a speaker on a shared voice behind an fd.
 */

#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/speechfile.h>

BMDS_NAMESPACE_BEGIN

Result<FileHandle::Ptr> SpeechFile::open(const Config &config) {
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(SpeechFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *file = ::new (mem) SpeechFile();
        auto [voice, verr] = Voices::acquire(config.voice);
        Error err = verr;
        if (err.isOk()) {
                file->_voice = voice;
                err = file->_speaker.open(Voices::files(voice), config.markup, config.prosody);
        }
        if (err.isError()) {
                file->handleClosed(0);
                return makeError<FileHandle::Ptr>(err);
        }
        file->_config = config;
        file->_config.voice = Voices::name(voice);
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, FileHandle::ReadWrite);
        if (handle.error().isError()) file->handleClosed(0);
        return handle;
}

void SpeechFile::handleClosed(uint8_t) {
        _speaker.close();
        Voices::release(_voice);
        this->~SpeechFile();
        Memory::free(this);
        return;
}

SpeechFile::Info SpeechFile::info() {
        Mutex::Locker lock(_lock, WaitForever);
        Info          i;
        i.voice = _config.voice;
        i.prosody = _config.prosody;
        i.markup = _config.markup;
        i.frames = _frames;
        return i;
}

Result<size_t> SpeechFile::write(ByteSpan data, unsigned int) {
        if (data.isEmpty()) return makeResult(size_t(0));
        Mutex::Locker lock(_lock, WaitForever);
        auto [n, err] = _speaker.put(data);
        if (err.isError()) return makeError<size_t>(Error::IoError);
        if (n == 0) return makeError<size_t>(Error::WouldBlock);
        return makeResult(n);
}

Result<size_t> SpeechFile::read(WritableByteSpan buf, unsigned int) {
        const size_t samples = buf.size() / 2;
        if (samples == 0) return makeError<size_t>(Error::Invalid);
        Mutex::Locker lock(_lock, WaitForever);
        size_t        made = 0;
        if ((reinterpret_cast<uintptr_t>(buf.data()) & 1) == 0) {
                // Straight into the buffer when it is aligned for samples.
                auto [n, err] = _speaker.render(Span<int16_t>(reinterpret_cast<int16_t *>(buf.data()), samples));
                if (err.isError()) return makeError<size_t>(Error::IoError);
                made = n;
        } else {
                while (made < samples) {
                        const size_t want = samples - made < BlockSamples ? samples - made : BlockSamples;
                        auto [n, err] = _speaker.render(Span<int16_t>(_block, want));
                        if (err.isError()) {
                                if (made != 0) break;
                                return makeError<size_t>(Error::IoError);
                        }
                        std::memcpy(buf.data() + made * 2, _block, n * 2);
                        made += n;
                        if (n < want) break;
                }
        }
        _frames += made;
        return makeResult(made * 2);
}

BMDS_NAMESPACE_END
