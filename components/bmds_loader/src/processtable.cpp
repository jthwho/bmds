/**
 * @file      processtable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/memory.h>
#include <bmds/processtable.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Ids count up to here, then start again at 2, skipping those in use.
        constexpr int32_t MaxPid = 32767;

} // namespace

ProcessTable::~ProcessTable() {
        Memory::free(_entries);
}

Error ProcessTable::init() {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries != nullptr) return Error::Ok;
        _entries = static_cast<Entry *>(Memory::alloc(MemSpace::Psram, sizeof(Entry) * Capacity));
        if (_entries == nullptr) return Error::NoMem;
        for (int i = 0; i < Capacity; ++i) _entries[i] = Entry{};
        return Error::Ok;
}

ProcessTable::Entry *ProcessTable::find(int32_t pid) {
        if (_entries == nullptr || pid <= ShellPid) return nullptr;
        for (int i = 0; i < Capacity; ++i) {
                if (_entries[i].pid == pid) return &_entries[i];
        }
        return nullptr;
}

void ProcessTable::free(Entry &e) {
        e = Entry{};
        return;
}

Result<int32_t> ProcessTable::add(int32_t parent, int32_t job, Semaphore *notify, void *runtime) {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return makeError<int32_t>(Error::InvalidState);
        Entry *slot = nullptr;
        for (int i = 0; i < Capacity && slot == nullptr; ++i) {
                if (_entries[i].pid == 0) slot = &_entries[i];
        }
        if (slot == nullptr) return makeError<int32_t>(Error::Full);
        // The next id not in use (there are more ids than entries).
        int32_t pid = _lastPid;
        do {
                pid = pid >= MaxPid ? ShellPid + 1 : pid + 1;
        } while (find(pid) != nullptr);
        _lastPid = pid;
        *slot = Entry{pid, parent, job != 0 ? job : pid, 0, notify, runtime, false};
        return makeResult(pid);
}

void ProcessTable::setRuntime(int32_t pid, void *runtime) {
        Mutex::Locker locker(_lock, WaitForever);
        if (Entry *e = find(pid); e != nullptr) e->runtime = runtime;
        return;
}

void ProcessTable::exited(int32_t pid, int32_t status) {
        Mutex::Locker locker(_lock, WaitForever);
        Entry        *e = find(pid);
        if (e == nullptr || e->ended) return;
        // Its children: zombies go (nobody can wait for them now), the living
        // become orphans, freed when they end.
        for (int i = 0; i < Capacity; ++i) {
                Entry &c = _entries[i];
                if (c.pid == 0 || c.parent != pid) continue;
                if (c.ended) {
                        free(c);
                } else {
                        c.parent = 0;
                        c.notify = nullptr;
                }
        }
        Semaphore *notify = e->notify;
        if (e->parent == 0) {
                free(*e);
        } else {
                e->ended = true;
                e->status = status;
                e->runtime = nullptr;
        }
        if (notify != nullptr) notify->release();
        return;
}

Result<ProcessTable::Ended> ProcessTable::reap(int32_t parent, int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return makeError<Ended>(Error::NotExist);
        bool waiting = false;
        for (int i = 0; i < Capacity; ++i) {
                Entry &c = _entries[i];
                if (c.pid == 0 || c.parent != parent || (pid != -1 && c.pid != pid)) continue;
                if (!c.ended) {
                        waiting = true;
                        continue;
                }
                const Ended ended{c.pid, c.status};
                free(c);
                return makeResult(ended);
        }
        return makeError<Ended>(waiting ? Error::WouldBlock : Error::NotExist);
}

int32_t ProcessTable::parentOf(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *e = find(pid);
        return e != nullptr ? e->parent : 0;
}

int ProcessTable::count() {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return 0;
        int n = 0;
        for (int i = 0; i < Capacity; ++i) n += _entries[i].pid != 0 ? 1 : 0;
        return n;
}

BMDS_NAMESPACE_END
