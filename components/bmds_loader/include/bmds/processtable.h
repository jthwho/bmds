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
 * @brief The processes: ids, parents, jobs, and how ended ones ended until
 *        their parents wait for them (devplan/programs.md §13).
 *
 * An entry is added when a process starts. When it ends it becomes a
 * zombie (only its id and status are kept) until its parent waits for it;
 * a process without a parent left is freed at once. The shell is
 * `ShellPid` to the programs it starts: not an entry, but their parent,
 * so the launcher waits for them as a program waits for its children.
 *
 * Each entry has the semaphore to release when it ends (its parent's, for
 * waiting) and an opaque pointer for its runner (the loader's running
 * program).
 *
 * @par Thread Safety
 * Thread-safe. `lock()` covers every method; `forEachInJob()` calls its
 * function with it held.
 */
class ProcessTable {
        public:
                /** @brief How many processes there may be at once, zombies included. */
                static constexpr int Capacity = 32;

                /** @brief The parent of the programs the shell starts. */
                static constexpr int32_t ShellPid = 1;

                /** @brief An ended child, from `reap()`. */
                struct Ended {
                                int32_t pid;    ///< Its id.
                                int32_t status; ///< How it ended (`exitStatus()`, `killedStatus()`).
                };

                /** @brief The wait status of a process that exited with @p code (Linux's layout). */
                static constexpr int32_t exitStatus(int32_t code) { return (code & 0xff) << 8; }

                /** @brief The wait status of a process killed by signal @p signal. */
                static constexpr int32_t killedStatus(int32_t signal) { return signal & 0x7f; }

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
                 * @param parent  Its parent's id (`ShellPid` for the shell's programs).
                 * @param job     Its job (the shell's program it descends from); 0: its own id.
                 * @param notify  Released when it ends (the parent's waits); may be null.
                 * @param runtime Its runner's object (`runtime()`).
                 * @return Its id, or `Full` (`Capacity` reached), `InvalidState` (not initialized).
                 */
                Result<int32_t> add(int32_t parent, int32_t job, Semaphore *notify, void *runtime);

                /** @brief Changes the runner's object of running process @p pid (an exec). */
                void setRuntime(int32_t pid, void *runtime);

                /**
                 * @brief Records that @p pid ended with wait status @p status.
                 *
                 * It becomes a zombie for its parent, or is freed if it has none;
                 * its own zombies are freed, its living children become orphans;
                 * its parent's semaphore is released.
                 */
                void exited(int32_t pid, int32_t status);

                /**
                 * @brief Frees an ended child of @p parent: @p pid, or any if -1.
                 * @return The child, `WouldBlock` (matching children, none ended yet),
                 *         or `NotExist` (no such child).
                 */
                Result<Ended> reap(int32_t parent, int32_t pid);

                /** @brief Returns @p pid's parent: an id, `ShellPid`, or 0 (none, or no such process). */
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
                                int32_t parent;  ///< Its parent's (`ShellPid`; 0: an orphan).
                                int32_t job;     ///< Its job.
                                int32_t status;  ///< How it ended (a zombie).
                                void   *runtime; ///< Its runner's object (running only; may be null).
                                bool    ended;   ///< A zombie.
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
                                fn(View{next->pid, next->parent, next->job, next->status,
                                        next->ended ? nullptr : next->runtime, next->ended});
                        }
                }

                /** @brief Returns how many entries are in use, zombies included. */
                int count();

                /**
                 * @brief Calls @p fn(pid, runtime) for each running process in job
                 *        @p job, with the table locked.
                 *
                 * @p fn must not call the table (its lock isn't recursive); the
                 * same holds for `withProcess()` and `forEachRunning()`.
                 */
                template <typename Fn> void forEachInJob(int32_t job, Fn &&fn) {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (_entries == nullptr) return;
                        for (int i = 0; i < Capacity; ++i) {
                                Entry &e = _entries[i];
                                if (e.pid != 0 && e.job == job && !e.ended) fn(e.pid, e.runtime);
                        }
                        return;
                }

        private:
                struct Entry {
                                int32_t    pid; // 0: free
                                int32_t    parent;
                                int32_t    job;
                                int32_t    status;
                                Semaphore *notify;
                                void      *runtime;
                                bool       ended;
                };

                Entry *find(int32_t pid);
                void   free(Entry &e);

                Mutex   _lock;
                Entry  *_entries = nullptr;
                int32_t _lastPid = ShellPid;
};

BMDS_NAMESPACE_END
