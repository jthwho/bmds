/**
 * @file      fatlayer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the storage lock, the FAT volumes, and what the VFS,
 * the registry and the FatFs port share. Functions named `...Locked` and
 * everything on FatVolume expect the storage lock to be held.
 */

#pragma once

#include <cstdint>
#include <bmds/blockdevices.h>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

#include "ff.h"

static_assert(FF_VOLUMES == bmds::Vfs::MaxMounts, "one FatFs volume per mount");

BMDS_NAMESPACE_BEGIN

/**
 * @brief One of the `FF_VOLUMES` FatFs volumes: a FAT filesystem while
 *        mounted on a block device.
 *
 * A static pool. Each volume's FatFs object (`FATFS`, Psram) is allocated
 * the first time it is used and kept, so files and directories left open
 * when it is unmounted fail FatFs's validity check (their volume id no
 * longer matches) even after the volume is mounted on another device.
 * Everything here expects the storage lock.
 */
class SectorCache;

class FatVolume {
        public:
                constexpr FatVolume() = default;
                FatVolume(const FatVolume &) = delete;
                FatVolume &operator=(const FatVolume &) = delete;

                /** @brief Takes a free volume; null if all are in use (or no memory). */
                static FatVolume *acquire();

                /** @brief Returns the volume with FatFs drive number @p drive. */
                static FatVolume &at(uint8_t drive);

                /** @brief Returns the volume to the pool (unmounted). */
                void release();

                /** @brief Mounts the filesystem on @p device. */
                Error mount(BlockDevice &device);

                /** @brief Unmounts; open files and directories go stale. */
                void unmount();

                /** @brief Returns true while mounted. */
                bool isMounted() const { return _mounted; }

                /** @brief Returns the device while mounted (or formatting); else null. */
                BlockDevice *device() const { return _device; }

                /** @brief Returns true if @p buffer is FatFs's window (its metadata sector) for this volume. */
                bool isWindow(const void *buffer) const { return _fs != nullptr && buffer == _fs->win; }

                /** @brief Returns the FatFs drive number. */
                uint8_t drive() const;

                /** @brief Returns the filesystem type, size and free space. */
                Result<FsInfo> info();

                /** @brief Returns "fat12", "fat16", "fat32" or "exfat" while mounted; else "". */
                const char *typeName() const;

                /** @brief Writes "N:" (the volume's FatFs root) into @p path. */
                void root(char (&path)[3]) const;

                /** @brief Formats @p device (a partition table first if @p partitionTable). */
                static Error format(BlockDevice &device, FsFormat format, bool partitionTable);

        private:
                FATFS       *_fs = nullptr;
                BlockDevice *_device = nullptr;
                bool         _inUse = false;
                bool         _mounted = false;
};

namespace Fat {

        /**
         * @brief The storage lock: the registry, the mount table, and every
         *        FatFs call (all volumes).
         *
         * FatFs's own locking is off (`FF_FS_REENTRANT 0`): it doesn't cover
         * f_mount()/f_mkfs(), and a card can be unmounted at any moment.
         */
        Mutex &lock();

        /** @brief Returns the metadata cache under FatFs (sectorcache.h). Lock held. */
        SectorCache &cache();

        /** @brief Creates the lock and the cache (from `Vfs::init()`); idempotent. */
        Error init();

        /** @brief Translates a FatFs result. `FR_INVALID_OBJECT` (stale after unmount) is `NoMedia`. */
        Error toError(FRESULT result);

        /**
         * @brief Translates a FatFs result for an operation on @p volume: a
         *        disk error with the medium gone is `NoMedia`.
         */
        Error toError(FRESULT result, const FatVolume &volume);

        /** @brief Converts a FAT date and time (local, kernel zone) to UTC. */
        Timestamp toTimestamp(WORD fdate, WORD ftime);

        /** @brief Converts UTC to a packed FAT date (high 16 bits) and time (low 16), in the kernel zone. */
        DWORD toFatTime(Timestamp time);

        /** @brief Fills @p info from a FatFs directory entry. */
        Error toFileInfo(const FILINFO &entry, FileInfo &info);

        /**
         * @brief Opens the file at @p path (a FatFs path on @p volume) as a
         *        stream behind a FileHandle. Lock held.
         * @param flags `Vfs::OpenFlag` bits.
         * @param device, id The file's identity (`FileInfo`): opened for writing,
         *              its last handle's close calls the change hook with them.
         * @param raw   [out] FatFs's result, if not null (`FR_NO_PATH` tells a
         *              missing directory from a missing file).
         * @param link  [out] If not null and the file is an emulated symbolic
         *              link: its target, and the file is closed again (`Invalid`).
         */
        Result<FileHandle::Ptr> openFile(FatVolume &volume, const char *path, uint8_t flags, uint32_t device,
                                         uint64_t id, FRESULT *raw = nullptr, String *link = nullptr);

        /**
         * @brief Calls the change hook, if any (`Vfs::setChangeHook()`), and
         *        tells the zone database (zoneinfo.h). Lock held.
         */
        void changedLocked(uint32_t device, uint64_t id);

        /** @brief Returns the identity (`FileInfo::id`) of normalized path @p normalized. */
        uint64_t pathId(StringView normalized);

        /**
         * @brief Opens the regular file at normalized @p path for reading,
         *        on @p file (links aren't followed). Lock held.
         * @param modified [out] If not null: the file's modification time.
         * @return `Ok` (@p volume set; close @p file), `NotExist`, `IsDir`, `NoMedia`, ...
         */
        Error openPlainLocked(StringView path, FIL &file, FatVolume *&volume, Timestamp *modified = nullptr);

        /** @brief The largest file that can be an emulated symbolic link. */
        constexpr size_t MaxLinkFile = 10 + 2 + 2 * 1024 + 2;

        /**
         * @brief The target of an emulated symbolic link from the file's first
         *        @p size bytes (devplan/storage.md §4).
         * @return `Ok`, or `Invalid` if it isn't one.
         */
        Error parseLink(const uint8_t *data, size_t size, String &target);

        // The block device registry, lock held.

        /** @brief Returns the entry for @p device, or null. */
        const BlockDevices::Entry *findEntryLocked(const BlockDevice &device);

        /** @brief Returns the entry named @p name, or null. */
        const BlockDevices::Entry *findEntryLocked(StringView name);

        /** @brief Registers @p device (see `BlockDevices::add()`). */
        Error addDeviceLocked(BlockDevice &device, StringView name, BlockDevice *parent);

        /** @brief Unregisters @p device. */
        void removeDeviceLocked(BlockDevice &device);

        /** @brief Returns the number of registered devices. */
        size_t deviceCountLocked();

        /** @brief Returns registered device @p index (less than `deviceCountLocked()`). */
        const BlockDevices::Entry &deviceAtLocked(size_t index);

} // namespace Fat

BMDS_NAMESPACE_END
