/**
 * @file      workerpool.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/assert.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/workerpool.h>

BMDS_NAMESPACE_BEGIN

class WorkerPool::Worker {
        public:
                static void main(void *arg) {
                        Worker *w = static_cast<Worker *>(arg);
                        for (;;) {
                                const unsigned int timeoutMs = w->pool->_config.idleTimeoutMs;
                                if (timeoutMs == 0) {
                                        (void)w->ready.acquire(WaitForever);
                                } else if (w->ready.acquire(timeoutMs).isError()) {
                                        // Idle too long: off the list and gone, unless a job took it meanwhile
                                        // (then its job is posted: wait for it).
                                        // Counted while inside leave(): its unlock must finish before
                                        // the pool (and its lock) may go.
                                        WorkerPool *pool = w->pool;
                                        pool->_leaving.fetch_add(1, std::memory_order_acq_rel);
                                        const bool left = pool->leave(w);
                                        pool->_leaving.fetch_sub(1, std::memory_order_acq_rel); // the pool's last use
                                        if (!left) continue;
                                        w->thread.detach(); // freed by the system once this returns
                                        w->~Worker();
                                        Memory::free(w);
                                        return;
                                }
                                if (w->job == nullptr) return; // retired
                                w->job(w->arg);
                                w->done.release();
                        }
                }

                WorkerPool *pool = nullptr;
                Thread      thread;
                Semaphore   ready{1, 0}; // a job (or retirement) was posted
                Semaphore   done{1, 0};  // the job finished
                Job         job = nullptr;
                void       *arg = nullptr;
                Worker     *next = nullptr; // idle list
};

WorkerPool::~WorkerPool() {
        BMDS_ASSERT(_busyCount == 0);
        // The idle ones taken first, then retired unlocked (a worker timing out meanwhile takes the
        // lock, finds itself gone from the list, and is retired as the others).
        Worker *idle = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                idle = _idle;
                _idle = nullptr;
                _idleCount = 0;
        }
        while (idle != nullptr) {
                Worker *w = idle;
                idle = w->next;
                retire(w);
        }
        // A worker that took itself off the list just before may still be unlocking: wait for it.
        // (No other can come: only listed workers time out, and those were retired.)
        Semaphore pause(1, 0);
        while (_leaving.load(std::memory_order_acquire) != 0) (void)pause.acquire(1);
}

size_t WorkerPool::idleCount() {
        Mutex::Locker lock(_lock, WaitForever);
        return _idleCount;
}

size_t WorkerPool::busyCount() {
        Mutex::Locker lock(_lock, WaitForever);
        return _busyCount;
}

bool WorkerPool::leave(Worker *worker) {
        Mutex::Locker lock(_lock, WaitForever);
        for (Worker **at = &_idle; *at != nullptr; at = &(*at)->next) {
                if (*at != worker) continue;
                *at = worker->next;
                --_idleCount;
                return true;
        }
        return false;
}

Result<WorkerPool::Worker *> WorkerPool::start(Job job, void *arg) {
        Worker *w = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                w = _idle;
                if (w != nullptr) {
                        _idle = w->next;
                        --_idleCount;
                } else if (_busyCount >= _config.maxWorkers) {
                        return makeError<Worker *>(Error::Busy);
                }
                ++_busyCount; // counted now: a new worker below is busy as soon as it exists
        }
        if (w == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Worker));
                Error err = mem == nullptr ? Error(Error::NoMem) : Error();
                if (err.isOk()) {
                        w = new (mem) Worker();
                        w->pool = this;
                        err = w->thread.start(_config.thread, &Worker::main, w);
                        if (err.isError()) {
                                w->~Worker();
                                Memory::free(mem);
                        }
                }
                if (err.isError()) {
                        Mutex::Locker lock(_lock, WaitForever);
                        --_busyCount;
                        return makeError<Worker *>(err);
                }
        }
        w->job = job;
        w->arg = arg;
        w->next = nullptr;
        w->ready.release();
        return makeResult(w);
}

void WorkerPool::wait(Worker *worker) {
        (void)worker->done.acquire(WaitForever);
        {
                Mutex::Locker lock(_lock, WaitForever);
                --_busyCount;
                if (_idleCount < _config.maxIdle) {
                        worker->next = _idle;
                        _idle = worker;
                        ++_idleCount;
                        return;
                }
        }
        retire(worker); // more than are kept
        return;
}

void WorkerPool::retire(Worker *worker) {
        worker->job = nullptr;
        worker->ready.release();
        worker->thread.join();
        worker->~Worker();
        Memory::free(worker);
        return;
}

BMDS_NAMESPACE_END
