/**
 * @file      blake2s.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/blake2s.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

        constexpr uint8_t Sigma[10][16] = {
                {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
                {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
                {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
                {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
                {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
                {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
                {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
                {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
                {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
        };

        uint32_t rotr(uint32_t v, int n) {
                return v >> n | v << (32 - n);
        }

        void mixG(uint32_t *v, int a, int b, int c, int d, uint32_t x, uint32_t y) {
                v[a] = v[a] + v[b] + x;
                v[d] = rotr(v[d] ^ v[a], 16);
                v[c] = v[c] + v[d];
                v[b] = rotr(v[b] ^ v[c], 12);
                v[a] = v[a] + v[b] + y;
                v[d] = rotr(v[d] ^ v[a], 8);
                v[c] = v[c] + v[d];
                v[b] = rotr(v[b] ^ v[c], 7);
                return;
        }

} // namespace

Blake2s::Blake2s() {
        std::memcpy(_h, Iv, sizeof(_h));
        _h[0] ^= 0x01010000u ^ DigestBytes; // no key, fanout and depth 1
}

void Blake2s::compress(bool last) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i)
                m[i] = static_cast<uint32_t>(_block[4 * i]) | static_cast<uint32_t>(_block[4 * i + 1]) << 8 |
                       static_cast<uint32_t>(_block[4 * i + 2]) << 16 | static_cast<uint32_t>(_block[4 * i + 3]) << 24;
        uint32_t v[16];
        for (int i = 0; i < 8; ++i) {
                v[i] = _h[i];
                v[i + 8] = Iv[i];
        }
        v[12] ^= _t[0];
        v[13] ^= _t[1];
        if (last) v[14] = ~v[14];
        for (const auto &s : Sigma) {
                mixG(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
                mixG(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
                mixG(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
                mixG(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
                mixG(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
                mixG(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
                mixG(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
                mixG(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i) _h[i] ^= v[i] ^ v[i + 8];
        return;
}

void Blake2s::update(const void *data, size_t size) {
        const auto *p = static_cast<const uint8_t *>(data);
        while (size > 0) {
                // A full block is compressed only once more follows: the last is final()'s.
                if (_used == BlockBytes) {
                        _t[0] += BlockBytes;
                        if (_t[0] < BlockBytes) ++_t[1];
                        compress(false);
                        _used = 0;
                }
                const size_t n = size < BlockBytes - _used ? size : BlockBytes - _used;
                std::memcpy(_block + _used, p, n);
                _used += n;
                p += n;
                size -= n;
        }
        return;
}

void Blake2s::final(uint8_t (&out)[DigestBytes]) {
        _t[0] += static_cast<uint32_t>(_used);
        if (_t[0] < _used) ++_t[1];
        std::memset(_block + _used, 0, BlockBytes - _used);
        compress(true);
        for (int i = 0; i < 8; ++i) {
                out[4 * i] = static_cast<uint8_t>(_h[i]);
                out[4 * i + 1] = static_cast<uint8_t>(_h[i] >> 8);
                out[4 * i + 2] = static_cast<uint8_t>(_h[i] >> 16);
                out[4 * i + 3] = static_cast<uint8_t>(_h[i] >> 24);
        }
        // Nothing of the input left behind (the pool's key goes through here).
        volatile uint8_t *b = _block;
        for (size_t i = 0; i < sizeof(_block); ++i) b[i] = 0;
        volatile uint32_t *h = _h;
        for (int i = 0; i < 8; ++i) h[i] = 0;
        return;
}

BMDS_NAMESPACE_END
