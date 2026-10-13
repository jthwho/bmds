/**
 * @file      sha1.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/sha1.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t rotl(uint32_t x, int n) {
                return (x << n) | (x >> (32 - n));
        }

} // namespace

Sha1::Sha1() : _h{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u} {}

void Sha1::compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
                w[i] = uint32_t(_block[4 * i]) << 24 | uint32_t(_block[4 * i + 1]) << 16 |
                       uint32_t(_block[4 * i + 2]) << 8 | _block[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3], e = _h[4];
        for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) {
                        f = (b & c) | (~b & d);
                        k = 0x5a827999u;
                } else if (i < 40) {
                        f = b ^ c ^ d;
                        k = 0x6ed9eba1u;
                } else if (i < 60) {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8f1bbcdcu;
                } else {
                        f = b ^ c ^ d;
                        k = 0xca62c1d6u;
                }
                const uint32_t t = rotl(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rotl(b, 30);
                b = a;
                a = t;
        }
        _h[0] += a;
        _h[1] += b;
        _h[2] += c;
        _h[3] += d;
        _h[4] += e;
        return;
}

void Sha1::update(const void *data, size_t size) {
        const auto *p = static_cast<const uint8_t *>(data);
        _bytes += size;
        while (size > 0) {
                const size_t n = size < BlockBytes - _used ? size : BlockBytes - _used;
                std::memcpy(_block + _used, p, n);
                _used += n;
                p += n;
                size -= n;
                if (_used == BlockBytes) {
                        compress();
                        _used = 0;
                }
        }
        return;
}

void Sha1::final(uint8_t (&out)[DigestBytes]) {
        const uint64_t bits = _bytes * 8;
        const uint8_t  one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while (_used != BlockBytes - 8) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(length, sizeof(length));
        for (int i = 0; i < 5; ++i)
                for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(_h[i] >> (24 - 8 * j));
        return;
}

BMDS_NAMESPACE_END
