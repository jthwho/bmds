/**
 * @file      folderwatches.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's watched folders, shared by its views (devplan/filebrowser.md §4i).
 */

#include <new>
#include <bmds/folderwatches.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        const Vfs::Context Root{"/", nullptr};

} // namespace

FolderWatches::~FolderWatches() {
        for (Watched *w : _watched) {
                w->~Watched();
                Memory::free(w);
        }
        _watched.clear();
        return;
}

int FolderWatches::indexOf(StringView path) const {
        for (size_t i = 0; i < _watched.size(); ++i)
                if (_watched[i]->path.view() == path) return int(i);
        return -1;
}

uint32_t FolderWatches::uses(StringView path) const {
        const int i = indexOf(path);
        return i < 0 ? 0 : _watched[size_t(i)]->uses;
}

Error FolderWatches::add(StringView path) {
        if (const int i = indexOf(path); i >= 0) {
                ++_watched[size_t(i)]->uses;
                return Error::Ok;
        }
        Error err = _watched.reserve(_watched.size() + 1);
        if (err.isError()) return err;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Watched));
        if (mem == nullptr) return Error::NoMem;
        auto *w = ::new (mem) Watched();
        err = w->path.assign(path);
        if (err.isError()) {
                w->~Watched();
                Memory::free(mem);
                return err;
        }
        w->uses = 1;
        w->watch.setNotify(_notify, _arg);
        (void)w->watch.watch(path, Root); // not there: watched for mounts
        (void)_watched.append(w);         // reserved
        return Error::Ok;
}

void FolderWatches::remove(StringView path) {
        const int i = indexOf(path);
        if (i < 0) return;
        Watched *w = _watched[size_t(i)];
        if (--w->uses > 0) return;
        _watched.removeAt(size_t(i));
        w->~Watched();
        Memory::free(w);
        return;
}

void FolderWatches::forget(StringView path) {
        const int i = indexOf(path);
        if (i < 0) return;
        Watched *w = _watched[size_t(i)];
        // Kept: a mount's news (every folder is looked for again).
        w->taken = static_cast<uint8_t>((w->taken | w->watch.takeChanges()) & DirectoryWatch::Mounts);
        return;
}

void FolderWatches::take(Changed fn, void *arg) {
        bool mounted = false;
        for (Watched *w : _watched) {
                w->taken |= w->watch.takeChanges();
                mounted = mounted || (w->taken & DirectoryWatch::Mounts) != 0;
        }
        if (mounted)
                for (Watched *w : _watched) (void)w->watch.watch(w->path.view(), Root);
        // One at a time from the start: a call may add or remove folders.
        String path(MemSpace::Psram);
        for (;;) {
                Watched *next = nullptr;
                for (Watched *w : _watched) {
                        if (w->taken != 0) {
                                next = w;
                                break;
                        }
                }
                if (next == nullptr) break;
                const uint8_t changes = next->taken;
                next->taken = 0;
                if (path.assign(next->path.view()).isError()) continue; // told nothing: no memory
                fn(arg, path.view(), changes);
        }
        return;
}

BMDS_NAMESPACE_END
