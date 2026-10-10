/**
 * @file      processtable.h
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
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The processes: ids, parents, process groups and sessions, and how
 *        ended ones ended until their parents wait for them
 *        (devplan/programs.md §13, devplan/processes.md §3.6).
 *
 * An entry is added when a process starts. When it ends it becomes a
 * zombie (only its id and status are kept) until its parent waits for it;
 * a process without a parent left is freed at once. Pid 1 is the first
 * process (the serial shell). Kernel code that starts a process and waits
 * for it is `KernelParent` to it: not an entry, but its parent.
 *
 * Each process is in a group (what the shell calls a job) and a session,
 * as POSIX has them. A new pid is never one in use as a group or session.
 *
 * Each entry has the semaphore to release when it ends (its parent's, for
 * waiting) and an opaque pointer for its runner (the loader's running
 * program). A process that stops or continues is reported to its parent
 * the same way (the semaphore), until the parent takes it with `reap()`
 * (devplan/processes.md §3.7.1).
 *
 * @par Thread Safety
 * Thread-safe. One lock covers every method; the `forEach` methods and
 * `withProcess()` call their function with it held.
 */
class ProcessTable {
        public:
                /** @brief How many processes there may be at once, zombies included. */
                static constexpr int Capacity = 32;

                /** @brief The first process: the serial shell (devplan/processes.md §3.4). */
                static constexpr int32_t InitPid = 1;

                /** @brief The parent of a process kernel code starts and waits for (not an entry). */
                static constexpr int32_t KernelParent = -1;

                /** @brief `add()`: the parent's group or session (a new one without a parent). */
                static constexpr int32_t Inherit = -1;

                /** @brief `add()`: a new group or session, led by the new process. */
                static constexpr int32_t New = 0;

                /** @brief An ended child, from `reap()`. */
                struct Ended {
                                int32_t pid;    ///< Its id.
                                int32_t status; ///< How it ended (`exitStatus()`, `killedStatus()`).
                };

                /** @brief The wait status of a process that exited with @p code (Linux's layout). */
                static constexpr int32_t exitStatus(int32_t code) { return (code & 0xff) << 8; }

                /** @brief The wait status of a process killed by signal @p signal. */
                static constexpr int32_t killedStatus(int32_t signal) { return signal & 0x7f; }

                /** @brief The wait status of a process stopped by signal @p signal. */
                static constexpr int32_t stoppedStatus(int32_t signal) { return ((signal & 0xff) << 8) | 0x7f; }

                /** @brief The wait status of a process that continued. */
                static constexpr int32_t ContinuedStatus = 0xffff;

                /** @brief Returns true if @p status is `stoppedStatus()`'s. */
                static constexpr bool isStoppedStatus(int32_t status) { return (status & 0xff) == 0x7f; }

                /** @brief `reap()` flags. */
                enum ReapFlag : uint32_t {
                        ReapStopped = 1u << 0,   ///< Also a child that stopped (once per stop).
                        ReapContinued = 1u << 1, ///< Also a child that continued (once).
                };

                ProcessTable() = default;
                ~ProcessTable();
                ProcessTable(const ProcessTable &) = delete;
                ProcessTable &operator=(const ProcessTable &) = delete;

                /**
                 * @brief Allocates the entries (`Psram`); idempotent.
                 * @return `Ok` or `NoMem`.
                 */
                Error init();

                /**
                 * @brief Adds a running process.
                 * @param parent  Its parent's id; `KernelParent`; 0: none (an orphan).
                 * @param group   Its group: `Inherit`, `New`, or one in its session to join.
                 * @param session Its session: `Inherit` or `New`.
                 * @param notify  Released when it ends (the parent's waits); may be null.
                 * @param runtime Its runner's object (`runtime()`).
                 * @return Its id, or `Full` (`Capacity` reached), `InvalidState` (not
                 *         initialized), `PermissionDenied` (@p group is another session's).
                 */
                Result<int32_t> add(int32_t parent, int32_t group, int32_t session, Semaphore *notify, void *runtime);

                /**
                 * @brief `setpgid()`: puts @p pid (0: @p caller) in group @p group
                 *        (0: a new one, @p pid's own).
                 * @return `Ok`; `NotExist` (@p pid is neither @p caller nor its
                 *         running child), `PermissionDenied` (another session, a
                 *         session leader, or no such group in the session), `Invalid`.
                 */
                Error setGroup(int32_t caller, int32_t pid, int32_t group);

                /**
                 * @brief `setsid()`: makes @p pid the leader of a new session and group.
                 * @return The session's id, or `PermissionDenied` (it leads a group already).
                 */
                Result<int32_t> newSession(int32_t pid);

                /** @brief Returns @p pid's group, or 0 (no such process). */
                int32_t groupOf(int32_t pid);

                /** @brief Returns @p pid's session, or 0 (no such process). */
                int32_t sessionOf(int32_t pid);

                /** @brief Returns true if a process of @p group other than @p except is running. */
                bool hasRunningInGroup(int32_t group, int32_t except);

                /** @brief Returns true if @p group has a running process in session @p session. */
                bool hasGroupInSession(int32_t group, int32_t session);

                /** @brief Changes the runner's object of running process @p pid (an exec). */
                void setRuntime(int32_t pid, void *runtime);

                /**
                 * @brief Records that @p pid ended with wait status @p status, having
                 *        run @p cpuUs (kept while it is a zombie).
                 *
                 * It becomes a zombie for its parent, or is freed if it has none;
                 * its own zombies are freed, its living children become orphans;
                 * its parent's semaphore is released.
                 */
                void exited(int32_t pid, int32_t status, uint64_t cpuUs = 0);

                /**
                 * @brief Records that @p pid stopped (by @p signal) or, with 0, continued:
                 *        its parent is told (its semaphore) until it `reap()`s it.
                 * @return Its parent (to send `SIGCHLD`), or 0 (none, or not running).
                 */
                int32_t setStopped(int32_t pid, int signal);

                /** @brief Returns true if @p pid is stopped. */
                bool isStopped(int32_t pid);

                /**
                 * @brief Puts @p parent's stopped children in @p out (as many as fit).
                 * @return How many there are.
                 */
                size_t stoppedChildren(int32_t parent, int32_t *out, size_t capacity);

                /** @brief Changes the semaphore released when @p pid ends, stops or continues. */
                void setNotify(int32_t pid, Semaphore *notify);

                /**
                 * @brief Frees an ended child of @p parent: @p pid, or any if -1; with
                 *        @p flags (`ReapFlag`), takes a stop or continue instead. A
                 *        child freed adds its time, and its own children's, to
                 *        @p parent's `childrenCpuUs` (as Linux's `RUSAGE_CHILDREN`).
                 * @return The child (its status says which), `WouldBlock` (matching
                 *         children, none to take yet), or `NotExist` (no such child).
                 */
                Result<Ended> reap(int32_t parent, int32_t pid, uint32_t flags = 0);

                /** @brief Returns @p pid's parent: an id, `KernelParent`, or 0 (none, or no such process). */
                int32_t parentOf(int32_t pid);

                /** @brief Returns true if @p pid has ended (a zombie) or doesn't exist. */
                bool isEnded(int32_t pid);

                /**
                 * @brief Makes @p pid an orphan: no parent, nothing released when it
                 *        ends (for a parent that stops waiting before it ends). A
                 *        zombie is freed.
                 */
                void detach(int32_t pid);

                /** @brief What `withProcess()` found. */
                enum class Found : uint8_t {
                        None,    ///< No such process.
                        Ended,   ///< A zombie: @p fn wasn't called.
                        Running, ///< @p fn was called.
                };

                /**
                 * @brief Calls @p fn(runtime) for @p pid if it is running, with the
                 *        table locked.
                 */
                template <typename Fn> Found withProcess(int32_t pid, Fn &&fn) {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (_entries == nullptr) return Found::None;
                        Entry *e = find(pid);
                        if (e == nullptr) return Found::None;
                        if (e->ended) return Found::Ended;
                        fn(e->runtime);
                        return Found::Running;
                }

                /**
                 * @brief Calls @p fn(pid, runtime) for each running process, with the
                 *        table locked.
                 */
                template <typename Fn> void forEachRunning(Fn &&fn) {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (_entries == nullptr) return;
                        for (int i = 0; i < Capacity; ++i) {
                                Entry &e = _entries[i];
                                if (e.pid != 0 && !e.ended) fn(e.pid, e.runtime);
                        }
                        return;
                }

                /** @brief An entry, as `forEach()` shows it. */
                struct View {
                                int32_t pid;     ///< Its id.
                                int32_t parent;  ///< Its parent's (`KernelParent`; 0: an orphan).
                                int32_t group;   ///< Its process group.
                                int32_t session; ///< Its session.
                                int32_t status;  ///< How it ended (a zombie).
                                void   *runtime; ///< Its runner's object (running only; may be null).
                                bool     ended;   ///< A zombie.
                                bool     stopped; ///< Stopped.
                                int64_t  startedUs; ///< When it was added (monotonic).
                                uint64_t cpuUs;     ///< A zombie's time run (`exited()`).
                                uint64_t childrenCpuUs; ///< Its children's that it reaped, and theirs (`reap()`).
                };

                /**
                 * @brief Calls @p fn(const View &) for each entry, zombies included,
                 *        in pid order, with the table locked.
                 */
                template <typename Fn> void forEach(Fn &&fn) {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (_entries == nullptr) return;
                        // Pids are few (Capacity): a selection by pid, no sorting space.
                        int32_t last = 0;
                        for (;;) {
                                Entry *next = nullptr;
                                for (int i = 0; i < Capacity; ++i) {
                                        Entry &e = _entries[i];
                                        if (e.pid > last && (next == nullptr || e.pid < next->pid)) next = &e;
                                }
                                if (next == nullptr) return;
                                last = next->pid;
                                fn(View{next->pid, next->parent, next->group, next->session, next->status,
                                        next->ended ? nullptr : next->runtime, next->ended, next->stopped,
                                        next->startedUs, next->cpuUs, next->childrenCpuUs});
                        }
                }

                /** @brief Returns how many entries are in use, zombies included. */
                int count();

                /**
                 * @brief Calls @p fn(pid, runtime) for each running process in group
                 *        @p group, with the table locked.
                 *
                 * @p fn must not call the table (its lock isn't recursive); the
                 * same holds for every method taking a function.
                 */
                template <typename Fn> void forEachInGroup(int32_t group, Fn &&fn) {
                        forEachWhere([group](const Entry &e) { return e.group == group; }, fn);
                        return;
                }

                /** @brief Calls @p fn(pid, runtime) for each running process in session @p session. */
                template <typename Fn> void forEachInSession(int32_t session, Fn &&fn) {
                        forEachWhere([session](const Entry &e) { return e.session == session; }, fn);
                        return;
                }

        private:
                struct Entry {
                                int32_t    pid; // 0: free
                                int32_t    parent;
                                int32_t    group;
                                int32_t    session;
                                int32_t    status;
                                Semaphore *notify;
                                void      *runtime;
                                int32_t    change; // a stop or continue to report (its status), or 0
                                bool       ended;
                                bool       stopped;
                                int64_t    startedUs;     // monotonic
                                uint64_t   cpuUs;         // a zombie's
                                uint64_t   childrenCpuUs; // the reaped children's and theirs (getrusage's)
                };

                Entry *find(int32_t pid);
                void   free(Entry &e);
                bool   isIdInUse(int32_t id) const;
                bool   isGroupInSession(int32_t group, int32_t session) const;

                template <typename Match, typename Fn> void forEachWhere(Match &&match, Fn &&fn) {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (_entries == nullptr) return;
                        for (int i = 0; i < Capacity; ++i) {
                                Entry &e = _entries[i];
                                if (e.pid != 0 && !e.ended && match(e)) fn(e.pid, e.runtime);
                        }
                        return;
                }

                Mutex   _lock;
                Entry  *_entries = nullptr;
                int32_t _lastPid = 0;
};

BMDS_NAMESPACE_END
