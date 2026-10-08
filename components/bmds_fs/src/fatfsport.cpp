/**
 * @file      fatfsport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What FatFs asks of its system: disk I/O (onto a FatVolume's BlockDevice),
 * memory (Psram), the time (the kernel Clock); and the FAT layer's shared
 * pieces (the metadata cache, result and time conversions).
 */

#include <atomic>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/thread.h>

#include "fatlayer.h"
#include "sectorcache.h"
// After ff.h (from fatlayer.h), which defines its types.
#include "diskio.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit SectorCache sectorCache;

        // Writing metadata back (devplan/storage.md §3): a held write waits at most this long, for the
        // flusher thread (a kernel thread, started at boot: one a program's task started would end
        // with the program).
        constexpr unsigned int            FlushDelayMs = 1000;
        constinit StaticObject<Semaphore> flushWake;
        constinit Thread                  flusher;
        constinit std::atomic<bool>       flushScheduled{false};
        constinit bool                    flusherRunning = false; // set at boot
        constinit Error::Code             lostError = Error::Ok;  // a write-back failed: told by the next sync

        void flusherMain(void *) {
                for (;;) {
                        (void)flushWake->acquire(WaitForever);
                        Thread::sleep(FlushDelayMs);
                        Mutex::Locker lock(Fs::lock(), WaitForever);
                        flushScheduled.store(false, std::memory_order_release); // a hold now schedules again
                        (void)Fat::flushAllLocked();
                }
        }

        // A write held: the flusher woken, once until it flushes. Under the lock.
        void scheduleFlush() {
                if (flushScheduled.exchange(true, std::memory_order_acq_rel)) return;
                if (!flusherRunning) {
                        // No flusher (it couldn't start): written back now rather than never.
                        flushScheduled.store(false, std::memory_order_release);
                        (void)Fat::flushAllLocked();
                        return;
                }
                flushWake->release();
                return;
        }

        // True if a write of @p count sectors at @p sector lands in a cluster whose free is held: used
        // on the card by a file removed since. Written first, the free must reach the card before
        // (devplan/storage.md §3): a cut would leave the old entry pointing at new bytes.
        bool landsOnFreeHeld(uint8_t drive, const FATFS &fs, LBA_t sector, UINT count) {
                if (!sectorCache.hasFreesHeld() || fs.fs_type == 0 || sector + count <= fs.database) return false;
                const LBA_t first = sector < fs.database ? fs.database : sector;
                const DWORD from = DWORD((first - fs.database) / fs.csize) + 2;
                const DWORD to = DWORD((sector + count - 1 - fs.database) / fs.csize) + 2;
                for (DWORD c = from; c <= to && c < fs.n_fatent; ++c) {
                        bool held = false;
                        if (fs.fs_type == FS_EXFAT) {
                                const DWORD bit = c - 2;
                                held = sectorCache.isFreeHeld(drive, fs.bitbase + bit / (FF_MAX_SS * 8),
                                                              bit % (FF_MAX_SS * 8));
                        } else {
                                const DWORD per = fs.fs_type == FS_FAT16 ? FF_MAX_SS / 2 : FF_MAX_SS / 4;
                                held = sectorCache.isFreeHeld(drive, fs.fatbase + c / per, c % per);
                        }
                        if (held) return true;
                }
                return false;
        }

        // How sector @p sector of volume @p fs is written back; false: at once (FAT12, or not mounted).
        bool kindOf(const FATFS &fs, LBA_t sector, SectorCache::Kind &kind) {
                if (fs.fs_type == 0 || fs.fs_type == FS_FAT12) return false;
                if (sector < fs.fatbase) {
                        kind = SectorCache::Kind::Other; // boot sectors, FSInfo
                        return true;
                }
                if (sector < fs.fatbase + LBA_t(fs.fsize) * fs.n_fats) {
                        kind = fs.fs_type == FS_FAT16 ? SectorCache::Kind::Fat16 : SectorCache::Kind::Fat32;
                        return true;
                }
                if (fs.fs_type == FS_EXFAT) {
                        const LBA_t bitmapSectors = ((LBA_t(fs.n_fatent) - 2 + 7) / 8 + FF_MAX_SS - 1) / FF_MAX_SS;
                        if (sector >= fs.bitbase && sector < fs.bitbase + bitmapSectors) {
                                kind = SectorCache::Kind::Bitmap;
                                return true;
                        }
                }
                kind = SectorCache::Kind::Directory;
                return true;
        }

        static_assert(FF_MIN_SS == SectorCache::SectorBytes && FF_MAX_SS == SectorCache::SectorBytes);

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

SectorCache &Fat::cache() {
        return sectorCache;
}

Error Fat::init() {
        Error err = sectorCache.init();
        if (err.isError() || flushWake.isCreated()) return err;
        flushWake.create(1u, 0u);
        // For good: never joined (a kernel thread, as the boot stage starts it).
        Thread::Config config;
        config.name = "fsflush";
        config.stackBytes = 4096;
        config.stackSpace = MemSpace::Psram; // the card is written by its driver's task, not this one
        config.priority = 3;                 // the storage service's
        // Without it, a held write is written back at once (scheduleFlush()).
        flusherRunning = flusher.start(config, flusherMain, nullptr).isOk();
        return Error();
}

Error Fat::flushLocked(uint8_t drive, BlockDevice &device) {
        const Error err = sectorCache.flush(drive, device);
        if (err.isError() && lostError == Error::Ok) lostError = err.code(); // for the next sync
        return err;
}

Error Fat::takeLostLocked() {
        const Error err = lostError;
        lostError = Error::Ok;
        return err;
}

Error Fat::flushAllLocked() {
        Error first;
        for (uint8_t d = 0; d < FF_VOLUMES; ++d) {
                FatVolume &v = FatVolume::at(d);
                if (!v.isMounted() || v.device() == nullptr || !sectorCache.hasHeld(d)) continue;
                const Error err = flushLocked(d, *v.device());
                if (first.isOk()) first = err;
        }
        return first;
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
        // What FatFs reads into its window is metadata (FF_FS_TINY is off:
        // file data goes through each file's own buffer or straight to the
        // caller's). It is cached, read ahead on a miss, unless the device is
        // memory itself (devplan/storage.md §3).
        if (count == 1 && FatVolume::at(pdrv).isWindow(buff) && !device->isMemory()) {
                if (sectorCache.read(pdrv, sector, buff)) return RES_OK;
                return toDiskResult(sectorCache.fill(pdrv, *device, sector, buff));
        }
        return toDiskResult(
                device->read(sector, WritableByteSpan(buff, static_cast<size_t>(count) * device->sectorSize())));
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
        BlockDevice *device = deviceOf(pdrv);
        if (device == nullptr) return RES_PARERR;
        // Metadata from the window of a mounted volume on a card: held, written back within a second
        // in an order a cut can't harm (devplan/storage.md §3). Data is written now, so it reaches
        // the card before what points at it.
        const FatVolume  &volume = FatVolume::at(pdrv);
        SectorCache::Kind kind = SectorCache::Kind::Directory;
        // Data or a directory into a cluster freed but not written freed yet: the free first.
        if (volume.isMounted() && !device->isMemory() && landsOnFreeHeld(pdrv, *volume.fatfs(), sector, count)) {
                const Error flushed = Fat::flushLocked(pdrv, *device);
                if (flushed.isError()) return toDiskResult(flushed);
        }
        if (count == 1 && volume.isWindow(buff) && volume.isMounted() && !device->isMemory() &&
            kindOf(*volume.fatfs(), sector, kind)) {
                Error err = sectorCache.hold(pdrv, *device, sector, buff, kind);
                if (err == Error::Full) {
                        err = Fat::flushLocked(pdrv, *device);
                        if (err.isOk()) err = sectorCache.hold(pdrv, *device, sector, buff, kind);
                }
                if (err.isOk()) {
                        scheduleFlush();
                        return RES_OK;
                }
                if (err != Error::NoMem) return toDiskResult(err); // no cache: written now, below
        }
        const Error err = device->write(sector, ByteSpan(buff, static_cast<size_t>(count) * device->sectorSize()));
        // Write-through: the cached copies follow the device, or go when its
        // contents are uncertain. Memory devices have none.
        if (device->isMemory()) return toDiskResult(err);
        if (err.isOk())
                sectorCache.written(pdrv, sector, count, buff);
        else
                sectorCache.discard(pdrv, sector, count);
        return toDiskResult(err);
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
                        sectorCache.discard(pdrv, range[0], range[1] - range[0] + 1);
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
