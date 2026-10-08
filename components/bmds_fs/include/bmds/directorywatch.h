/**
 * @file      directorywatch.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Tells of changes in one folder (`devplan/filebrowser.md` §4i): an
 *        entry made, removed or renamed in it, a file in it written, a mount.
 *
 * The caller owns the watch; the VFS keeps it in a list while it watches
 * (nothing allocated, no limit). Changes accumulate as bits until
 * `takeChanges()` takes them, however many come; the notify, called at each
 * change, only wakes the owner.
 *
 * Not told: the folders below one renamed or removed (a watch doesn't know its
 * ancestors), and `/dev` gaining a device.
 *
 * @par Thread Safety
 * `watch()`, `stop()` and the destructor from the owner's thread (they take the
 * storage lock); `takeChanges()` from any thread. The notify runs in whatever
 * task made the change, with the storage lock held.
 */
class DirectoryWatch {
        public:
                /** @brief What changed (bits). */
                enum Change : uint8_t {
                        Entries = 1u << 0,  ///< A folder or link made; an entry removed or renamed; the folder itself.
                        Contents = 1u << 1, ///< A file closed after writing (a file made too), or touched.
                        Mounts = 1u << 2,   ///< A mount, an unmount, or `/home` moved: anything may have.
                };

                /**
                 * @brief Told of a change: under the storage lock, in the task that
                 *        made it. Must not call the VFS or block.
                 */
                using Notify = void (*)(void *arg);

                DirectoryWatch() = default;

                /** @brief Stops watching. */
                ~DirectoryWatch() { stop(); }

                DirectoryWatch(const DirectoryWatch &) = delete;
                DirectoryWatch &operator=(const DirectoryWatch &) = delete;

                /** @brief Sets what is told of changes (before `watch()`). */
                void setNotify(Notify fn, void *arg) {
                        _notify = fn;
                        _arg = arg;
                        return;
                }

                /**
                 * @brief Watches folder @p path (relative to @p context's cwd; links and
                 *        `/home` followed), instead of the one watched before.
                 *
                 * Start it before reading the folder: no change then falls between.
                 * The changes not taken yet are kept.
                 * @return `Ok`, or `NotDir`, `NotExist` or another error finding it:
                 *         the watch is then listed with no folder, told of mounts only
                 *         (after which the folder may be there: call this again).
                 */
                Error watch(StringView path, const Vfs::Context &context);

                /** @brief Stops watching (the changes not taken are kept). */
                void stop();

                /** @brief Returns true while listed (since `watch()`, until `stop()`). */
                bool isWatching() const { return _listed; }

                /** @brief Returns the changes since the last call (`Change` bits), clearing them. @nonblocking */
                uint8_t takeChanges() { return _changes.exchange(0, std::memory_order_acq_rel); }

                /** @brief Implementation (directorywatch.cpp): the storage lock held. */
                struct Access;

        private:
                friend struct Access;

                DirectoryWatch      *_prev = nullptr; // the VFS's list, under the storage lock
                DirectoryWatch      *_next = nullptr;
                uint64_t             _folder = 0; // its id (FileInfo::id), if _hasFolder
                bool                 _hasFolder = false;
                bool                 _listed = false;
                std::atomic<uint8_t> _changes{0};
                Notify               _notify = nullptr;
                void                *_arg = nullptr;
};

BMDS_NAMESPACE_END
