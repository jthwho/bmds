/**
 * @file      fatlayer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the FAT volumes (FatFs behind the Volume interface)
 * and what they and the FatFs port share. Everything on FatVolume expects
 * the storage lock (fslayer.h) to be held.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

#include "ff.h"
#include "fslayer.h"

static_assert(FF_VOLUMES <= bmds::Vfs::MaxMounts, "no more FatFs volumes than mounts");

BMDS_NAMESPACE_BEGIN

class SectorCache;

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
class FatVolume final : public Volume {
        public:
                constexpr FatVolume() = default;
                FatVolume(const FatVolume &) = delete;
                FatVolume &operator=(const FatVolume &) = delete;

                /** @brief Takes a free volume; null if all are in use (or no memory). */
                static FatVolume *acquire();

                /** @brief Returns the volume with FatFs drive number @p drive. */
                static FatVolume &at(uint8_t drive);

                /** @brief Mounts the filesystem on @p device. */
                Error mount(BlockDevice &device);

                /** @brief Unmounts; open files and directories go stale. */
                void unmount();

                /** @brief Returns true while mounted. */
                bool isMounted() const { return _mounted; }

                /** @brief Returns FatFs's volume (its geometry); null before the first use. */
                const FATFS *fatfs() const { return _fs; }

                /** @brief Returns true if @p buffer is FatFs's window (its metadata sector) for this volume. */
                bool isWindow(const void *buffer) const { return _fs != nullptr && buffer == _fs->win; }

                /** @brief Returns the FatFs drive number. */
                uint8_t drive() const;

                /** @brief Writes "N:" (the volume's FatFs root) into @p path. */
                void root(char (&path)[3]) const;

                /** @brief Formats @p device (a partition table first if @p partitionTable). */
                static Error format(BlockDevice &device, FsFormat format, bool partitionTable);

                // Volume
                void                    release() override;
                BlockDevice            *device() const override { return _device; }
                const char             *typeName() const override;
                Result<FsInfo>          info() override;
                Error                   stat(StringView path, FileInfo &info, bool &noPath) override;
                Result<FileHandle::Ptr> open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                             String *link) override;
                Result<Reader *>        openReader(StringView path) override;
                void                    closeReader(Reader *reader) override;
                Error                   mkdir(StringView path, bool &noPath) override;
                Error                   rmdir(StringView path) override;
                Error                   remove(StringView path) override;
                Error                   rename(StringView from, StringView to, bool &noPath) override;
                Error                   setModified(StringView path, Timestamp time) override;
                Error                   writeLink(StringView path, ByteSpan contents, bool &noPath) override;
                Result<Cursor *>        openDirectory(StringView path, bool &noPath) override;
                void                    closeDirectory(Cursor *cursor) override;

        private:
                // The FatFs path of @p path on this volume: "N:/dir/file".
                Error fatPath(StringView path, String &out) const;

                FATFS       *_fs = nullptr;
                BlockDevice *_device = nullptr;
                bool         _inUse = false;
                bool         _mounted = false;
};

namespace Fat {

        /** @brief Returns the metadata cache under FatFs (sectorcache.h). Lock held. */
        SectorCache &cache();

        /** @brief Creates the cache (from `Fs::init()`); idempotent. */
        Error init();

        /**
         * @brief Writes back what the mounted volumes hold (devplan/storage.md §3).
         *        Lock held.
         * @return `Ok`, or the first error (what failed is lost; `takeLostLocked()` tells it).
         */
        Error flushAllLocked();

        /** @brief Writes back what drive @p drive holds, to @p device. Lock held. */
        Error flushLocked(uint8_t drive, BlockDevice &device);

        /**
         * @brief Returns the first write-back that failed since the last call
         *        (what it held was lost), and forgets it: `Vfs::sync()` tells it,
         *        as POSIX's `fsync` tells a failed write-back. Lock held.
         */
        Error takeLostLocked();

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
        Result<FileHandle::Ptr> openFile(FatVolume &volume, const char *path, uint8_t flags,
                                         const FileIdentity &identity, FRESULT *raw = nullptr, String *link = nullptr);

} // namespace Fat

BMDS_NAMESPACE_END
