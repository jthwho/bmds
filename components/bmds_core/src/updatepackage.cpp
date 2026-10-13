/**
 * @file      updatepackage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/updatepackage.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        uint32_t u32(const uint8_t *p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        int64_t i64(const uint8_t *p) {
                return static_cast<int64_t>(uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32));
        }

        // A NUL-padded field of 32 bytes: printable ASCII, then only NULs.
        bool text(const uint8_t *p, char (&out)[33]) {
                size_t n = 0;
                while (n < 32 && p[n] != 0) {
                        if (p[n] < 0x20 || p[n] > 0x7e) return false;
                        ++n;
                }
                for (size_t i = n; i < 32; ++i) {
                        if (p[i] != 0) return false;
                }
                std::memcpy(out, p, n);
                out[n] = '\0';
                return true;
        }

        constexpr uint8_t Magic[4] = {'B', 'M', 'U', 'P'};

} // namespace

const UpdatePackage::Section *UpdatePackage::Package::find(SectionType type) const {
        for (size_t i = 0; i < sectionCount; ++i) {
                if (sections[i].type == static_cast<uint32_t>(type)) return &sections[i];
        }
        return nullptr;
}

const char *UpdatePackage::verdictText(Verdict v) {
        switch (v) {
                case Verdict::Ok: return "good";
                case Verdict::NotAPackage: return "not an update package";
                case Verdict::NewerFormat: return "a newer package format than this firmware reads";
                case Verdict::Malformed: return "a damaged package";
                case Verdict::Truncated: return "cut short";
                case Verdict::BadSignature: return "its signature doesn't check";
                case Verdict::BadHash: return "its contents don't match their hash";
        }
        return "?";
}

UpdatePackage::Verdict UpdatePackage::headBytes(Span<const uint8_t> header, size_t &bytes) {
        if (header.size() < 8 || std::memcmp(header.data(), Magic, 4) != 0) return Verdict::NotAPackage;
        const uint8_t *h = header.data();
        if (u16(h + 4) != Format) return u16(h + 4) > Format ? Verdict::NewerFormat : Verdict::Malformed;
        if (header.size() < HeaderBytes) return Verdict::Truncated;
        if (u16(h + 6) != HeaderBytes) return Verdict::Malformed;
        const uint32_t count = u32(h + 12);
        if (count > MaxSections) return Verdict::Malformed;
        bytes = HeaderBytes + count * SectionBytes + Ed25519::SignatureBytes;
        return Verdict::Ok;
}

UpdatePackage::Verdict UpdatePackage::parse(Span<const uint8_t> head, uint64_t fileBytes, Package &out) {
        size_t        need = 0;
        const Verdict v = headBytes(head, need);
        if (v != Verdict::Ok) return v;
        if (head.size() < need || fileBytes < need) return Verdict::Truncated;
        const uint8_t *h = head.data();
        // Flags this code doesn't know may change what the package means: refused, not ignored.
        if (u32(h + 8) != 0) return Verdict::NewerFormat;
        out = Package{};
        out.sectionCount = u32(h + 12);
        out.abiVersion = u32(h + 16);
        out.minChipRevision = u16(h + 20);
        if (u16(h + 22) != 0) return Verdict::Malformed;
        if (!text(h + 24, out.board) || !text(h + 56, out.version)) return Verdict::Malformed;
        std::memcpy(out.buildId, h + 88, sizeof(out.buildId));
        out.madeAt = i64(h + 120);
        out.signedBytes = HeaderBytes + out.sectionCount * SectionBytes;
        out.headBytes = need;
        for (size_t i = 0; i < out.sectionCount; ++i) {
                const uint8_t *e = h + HeaderBytes + i * SectionBytes;
                Section       &s = out.sections[i];
                s.type = u32(e);
                if (u32(e + 4) != 0) return Verdict::NewerFormat;
                s.offset = u32(e + 8);
                s.size = u32(e + 12);
                std::memcpy(s.hash, e + 16, HashBytes);
                if (s.type == 0) return Verdict::Malformed;
                if (s.offset < need) return Verdict::Malformed;
                if (uint64_t(s.offset) + s.size > fileBytes) return Verdict::Truncated;
                for (size_t j = 0; j < i; ++j) {
                        if (out.sections[j].type == s.type) return Verdict::Malformed;
                }
        }
        std::memcpy(out.signature, h + out.signedBytes, sizeof(out.signature));
        return Verdict::Ok;
}

UpdatePackage::Verdict UpdatePackage::verify(const Package &package, Span<const uint8_t> head, Span<const Key> keys) {
        if (head.size() < package.signedBytes) return Verdict::Truncated;
        for (const Key &k : keys) {
                if (Ed25519::verify(package.signature, k.bytes, head.data(), package.signedBytes)) return Verdict::Ok;
        }
        return Verdict::BadSignature;
}

BMDS_NAMESPACE_END
