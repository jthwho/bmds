/**
 * @file      directorywatch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The watches' list and what tells them (devplan/filebrowser.md §4i).
 */

#include <bmds/directorywatch.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The watches listed, newest first; under the storage lock.
        constinit DirectoryWatch *firstWatch = nullptr;

} // namespace

struct DirectoryWatch::Access {
                static void placeLocked(DirectoryWatch &w, bool hasFolder, uint64_t folder) {
                        w._hasFolder = hasFolder;
                        w._folder = folder;
                        if (w._listed) return;
                        w._prev = nullptr;
                        w._next = firstWatch;
                        if (firstWatch != nullptr) firstWatch->_prev = &w;
                        firstWatch = &w;
                        w._listed = true;
                        return;
                }

                static void unlistLocked(DirectoryWatch &w) {
                        if (!w._listed) return;
                        if (w._prev != nullptr) {
                                w._prev->_next = w._next;
                        } else {
                                firstWatch = w._next;
                        }
                        if (w._next != nullptr) w._next->_prev = w._prev;
                        w._prev = w._next = nullptr;
                        w._listed = false;
                        w._hasFolder = false;
                        return;
                }

                static void tell(DirectoryWatch &w, uint8_t changes) {
                        w._changes.fetch_or(changes, std::memory_order_acq_rel);
                        if (w._notify != nullptr) w._notify(w._arg);
                        return;
                }

                static void tellLocked(uint64_t folder, uint64_t entry, uint8_t changes) {
                        for (DirectoryWatch *w = firstWatch; w != nullptr; w = w->_next) {
                                if (!w->_hasFolder) continue;
                                if (folder != 0 && w->_folder == folder) {
                                        tell(*w, changes);
                                } else if (entry != 0 && w->_folder == entry) {
                                        tell(*w, Entries);
                                }
                        }
                        return;
                }

                static void tellAllLocked() {
                        for (DirectoryWatch *w = firstWatch; w != nullptr; w = w->_next) tell(*w, Mounts);
                        return;
                }
};

void DirectoryWatch::stop() {
        if (!_listed) return;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        Access::unlistLocked(*this);
        return;
}

uint64_t Fs::folderId(StringView storage) {
        const size_t slash = storage.lastIndexOf('/');
        if (slash == StringView::NPos || slash == 0) return pathId("/");
        return pathId(storage.left(slash));
}

void Fs::writtenLocked(const FileIdentity &identity) {
        changedLocked(identity.device, identity.id);
        DirectoryWatch::Access::tellLocked(identity.folder, 0, DirectoryWatch::Contents);
        return;
}

void Fs::tellEntryLocked(StringView storage, uint8_t changes, bool itself) {
        if (firstWatch == nullptr) return; // nothing watches: not even the ids
        DirectoryWatch::Access::tellLocked(folderId(storage), itself ? pathId(storage) : 0, changes);
        return;
}

void Fs::placeWatchLocked(DirectoryWatch &watch, bool hasFolder, uint64_t folder) {
        DirectoryWatch::Access::placeLocked(watch, hasFolder, folder);
        return;
}

void Fs::tellAllWatchesLocked() {
        DirectoryWatch::Access::tellAllLocked();
        return;
}

BMDS_NAMESPACE_END
