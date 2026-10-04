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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
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

Error Thread::start(const Config &, Entry entry, void *arg) {
        if (_impl != nullptr) return Error::InvalidState;
        _impl = new std::thread(entry, arg);
        return Error::Ok;
}

void Thread::join() {
        auto *t = static_cast<std::thread *>(_impl);
        if (t == nullptr) return;
        t->join();
        delete t;
        _impl = nullptr;
        return;
}

void Semaphore::releaseFromIsr() {
        release();
        return;
}

BMDS_NAMESPACE_END
