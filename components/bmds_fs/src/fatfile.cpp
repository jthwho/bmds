/**
 * @file      fatfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/memory.h>
#include <bmds/stream.h>

#include "fatlayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        /**
         * An open FatFs file as a Stream. Lives in Psram (with FatFs's FIL and
         * its sector buffer) until its FileHandle closes. Files don't block,
         * so the stream timeouts don't apply.
         */
        class FatFile final : public Stream {
                public:
                        FatFile(FatVolume &volume, bool append, const FileIdentity &identity)
                            : _volume(volume), _identity(identity), _append(append) {}

                        FIL &fil() { return _fil; }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (_hole != 0 && !buf.isEmpty()) return makeError<size_t>(Error::EndOfFile);
                                UINT          got = 0;
                                const FRESULT result = f_read(&_fil, buf.data(), static_cast<UINT>(buf.size()), &got);
                                if (result != FR_OK) return makeError<size_t>(Fat::toError(result, _volume));
                                if (got == 0 && !buf.isEmpty()) return makeError<size_t>(Error::EndOfFile);
                                return makeResult(static_cast<size_t>(got));
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                Error         err;
                                if (_append) {
                                        _hole = 0;
                                        err = Fat::toError(f_lseek(&_fil, f_size(&_fil)), _volume);
                                } else if (_hole != 0) {
                                        // Written past the end: the gap reads as zeros.
                                        err = zeroFill(_hole);
                                        _hole = 0;
                                }
                                if (err.isError()) return makeError<size_t>(err);
                                UINT          put = 0;
                                const FRESULT result = f_write(&_fil, data.data(), static_cast<UINT>(data.size()), &put);
                                if (result != FR_OK) return makeError<size_t>(Fat::toError(result, _volume));
                                // FatFs writes short only when the volume is full.
                                if (put < data.size()) return makeError<size_t>(Error::NoSpace);
                                return makeResult(static_cast<size_t>(put));
                        }

                        Error flush(unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                return Fat::toError(f_sync(&_fil), _volume);
                        }

                        // Past the end, the position is a hole that the next write
                        // fills with zeros (FatFs would grow the file at once, with
                        // whatever its new clusters held).
                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                int64_t       base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(position());
                                if (from == SeekFrom::End) base = static_cast<int64_t>(f_size(&_fil));
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                const Error err = moveTo(static_cast<FSIZE_t>(base + offset));
                                if (err.isError()) return makeError<uint64_t>(err);
                                return makeResult(static_cast<uint64_t>(position()));
                        }

                        Error truncate(uint64_t size) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if ((_fil.flag & FA_WRITE) == 0) return Error::BadDescriptor;
                                const FSIZE_t here = position();
                                Error         err;
                                if (size < f_size(&_fil)) {
                                        err = Fat::toError(f_lseek(&_fil, static_cast<FSIZE_t>(size)), _volume);
                                        if (err.isOk()) err = Fat::toError(f_truncate(&_fil), _volume);
                                } else if (size > f_size(&_fil)) {
                                        err = zeroFill(static_cast<FSIZE_t>(size));
                                }
                                // The position stays where it was, even past the new end.
                                if (err.isOk()) err = moveTo(here);
                                return err;
                        }

                        void handleClosed(uint8_t) override {
                                {
                                        Mutex::Locker lock(Fs::lock(), WaitForever);
                                        // A stale file (its card went) just fails here.
                                        const bool written = (_fil.flag & FA_WRITE) != 0;
                                        (void)f_close(&_fil);
                                        if (written) Fs::writtenLocked(_identity);
                                }
                                this->~FatFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        FSIZE_t position() const { return _hole != 0 ? _hole : f_tell(&_fil); }

                        // Moves to @p to: there, or to the end with a hole. Lock held.
                        Error moveTo(FSIZE_t to) {
                                _hole = 0;
                                if (to > f_size(&_fil)) {
                                        _hole = to;
                                        to = f_size(&_fil);
                                }
                                return Fat::toError(f_lseek(&_fil, to), _volume);
                        }

                        // Writes zeros from the end to @p to; the position is then @p to. Lock held.
                        Error zeroFill(FSIZE_t to) {
                                static const uint8_t zeros[512] = {};
                                FRESULT              result = f_lseek(&_fil, f_size(&_fil));
                                while (result == FR_OK && f_tell(&_fil) < to) {
                                        const FSIZE_t left = to - f_tell(&_fil);
                                        const UINT    n = left < sizeof(zeros) ? static_cast<UINT>(left) : sizeof(zeros);
                                        UINT          put = 0;
                                        result = f_write(&_fil, zeros, n, &put);
                                        if (result == FR_OK && put < n) return Error::NoSpace;
                                }
                                return Fat::toError(result, _volume);
                        }

                        FIL            _fil = {};
                        FatVolume     &_volume;
                        FSIZE_t        _hole = 0; // A position past the end, or 0.
                        const FileIdentity _identity; // told at close after writing
                        const bool         _append;
        };

        BYTE fatMode(uint8_t flags) {
                BYTE mode = 0;
                if (flags & Vfs::Read) mode |= FA_READ;
                if (flags & Vfs::Write) mode |= FA_WRITE;
                if ((flags & Vfs::Create) && (flags & Vfs::Exclusive)) {
                        mode |= FA_CREATE_NEW;
                } else if ((flags & Vfs::Create) && (flags & Vfs::Truncate)) {
                        mode |= FA_CREATE_ALWAYS;
                } else if (flags & Vfs::Create) {
                        mode |= FA_OPEN_ALWAYS;
                }
                return mode;
        }

} // namespace

Result<FileHandle::Ptr> Fat::openFile(FatVolume &volume, const char *path, uint8_t flags, const FileIdentity &identity,
                                      FRESULT *raw, String *link) {
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(FatFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto   *file = ::new (mem) FatFile(volume, (flags & Vfs::Append) != 0, identity);
        FRESULT result = f_open(&file->fil(), path, fatMode(flags));
        // Truncate without Create: FatFs has no mode for it.
        if (result == FR_OK && (flags & Vfs::Truncate) && !(flags & Vfs::Create)) {
                result = f_truncate(&file->fil());
                if (result != FR_OK) (void)f_close(&file->fil());
        }
        if (raw != nullptr) *raw = result;
        // A System file may be an emulated link: read it here, on this FIL
        // (the lock is held: a second open, or a handle closed now, can't be).
        FIL &fil = file->fil();
        if (result == FR_OK && link != nullptr && (fil.obj.attr & AM_SYS) != 0 && f_size(&fil) > 10 &&
            f_size(&fil) <= Fs::MaxLinkFile) {
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Fs::MaxLinkFile));
                UINT  got = 0;
                if (buf != nullptr && f_read(&fil, buf, Fs::MaxLinkFile, &got) == FR_OK &&
                    Fs::parseLink(buf, got, *link).isOk()) {
                        Memory::free(buf);
                        (void)f_close(&fil);
                        file->~FatFile();
                        Memory::free(mem);
                        return makeError<FileHandle::Ptr>(Error::Invalid);
                }
                Memory::free(buf);
                link->clear();
                result = f_lseek(&fil, 0);
                if (result != FR_OK) (void)f_close(&fil);
        }
        if (result != FR_OK) {
                Error err = toError(result, volume);
                // FatFs says "no file" (or "denied", when creating) for a directory.
                if (err == Error::NotExist || err == Error::PermissionDenied) {
                        FILINFO info;
                        if (f_stat(path, &info) == FR_OK && (info.fattrib & AM_DIR) != 0) err = Error::IsDir;
                }
                file->~FatFile();
                Memory::free(mem);
                return makeError<FileHandle::Ptr>(err);
        }
        const uint8_t mode =
                ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, mode);
        if (handle.error().isError()) {
                (void)f_close(&file->fil());
                file->~FatFile();
                Memory::free(mem);
        }
        return handle;
}

BMDS_NAMESPACE_END
