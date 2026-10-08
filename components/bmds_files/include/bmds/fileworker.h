/**
 * @file      fileworker.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fileops.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A thread running a file manager's long operations, one job at a
 *        time, the others queued (`devplan/filebrowser.md` §4j, FB13).
 *
 * A job copies or moves paths into a folder, or removes them, through
 * `FileOps`. It counts its items and bytes first. The thread starts with the
 * first job and ends when none is left; the owner's loop joins it when it
 * takes the news (`takeDone()`, `reap()`), so that never waits long.
 *
 * News is told to a `Notify` on the thread, which must only wake the owner:
 * progress (and the item) at most every `NotifyEveryMs`; a job begun or done,
 * its counting done, and a question at once. The owner then reads `status()`, answers a question
 * (`answer()`) and takes the jobs done.
 *
 * @par Thread Safety
 * The owner's thread (one), except `Notify`, which is called on the worker's.
 */
class FileWorker {
        public:
                /** @brief What a job does. */
                enum class Kind : uint8_t {
                        Copy,   ///< The paths copied into the target.
                        Move,   ///< The paths moved into the target.
                        Remove, ///< The paths removed, with everything in them.
                };

                /** @brief Something to take: called on the worker's thread. */
                using Notify = void (*)(void *arg);

                /** @brief Progress is told at most this often (ms). */
                static constexpr unsigned int NotifyEveryMs = 250;

                /** @brief The thread's stack (PSRAM, while there is work). */
                static constexpr size_t StackBytes = 32 * 1024;

                /** @brief Where the running job is. */
                struct Status {
                                uint32_t job = 0;                    ///< Its number; 0: none running.
                                Kind     kind = Kind::Copy;          ///< What it does.
                                bool     counting = false;           ///< Counting its items and bytes.
                                uint64_t items = 0;                  ///< Items begun.
                                uint64_t totalItems = 0;             ///< Items in all (counted).
                                uint64_t bytes = 0;                  ///< Bytes copied.
                                uint64_t totalBytes = 0;             ///< Bytes to copy (counted).
                                size_t   waiting = 0;                ///< Jobs queued after it.
                                String   item{MemSpace::Psram};      ///< The item being done.
                                String   target{MemSpace::Psram};    ///< The folder it goes to (not `Remove`).
                                bool     asking = false;             ///< A name taken: waiting for `answer()`.
                                String   askSource{MemSpace::Psram}; ///< The item that would take it.
                                String   askTarget{MemSpace::Psram}; ///< What is there.
                                bool     askIsDirectory = false;     ///< What is there is a folder.
                };

                /** @brief A job done. */
                struct Done {
                                uint32_t job = 0;           ///< Its number.
                                Kind     kind = Kind::Copy; ///< What it did.
                                Error    err;               ///< `Ok`, `Interrupted` (cancelled), or the first error.
                                size_t   failures = 0;      ///< Items that failed.
                                String   failed{MemSpace::Psram}; ///< The first one's path.
                };

                /** @brief A worker telling @p fn(@p arg); nothing runs until `add()`. */
                FileWorker(Notify fn, void *arg) : _notify(fn), _notifyArg(arg) {}

                /** @brief Cancels the running job, drops the queue and waits for the thread. @blocking */
                ~FileWorker();

                FileWorker(const FileWorker &) = delete;
                FileWorker &operator=(const FileWorker &) = delete;

                /**
                 * @brief Queues a job: @p paths (absolute, a line each) copied or moved
                 *        into folder @p target, or removed (@p target unused).
                 * @return The job's number, or `Invalid` (no paths), `NoMem`, or the
                 *         thread's error.
                 */
                Result<uint32_t> add(Kind kind, StringView paths, StringView target);

                /** @brief Drops job @p job if queued, or stops it at its next chunk if running. */
                void cancel(uint32_t job);

                /** @brief Answers the question asked (`Status::asking`); @p forAll for the job's later ones too. */
                void answer(FileOps::Resolution resolution, bool forAll);

                /** @brief Copies where the running job is into @p out (`job` 0: none). */
                void status(Status &out);

                /** @brief Moves a job done into @p out. @return False if there is none. */
                bool takeDone(Done &out);

                /** @brief Returns true while a job runs or waits. */
                bool isBusy();

                /** @brief Joins the thread if it has ended (after a notify). */
                void reap();

        private:
                struct Job {
                                uint32_t            id = 0;
                                Kind                kind = Kind::Copy;
                                String              paths{MemSpace::Psram};
                                String              target{MemSpace::Psram};
                                bool                answered = false; // a resolution for all
                                FileOps::Resolution resolution = FileOps::Resolution::Skip;
                };

                class Progress;
                friend class Progress;

                static void threadMain(void *arg);
                void        run(Job &job);
                void        tell(bool now);
                static void freeJob(Job *job);

                Notify              _notify;
                void               *_notifyArg;
                Mutex               _lock; // all below but the thread
                List<Job *>         _queue{MemSpace::Psram};
                List<Done *>        _done{MemSpace::Psram};
                Job                *_current = nullptr;
                Status              _status;
                FileOps::Resolution _answer = FileOps::Resolution::Skip;
                bool                _answerForAll = false;
                bool                _answerGiven = false; // the question asked has its answer
                bool                _ended = false;       // the thread left its loop (joined by the owner)
                uint32_t            _nextId = 1;
                int64_t             _toldNs = 0; // progress last told
                Semaphore           _answered{1, 0};
                std::atomic<bool>   _cancelled{false}; // the running job
                Thread              _thread;
};

BMDS_NAMESPACE_END
