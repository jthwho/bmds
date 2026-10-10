/**
 * @file      lfsvolume.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: littlefs volumes, behind the Volume interface
 * (devplan/storage.md §13d). Everything expects the storage lock.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/flashdevice.h>
#include <bmds/vfs.h>

#include "fslayer.h"
#include "lfs.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief A littlefs filesystem while mounted on a flash.
 *
 * A static pool of `MaxVolumes`. littlefs's buffers (Psram) are
 * allocated at mount and freed at unmount. Files and directories still
 * open when it is unmounted go stale: they fail with `NoMedia` and are only
 * freed when closed (a generation number tells them).
 *
 * littlefs lets a file be open twice, each handle with its own view; this
 * volume refuses that as FAT does (`Busy`): a file open for writing can't be
 * opened again, and one open at all can't be opened for writing, removed or
 * renamed.
 */
class LittleFsVolume final : public Volume {
        public:
                /** @brief littlefs filesystems mounted at once. */
                static constexpr size_t MaxVolumes = 4;

                /** @brief littlefs's read and program caches, and each file's buffer. */
                static constexpr uint32_t CacheBytes = 512;

                /** @brief Files open at once on one volume (as FatFs's `FF_FS_LOCK`). */
                static constexpr size_t MaxOpenFiles = 16;

                /** @brief Attribute: the modification time (int64 nanoseconds, UTC). */
                static constexpr uint8_t TimeAttribute = 't';

                /** @brief Attribute: FAT's attribute byte (`FileInfo::Attribute`). */
                static constexpr uint8_t AttributesAttribute = 'a';

                constexpr LittleFsVolume() = default;
                LittleFsVolume(const LittleFsVolume &) = delete;
                LittleFsVolume &operator=(const LittleFsVolume &) = delete;

                /** @brief Takes a free volume; null if all are in use. */
                static LittleFsVolume *acquire();

                /**
                 * @brief Mounts the filesystem on @p flash (@p device's).
                 * @return `Ok`, `NoFilesystem` (none there), `NoMem`, `IoError`, ...
                 */
                Error mount(BlockDevice &device, FlashDevice &flash);

                /** @brief Makes a new, empty filesystem on @p flash. */
                static Error format(FlashDevice &flash);

                /** @brief Returns littlefs's state (for the volume's files). */
                lfs_t &lfs() { return _lfs; }

                /** @brief Returns the flash. */
                FlashDevice &flash() const { return *_flash; }

                /** @brief Returns the mount's generation: a file's must match, else it is stale. */
                uint32_t generation() const { return _generation; }

                /**
                 * @brief Takes the open-file table's entry for @p key (a path's hash).
                 * @return `Ok`, `Busy` (open in a way that excludes this), or `TooManyOpen`.
                 */
                Error claim(uint64_t key, bool writing);

                /** @brief Gives back what `claim()` took. */
                void unclaim(uint64_t key, bool writing);

                // Volume
                void                    release() override;
                BlockDevice            *device() const override { return _device; }
                const char             *typeName() const override { return "littlefs"; }
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

                /** @brief Translates a littlefs result (negative: an error). */
                static Error toError(int result);

                /** @brief Fills @p info's time and attributes from @p path's littlefs attributes. */
                void readAttributes(const char *path, FileInfo &info);

        private:
                struct OpenFile {
                                uint64_t key = 0;
                                uint16_t readers = 0;
                                bool     writer = false;
                };

                void unmount();
                bool isOpen(uint64_t key) const;
                // NotExist with @p noPath when @p path's parent isn't a directory.
                Error notFound(StringView path, bool &noPath);

                lfs_t        _lfs = {};
                lfs_config   _config = {};
                uint8_t     *_buffers = nullptr; // read cache, program cache, lookahead (Psram)
                FlashDevice *_flash = nullptr;
                BlockDevice *_device = nullptr;
                uint32_t     _generation = 0;
                bool         _inUse = false;
                bool         _mounted = false;
                OpenFile     _open[MaxOpenFiles] = {};
};

BMDS_NAMESPACE_END
