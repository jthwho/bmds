/**
 * @file      memtest.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/memtest.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A 32-bit integer hash with good avalanche (lowbias32, Chris Wellons).
        inline uint32_t hash(uint32_t x) {
                x ^= x >> 16;
                x *= 0x7feb352dU;
                x ^= x >> 15;
                x *= 0x846ca68bU;
                x ^= x >> 16;
                return x;
        }

        inline uint32_t word(MemTest::Pattern pattern, uint32_t seed, uintptr_t key) {
                const auto k = static_cast<uint32_t>(key);
                switch (pattern) {
                        case MemTest::Pattern::Address: return k;
                        case MemTest::Pattern::InvertedAddress: return ~k;
                        case MemTest::Pattern::Checkerboard: return ((k >> 2) ^ seed) & 1 ? 0xaaaaaaaaU : 0x55555555U;
                        case MemTest::Pattern::Random: return hash(k ^ hash(seed));
                }
                return 0;
        }

} // namespace

const char *MemTest::name(Pattern pattern) {
        switch (pattern) {
                case Pattern::Address: return "address";
                case Pattern::InvertedAddress: return "inverse";
                case Pattern::Checkerboard: return "checker";
                case Pattern::Random: return "random";
        }
        return "?";
}

uint32_t MemTest::expected(Pattern pattern, uint32_t seed, uintptr_t key) {
        return word(pattern, seed, key);
}

void MemTest::fill(Pattern pattern, uint32_t seed, uint32_t *words, size_t count, uintptr_t key) {
        for (size_t i = 0; i < count; ++i, key += 4) words[i] = word(pattern, seed, key);
        return;
}

size_t MemTest::verify(Pattern pattern, uint32_t seed, const uint32_t *words, size_t count, uintptr_t key,
                       Failure *failures, size_t maxFailures) {
        size_t wrong = 0;
        for (size_t i = 0; i < count; ++i, key += 4) {
                const uint32_t want = word(pattern, seed, key);
                const uint32_t got = words[i];
                if (got == want) continue;
                if (failures != nullptr && wrong < maxFailures)
                        failures[wrong] = {reinterpret_cast<uintptr_t>(&words[i]), want, got};
                ++wrong;
        }
        return wrong;
}

BMDS_NAMESPACE_END
