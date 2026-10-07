/**
 * @file      chacha20.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/chacha20.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t load32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

        uint32_t rotl(uint32_t v, int n) {
                return v << n | v >> (32 - n);
        }

        void quarter(uint32_t *x, int a, int b, int c, int d) {
                x[a] += x[b];
                x[d] = rotl(x[d] ^ x[a], 16);
                x[c] += x[d];
                x[b] = rotl(x[b] ^ x[c], 12);
                x[a] += x[b];
                x[d] = rotl(x[d] ^ x[a], 8);
                x[c] += x[d];
                x[b] = rotl(x[b] ^ x[c], 7);
                return;
        }

} // namespace

ChaCha20::ChaCha20(const uint8_t (&key)[KeyBytes], const uint8_t (&nonce)[NonceBytes], uint32_t counter) {
        _state[0] = 0x61707865; // "expand 32-byte k"
        _state[1] = 0x3320646e;
        _state[2] = 0x79622d32;
        _state[3] = 0x6b206574;
        for (int i = 0; i < 8; ++i) _state[4 + i] = load32(key + 4 * i);
        _state[12] = counter;
        for (int i = 0; i < 3; ++i) _state[13 + i] = load32(nonce + 4 * i);
}

ChaCha20::~ChaCha20() {
        volatile uint32_t *p = _state;
        for (int i = 0; i < 16; ++i) p[i] = 0;
}

void ChaCha20::block(uint8_t (&out)[BlockBytes]) {
        uint32_t x[16];
        std::memcpy(x, _state, sizeof(x));
        for (int round = 0; round < 10; ++round) {
                quarter(x, 0, 4, 8, 12);
                quarter(x, 1, 5, 9, 13);
                quarter(x, 2, 6, 10, 14);
                quarter(x, 3, 7, 11, 15);
                quarter(x, 0, 5, 10, 15);
                quarter(x, 1, 6, 11, 12);
                quarter(x, 2, 7, 8, 13);
                quarter(x, 3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) {
                const uint32_t v = x[i] + _state[i];
                out[4 * i] = static_cast<uint8_t>(v);
                out[4 * i + 1] = static_cast<uint8_t>(v >> 8);
                out[4 * i + 2] = static_cast<uint8_t>(v >> 16);
                out[4 * i + 3] = static_cast<uint8_t>(v >> 24);
        }
        ++_state[12];
        volatile uint32_t *w = x;
        for (int i = 0; i < 16; ++i) w[i] = 0;
        return;
}

void ChaCha20::fill(uint8_t *out, size_t size) {
        uint8_t buf[BlockBytes];
        while (size >= BlockBytes) {
                block(*reinterpret_cast<uint8_t(*)[BlockBytes]>(out));
                out += BlockBytes;
                size -= BlockBytes;
        }
        if (size != 0) {
                block(buf);
                std::memcpy(out, buf, size);
        }
        return;
}

BMDS_NAMESPACE_END
