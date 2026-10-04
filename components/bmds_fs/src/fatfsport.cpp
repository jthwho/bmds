/**
 * @file      fatfsport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What FatFs asks of its system: disk I/O (onto a FatVolume's BlockDevice),
 * memory (Psram), the time (the kernel Clock); and the FAT layer's shared
 * pieces (the lock, the drive table, result and time conversions).
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>

#include "fatlayer.h"
// After ff.h (from fatlayer.h), which defines its types.
#include "diskio.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<Mutex> storageLock;

        // FatFs calls these with the storage lock held (every FatFs call is).
        BlockDevice *deviceOf(BYTE pdrv) {
                if (pdrv >= FF_VOLUMES) return nullptr;
                return FatVolume::at(pdrv).device();
        }

        DRESULT toDiskResult(Error err) {
                switch (err.code()) {
                        case Error::Ok: return RES_OK;
                        case Error::NoMedia: return RES_NOTRDY;
                        case Error::ReadOnly: return RES_WRPRT;
                        case Error::Invalid:
                        case Error::OutOfRange: return RES_PARERR;
                        default: return RES_ERROR;
                }
        }

} // namespace

Mutex &Fat::lock() {
        return *storageLock;
}

Error Fat::init() {
        if (!storageLock.isCreated()) storageLock.create();
        return Error();
}

Error Fat::toError(FRESULT result) {
        switch (result) {
                case FR_OK: return Error();
                case FR_DISK_ERR: return Error::IoError;
                case FR_INT_ERR: return Error::CorruptData;
                case FR_NOT_READY: return Error::NoMedia;
                case FR_NO_FILE:
                case FR_NO_PATH: return Error::NotExist;
                case FR_INVALID_NAME: return Error::Invalid;
                case FR_DENIED: return Error::PermissionDenied;
                case FR_EXIST: return Error::Exists;
                case FR_INVALID_OBJECT: return Error::NoMedia; // Stale: its volume was unmounted.
                case FR_WRITE_PROTECTED: return Error::ReadOnly;
                case FR_INVALID_DRIVE: return Error::NotExist;
                case FR_NOT_ENABLED: return Error::NoMedia;
                case FR_NO_FILESYSTEM: return Error::NoFilesystem;
                case FR_MKFS_ABORTED: return Error::Invalid;
                case FR_TIMEOUT: return Error::Timeout;
                case FR_LOCKED: return Error::Busy;
                case FR_NOT_ENOUGH_CORE: return Error::NoMem;
                case FR_TOO_MANY_OPEN_FILES: return Error::TooManyOpen;
                case FR_INVALID_PARAMETER: return Error::Invalid;
        }
        return Error::LibraryFailure;
}

Error Fat::toError(FRESULT result, const FatVolume &volume) {
        if (result == FR_DISK_ERR && (volume.device() == nullptr || !volume.device()->isPresent()))
                return Error::NoMedia;
        return toError(result);
}

Timestamp Fat::toTimestamp(WORD fdate, WORD ftime) {
        if (fdate == 0) return Timestamp{};
        DateTime local;
        local.year = 1980 + (fdate >> 9);
        local.month = static_cast<uint8_t>((fdate >> 5) & 0xf);
        local.day = static_cast<uint8_t>(fdate & 0x1f);
        local.hour = static_cast<uint8_t>(ftime >> 11);
        local.minute = static_cast<uint8_t>((ftime >> 5) & 0x3f);
        local.second = static_cast<uint8_t>((ftime & 0x1f) * 2);
        if (!local.isValid()) return Timestamp{};
        return Timestamp::fromSeconds(Clock::timeZone().toUtc(local.toSeconds()));
}

DWORD Fat::toFatTime(Timestamp time) {
        const DateTime d = Clock::localTime(time).local;
        // FAT dates run from 1980 to 2107.
        if (d.year < 1980) return (1u << 21) | (1u << 16);
        if (d.year > 2107) return (127u << 25) | (12u << 21) | (31u << 16) | (23u << 11) | (59u << 5) | 29u;
        return (static_cast<DWORD>(d.year - 1980) << 25) | (static_cast<DWORD>(d.month) << 21) |
               (static_cast<DWORD>(d.day) << 16) | (static_cast<DWORD>(d.hour) << 11) |
               (static_cast<DWORD>(d.minute) << 5) | (static_cast<DWORD>(d.second) / 2);
}

Error Fat::toFileInfo(const FILINFO &entry, FileInfo &info) {
        const Error err = info.name.assign(StringView(entry.fname));
        if (err.isError()) return err;
        info.type = (entry.fattrib & AM_DIR) != 0 ? FileType::Directory : FileType::File;
        info.size = info.type == FileType::File ? static_cast<uint64_t>(entry.fsize) : 0;
        info.modified = toTimestamp(entry.fdate, entry.ftime);
        info.attributes = entry.fattrib & (AM_RDO | AM_HID | AM_SYS | AM_ARC);
        return Error();
}

BMDS_NAMESPACE_END

// FatFs's system interface (C linkage, declared in ff.h and diskio.h).

using namespace bmds;

extern "C" DSTATUS disk_status(BYTE pdrv) {
        const BlockDevice *device = deviceOf(pdrv);
        if (device == nullptr) return STA_NOINIT | STA_NODISK;
        return device->isPresent() ? 0 : (STA_NOINIT | STA_NODISK);
}

extern "C" DSTATUS disk_initialize(BYTE pdrv) {
        // The device's owner brings it up; FatFs only asks whether it's there.
        return disk_status(pdrv);
}

extern "C" DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
        BlockDevice *device = deviceOf(pdrv);
        if (device == nullptr) return RES_PARERR;
        return toDiskResult(
                device->read(sector, WritableByteSpan(buff, static_cast<size_t>(count) * device->sectorSize())));
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
        BlockDevice *device = deviceOf(pdrv);
        if (device == nullptr) return RES_PARERR;
        return toDiskResult(device->write(sector, ByteSpan(buff, static_cast<size_t>(count) * device->sectorSize())));
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
        BlockDevice *device = deviceOf(pdrv);
        if (device == nullptr) return RES_PARERR;
        switch (cmd) {
                case CTRL_SYNC: return toDiskResult(device->sync());
                case GET_SECTOR_COUNT: *static_cast<LBA_t *>(buff) = device->sectorCount(); return RES_OK;
                case GET_SECTOR_SIZE:
                        *static_cast<WORD *>(buff) = static_cast<WORD>(device->sectorSize());
                        return RES_OK;
                case GET_BLOCK_SIZE: *static_cast<DWORD *>(buff) = device->eraseBlockSectors(); return RES_OK;
                case CTRL_TRIM: {
                        // Inclusive range; trimming is advisory.
                        const LBA_t *range = static_cast<const LBA_t *>(buff);
                        (void)device->trim(range[0], range[1] - range[0] + 1);
                        return RES_OK;
                }
                default: return RES_PARERR;
        }
}

extern "C" DWORD get_fattime(void) {
        return Fat::toFatTime(Clock::now());
}

extern "C" void *ff_memalloc(UINT msize) {
        // Long-name working buffers: per call, never on a hot path.
        return Memory::alloc(MemSpace::Psram, msize);
}

extern "C" void ff_memfree(void *mblock) {
        Memory::free(mblock);
        return;
}
