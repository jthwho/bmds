/**
 * @file      folderwatches.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/directorywatch.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The folders a program watches, each by one `DirectoryWatch` however
 *        many of its views show it (`devplan/filebrowser.md` §4i).
 *
 * A folder is watched while it has uses (`add()`, `remove()`). One notify
 * wakes the owner for all of them; the owner then takes the folders changed
 * (`take()`), as many changes as came merged into one call for each.
 *
 * Paths are absolute and normalized (`Vfs::normalize()`); a folder is named as
 * its views name it (`/home/Desktop`), its links followed only to watch it.
 *
 * @par Thread Safety
 * One owner (a loop); the notify comes from any task, under the storage lock
 * (`DirectoryWatch::Notify`).
 */
class FolderWatches {
        public:
                /** @brief Told of a folder changed: @p changes are `DirectoryWatch::Change` bits. */
                using Changed = void (*)(void *arg, StringView path, uint8_t changes);

                /** @brief Watches with @p notify (`DirectoryWatch`'s rules) told of every change. */
                FolderWatches(DirectoryWatch::Notify notify, void *arg) : _notify(notify), _arg(arg) {}

                /** @brief Stops every watch. */
                ~FolderWatches();

                FolderWatches(const FolderWatches &) = delete;
                FolderWatches &operator=(const FolderWatches &) = delete;

                /**
                 * @brief Counts a use of folder @p path, watching it from the first.
                 *
                 * A folder not there is watched for mounts (`DirectoryWatch::watch()`).
                 * @return `Ok` or `NoMem`.
                 */
                Error add(StringView path);

                /** @brief Drops a use of folder @p path; the last stops its watch. */
                void remove(StringView path);

                /**
                 * @brief Tells @p fn of each folder changed since the last call, once.
                 *
                 * After a mount every folder is changed, and is looked for again
                 * first (`/home` may lead elsewhere now). @p fn may add and remove.
                 */
                void take(Changed fn, void *arg);

                /** @brief Drops what folder @p path was told (its owner has read it since). */
                void forget(StringView path);

                /** @brief Returns the folders watched. */
                size_t count() const { return _watched.size(); }

                /** @brief Returns the uses of folder @p path (0: not watched). */
                uint32_t uses(StringView path) const;

        private:
                struct Watched {
                                DirectoryWatch watch;
                                String         path{MemSpace::Psram};
                                uint32_t       uses = 0;
                                uint8_t        taken = 0; // changes taken from the watch, not yet told
                };

                int indexOf(StringView path) const;

                List<Watched *>              _watched{MemSpace::Psram};
                const DirectoryWatch::Notify _notify;
                void *const                  _arg;
};

BMDS_NAMESPACE_END
