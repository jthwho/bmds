/**
 * @file      randompool.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/chacha20.h>
#include <bmds/randompool.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t ZeroNonce[ChaCha20::NonceBytes] = {};

} // namespace

void RandomPool::mix(const void *data, size_t size) {
        Blake2s hash;
        hash.update(_key, sizeof(_key));
        hash.update(data, size);
        hash.final(_key);
        return;
}

void RandomPool::next(uint8_t (&readKey)[KeyBytes]) {
        // One block: the first half the pool's next key, the second this read's.
        uint8_t  block[ChaCha20::BlockBytes];
        ChaCha20 stream(_key, ZeroNonce);
        stream.block(block);
        std::memcpy(_key, block, KeyBytes);
        std::memcpy(readKey, block + KeyBytes, KeyBytes);
        wipe(block, sizeof(block));
        return;
}

void RandomPool::generate(const uint8_t (&readKey)[KeyBytes], void *out, size_t size) {
        ChaCha20 stream(readKey, ZeroNonce);
        stream.fill(static_cast<uint8_t *>(out), size);
        return;
}

void RandomPool::wipe(void *data, size_t size) {
        volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
        for (size_t i = 0; i < size; ++i) p[i] = 0;
        return;
}

BMDS_NAMESPACE_END
