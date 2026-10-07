/**
 * @file      memorystream.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <bmds/blake2s.h>
#include <bmds/chacha20.h>
#include <bmds/memorystream.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::MemoryStream;
using bmds::NullStream;
using bmds::WaitForever;
using bmds::WritableByteSpan;

TEST_CASE("MemoryStream") {
        const size_t base = HostMemory::liveAllocations();
        {
                MemoryStream s(bmds::MemSpace::Psram);
                CHECK_FALSE(s.isTerminal());
                uint8_t buf[8];
                CHECK(s.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);

                CHECK(s.write(bmds::StringView("hello, "), WaitForever).isOk());
                CHECK(s.write(bmds::StringView("world, this is long enough to allocate"), WaitForever).isOk());
                CHECK(s.contents().startsWith("hello, world"));

                auto [n, err] = s.read(WritableByteSpan(buf), WaitForever);
                CHECK(err.isOk());
                CHECK(n == 8);
                CHECK(std::string(reinterpret_cast<char *>(buf), 8) == "hello, w");
                CHECK(s.contents().startsWith("orld"));

                bmds::String taken = s.takeContents();
                CHECK(taken.view().startsWith("orld, this"));
                CHECK(s.contents().isEmpty());
                CHECK(s.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);

                CHECK(s.write(bmds::StringView("abc"), WaitForever).isOk());
                {
                        HostMemory::FailGuard fail;
                        auto [w, werr] = s.write(bmds::StringView("this will not fit inline anymore"), WaitForever);
                        CHECK(werr == Error::NoMem);
                        CHECK(w == 0);
                }
                CHECK(s.contents() == "abc");
                s.clear();
                CHECK(s.contents().isEmpty());
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("NullStream") {
        NullStream &null = NullStream::instance();
        uint8_t     buf[4];
        CHECK(null.read(WritableByteSpan(buf), WaitForever).error() == Error::EndOfFile);
        auto [n, err] = null.write(bmds::StringView("gone"), WaitForever);
        CHECK(err.isOk());
        CHECK(n == 4);
}

TEST_CASE("ChaCha20: RFC 8439's key stream") {
        // RFC 8439 §2.3.2: key 00..1f, nonce 00 00 00 09 00 00 00 4a 00 00 00 00, counter 1.
        uint8_t key[bmds::ChaCha20::KeyBytes];
        for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
        const uint8_t  nonce[bmds::ChaCha20::NonceBytes] = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0};
        bmds::ChaCha20 c(key, nonce, 1);
        uint8_t        out[bmds::ChaCha20::BlockBytes];
        c.block(out);
        const uint8_t expected[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
                                      0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4};
        CHECK(std::memcmp(out, expected, sizeof(expected)) == 0);
        const uint8_t last[4] = {0xa2, 0x50, 0x3c, 0x4e};
        CHECK(std::memcmp(out + 60, last, sizeof(last)) == 0);
        // fill() is the same stream, a partial block included.
        bmds::ChaCha20 d(key, nonce, 1);
        uint8_t        filled[100];
        d.fill(filled, sizeof(filled));
        CHECK(std::memcmp(filled, out, sizeof(out)) == 0);
}

TEST_CASE("BLAKE2s: RFC 7693's digests") {
        // Expected values: Python's hashlib.blake2s.
        const auto hex = [](const std::string &message) {
                bmds::Blake2s h;
                h.update(message.data(), message.size());
                uint8_t digest[bmds::Blake2s::DigestBytes];
                h.final(digest);
                std::string out;
                for (uint8_t b : digest) {
                        char two[3];
                        std::snprintf(two, sizeof(two), "%02x", b);
                        out += two;
                }
                return out;
        };
        std::string block(64, '\0');
        for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>(i);
        std::string longer(1000, '\0');
        for (size_t i = 0; i < longer.size(); ++i) longer[i] = static_cast<char>(i % 251);
        CHECK(hex("") == "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9");
        CHECK(hex("abc") == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
        CHECK(hex(block) == "56f34e8b96557e90c1f24b52d0c89d51086acf1b00f634cf1dde9233b8eaaa3e");
        CHECK(hex(block + '@') == "1b53ee94aaf34e4b159d48de352c7f0661d0a40edff95a0b1639b4090e974472");
        CHECK(hex(longer) == "1c067a5e746fb0f6734efac9a8cdb0e11061f0077f255184365c690115392501");
        // In pieces: the same.
        bmds::Blake2s h;
        for (size_t i = 0; i < longer.size(); i += 7)
                h.update(longer.data() + i, std::min<size_t>(7, longer.size() - i));
        uint8_t digest[bmds::Blake2s::DigestBytes];
        h.final(digest);
        char first[3];
        std::snprintf(first, sizeof(first), "%02x", digest[0]);
        CHECK(std::string(first) == "1c");
}

TEST_CASE("Random pool: mixing and fast key erasure") {
        using bmds::RandomPool;
        RandomPool a;
        RandomPool b;
        a.mix("seed", 4);
        b.mix("seed", 4);
        uint8_t ka[RandomPool::KeyBytes];
        uint8_t kb[RandomPool::KeyBytes];
        a.next(ka);
        b.next(kb);
        CHECK(std::memcmp(ka, kb, sizeof(ka)) == 0); // deterministic from what went in
        a.next(ka);
        CHECK(std::memcmp(ka, kb, sizeof(ka)) != 0); // each read its own key
        b.next(kb);
        CHECK(std::memcmp(ka, kb, sizeof(ka)) == 0);
        b.mix("more", 4);
        a.next(ka);
        b.next(kb);
        CHECK(std::memcmp(ka, kb, sizeof(ka)) != 0); // input changes everything after
        // Output: the read key's ChaCha20 stream, any length.
        uint8_t one[100];
        uint8_t two[100];
        RandomPool::generate(ka, one, sizeof(one));
        RandomPool::generate(ka, two, 37);
        CHECK(std::memcmp(one, two, 37) == 0);
        // The kernel's pool: two reads differ.
        bmds::Random::mix("x", 1);
        uint8_t r1[48];
        uint8_t r2[48];
        bmds::Random::fill(r1, sizeof(r1));
        bmds::Random::fill(r2, sizeof(r2));
        CHECK(std::memcmp(r1, r2, sizeof(r1)) != 0);
}
