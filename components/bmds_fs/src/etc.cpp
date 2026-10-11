/**
 * @file      etc.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The files of /etc (etc.h): a fixed table filled at boot, and a stream
 * holding one file as it was built.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/stream.h>
#include <bmds/string.h>

#include "etc.h"
#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit Vfs::EtcFile        files[Vfs::MaxEtcFiles];
        constinit std::atomic<size_t> fileCount{0}; // added under the VFS's lock

        // The file's bytes follow the stream in its allocation.
        class EtcStream final : public Stream {
                public:
                        explicit EtcStream(size_t size) : _size(size) {}

                        uint8_t *bytes() { return reinterpret_cast<uint8_t *>(this + 1); }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever); // a handle's tasks share the position
                                if (buf.isEmpty()) return makeResult(size_t(0));
                                if (_position >= _size) return makeError<size_t>(Error::EndOfFile);
                                const size_t n = buf.size() < _size - _position ? buf.size() : _size - _position;
                                std::memcpy(buf.data(), bytes() + _position, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan, unsigned int) override {
                                return makeError<size_t>(Error::ReadOnly);
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                int64_t       base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(_position);
                                if (from == SeekFrom::End) base = static_cast<int64_t>(_size);
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                const auto at = static_cast<uint64_t>(base + offset);
                                _position = at < _size ? static_cast<size_t>(at) : _size;
                                return makeResult(at);
                        }

                        void handleClosed(uint8_t) override {
                                this->~EtcStream();
                                Memory::free(this);
                                return;
                        }

                private:
                        const size_t _size;
                        size_t       _position = 0;
        };

} // namespace

Error Etc::add(const Vfs::EtcFile &file) {
        if (file.name == nullptr || file.generate == nullptr) return Error::Invalid;
        const StringView name(file.name);
        if (name.isEmpty() || name.contains('/')) return Error::Invalid;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const size_t  n = fileCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
                if (name == files[i].name) return Error::Exists;
        }
        if (n == Vfs::MaxEtcFiles) return Error::Full;
        files[n] = file;
        fileCount.store(n + 1, std::memory_order_release);
        return Error::Ok;
}

size_t Etc::count() {
        return fileCount.load(std::memory_order_acquire);
}

const Vfs::EtcFile &Etc::at(size_t index) {
        return files[index];
}

const Vfs::EtcFile *Etc::find(StringView name) {
        const size_t n = count();
        for (size_t i = 0; i < n; ++i) {
                if (name == files[i].name) return &files[i];
        }
        return nullptr;
}

Result<FileHandle::Ptr> Etc::open(const Vfs::EtcFile &file) {
        String      text(MemSpace::Psram);
        const Error err = file.generate(file.arg, text);
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(EtcStream) + text.size());
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *stream = ::new (mem) EtcStream(text.size());
        std::memcpy(stream->bytes(), text.data(), text.size());
        Result<FileHandle::Ptr> handle = FileHandle::open(*stream, FileHandle::Read);
        if (handle.error().isError()) {
                stream->~EtcStream();
                Memory::free(mem);
        }
        return handle;
}

BMDS_NAMESPACE_END
