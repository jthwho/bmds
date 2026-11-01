/**
 * @file      ghash.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ghash: the GCM specification's test cases, and agreement with SP 800-38D's bit-at-a-time
 * multiplication on random keys and data.
 */

#include <doctest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <bmds/ghash.h>

using bmds::Ghash;

namespace {

        std::vector<uint8_t> hex(const char *text) {
                std::vector<uint8_t> out;
                for (size_t i = 0; text[i] != '\0' && text[i + 1] != '\0'; i += 2)
                        out.push_back(static_cast<uint8_t>(std::stoi(std::string(text + i, 2), nullptr, 16)));
                return out;
        }

        // SP 800-38D's Algorithm 1: x * h a bit at a time.
        void multiply(uint8_t (&x)[16], const uint8_t (&h)[16]) {
                uint8_t z[16] = {};
                uint8_t v[16];
                std::memcpy(v, h, 16);
                for (int i = 0; i < 128; ++i) {
                        if (x[i / 8] & (0x80 >> (i % 8)))
                                for (int k = 0; k < 16; ++k) z[k] ^= v[k];
                        const bool lsb = (v[15] & 1) != 0;
                        for (int k = 15; k > 0; --k) v[k] = static_cast<uint8_t>(v[k] >> 1 | v[k - 1] << 7);
                        v[0] >>= 1;
                        if (lsb) v[0] ^= 0xe1;
                }
                std::memcpy(x, z, 16);
                return;
        }

        void reference(uint8_t (&y)[16], const uint8_t (&h)[16], const uint8_t *data, size_t size) {
                for (size_t at = 0; at < size; at += 16) {
                        for (size_t i = 0; i < 16 && at + i < size; ++i) y[i] ^= data[at + i];
                        multiply(y, h);
                }
                return;
        }

        // GHASH(H, A, C) as GCM ends it: A and C padded, then their lengths in bits.
        std::vector<uint8_t> ghash(const char *h, const char *a, const char *c) {
                uint8_t key[16];
                std::memcpy(key, hex(h).data(), 16);
                Ghash g;
                g.setKey(key);
                const std::vector<uint8_t> aad = hex(a), text = hex(c);
                uint8_t                    y[16] = {};
                g.update(y, aad.data(), aad.size());
                g.update(y, text.data(), text.size());
                uint8_t lengths[16] = {};
                for (int i = 0; i < 8; ++i) {
                        lengths[7 - i] = static_cast<uint8_t>(uint64_t(aad.size()) * 8 >> (8 * i));
                        lengths[15 - i] = static_cast<uint8_t>(uint64_t(text.size()) * 8 >> (8 * i));
                }
                g.update(y, lengths, sizeof(lengths));
                return std::vector<uint8_t>(y, y + 16);
        }

} // namespace

TEST_CASE("Ghash: the GCM specification's test cases") {
        // McGrew and Viega, "The Galois/Counter Mode of Operation", test cases 2 and 4.
        CHECK(ghash("66e94bd4ef8a2c3b884cfa59ca342b2e", "", "0388dace60b6a392f328c2b971b2fe78") ==
              hex("f38cbb1ad69223dcc3457ae5b6b0f885"));
        CHECK(ghash("b83b533708bf535d0aa6e52980d53b78", "feedfacedeadbeeffeedfacedeadbeefabaddad2",
                    "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5"
                    "aac84aa051ba30b396a0aac973d58e091") == hex("698e57f70e6ecc7fd9463b7260a9ae5f"));
}

TEST_CASE("Ghash: agrees with the bit-at-a-time multiplication") {
        std::mt19937 random(20261101);
        for (int round = 0; round < 200; ++round) {
                uint8_t h[16];
                for (uint8_t &b : h) b = static_cast<uint8_t>(random());
                if (round == 0) std::memset(h, 0, sizeof(h));
                if (round == 1) std::memset(h, 0xff, sizeof(h));
                Ghash g;
                g.setKey(h);
                std::vector<uint8_t> data(random() % 300);
                for (uint8_t &b : data) b = static_cast<uint8_t>(random());
                uint8_t y[16], want[16];
                for (int i = 0; i < 16; ++i) y[i] = want[i] = static_cast<uint8_t>(random());
                // In two calls, split anywhere: each call's last block is padded on its own.
                const size_t split = data.empty() ? 0 : (random() % data.size()) / 16 * 16;
                g.update(y, data.data(), split);
                g.update(y, data.data() + split, data.size() - split);
                reference(want, h, data.data(), data.size());
                REQUIRE(std::memcmp(y, want, 16) == 0);
        }
}
