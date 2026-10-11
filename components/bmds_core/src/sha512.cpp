/**
 * @file      sha512.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/sha512.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint64_t Iv[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                    0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};

        constexpr uint64_t K[80] = {
                0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
                0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
                0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
                0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
                0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
                0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
                0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
                0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
                0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
                0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
                0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
                0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
                0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
                0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
                0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
                0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
        };

        uint64_t rotr(uint64_t v, int n) {
                return v >> n | v << (64 - n);
        }

} // namespace

Sha512::Sha512() {
        std::memcpy(_h, Iv, sizeof(_h));
}

void Sha512::compress() {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
                uint64_t v = 0;
                for (int b = 0; b < 8; ++b) v = v << 8 | _block[8 * i + b];
                w[i] = v;
        }
        for (int i = 16; i < 80; ++i) {
                const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
                const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = _h[0], b = _h[1], c = _h[2], d = _h[3], e = _h[4], f = _h[5], g = _h[6], h = _h[7];
        for (int i = 0; i < 80; ++i) {
                const uint64_t t1 = h + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
                const uint64_t t2 = (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
        }
        _h[0] += a;
        _h[1] += b;
        _h[2] += c;
        _h[3] += d;
        _h[4] += e;
        _h[5] += f;
        _h[6] += g;
        _h[7] += h;
        return;
}

void Sha512::update(const void *data, size_t size) {
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

void Sha512::final(uint8_t (&out)[DigestBytes]) {
        const uint64_t bits = _bytes * 8;
        _block[_used++] = 0x80;
        if (_used > BlockBytes - 16) {
                std::memset(_block + _used, 0, BlockBytes - _used);
                compress();
                _used = 0;
        }
        std::memset(_block + _used, 0, BlockBytes - _used);
        for (int b = 0; b < 8; ++b) _block[BlockBytes - 1 - b] = static_cast<uint8_t>(bits >> (8 * b));
        compress();
        for (int i = 0; i < 8; ++i)
                for (int b = 0; b < 8; ++b) out[8 * i + b] = static_cast<uint8_t>(_h[i] >> (56 - 8 * b));
        // Spent: nothing of the input stays behind.
        volatile uint8_t *wipe = reinterpret_cast<volatile uint8_t *>(this);
        for (size_t i = 0; i < sizeof(*this); ++i) wipe[i] = 0;
        return;
}

BMDS_NAMESPACE_END
