/**
 * @file      ghash.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * GHASH with 8-bit tables (ghash.h). GCM's bit order: a block's first byte's top bit is x^0, so
 * multiplying by x shifts the block right a bit, and what leaves x^127 comes back as
 * x^128 = 1 + x + x^2 + x^7: 0xe1 in the first byte.
 */

#include <cstring>
#include <bmds/ghash.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // What a byte shifted out of the block's end (times x^8) leaves: the reduction's 16 bits, at
        // the top of the first word. Bit k of the byte (from its bottom, x^(127-k)) becomes x^(135-k),
        // reduced: 0xe1 shifted right by (7-k) bits within the first two bytes.
        constexpr uint16_t reductionOf(unsigned byte) {
                uint16_t r = 0;
                for (unsigned k = 0; k < 8; ++k)
                        if (byte & (1u << k)) r ^= static_cast<uint16_t>(0xe100u >> (7 - k));
                return r;
        }

        struct Reductions {
                        uint16_t r[256];
        };

        constexpr Reductions makeReductions() {
                Reductions t{};
                for (unsigned b = 0; b < 256; ++b) t.r[b] = reductionOf(b);
                return t;
        }

        constexpr Reductions Reduce = makeReductions();

        uint32_t loadBe(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        void storeBe(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v >> 24);
                p[1] = static_cast<uint8_t>(v >> 16);
                p[2] = static_cast<uint8_t>(v >> 8);
                p[3] = static_cast<uint8_t>(v);
                return;
        }

} // namespace

void Ghash::setKey(const uint8_t (&h)[BlockBytes]) {
        // The byte 0x80 is x^0: H itself; each bit lower is one more factor of x.
        uint32_t v[4] = {loadBe(h), loadBe(h + 4), loadBe(h + 8), loadBe(h + 12)};
        for (unsigned bit = 0x80; bit != 0; bit >>= 1) {
                std::memcpy(_table[bit], v, sizeof(v));
                const bool carry = (v[3] & 1) != 0;
                v[3] = v[3] >> 1 | v[2] << 31;
                v[2] = v[2] >> 1 | v[1] << 31;
                v[1] = v[1] >> 1 | v[0] << 31;
                v[0] = v[0] >> 1 ^ (carry ? 0xe1000000u : 0u);
        }
        std::memset(_table[0], 0, sizeof(_table[0]));
        // The rest are sums of those: i = its top bit + what is below it.
        for (unsigned i = 2; i < 256; i <<= 1)
                for (unsigned j = 1; j < i; ++j)
                        for (unsigned w = 0; w < 4; ++w) _table[i + j][w] = _table[i][w] ^ _table[j][w];
        return;
}

void Ghash::update(uint8_t (&y)[BlockBytes], const void *data, size_t size) const {
        const auto *in = static_cast<const uint8_t *>(data);
        uint8_t     x[BlockBytes];
        std::memcpy(x, y, BlockBytes);
        while (size > 0) {
                const size_t n = size < BlockBytes ? size : BlockBytes;
                for (size_t i = 0; i < n; ++i) x[i] ^= in[i];
                in += n;
                size -= n;
                // x * H by Horner over its bytes, the last (highest powers) first.
                uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
                for (int i = BlockBytes - 1; i >= 0; --i) {
                        const unsigned out = z3 & 0xff;
                        z3 = z3 >> 8 | z2 << 24;
                        z2 = z2 >> 8 | z1 << 24;
                        z1 = z1 >> 8 | z0 << 24;
                        z0 = z0 >> 8 ^ uint32_t(Reduce.r[out]) << 16;
                        const uint32_t(&t)[4] = _table[x[i]];
                        z0 ^= t[0];
                        z1 ^= t[1];
                        z2 ^= t[2];
                        z3 ^= t[3];
                }
                storeBe(x, z0);
                storeBe(x + 4, z1);
                storeBe(x + 8, z2);
                storeBe(x + 12, z3);
        }
        std::memcpy(y, x, BlockBytes);
        return;
}

BMDS_NAMESPACE_END
