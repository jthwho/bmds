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
                                (void)w->ready.acquire(WaitForever);
                                if (w->job == nullptr) return; // retired
                                w->job(w->arg);
                                w->done.release();
                        }
                }

                Thread    thread;
                Semaphore ready{1, 0}; // a job (or retirement) was posted
                Semaphore done{1, 0};  // the job finished
                Job       job = nullptr;
                void     *arg = nullptr;
                Worker   *next = nullptr; // idle list
};

WorkerPool::~WorkerPool() {
        BMDS_ASSERT(_busyCount == 0);
        while (_idle != nullptr) {
                Worker *w = _idle;
                _idle = w->next;
                retire(w);
        }
}

Result<WorkerPool::Worker *> WorkerPool::start(Job job, void *arg) {
        Worker *w = _idle;
        if (w != nullptr) {
                _idle = w->next;
                --_idleCount;
        } else {
                if (_busyCount >= _config.maxWorkers) return makeError<Worker *>(Error::Busy);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Worker));
                if (mem == nullptr) return makeError<Worker *>(Error::NoMem);
                w = new (mem) Worker();
                Error err = w->thread.start(_config.thread, &Worker::main, w);
                if (err.isError()) {
                        w->~Worker();
                        Memory::free(mem);
                        return makeError<Worker *>(err);
                }
        }
        w->job = job;
        w->arg = arg;
        w->next = nullptr;
        ++_busyCount;
        w->ready.release();
        return makeResult(w);
}

void WorkerPool::wait(Worker *worker) {
        (void)worker->done.acquire(WaitForever);
        --_busyCount;
        if (_idleCount >= _config.maxIdle) {
                retire(worker);
                return;
        }
        worker->next = _idle;
        _idle = worker;
        ++_idleCount;
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
