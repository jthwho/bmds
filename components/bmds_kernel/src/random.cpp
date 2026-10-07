/**
 * @file      random.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/platform.h>
#include <bmds/mutex.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Bytes of the main noise source at boot; of the hardware generator per read.
        constexpr size_t BootEntropyBytes = 256;
        constexpr size_t ReadEntropyBytes = 32;

        constinit StaticObject<Mutex> lock;
        constinit RandomPool          pool; // under the lock

} // namespace

Error Random::init() {
        if (!lock.isCreated()) lock.create();
        uint8_t seed[BootEntropyBytes];
        Detail::fillBootEntropy(seed, sizeof(seed));
        mix(seed, sizeof(seed));
        RandomPool::wipe(seed, sizeof(seed));
        return Error();
}

void Random::mix(const void *data, size_t size) {
        Mutex::Locker locker(*lock, WaitForever);
        pool.mix(data, size);
        return;
}

void Random::fill(void *out, size_t size) {
        // The hardware's bytes are fetched before the lock: they take a while.
        uint8_t fresh[ReadEntropyBytes];
        uint8_t key[RandomPool::KeyBytes];
        Detail::fillRandom(fresh, sizeof(fresh));
        {
                Mutex::Locker locker(*lock, WaitForever);
                pool.mix(fresh, sizeof(fresh));
                pool.next(key);
        }
        RandomPool::generate(key, out, size);
        RandomPool::wipe(key, sizeof(key));
        RandomPool::wipe(fresh, sizeof(fresh));
        return;
}

BMDS_NAMESPACE_END
