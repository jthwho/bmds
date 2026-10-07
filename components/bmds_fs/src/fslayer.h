/**
 * @file      fslayer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the storage lock, the block device registry, the
 * change hook, and the interface every filesystem implements for the VFS
 * (devplan/storage.md §13d). Functions named `...Locked` and everything on a
 * Volume expect the storage lock to be held.
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
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A mounted filesystem, as the VFS uses it.
 *
 * Paths are within the volume: "" for its root, else "/dir/file"
 * (normalized, no links followed). Where an operation reports @p noPath, it
 * is set when the failure is a directory along the path that is missing or
 * is a file (the VFS then looks for a symbolic link along the way); the
 * error is `NotExist` then.
 *
 * Implementations come from static pools (`FatVolume`, `LittleFsVolume`),
 * acquired when mounted and released when unmounted; never deleted through
 * this interface. Everything expects the storage lock.
 */
class Volume {
        public:
                /** @brief A file opened for plain reads (link files, the zone database). */
                class Reader {
                        public:
                                /** @brief Returns the file's size. */
                                virtual uint64_t size() const = 0;

                                /**
                                 * @brief Reads up to `buf.size()` bytes at @p offset into @p buf.
                                 * @return The bytes read (fewer at the end), or an error.
                                 */
                                virtual Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) = 0;

                        protected:
                                ~Reader() = default;
                };

                /** @brief An open directory's position. */
                class Cursor {
                        public:
                                /**
                                 * @brief Fills @p info (name, type, size, time, attributes) with
                                 *        the next entry, without `.` and `..`.
                                 * @return `Ok`, `EndOfFile` after the last, or an error.
                                 */
                                virtual Error next(FileInfo &info) = 0;

                        protected:
                                ~Cursor() = default;
                };

                /** @brief Unmounts and returns the volume to its pool. */
                virtual void release() = 0;

                /** @brief Returns the device it is mounted on. */
                virtual BlockDevice *device() const = 0;

                /** @brief Returns its type for `listMounts()` ("fat32", "littlefs", ...). */
                virtual const char *typeName() const = 0;

                /** @brief Returns its type, size and free space. */
                virtual Result<FsInfo> info() = 0;

                /** @brief Describes @p path (not the root): name, type, size, time, attributes. */
                virtual Error stat(StringView path, FileInfo &info, bool &noPath) = 0;

                /**
                 * @brief Opens file @p path as a stream behind a FileHandle.
                 * @param flags  `Vfs::OpenFlag` bits.
                 * @param device, id The file's identity (`FileInfo`): opened for writing,
                 *               its last handle's close calls the change hook with them.
                 * @param link   If not null and the file is an emulated symbolic link
                 *               (a System file holding a link, §4): its target, and
                 *               `Invalid` (the file isn't opened).
                 * @return The handle, or `NotExist`, `IsDir`, `Exists`, `Busy`, ...
                 */
                virtual Result<FileHandle::Ptr> open(StringView path, uint8_t flags, uint32_t device, uint64_t id,
                                                     bool &noPath, String *link) = 0;

                /** @brief Opens file @p path for `readAt()`; close it with `closeReader()`. */
                virtual Result<Reader *> openReader(StringView path) = 0;

                /** @brief Closes @p reader (null is fine). */
                virtual void closeReader(Reader *reader) = 0;

                /** @brief Makes directory @p path (`Exists` if anything has the name). */
                virtual Error mkdir(StringView path, bool &noPath) = 0;

                /** @brief Removes directory @p path, a directory (`NotEmpty`). */
                virtual Error rmdir(StringView path) = 0;

                /** @brief Removes file @p path, a file (`Busy` if open). */
                virtual Error remove(StringView path) = 0;

                /** @brief Renames @p from to @p to (`Exists` if @p to is there; `Busy` if open). */
                virtual Error rename(StringView from, StringView to, bool &noPath) = 0;

                /** @brief Sets @p path's modification time. */
                virtual Error setModified(StringView path, Timestamp time) = 0;

                /**
                 * @brief Makes @p path a new System file holding @p contents (an emulated
                 *        symbolic link, §4).
                 * @return `Ok`, `Exists`, `Invalid` (the name), `NotExist`, `NoSpace`, ...
                 */
                virtual Error writeLink(StringView path, ByteSpan contents, bool &noPath) = 0;

                /** @brief Opens directory @p path ("" for the root); close it with `closeDirectory()`. */
                virtual Result<Cursor *> openDirectory(StringView path, bool &noPath) = 0;

                /** @brief Closes @p cursor (null is fine). */
                virtual void closeDirectory(Cursor *cursor) = 0;

        protected:
                constexpr Volume() = default;
                ~Volume() = default;
};

namespace Fs {

        /**
         * @brief The storage lock: the registry, the mount table, and every
         *        filesystem call (all volumes).
         */
        Mutex &lock();

        /** @brief Creates the lock and the filesystems' shared state (from `Vfs::init()`); idempotent. */
        Error init();

        /**
         * @brief Calls the change hook, if any (`Vfs::setChangeHook()`), and
         *        tells the zone database (zoneinfo.h). Lock held.
         */
        void changedLocked(uint32_t device, uint64_t id);

        /** @brief Returns the identity (`FileInfo::id`) of normalized path @p normalized. */
        uint64_t pathId(StringView normalized);

        /**
         * @brief Opens the regular file at normalized @p path for plain reads
         *        (links aren't followed). Lock held.
         * @param modified [out] If not null: the file's modification time.
         * @return `Ok` (@p volume and @p reader set: close it with the volume),
         *         `NotExist`, `IsDir`, `NoMedia`, ...
         */
        Error openReaderLocked(StringView path, Volume *&volume, Volume::Reader *&reader,
                               Timestamp *modified = nullptr);

        /** @brief The largest file that can be an emulated symbolic link. */
        constexpr size_t MaxLinkFile = 10 + 2 + 2 * 1024 + 2;

        /** @brief True if a file with @p attributes, @p type and @p size may be an emulated link. */
        bool isLinkCandidate(uint8_t attributes, FileType type, uint64_t size);

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

} // namespace Fs

BMDS_NAMESPACE_END
