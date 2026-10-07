/**
 * @file      vfs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/builtinprograms.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/timezone.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class BlockDevice;
class Stream;

/** @brief What a path names. */
enum class FileType : uint8_t {
        File,      ///< A regular file.
        Directory, ///< A directory (also `/`, `/bin`, `/dev` and mount points).
        Device,    ///< A device node (`/dev/null`, `/dev/console`).
        Link,      ///< A symbolic link (only from `lstat()` and `Directory::next()`).
};

/** @brief What `Vfs::stat()` and `Directory::next()` report. */
struct FileInfo {
                /** @brief FAT attribute bits. */
                enum Attribute : uint8_t {
                        ReadOnly = 0x01, ///< Read-only.
                        Hidden = 0x02,   ///< Hidden.
                        System = 0x04,   ///< System.
                        Archive = 0x20,  ///< Changed since last backed up.
                };

                String    name;                  ///< The last path component ("" for `/`).
                FileType  type = FileType::File; ///< What it is.
                uint64_t  size = 0;              ///< Size in bytes (0 for directories and devices).
                Timestamp modified;              ///< Last modified (UTC); zero if unknown.
                uint8_t   attributes = 0;        ///< `Attribute` bits.
                /**
                 * Identifies the file: the same for each of its names (`.`, `..`,
                 * case) and every handle of it (`FileHandle::id()`), different for
                 * other files. A hash of its normalized path with letters folded,
                 * so it changes when the file is renamed; for a device node, the
                 * device's object. 0 if unknown.
                 */
                uint64_t id = 0;
                uint32_t device = 0; ///< The filesystem it is on: its mount slot + 1; 0 outside them.

                /** @brief Returns true for directories. */
                bool isDirectory() const { return type == FileType::Directory; }
};

/** @brief What `Vfs::format()` creates. */
enum class FsFormat : uint8_t {
        Auto,  ///< FatFs's choice by size: FAT12/16 when small, FAT32 up to 32 GB, exFAT above (as SD cards ship).
        Fat32, ///< FAT32 (`Invalid` if the device is too small for it, under about 32 MB).
        ExFat, ///< exFAT.
};

/** @brief What `Vfs::statfs()` reports about a mounted filesystem. */
struct FsInfo {
                const char *type = "";        ///< "FAT12", "FAT16", "FAT32" or "exFAT".
                uint64_t    totalBytes = 0;   ///< Capacity of the data area.
                uint64_t    freeBytes = 0;    ///< Free space.
                uint32_t    clusterBytes = 0; ///< Allocation unit.
};

/**
 * @brief An open directory, read entry by entry.
 *
 * From `Vfs::openDirectory()`. Move-only; closes when destroyed. Entries
 * come in the filesystem's order, without `.` and `..`.
 *
 * @par Thread Safety
 * Use one Directory from one task at a time.
 */
class Directory {
        public:
                /** @brief Constructs a closed directory. */
                Directory() = default;

                /** @brief Closes the directory. */
                ~Directory();

                Directory(Directory &&other);
                Directory &operator=(Directory &&other);
                Directory(const Directory &) = delete;
                Directory &operator=(const Directory &) = delete;

                /** @brief Returns true while open. */
                bool isOpen() const { return _state != nullptr; }

                /**
                 * @brief Returns the next entry.
                 * @return The entry, `EndOfFile` after the last, `NoMedia` if the
                 *         card went, or another filesystem error.
                 */
                Result<FileInfo> next();

                /** @brief Closes the directory (also done by the destructor). */
                void close();

                struct State; ///< Implementation (vfs.cpp).

        private:
                friend class VfsAccess;
                State *_state = nullptr;
};

/**
 * @brief The virtual filesystem: one tree of paths over the mounted
 *        filesystems and the device nodes.
 *
 * ```
 * /              virtual: /bin, /dev, and the directories leading to mounts
 * /bin/NAME      a program built into the firmware (BuiltinPrograms): an
 *                empty file, run by the loader; /bin is read-only (ReadOnly)
 * /dev/null      discards writes, reads end at once
 * /dev/zero      reads zeros; /dev/urandom, /dev/random read random bytes
 * /dev/console   the caller's terminal (Context::terminal)
 * /share/zoneinfo  the time zone database (setZoneDatabase()), read-only:
 *                one TZif file per zone name (devplan/time.md §9)
 * /dev/sd0       a block device (BlockDevices); /dev/sd0p0 a partition
 * /sd0/p0        a mounted FAT or exFAT filesystem (the storage service's default path)
 * ```
 *
 * The mount table is run-time state: `mount()` puts a filesystem at any
 * absolute path outside `/dev` and other mounts (up to four); the
 * directories leading to it are virtual. `unmount()` removes the path.
 *
 * Paths are UTF-8 with `/` separators. A relative path is resolved against
 * `Context::cwd`; `.` and `..` are resolved here (`..` never climbs above
 * `/`). FAT names are case-insensitive and case-preserving.
 *
 * Files are Streams behind FileHandles: `dup` and `2>&1` share one offset,
 * and the file closes with its last handle. Files and directories still
 * open when their filesystem is unmounted (its card pulled) fail with
 * `NoMedia` from then on.
 *
 * @par Thread Safety
 * Thread-safe. Every call resolves its path and does its work under one
 * storage lock, so a mount can't change under it; FatFs calls are thereby
 * serialized. `init()` runs first (boot, or a test).
 */
namespace Vfs {

        /** @brief Longest path the VFS handles, in bytes. */
        inline constexpr size_t MaxPath = 1024;

        /** @brief The caller's view: where relative paths start, and its terminal. */
        struct Context {
                        StringView cwd = "/";          ///< Current directory (absolute, normalized).
                        Stream    *terminal = nullptr; ///< What `/dev/console` opens; null for none.
        };

        /** @brief `open()` flags. */
        enum OpenFlag : uint8_t {
                Read = 1u << 0,      ///< Open for reading.
                Write = 1u << 1,     ///< Open for writing.
                Create = 1u << 2,    ///< Create the file if it doesn't exist.
                Truncate = 1u << 3,  ///< Empty an existing file.
                Append = 1u << 4,    ///< Every write goes to the end.
                Exclusive = 1u << 5, ///< With `Create`: fail with `Exists` if it exists.
        };

        /** @brief Longest mount path. */
        inline constexpr size_t MaxMountPath = 63;

        /** @brief Most mounts at once. */
        inline constexpr size_t MaxMounts = 4;

        /** @brief A mount, as `listMounts()` reports it. */
        struct MountInfo {
                        char        path[MaxMountPath + 1] = {}; ///< Where it is mounted.
                        char        device[12] = {};             ///< The device's name ("sd0p0").
                        const char *type = "";                   ///< "fat12", "fat16", "fat32" or "exfat".
        };

        /** @brief Sets up the VFS (the storage lock). Boot stage; idempotent. */
        Error init();

        /**
         * @brief Told that a file's contents or name may have changed: its
         *        `FileInfo::device` and `id`, or `(0, 0)` for every file.
         *
         * Called with the storage lock held, so it must not call the VFS.
         */
        using ChangeHook = void (*)(uint32_t device, uint64_t id);

        /**
         * @brief Installs @p hook (null: none), called when a handle open for
         *        writing closes, a file is removed, renamed (with its old name)
         *        or touched (`setModified()`), and after an unmount (`(0, 0)`:
         *        mount slots move).
         */
        void setChangeHook(ChangeHook hook);

        /**
         * @brief Mounts the FAT or exFAT filesystem on @p device at @p path.
         * @param path Absolute (normalized here); not `/`, not in `/dev`, `/bin`
         *        or `/share/zoneinfo`, not inside or above another mount; at
         *        most `MaxMountPath` bytes.
         * @return `Ok`, `Invalid` (the path), `Exists` (something is there),
         *         `Busy` (@p device, a partition of it or the device it is part of
         *         is mounted already), `Full`, `NoMedia`,
         *         `NoFilesystem`, `IoError`, ...
         */
        Error mount(StringView path, BlockDevice &device);

        /**
         * @brief Unmounts the filesystem at @p path.
         * @return `Ok`, `NotExist` (nothing mounted there), or `Busy` (pinned).
         */
        Error unmount(StringView path);

        /**
         * @brief Pins the mount at @p path: `unmount()` refuses it for good
         *        (`/boot`, `devplan/storage.md` §13).
         * @return `Ok` or `NotExist`.
         */
        Error pin(StringView path);

        /**
         * @brief Returns the program built into the firmware that @p path
         *        names (`/bin/NAME`, or a link to it), or null
         *        (`devplan/programs.md` §17).
         */
        const ProgramEntry *builtinProgram(StringView path, const Context &context);

        /** @brief Returns true if a filesystem is mounted at @p path (absolute). */
        bool isMounted(StringView path);

        /** @brief Released after each mount and unmount: wait on it for a mount (`waitmount`). */
        WaitList &mountWaiters();

        /**
         * @brief Unmounts everything on @p device and its partitions (a medium went).
         * @return How many mounts went.
         */
        size_t unmountDevice(BlockDevice &device);

        /**
         * @brief A medium arrived in registered @p device: registers its
         *        partitions (`Partitions::attach()`) and mounts each one holding
         *        a FAT or exFAT filesystem at `/<device>/p<N>` (`/sd0/p0`).
         * @return How many were mounted, or the error reading the partition table.
         */
        Result<size_t> automount(BlockDevice &device);

        /** @brief @p device's medium went: unmounts everything on it and unregisters its partitions. */
        void eject(BlockDevice &device);

        /** @brief Copies up to `out.size()` mounts, in mount order; returns how many. */
        size_t listMounts(Span<MountInfo> out);

        /** @brief Returns the device at @p path (`/dev/sd0`), or null. */
        BlockDevice *findDevice(StringView path, const Context &context);

        /**
         * @brief Creates a new, empty filesystem on @p device: on a whole
         *        device, a partition table with one partition (GPT from
         *        128 GB, else MBR); on a partition, the filesystem alone.
         * @return `Ok`, `Busy` (mounted), `NoMedia`, `Invalid` (too small), `NoMem`, `IoError`, ...
         */
        Error format(BlockDevice &device, FsFormat format);

        /**
         * @brief Opens a file or device.
         * @param flags `OpenFlag` bits; at least one of `Read`, `Write`.
         * @return A handle (its mode `Read`/`Write` as asked), or `NotExist`,
         *         `IsDir`, `Exists`, `Busy` (open for writing elsewhere),
         *         `PermissionDenied` (creating outside every filesystem),
         *         `TooManyOpen`, `NoMedia`, `ReadOnly`, `Invalid`, ...
         */
        Result<FileHandle::Ptr> open(StringView path, uint8_t flags, const Context &context);

        /**
         * @brief Finds program @p name in the `:`-separated directories of
         *        @p searchPath (`$PATH`; an empty entry is the current
         *        directory), as the shell does: the first file named
         *        `NAME`, `NAME.elf` or `NAME.sh`, a directory at a time.
         * @return `Ok` with its path in @p out, `NotExist`, or `NoMem`.
         */
        Error findProgram(StringView name, StringView searchPath, const Context &context, String &out);

        /**
         * @brief Returns what @p path names, following symbolic links.
         * @return The file, or `NotExist`, `TooManyLinks` (more than 8 in a row), ...
         */
        Result<FileInfo> stat(StringView path, const Context &context);

        /**
         * @brief As `stat()`, but a symbolic link as the last component is
         *        described itself: `FileType::Link`, the size its target's length.
         */
        Result<FileInfo> lstat(StringView path, const Context &context);

        /**
         * @brief Returns the target of the symbolic link @p path (as written:
         *        absolute, or relative to the link's directory).
         * @return The target, or `NotExist`, `Invalid` (not a link), ...
         */
        Result<String> readLink(StringView path, const Context &context);

        /**
         * @brief Makes @p path a symbolic link to @p target (which needn't exist).
         *
         * Emulated (devplan/storage.md §4): a System file holding Cygwin's
         * `!<symlink>` cookie and the target in UTF-16LE.
         *
         * @return `Ok`, or `Exists` (the name is taken), `Invalid` (an empty
         *         or too long target), `PermissionDenied` (outside every
         *         filesystem), `NotExist` (no such directory), ...
         */
        Error symlink(StringView target, StringView path, const Context &context);

        /**
         * @brief Returns the physical path @p path names: normalized, with every
         *        symbolic link followed (for a current directory).
         * @return The path, or `NotExist`, `TooManyLinks`, ...
         */
        Result<String> realPath(StringView path, const Context &context);

        /** @brief Opens a directory for listing. */
        Result<Directory> openDirectory(StringView path, const Context &context);

        /** @brief Creates a directory (`Exists` if anything has that name). */
        Error mkdir(StringView path, const Context &context);

        /** @brief Removes an empty directory (`NotEmpty`, `NotDir`). */
        Error rmdir(StringView path, const Context &context);

        /** @brief Removes a file (`IsDir` for a directory; `Busy` if open). */
        Error remove(StringView path, const Context &context);

        /**
         * @brief Renames or moves @p from to @p to, within one filesystem.
         * @return `Ok`, `Exists` (@p to exists), `NotSupported` (across
         *         filesystems), `NotExist`, `Busy`, ...
         */
        Error rename(StringView from, StringView to, const Context &context);

        /** @brief Sets the modification time of a file or directory. */
        Error setModified(StringView path, Timestamp time, const Context &context);

        /** @brief Returns size and free space of the filesystem holding @p path. */
        Result<FsInfo> statfs(StringView path, const Context &context);

        /**
         * @brief Returns @p path as an absolute path without `.`, `..`, empty
         *        components or a trailing `/`.
         * @return The path, `Invalid` (empty), `OutOfRange` (longer than `MaxPath`), `NoMem`.
         */
        Result<String> normalize(StringView path, StringView cwd);

        // --- The time zone database (devplan/time.md §9) ---------------------

        /** @brief Where the database is shown: one file per zone name below it. */
        inline constexpr StringView ZoneInfoPath("/share/zoneinfo");

        /** @brief Longest database path (`setZoneDatabase()`). */
        inline constexpr size_t MaxZoneDatabasePath = 127;

        /**
         * @brief Uses the bundle at @p path (`zoneinfo.bin`, built by
         *        `tools/tzbundle`) as the time zone database, and retries a
         *        pending kernel zone name with it.
         * @param path Absolute, at most `MaxZoneDatabasePath` bytes; "" for none.
         *        It needn't exist yet (the card may come later).
         * @return `Ok` or `Invalid`.
         */
        Error setZoneDatabase(StringView path);

        /** @brief Returns the database's path ("" for none). */
        Result<String> zoneDatabase();

        /** @brief What the database holds. */
        struct ZoneDatabaseInfo {
                        char     version[9] = {}; ///< The tzdata release, "2026e".
                        uint32_t count = 0;       ///< Zone names, aliases included.
        };

        /**
         * @brief Opens the database and reads its header.
         * @return The facts, `NotExist` (no file there, or no path), `NoMedia`,
         *         `CorruptData` (not a database, or cut short), `NotSupported`
         *         (a later format), ...
         */
        Result<ZoneDatabaseInfo> zoneDatabaseInfo();

        /**
         * @brief Returns the current rules of zone @p name (`Europe/Berlin`)
         *        from the database: its TZif file's footer.
         *
         * The last answer is kept (under the storage lock) until a mount, an
         * unmount, a change to the database or another name, so a caller can
         * ask per line. Blocking (reads the card); not from the storage lock.
         *
         * @return The rules, `NotExist` (the database has no such zone), or
         *         `NotReady` (no database to ask: no card, no file, damaged).
         */
        Result<TimeZone> zoneRules(StringView name);

} // namespace Vfs

BMDS_NAMESPACE_END
