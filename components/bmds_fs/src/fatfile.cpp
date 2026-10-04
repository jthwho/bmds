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
                        FatFile(FatVolume &volume, bool append) : _volume(volume), _append(append) {}

                        FIL &fil() { return _fil; }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fat::lock(), WaitForever);
                                UINT          got = 0;
                                const FRESULT result = f_read(&_fil, buf.data(), static_cast<UINT>(buf.size()), &got);
                                if (result != FR_OK) return makeError<size_t>(Fat::toError(result, _volume));
                                if (got == 0 && !buf.isEmpty()) return makeError<size_t>(Error::EndOfFile);
                                return makeResult(static_cast<size_t>(got));
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                Mutex::Locker lock(Fat::lock(), WaitForever);
                                FRESULT       result = FR_OK;
                                if (_append) result = f_lseek(&_fil, f_size(&_fil));
                                UINT put = 0;
                                if (result == FR_OK)
                                        result = f_write(&_fil, data.data(), static_cast<UINT>(data.size()), &put);
                                if (result != FR_OK) return makeError<size_t>(Fat::toError(result, _volume));
                                // FatFs writes short only when the volume is full.
                                if (put < data.size()) return makeError<size_t>(Error::NoSpace);
                                return makeResult(static_cast<size_t>(put));
                        }

                        Error flush(unsigned int) override {
                                Mutex::Locker lock(Fat::lock(), WaitForever);
                                return Fat::toError(f_sync(&_fil), _volume);
                        }

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fat::lock(), WaitForever);
                                int64_t       base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(f_tell(&_fil));
                                if (from == SeekFrom::End) base = static_cast<int64_t>(f_size(&_fil));
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                const FRESULT result = f_lseek(&_fil, static_cast<FSIZE_t>(base + offset));
                                if (result != FR_OK) return makeError<uint64_t>(Fat::toError(result, _volume));
                                return makeResult(static_cast<uint64_t>(f_tell(&_fil)));
                        }

                        void handleClosed(uint8_t) override {
                                {
                                        Mutex::Locker lock(Fat::lock(), WaitForever);
                                        // A stale file (its card went) just fails here.
                                        (void)f_close(&_fil);
                                }
                                this->~FatFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        FIL        _fil = {};
                        FatVolume &_volume;
                        const bool _append;
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

Result<FileHandle::Ptr> Fat::openFile(FatVolume &volume, const char *path, uint8_t flags) {
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(FatFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto   *file = ::new (mem) FatFile(volume, (flags & Vfs::Append) != 0);
        FRESULT result = f_open(&file->fil(), path, fatMode(flags));
        // Truncate without Create: FatFs has no mode for it.
        if (result == FR_OK && (flags & Vfs::Truncate) && !(flags & Vfs::Create)) {
                result = f_truncate(&file->fil());
                if (result != FR_OK) (void)f_close(&file->fil());
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
