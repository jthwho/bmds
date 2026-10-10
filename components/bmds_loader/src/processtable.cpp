/**
 * @file      processtable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/platform.h>
#include <bmds/memory.h>
#include <bmds/processtable.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Ids count up to here, then start again at 2 (1 is the first process's for good),
        // skipping those in use as a process, group or session.
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
        if (_entries == nullptr || pid <= 0) return nullptr;
        for (int i = 0; i < Capacity; ++i) {
                if (_entries[i].pid == pid) return &_entries[i];
        }
        return nullptr;
}

void ProcessTable::free(Entry &e) {
        e = Entry{};
        return;
}

bool ProcessTable::isIdInUse(int32_t id) const {
        for (int i = 0; i < Capacity; ++i) {
                const Entry &e = _entries[i];
                if (e.pid == id || (e.pid != 0 && (e.group == id || e.session == id))) return true;
        }
        return false;
}

bool ProcessTable::isGroupInSession(int32_t group, int32_t session) const {
        for (int i = 0; i < Capacity; ++i) {
                const Entry &e = _entries[i];
                if (e.pid != 0 && !e.ended && e.group == group && e.session == session) return true;
        }
        return false;
}

Result<int32_t> ProcessTable::add(int32_t parent, int32_t group, int32_t session, Semaphore *notify, void *runtime) {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return makeError<int32_t>(Error::InvalidState);
        Entry *slot = nullptr;
        for (int i = 0; i < Capacity && slot == nullptr; ++i) {
                if (_entries[i].pid == 0) slot = &_entries[i];
        }
        if (slot == nullptr) return makeError<int32_t>(Error::Full);
        // The parent's group and session, when it has them.
        const Entry *from = find(parent);
        if (from != nullptr && from->ended) from = nullptr;
        const int32_t sessionId = session == Inherit && from != nullptr ? from->session : 0;
        int32_t       groupId = group;
        if (group == Inherit) groupId = from != nullptr && session == Inherit ? from->group : 0;
        // A group to join is one of the session's; or none now (its processes ended), its id
        // still kept from new processes until the last of them is gone.
        if (groupId > 0 && sessionId != 0) {
                for (int i = 0; i < Capacity; ++i) {
                        const Entry &e = _entries[i];
                        if (e.pid != 0 && e.group == groupId && e.session != sessionId)
                                return makeError<int32_t>(Error::PermissionDenied);
                }
        }
        // The next id not in use (there are more ids than entries).
        int32_t pid = _lastPid;
        do {
                pid = pid >= MaxPid ? InitPid + 1 : pid + 1;
        } while (isIdInUse(pid));
        _lastPid = pid;
        *slot = Entry{pid,
                      parent,
                      groupId > 0 ? groupId : pid,
                      sessionId != 0 ? sessionId : pid,
                      0,
                      notify,
                      runtime,
                      0,
                      false,
                      false,
                      Detail::monotonicUs(),
                      0,
                      0};
        return makeResult(pid);
}

Error ProcessTable::setGroup(int32_t caller, int32_t pid, int32_t group) {
        if (group < 0) return Error::Invalid;
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *self = find(caller);
        Entry        *target = find(pid == 0 ? caller : pid);
        if (self == nullptr || target == nullptr || target->ended || (target != self && target->parent != caller))
                return Error::NotExist;
        if (target->session != self->session || target->session == target->pid) return Error::PermissionDenied;
        const int32_t to = group == 0 ? target->pid : group;
        if (to != target->pid && !isGroupInSession(to, self->session)) return Error::PermissionDenied;
        target->group = to;
        return Error::Ok;
}

Result<int32_t> ProcessTable::newSession(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        Entry        *e = find(pid);
        if (e == nullptr || e->ended) return makeError<int32_t>(Error::NotExist);
        for (int i = 0; i < Capacity; ++i) {
                if (_entries[i].pid != 0 && _entries[i].group == pid)
                        return makeError<int32_t>(Error::PermissionDenied);
        }
        e->group = e->session = pid;
        return makeResult(pid);
}

int32_t ProcessTable::groupOf(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *e = find(pid);
        return e != nullptr ? e->group : 0;
}

int32_t ProcessTable::sessionOf(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *e = find(pid);
        return e != nullptr ? e->session : 0;
}

bool ProcessTable::hasGroupInSession(int32_t group, int32_t session) {
        Mutex::Locker locker(_lock, WaitForever);
        return _entries != nullptr && isGroupInSession(group, session);
}

bool ProcessTable::hasRunningInGroup(int32_t group, int32_t except) {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return false;
        for (int i = 0; i < Capacity; ++i) {
                const Entry &e = _entries[i];
                if (e.pid != 0 && e.pid != except && !e.ended && e.group == group) return true;
        }
        return false;
}

void ProcessTable::setRuntime(int32_t pid, void *runtime) {
        Mutex::Locker locker(_lock, WaitForever);
        if (Entry *e = find(pid); e != nullptr) e->runtime = runtime;
        return;
}

void ProcessTable::exited(int32_t pid, int32_t status, uint64_t cpuUs) {
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
                e->cpuUs = cpuUs;
        }
        if (notify != nullptr) notify->release();
        return;
}

int32_t ProcessTable::setStopped(int32_t pid, int signal) {
        Mutex::Locker locker(_lock, WaitForever);
        Entry        *e = find(pid);
        if (e == nullptr || e->ended || e->stopped == (signal != 0)) return 0;
        e->stopped = signal != 0;
        e->change = signal != 0 ? stoppedStatus(signal) : ContinuedStatus;
        if (e->notify != nullptr) e->notify->release();
        return e->parent > 0 ? e->parent : 0;
}

bool ProcessTable::isStopped(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *e = find(pid);
        return e != nullptr && !e->ended && e->stopped;
}

size_t ProcessTable::stoppedChildren(int32_t parent, int32_t *out, size_t capacity) {
        Mutex::Locker locker(_lock, WaitForever);
        size_t        n = 0;
        for (int i = 0; _entries != nullptr && i < Capacity; ++i) {
                const Entry &c = _entries[i];
                if (c.pid == 0 || c.ended || !c.stopped || c.parent != parent) continue;
                if (n < capacity) out[n] = c.pid;
                ++n;
        }
        return n;
}

void ProcessTable::setNotify(int32_t pid, Semaphore *notify) {
        Mutex::Locker locker(_lock, WaitForever);
        if (Entry *e = find(pid); e != nullptr && !e->ended) e->notify = notify;
        return;
}

Result<ProcessTable::Ended> ProcessTable::reap(int32_t parent, int32_t pid, uint32_t flags) {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return makeError<Ended>(Error::NotExist);
        bool waiting = false;
        for (int i = 0; i < Capacity; ++i) {
                Entry &c = _entries[i];
                if (c.pid == 0 || c.parent != parent || (pid != -1 && c.pid != pid)) continue;
                if (!c.ended) {
                        const bool wanted = c.change != 0 && (isStoppedStatus(c.change) ? (flags & ReapStopped) != 0
                                                                                        : (flags & ReapContinued) != 0);
                        if (wanted) {
                                const Ended changed{c.pid, c.change};
                                c.change = 0;
                                return makeResult(changed);
                        }
                        waiting = true;
                        continue;
                }
                const Ended ended{c.pid, c.status};
                // Waited for: its time, and its own children's, are its parent's children's now.
                if (Entry *p = find(parent); p != nullptr) p->childrenCpuUs += c.cpuUs + c.childrenCpuUs;
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

bool ProcessTable::isEnded(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        const Entry  *e = find(pid);
        return e == nullptr || e->ended;
}

void ProcessTable::detach(int32_t pid) {
        Mutex::Locker locker(_lock, WaitForever);
        Entry        *e = find(pid);
        if (e == nullptr) return;
        if (e->ended) {
                free(*e);
                return;
        }
        e->parent = 0;
        e->notify = nullptr;
        return;
}

int ProcessTable::count() {
        Mutex::Locker locker(_lock, WaitForever);
        if (_entries == nullptr) return 0;
        int n = 0;
        for (int i = 0; i < Capacity; ++i) n += _entries[i].pid != 0 ? 1 : 0;
        return n;
}

BMDS_NAMESPACE_END
