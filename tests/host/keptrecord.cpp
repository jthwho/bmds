/**
 * @file      keptrecord.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstring>
#include <bmds/keptrecord.h>

using bmds::KeptRecord;

namespace {

        struct Payload {
                        uint32_t count;
                        uint64_t total;
                        char     name[9]; // a size that isn't a multiple of 4
        };

        constexpr uint32_t Magic = 0x54534554; // "TEST"
        constexpr size_t   Words = KeptRecord::storageWords(sizeof(Payload));

        Payload make(uint32_t n) {
                Payload p;
                std::memset(&p, 0, sizeof(p));
                p.count = n;
                p.total = uint64_t(n) * 1000003;
                std::strcpy(p.name, "kept");
                return p;
        }

} // namespace

TEST_CASE("KeptRecord: garbage doesn't load; saved records do, the newest") {
        uint32_t storage[Words];
        std::memset(storage, 0xa5, sizeof(storage));
        KeptRecord r(storage, sizeof(Payload), Magic);
        Payload    out;
        CHECK_FALSE(r.load(&out));
        for (uint32_t n = 1; n <= 5; ++n) {
                const Payload in = make(n);
                r.save(&in);
                KeptRecord again(storage, sizeof(Payload), Magic); // as after a reset
                REQUIRE(again.load(&out));
                CHECK(std::memcmp(&in, &out, sizeof(in)) == 0);
        }
}

TEST_CASE("KeptRecord: a save cut short leaves the older copy") {
        uint32_t      storage[Words] = {};
        KeptRecord    r(storage, sizeof(Payload), Magic);
        const Payload first = make(1);
        const Payload second = make(2);
        r.save(&first);
        r.save(&second);
        // The newest copy (sequence 2: copy 0) torn: its magic not yet set, or a byte changed.
        storage[0] = 0;
        Payload    out;
        KeptRecord a(storage, sizeof(Payload), Magic);
        REQUIRE(a.load(&out));
        CHECK(out.count == 1);
        storage[0] = Magic;
        reinterpret_cast<uint8_t *>(storage)[KeptRecord::HeaderBytes + 3] ^= 1;
        KeptRecord b(storage, sizeof(Payload), Magic);
        REQUIRE(b.load(&out));
        CHECK(out.count == 1);
        // Saving after loading the older goes on from it and replaces the torn copy.
        const Payload third = make(3);
        b.save(&third);
        KeptRecord c(storage, sizeof(Payload), Magic);
        REQUIRE(c.load(&out));
        CHECK(out.count == 3);
}

TEST_CASE("KeptRecord: another magic or size doesn't pass") {
        uint32_t      storage[Words] = {};
        KeptRecord    r(storage, sizeof(Payload), Magic);
        const Payload in = make(7);
        r.save(&in);
        Payload    out;
        KeptRecord other(storage, sizeof(Payload), Magic + 1);
        CHECK_FALSE(other.load(&out));
        KeptRecord smaller(storage, sizeof(Payload) - 4, Magic);
        CHECK_FALSE(smaller.load(&out));
}
