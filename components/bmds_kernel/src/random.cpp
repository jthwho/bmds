/**
 * @file      random.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <bmds/detail/platform.h>
#include <bmds/mutex.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Bytes of the main noise source at boot. Of the hardware generator: a word on each read
        // (its register), and 32 bytes at most once a second (paced by IDF: ~56 µs a word, with
        // clock queries over the analog I2C bus; devplan/programs.md §19).
        constexpr size_t   BootEntropyBytes = 256;
        constexpr size_t   ReseedWords = 8;
        constexpr uint32_t ReseedMs = 1000;

        constinit StaticObject<Mutex>   lock;
        constinit RandomPool            pool;       // under the lock
        constinit std::atomic<uint32_t> lastReseed; // ms, wrapping

        uint32_t nowMs() {
                return static_cast<uint32_t>(Detail::monotonicUs() / 1000);
        }

} // namespace

Error Random::init() {
        if (!lock.isCreated()) lock.create();
        uint8_t seed[BootEntropyBytes];
        Detail::fillBootEntropy(seed, sizeof(seed));
        mix(seed, sizeof(seed));
        RandomPool::wipe(seed, sizeof(seed));
        lastReseed.store(nowMs(), std::memory_order_relaxed);
        return Error();
}

void Random::mix(const void *data, size_t size) {
        Mutex::Locker locker(*lock, WaitForever);
        pool.mix(data, size);
        return;
}

void Random::fill(void *out, size_t size) {
        // The hardware's words are fetched before the lock: they take a while. Two reads at once
        // may both take 32 bytes; that does no harm.
        const uint32_t now = nowMs();
        const bool     reseed = now - lastReseed.load(std::memory_order_relaxed) >= ReseedMs;
        if (reseed) lastReseed.store(now, std::memory_order_relaxed);
        uint32_t     fresh[1 + ReseedWords];
        const size_t words = reseed ? ReseedWords : 1;
        fresh[0] = Detail::cycles();
        if (reseed)
                Detail::fillRandom(fresh + 1, words * sizeof(uint32_t));
        else
                fresh[1] = Detail::randomWord(); // a read's word: ms apart, no pacing needed
        uint8_t key[RandomPool::KeyBytes];
        {
                Mutex::Locker locker(*lock, WaitForever);
                pool.mix(fresh, (1 + words) * sizeof(uint32_t));
                pool.next(key);
        }
        RandomPool::generate(key, out, size);
        RandomPool::wipe(key, sizeof(key));
        RandomPool::wipe(fresh, sizeof(fresh));
        return;
}

BMDS_NAMESPACE_END
