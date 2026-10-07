/**
 * @file      workerpool.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Runs jobs on worker threads, keeping a few idle ones for reuse.
 *
 * `start()` hands a job to an idle worker, or starts a new one (up to
 * `Config::maxWorkers`); `wait()` waits for the job and returns the worker
 * to the pool. More than `Config::maxIdle` idle workers are joined and
 * freed, so an idle pool costs at most that many threads, and a worker idle
 * for `Config::idleTimeoutMs` ends by itself (its thread detached: the
 * system frees it), so an idle pool costs nothing for long. The shell runs
 * the earlier stages of a pipeline this way.
 *
 * Nothing is allocated until the first job; each worker is one `Psram`
 * record plus its Thread.
 *
 * @par Thread Safety
 * One owner task calls `start()` and `wait()`; the workers take themselves
 * off the idle list under the pool's lock. The jobs run concurrently on the
 * workers.
 */
class WorkerPool {
        public:
                /** @brief A job. */
                using Job = void (*)(void *arg);

                /** @brief Pool parameters. */
                struct Config {
                                Thread::Config thread;               ///< Each worker's thread.
                                size_t         maxWorkers = 7;       ///< Most workers at once.
                                size_t         maxIdle = 2;          ///< Most idle workers kept.
                                unsigned int   idleTimeoutMs = 5000; ///< An idle worker's life; 0: until retired.
                };

                class Worker;

                /** @brief Constructs an empty pool (no allocation). */
                explicit WorkerPool(const Config &config) : _config(config) {}

                /** @brief Joins the idle workers; no job may be running. */
                ~WorkerPool();

                WorkerPool(const WorkerPool &) = delete;
                WorkerPool &operator=(const WorkerPool &) = delete;

                /**
                 * @brief Runs @p job(@p arg) on a worker.
                 * @return The worker (pass it to `wait()`), `Busy` if `maxWorkers`
                 *         are all running jobs, or `NoMem`.
                 */
                Result<Worker *> start(Job job, void *arg);

                /**
                 * @brief Waits for @p worker's job to finish and returns it to the pool.
                 * @blocking
                 */
                void wait(Worker *worker);

                /** @brief Returns the number of idle workers. */
                size_t idleCount();

                /** @brief Returns the number of workers running jobs. */
                size_t busyCount();

        private:
                void retire(Worker *worker);
                bool leave(Worker *worker); // an idle worker off the list (its timeout); false: taken

                Config              _config;
                Mutex               _lock;       // the idle list and the counts
                std::atomic<size_t> _leaving{0}; // workers inside leave(): the pool must outlive them
                Worker             *_idle = nullptr;
                size_t              _idleCount = 0;
                size_t              _busyCount = 0;
};

BMDS_NAMESPACE_END
