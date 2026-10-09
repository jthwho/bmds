/**
 * @file      decodedfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A sound file read as PCM through an fd (devplan/audio.md §19).
 */

#include <cstring>
#include <new>
#include <bmds/decodedfile.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

// ---- Opening

Result<FileHandle::Ptr> DecodedFile::open(const FileHandle::Ptr &source, SampleFormat format, bool native) {
        if (!source || !source->canRead()) return makeError<FileHandle::Ptr>(Error::BadDescriptor);
        auto [size, serr] = source->stream().seek(0, SeekFrom::End);
        if (serr.isError()) return makeError<FileHandle::Ptr>(serr == Error::NotSupported ? Error::NotSupported : serr);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(DecodedFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *file = ::new (mem) DecodedFile();
        file->_source = source;
        auto [decoder, err] = Decoder::open({readSource, file, size});
        if (err.isError()) {
                file->destroy();
                return makeError<FileHandle::Ptr>(err);
        }
        file->_decoder = decoder;
        return finish(file, format, native);
}

Result<FileHandle::Ptr> DecodedFile::open(ByteSpan data, SampleFormat format, bool native) {
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(DecodedFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *file = ::new (mem) DecodedFile();
        file->_copy = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, data.size() != 0 ? data.size() : 1));
        if (file->_copy == nullptr) {
                file->destroy();
                return makeError<FileHandle::Ptr>(Error::NoMem);
        }
        if (data.size() != 0) std::memcpy(file->_copy, data.data(), data.size());
        file->_copyBytes = data.size();
        auto [decoder, err] = Decoder::open({readCopy, file, data.size()});
        if (err.isError()) {
                file->destroy();
                return makeError<FileHandle::Ptr>(err);
        }
        file->_decoder = decoder;
        return finish(file, format, native);
}

Result<FileHandle::Ptr> DecodedFile::finish(DecodedFile *file, SampleFormat format, bool native) {
        Error                   err = native ? Error::Ok : file->_decoder->setFormat(format);
        Result<FileHandle::Ptr> handle =
                err.isOk() ? FileHandle::open(*file, FileHandle::Read) : makeError<FileHandle::Ptr>(err);
        if (handle.error().isError()) file->destroy();
        return handle;
}

void DecodedFile::handleClosed(uint8_t) {
        destroy();
        return;
}

void DecodedFile::destroy() {
        Decoder::close(_decoder);
        Memory::free(_copy);
        this->~DecodedFile(); // drops the source's handle
        Memory::free(this);
        return;
}

// ---- The source

Error DecodedFile::readSource(void *arg, uint64_t offset, WritableByteSpan buf) {
        FileHandle &source = *static_cast<DecodedFile *>(arg)->_source;
        // Seeking first each time: the handle's position may be shared (dup).
        auto [at, serr] = source.stream().seek(static_cast<int64_t>(offset), SeekFrom::Start);
        if (serr.isError()) return serr;
        size_t done = 0;
        while (done < buf.size()) {
                auto [n, err] = source.read(buf.mid(done), WaitForever);
                if (err == Error::EndOfFile || (err.isOk() && n == 0)) return Error::EndOfFile;
                if (err.isError()) return err;
                done += n;
        }
        return Error::Ok;
}

Error DecodedFile::readCopy(void *arg, uint64_t offset, WritableByteSpan buf) {
        const auto *file = static_cast<const DecodedFile *>(arg);
        if (offset > file->_copyBytes || file->_copyBytes - offset < buf.size()) return Error::EndOfFile;
        std::memcpy(buf.data(), file->_copy + offset, buf.size());
        return Error::Ok;
}

// ---- Reading and seeking

Result<size_t> DecodedFile::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs; // only the file is waited for
        Mutex::Locker locker(_lock, WaitForever);
        const size_t  frame = _decoder->frameBytes();
        size_t        done = 0;
        // What is left of a frame read in pieces.
        if (_carryPos < _carryBytes) {
                const size_t n = _carryBytes - _carryPos < buf.size() ? _carryBytes - _carryPos : buf.size();
                std::memcpy(buf.data(), _carry + _carryPos, n);
                _carryPos += n;
                done = n;
        }
        if (done < buf.size() && buf.size() - done >= frame) {
                auto [n, err] = _decoder->decode(buf.mid(done));
                done += n;
                if (err.isError() && (done == 0 || err != Error::EndOfFile)) {
                        _position += done;
                        return Result<size_t>(done, err);
                }
        } else if (done == 0) {
                // Less than a frame asked for: a frame decoded, then given in pieces.
                auto [n, err] = _decoder->decode(WritableByteSpan(_carry, frame));
                if (err.isError()) return Result<size_t>(0, err);
                _carryBytes = n;
                _carryPos = buf.size() < n ? buf.size() : n;
                std::memcpy(buf.data(), _carry, _carryPos);
                done = _carryPos;
        }
        _position += done;
        return makeResult(done);
}

Result<size_t> DecodedFile::write(ByteSpan data, unsigned int timeoutMs) {
        (void)data;
        (void)timeoutMs;
        return makeError<size_t>(Error::NotSupported);
}

Result<uint64_t> DecodedFile::seek(int64_t offset, SeekFrom from) {
        Mutex::Locker locker(_lock, WaitForever);
        const size_t  frame = _decoder->frameBytes();
        const auto   &info = _decoder->info();
        const bool    known = info.frames != Decoder::UnknownFrames && !info.estimated;
        int64_t       base = 0;
        if (from == SeekFrom::Current) {
                base = static_cast<int64_t>(_position);
        } else if (from == SeekFrom::End) {
                if (!known) return makeError<uint64_t>(Error::NotSupported);
                base = static_cast<int64_t>(info.frames * frame);
        }
        if ((offset < 0 && -offset > base) || (offset > 0 && offset > INT64_MAX - base))
                return makeError<uint64_t>(Error::Invalid);
        const auto to = static_cast<uint64_t>(base + offset);
        if (to == _position) return makeResult(to); // telling, as fstat() does
        if (known && to > info.frames * frame) return makeError<uint64_t>(Error::Invalid);
        const Error err = _decoder->seek(to / frame);
        if (err.isError()) return makeError<uint64_t>(err == Error::OutOfRange ? Error::Invalid : err);
        _carryBytes = _carryPos = 0;
        _position = to - to % frame;
        // Inside a frame: that frame decoded, and its first bytes passed over.
        if (to % frame != 0) {
                auto [n, derr] = _decoder->decode(WritableByteSpan(_carry, frame));
                if (derr.isError() && derr != Error::EndOfFile) return makeError<uint64_t>(derr);
                _carryBytes = n;
                _carryPos = to % frame < n ? to % frame : n;
                _position += _carryPos;
        }
        return makeResult(_position);
}

BMDS_NAMESPACE_END
