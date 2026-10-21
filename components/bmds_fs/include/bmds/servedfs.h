/**
 * @file      servedfs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Filesystems served by programs (`devplan/netfs.md` step 2): a mount
 *        whose every operation is a request record a program reads from its
 *        server fd, answered by a reply record it writes there (`sshfs`). The
 *        records are the ABI's (`bmds_fs_request_t`, `bmds_fs_reply_t`,
 *        `bmds_fs_entry_t`, `bmds/sys.h`), mirrored here.
 *
 * The volume locks itself (`fslayer.h`): calls run without the storage lock,
 * up to `MaxPending` waiting for replies at once, each up to its timeout.
 * Reads are read ahead and writes gathered, `maxData` bytes at a time;
 * attributes are kept a while. Closing the server fd unmounts it. Several of the
 * server's threads may read its end at once, each given a request of its own (each
 * woken in turn: one wake, passed on while requests are queued, and at the end).
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace ServedFs {

        /** @brief Requests waiting for replies at once. */
        constexpr size_t MaxPending = 16;

        /** @brief The fewest and most bytes a read or write moves. */
        constexpr uint32_t MinData = 4096;
        constexpr uint32_t MaxData = 65536;
        constexpr uint32_t DefaultData = 32768;

        /** @brief The longest path in a request (`Vfs::MaxPath`). */
        constexpr size_t MaxPathBytes = 1024;

        /** @brief The longest type name, with its NUL. */
        constexpr size_t MaxType = 16;

        /** @brief A request's header (`bmds_fs_request_t`), then the paths, then data. */
        struct Request {
                        uint32_t size = 0;
                        uint32_t op = 0;
                        uint32_t id = 0;
                        uint32_t flags = 0;
                        uint64_t handle = 0;
                        uint64_t offset = 0;
                        uint32_t length = 0;
                        uint32_t pathLength = 0;
                        uint32_t path2Length = 0;
                        uint32_t reserved = 0;
                        int64_t  timeNs = 0;
        };
        static_assert(sizeof(Request) == 56, "bmds_fs_request_t");

        /** @brief A reply's header (`bmds_fs_reply_t`), then its payload. */
        struct Reply {
                        uint32_t size = 0;
                        uint32_t id = 0;
                        int32_t  error = 0;
                        uint32_t count = 0;
                        uint64_t handle = 0;
                        uint64_t reserved = 0;
        };
        static_assert(sizeof(Reply) == 32, "bmds_fs_reply_t");

        /** @brief A directory entry or a file's attributes (`bmds_fs_entry_t`), then its name. */
        struct Entry {
                        uint32_t size = 0; // with the name, padded to 8
                        uint32_t type = 0; // BMDS_TYPE_*
                        uint64_t bytes = 0;
                        int64_t  modifiedNs = 0;
                        uint32_t mode = 0;
                        uint32_t nameLength = 0;
        };
        static_assert(sizeof(Entry) == 32, "bmds_fs_entry_t");

        /** @brief `STATFS`'s payload (`bmds_fs_space_t`). */
        struct Info {
                        uint64_t total = 0;
                        uint64_t free = 0;
        };

        /** @brief The ops (`BMDS_FS_*`). */
        enum Op : uint32_t {
                Stat = 1,
                Open = 2,
                Read = 3,
                Write = 4,
                Close = 5,
                ReadDir = 6,
                MakeDir = 7,
                RemoveDir = 8,
                Remove = 9,
                Rename = 10,
                SetTime = 11,
                StatFs = 12,
                Truncate = 13,
                ReadLink = 14,
                SymLink = 15,
        };

        /** @brief `OPEN`'s flags (`BMDS_FS_READ`...), and `RENAME`'s. */
        constexpr uint32_t OpenRead = 1;
        constexpr uint32_t OpenWrite = 2;
        constexpr uint32_t OpenCreate = 4;
        constexpr uint32_t OpenExclusive = 8;
        constexpr uint32_t OpenTruncate = 16;
        constexpr uint32_t RenameReplace = 1;

        /** @brief `BMDS_TYPE_*`. */
        constexpr uint32_t TypeFile = 1;
        constexpr uint32_t TypeDirectory = 2;
        constexpr uint32_t TypeDevice = 3;
        constexpr uint32_t TypeLink = 4;

        /** @brief The buffer a server's read needs for a mount moving @p maxData (`BMDS_FS_REQUEST_MAX`). */
        constexpr size_t requestMax(uint32_t maxData) {
                return sizeof(Request) + 2 * MaxPathBytes + maxData;
        }

        /** @brief A server's error number (a negative `BMDS_E*`) as the kernel's. */
        using ErrorOf = Error (*)(int32_t error);

        /** @brief How to serve a mount (`bmds_fs_serve_t`, checked). */
        struct Config {
                        StringView path; ///< Where: normalized, absolute.
                        StringView type; ///< Its type's name (`df`).
                        uint32_t   maxData = DefaultData;
                        uint32_t   timeoutMs = 30'000; ///< A request's longest wait (`netfs/timeout`).
                        uint32_t   attrMs = 1000;      ///< How long an attribute is kept (`netfs/attr-ms`).
                        ErrorOf    errorOf = nullptr;  ///< The server's numbers as errors (the loader's).
        };

        /**
         * @brief Mounts a served filesystem at `config.path`.
         * @return The server's end (read requests, write replies; closing it
         *         unmounts), or `Invalid`, `Exists`, `Busy`, `Full`, `NoMem`.
         */
        Result<FileHandle::Ptr> serve(const Config &config);

} // namespace ServedFs

BMDS_NAMESPACE_END
