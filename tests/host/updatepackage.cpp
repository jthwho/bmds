/**
 * @file      updatepackage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ed25519 (RFC 8032's vectors) and update packages (devplan/update.md §2): one made by
 * tools/mkupdate (tests/host/update/test.bmup, signed with keys/dev.key), and packages built here.
 */

#include <doctest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <bmds/blake2s.h>
#include <bmds/ed25519.h>
#include <bmds/signedcontainer.h>
#include <bmds/updatepackage.h>

using bmds::Span;
namespace Ed25519 = bmds::Ed25519;
namespace UpdatePackage = bmds::UpdatePackage;
using Verdict = UpdatePackage::Verdict;

namespace {

        std::vector<uint8_t> unhex(const std::string &hex) {
                std::vector<uint8_t> out;
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                        out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
                return out;
        }

        template <size_t N> void copy(uint8_t (&to)[N], const std::vector<uint8_t> &from) {
                REQUIRE(from.size() == N);
                std::memcpy(to, from.data(), N);
                return;
        }

        void put16(std::vector<uint8_t> &v, size_t at, uint16_t x) {
                v[at] = uint8_t(x);
                v[at + 1] = uint8_t(x >> 8);
                return;
        }

        void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
                for (int i = 0; i < 4; ++i) v[at + size_t(i)] = uint8_t(x >> (8 * i));
                return;
        }

        constexpr uint8_t Seed[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                      17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

        // A package of @p sections (type, bytes), signed with Seed; its public key to @p key.
        std::vector<uint8_t> build(const std::vector<std::pair<uint32_t, std::string>> &sections,
                                   UpdatePackage::Key                                  &key) {
                const size_t         count = sections.size();
                const size_t         head = UpdatePackage::HeaderBytes + count * UpdatePackage::SectionBytes + 64;
                std::vector<uint8_t> v(head, 0);
                uint8_t              unused[64];
                Ed25519::sign(Seed, "", 0, unused, key.bytes);
                uint8_t id[bmds::SignedContainer::KeyIdBytes];
                bmds::SignedContainer::keyId(key, id);
                std::memcpy(v.data(), "BMUP", 4);
                put16(v, 4, 1);
                put16(v, 6, 144);
                put32(v, 12, uint32_t(count));
                std::memcpy(v.data() + 16, id, sizeof(id));
                put32(v, 24, 1);
                put16(v, 28, 103);
                std::memcpy(v.data() + 32, "olimex_esp32p4_pc", 17);
                std::memcpy(v.data() + 64, "0.2.0", 5);
                v[96] = 0xab;
                v[128] = 0x10; // made at 16 s
                put16(v, 136, 0);
                put16(v, 138, 2);
                put16(v, 140, 1);
                size_t offset = head;
                for (size_t i = 0; i < count; ++i) {
                        const size_t e = UpdatePackage::HeaderBytes + i * UpdatePackage::SectionBytes;
                        put32(v, e, sections[i].first);
                        put32(v, e + 8, uint32_t(offset));
                        put32(v, e + 12, uint32_t(sections[i].second.size()));
                        bmds::Blake2s h;
                        h.update(sections[i].second.data(), sections[i].second.size());
                        uint8_t digest[32];
                        h.final(digest);
                        std::memcpy(v.data() + e + 16, digest, 32);
                        offset += sections[i].second.size();
                }
                uint8_t      signature[64];
                const size_t signedBytes = head - 64;
                Ed25519::sign(Seed, v.data(), signedBytes, signature, key.bytes);
                std::memcpy(v.data() + signedBytes, signature, 64);
                for (const auto &s : sections) v.insert(v.end(), s.second.begin(), s.second.end());
                return v;
        }

        Verdict parse(const std::vector<uint8_t> &v, UpdatePackage::Package &p) {
                return UpdatePackage::parse(Span<const uint8_t>(v.data(), v.size()), v.size(), p);
        }

        // Re-signs @p v after an edit of its signed bytes, so the edit itself is what is tested.
        void resign(std::vector<uint8_t> &v) {
                const size_t       count = size_t(v[12]);
                const size_t       signedBytes = UpdatePackage::HeaderBytes + count * UpdatePackage::SectionBytes;
                uint8_t            signature[64];
                UpdatePackage::Key key;
                Ed25519::sign(Seed, v.data(), signedBytes, signature, key.bytes);
                std::memcpy(v.data() + signedBytes, signature, 64);
                return;
        }

} // namespace

TEST_CASE("Ed25519: RFC 8032's vectors") {
        // Test 1 (the empty message) and test 2 (one byte).
        uint8_t seed[32], publicKey[32], expectKey[32], signature[64], expect[64];
        copy(seed, unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
        copy(expectKey, unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
        copy(expect, unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bac"
                           "c61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));
        Ed25519::sign(seed, "", 0, signature, publicKey);
        CHECK(std::memcmp(publicKey, expectKey, 32) == 0);
        CHECK(std::memcmp(signature, expect, 64) == 0);
        CHECK(Ed25519::verify(signature, publicKey, "", 0));
        CHECK_FALSE(Ed25519::verify(signature, publicKey, "x", 1));

        copy(seed, unhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"));
        copy(expectKey, unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"));
        copy(expect, unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e"
                           "458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"));
        const uint8_t message = 0x72;
        Ed25519::sign(seed, &message, 1, signature, publicKey);
        CHECK(std::memcmp(publicKey, expectKey, 32) == 0);
        CHECK(std::memcmp(signature, expect, 64) == 0);
        CHECK(Ed25519::verify(signature, publicKey, &message, 1));
        signature[10] ^= 1;
        CHECK_FALSE(Ed25519::verify(signature, publicKey, &message, 1));
}

TEST_CASE("Update packages: one made by tools/mkupdate") {
        std::ifstream        file(BMDS_TEST_UPDATE_DIR "/test.bmup", std::ios::binary);
        std::vector<uint8_t> v((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        REQUIRE(v.size() > UpdatePackage::HeaderBytes);
        std::ifstream pub(BMDS_TEST_KEYS_DIR "/dev.pub");
        std::string   kind, hex;
        pub >> kind >> hex;
        REQUIRE(kind == "ed25519");
        UpdatePackage::Key key;
        copy(key.bytes, unhex(hex));

        size_t head = 0;
        REQUIRE(UpdatePackage::headBytes(Span<const uint8_t>(v.data(), UpdatePackage::HeaderBytes), head) ==
                Verdict::Ok);
        CHECK(head == 144 + 2 * 48 + 64);
        UpdatePackage::Package p;
        REQUIRE(parse(v, p) == Verdict::Ok);
        CHECK(std::string(p.board) == "olimex_esp32p4_pc");
        CHECK(p.abiVersion == 1);
        CHECK(std::string(p.versionText) == "0.1.0");
        CHECK(p.version == UpdatePackage::Version{0, 1, 0});
        CHECK(p.sectionCount == 2);
        size_t index = 1;
        CHECK(UpdatePackage::verify(p, Span<const uint8_t>(v.data(), head), Span<const UpdatePackage::Key>(&key, 1),
                                    index) == Verdict::Ok);
        CHECK(index == 0);
        const UpdatePackage::Section *notes = p.find(UpdatePackage::SectionType::Notes);
        REQUIRE(notes != nullptr);
        CHECK(std::string(reinterpret_cast<const char *>(v.data() + notes->offset), notes->size) ==
              "Release notes: the host tests.\n");
        bmds::Blake2s h;
        h.update(v.data() + notes->offset, notes->size);
        uint8_t digest[32];
        h.final(digest);
        CHECK(std::memcmp(digest, notes->hash, 32) == 0);
        CHECK(p.find(UpdatePackage::SectionType::Firmware) == nullptr);
}

TEST_CASE("Update packages: a version's text read") {
        UpdatePackage::Version v;
        CHECK(UpdatePackage::parseVersion("0.2.1+72cba49a-dirty", v));
        CHECK(v == UpdatePackage::Version{0, 2, 1});
        CHECK(UpdatePackage::parseVersion("10.0.65535", v));
        CHECK(v == UpdatePackage::Version{10, 0, 65535});
        CHECK(UpdatePackage::parseVersion("1.2.3-rc1", v));
        CHECK_FALSE(UpdatePackage::parseVersion("72cba49a-dirty", v));
        CHECK_FALSE(UpdatePackage::parseVersion("1.2", v));
        CHECK_FALSE(UpdatePackage::parseVersion("1.2.65536", v));
        CHECK_FALSE(UpdatePackage::parseVersion("1.2.3x", v));
        CHECK_FALSE(UpdatePackage::parseVersion("", v));
}

TEST_CASE("Update packages: built, checked, and each fault refused") {
        UpdatePackage::Key     key;
        std::vector<uint8_t>   v = build({{1, "firmware bytes"}, {2, "BMRO rom bytes"}, {99, "a newer section"}}, key);
        UpdatePackage::Package p;
        REQUIRE(parse(v, p) == Verdict::Ok);
        CHECK(std::string(p.versionText) == "0.2.0");
        CHECK(p.version == UpdatePackage::Version{0, 2, 1});
        CHECK(p.version > UpdatePackage::Version{0, 1, 9});
        CHECK(p.version < UpdatePackage::Version{1, 0, 0});
        CHECK(p.minChipRevision == 103);
        CHECK(p.madeAt == 16);
        CHECK(p.buildId[0] == 0xab);
        CHECK(p.sectionCount == 3);
        size_t     index = 9;
        const auto verify = [&](const std::vector<uint8_t> &bytes, const UpdatePackage::Package &parsed,
                                Span<const UpdatePackage::Key> keys) {
                return UpdatePackage::verify(parsed, Span<const uint8_t>(bytes.data(), parsed.headBytes), keys, index);
        };
        const auto keys = [&](const UpdatePackage::Key &k) {
                return Span<const UpdatePackage::Key>(&k, 1);
        };
        CHECK(verify(v, p, keys(key)) == Verdict::Ok);
        CHECK(index == 0);
        UpdatePackage::Key other = key;
        other.bytes[0] ^= 1;
        CHECK(verify(v, p, keys(other)) == Verdict::BadSignature);
        const UpdatePackage::Key both[2] = {other, key};
        CHECK(verify(v, p, Span<const UpdatePackage::Key>(both, 2)) == Verdict::Ok);
        CHECK(index == 1);

        SUBCASE("a byte of the signed head changed") {
                v[68] ^= 1; // the version
                REQUIRE(parse(v, p) == Verdict::Ok);
                CHECK(verify(v, p, keys(key)) == Verdict::BadSignature);
        }
        SUBCASE("the key id names another key") {
                v[16] ^= 1;
                resign(v); // by the right key, but the id says otherwise: the key isn't tried
                REQUIRE(parse(v, p) == Verdict::Ok);
                CHECK(verify(v, p, keys(key)) == Verdict::BadSignature);
        }
        SUBCASE("not a package, a newer one, a damaged one") {
                std::vector<uint8_t> w = v;
                w[0] = 'X';
                CHECK(parse(w, p) == Verdict::NotAPackage);
                w = v;
                put16(w, 4, 2);
                CHECK(parse(w, p) == Verdict::NewerFormat);
                w = v;
                put32(w, 8, 1); // a flag this code doesn't know
                CHECK(parse(w, p) == Verdict::NewerFormat);
                w = v;
                put32(w, UpdatePackage::HeaderBytes + 4, 1); // a section's flag
                CHECK(parse(w, p) == Verdict::NewerFormat);
                w = v;
                put16(w, 6, 64);
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                put32(w, 12, 17);
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                w[32] = 0x07; // not printable
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                w[64 + 20] = 'x'; // text after the version's NUL
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                put16(w, 142, 1); // the version's reserved u16
                CHECK(parse(w, p) == Verdict::Malformed);
        }
        SUBCASE("sections outside the file, inside the head, or twice") {
                std::vector<uint8_t> w(v.begin(), v.end() - 1);
                CHECK(parse(w, p) == Verdict::Truncated);
                w.assign(v.begin(), v.begin() + 100);
                CHECK(parse(w, p) == Verdict::Truncated);
                w = v;
                put32(w, UpdatePackage::HeaderBytes + 8, 10); // the firmware's offset, inside the head
                resign(w);
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                put32(w, UpdatePackage::HeaderBytes + 48, 1); // the ROM listed as a second firmware
                resign(w);
                CHECK(parse(w, p) == Verdict::Malformed);
                w = v;
                put32(w, UpdatePackage::HeaderBytes, 0); // type 0
                CHECK(parse(w, p) == Verdict::Malformed);
        }
        SUBCASE("verdicts have words") {
                for (int i = 0; i <= int(Verdict::BadHash); ++i) {
                        CHECK(std::strlen(UpdatePackage::verdictText(Verdict(i))) > 0);
                }
        }
}

TEST_CASE("Update packages: an app image's chip revisions, judged as the bootloader does") {
        constexpr uint16_t P4 = 18; // CONFIG_IDF_FIRMWARE_CHIP_ID
        // A header as IDF writes for ESP32P4_SELECTS_REV_LESS_V3: v0.0 to v1.99.
        uint8_t image[UpdatePackage::AppHeaderBytes] = {0xe9, 3};
        image[12] = P4;
        image[17] = 199;
        UpdatePackage::ChipRevisions r;
        REQUIRE(UpdatePackage::appRevisions(Span<const uint8_t>(image, sizeof(image)), P4, r));
        CHECK(r.min == 0);
        CHECK(r.max == 199);
        CHECK(UpdatePackage::runsOn(r, 103, false)); // our boards: v1.3
        CHECK_FALSE(UpdatePackage::runsOn(r, 300, false));
        CHECK(UpdatePackage::runsOn(r, 300, true)); // the eFuse that disables the check

        // v3.1 and up; no greatest (0 or 0xFFFF).
        image[15] = 301 & 0xff;
        image[16] = 301 >> 8;
        image[17] = image[18] = 0xff;
        REQUIRE(UpdatePackage::appRevisions(Span<const uint8_t>(image, sizeof(image)), P4, r));
        CHECK(r.min == 301);
        CHECK_FALSE(UpdatePackage::runsOn(r, 103, false));
        CHECK_FALSE(UpdatePackage::runsOn(r, 103, true)); // the least is always checked
        CHECK(UpdatePackage::runsOn(r, 9999, false));
        r.max = 0;
        CHECK(UpdatePackage::runsOn(r, 9999, false));

        // Not an app image for this chip.
        CHECK_FALSE(UpdatePackage::appRevisions(Span<const uint8_t>(image, sizeof(image) - 1), P4, r));
        CHECK_FALSE(UpdatePackage::appRevisions(Span<const uint8_t>(image, sizeof(image)), 13, r));
        image[0] = 0;
        CHECK_FALSE(UpdatePackage::appRevisions(Span<const uint8_t>(image, sizeof(image)), P4, r));
}
