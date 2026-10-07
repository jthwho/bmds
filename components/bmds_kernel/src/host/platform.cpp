/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Host (test) implementation of the kernel platform layer: Mutex,
 * Semaphore and the clock on top of the C++ thread library.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <random>
#include <thread>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/taskowner.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct HostSemaphore {
                        std::mutex              lock;
                        std::condition_variable cond;
                        unsigned int            count;
                        unsigned int            maxCount;
        };

        static_assert(sizeof(HostSemaphore) <= Detail::SyncStorageBytes);
        static_assert(alignof(HostSemaphore) <= Detail::SyncStorageAlign);

        HostSemaphore &semOf(unsigned char *storage) {
                return *std::launder(reinterpret_cast<HostSemaphore *>(storage));
        }

        // Waits up to @p remainingUs for a count (Deadline::Forever: no limit).
        Error take(HostSemaphore &s, int64_t remainingUs) {
                std::unique_lock<std::mutex> lock(s.lock);
                auto                         ready = [&s] {
                        return s.count > 0;
                };
                if (remainingUs == Deadline::Forever) {
                        s.cond.wait(lock, ready);
                } else if (!s.cond.wait_for(lock, std::chrono::microseconds(remainingUs), ready)) {
                        return Error::Timeout;
                }
                --s.count;
                return Error::Ok;
        }

        Error tryTake(HostSemaphore &s) {
                std::lock_guard<std::mutex> lock(s.lock);
                if (s.count == 0) return Error::WouldBlock;
                --s.count;
                return Error::Ok;
        }

        void give(HostSemaphore &s) {
                {
                        std::lock_guard<std::mutex> lock(s.lock);
                        if (s.count < s.maxCount) ++s.count;
                }
                s.cond.notify_one();
                return;
        }

} // namespace

int64_t Detail::monotonicUs() {
        using namespace std::chrono;
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Detail::suspendScheduler() {
        return;
}

void Detail::resumeScheduler() {
        return;
}

namespace {

        thread_local void *taskInterruptPointer = nullptr;
        thread_local void *taskOwnerPointer = nullptr;

} // namespace

void *Detail::taskOwner() {
        return taskOwnerPointer;
}

void Detail::setTaskOwner(void *owner) {
        taskOwnerPointer = owner;
        return;
}

void *Detail::taskInterrupt() {
        return taskInterruptPointer;
}

void Detail::setTaskInterrupt(void *interrupt) {
        taskInterruptPointer = interrupt;
        return;
}

int64_t Detail::persistentTimeUs() {
        return monotonicUs();
}

bool Detail::loadClockRecord(ClockRecord &) {
        return false;
}

void Detail::saveClockRecord(const ClockRecord &) {
        return;
}

void Detail::setLibcTime(int64_t) {
        return;
}

void Detail::fillBootEntropy(void *data, size_t size) {
        fillRandom(data, size);
        return;
}

void Detail::fillRandom(void *data, size_t size) {
        std::random_device device;
        auto              *p = static_cast<unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) p[i] = static_cast<unsigned char>(device());
        return;
}

// The host Mutex is a binary semaphore: timed std::timed_mutex locks go
// through pthread_mutex_clocklock, which ThreadSanitizer doesn't model.
// Ownership and priority inheritance don't matter in the tests.

Mutex::Mutex() {
        HostSemaphore *s = new (_storage) HostSemaphore();
        s->count = 1;
        s->maxCount = 1;
}

Mutex::~Mutex() {
        semOf(_storage).~HostSemaphore();
}

Error Mutex::lock(unsigned int timeoutMs) {
        Deadline deadline(timeoutMs);
        return lock(deadline);
}

Error Mutex::lock(const Deadline &deadline) {
        return take(semOf(_storage), deadline.remainingUs());
}

Error Mutex::tryLock() {
        return tryTake(semOf(_storage));
}

void Mutex::unlock() {
        give(semOf(_storage));
        return;
}

Semaphore::Semaphore(unsigned int maxCount, unsigned int initialCount) {
        HostSemaphore *s = new (_storage) HostSemaphore();
        s->count = initialCount;
        s->maxCount = maxCount;
}

Semaphore::~Semaphore() {
        semOf(_storage).~HostSemaphore();
}

Error Semaphore::acquire(unsigned int timeoutMs) {
        Deadline deadline(timeoutMs);
        return acquire(deadline);
}

Error Semaphore::acquire(const Deadline &deadline) {
        return take(semOf(_storage), deadline.remainingUs());
}

Error Semaphore::tryAcquire() {
        return tryTake(semOf(_storage));
}

void Semaphore::release() {
        give(semOf(_storage));
        return;
}

Thread::~Thread() {
        // Joined or never started; a joinable std::thread would terminate().
}

namespace {

        // A thread, and its membership of its starter's owner.
        struct HostThread {
                        std::thread       thread;
                        TaskOwner::Member member;
                        std::atomic<bool> detached{false};
        };

} // namespace

Error Thread::start(const Config &, Entry entry, void *arg) {
        if (_impl != nullptr) return Error::InvalidState;
        auto            *t = new HostThread;
        TaskOwner *const starter = TaskOwner::current();
        TaskOwner       *owner = starter != nullptr ? starter->threadOwner() : nullptr;
        if (owner != nullptr) {
                const Error err = owner->attach(t->member);
                if (err.isError()) {
                        delete t;
                        return err;
                }
        }
        t->thread = std::thread([t, owner, entry, arg] {
                TaskOwner::setCurrent(owner);
                entry(arg);
                if (owner != nullptr) owner->detach(t->member);
                if (t->detached.load()) delete t; // nobody joins it
        });
        _impl = t;
        return Error::Ok;
}

void Thread::detach() {
        auto *t = static_cast<HostThread *>(_impl);
        if (t == nullptr) return;
        t->thread.detach();
        t->detached.store(true);
        _impl = nullptr;
        return;
}

void Thread::join() {
        auto *t = static_cast<HostThread *>(_impl);
        if (t == nullptr) return;
        t->thread.join();
        delete t;
        _impl = nullptr;
        return;
}

void Semaphore::releaseFromIsr() {
        release();
        return;
}

BMDS_NAMESPACE_END
