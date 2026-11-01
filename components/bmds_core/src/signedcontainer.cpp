/**
 * @file      signedcontainer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/pkgmanifest.h>
#include <bmds/signedcontainer.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        uint32_t u32(const uint8_t *p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        void put16(uint8_t *p, uint16_t v) {
                p[0] = uint8_t(v);
                p[1] = uint8_t(v >> 8);
                return;
        }

        void put32(uint8_t *p, uint32_t v) {
                for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
                return;
        }

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        bool allZero(const uint8_t *p, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                        if (p[i] != 0) return false;
                }
                return true;
        }

} // namespace

const SignedContainer::Section *SignedContainer::Head::find(uint32_t type) const {
        for (size_t i = 0; i < sectionCount; ++i) {
                if (sections[i].type == type) return &sections[i];
        }
        return nullptr;
}

void SignedContainer::keyId(const Key &key, uint8_t (&out)[KeyIdBytes]) {
        Blake2s h;
        h.update(key.bytes, sizeof(key.bytes));
        uint8_t digest[Blake2s::DigestBytes];
        h.final(digest);
        std::memcpy(out, digest, KeyIdBytes);
        return;
}

const char *SignedContainer::verdictText(Verdict v) {
        switch (v) {
                case Verdict::Ok: return "good";
                case Verdict::NotAPackage: return "not a file of this kind";
                case Verdict::NewerFormat: return "a newer format than this firmware reads";
                case Verdict::Malformed: return "damaged";
                case Verdict::Truncated: return "cut short";
                case Verdict::BadSignature: return "its signature doesn't check";
                case Verdict::BadHash: return "its contents don't match their hash";
        }
        return "?";
}

SignedContainer::Verdict SignedContainer::headBytes(Span<const uint8_t> header, const Kind &kind, size_t &bytes) {
        if (header.size() < 8 || std::memcmp(header.data(), kind.magic, 4) != 0) return Verdict::NotAPackage;
        const uint8_t *h = header.data();
        if (u16(h + 4) != kind.format) return u16(h + 4) > kind.format ? Verdict::NewerFormat : Verdict::Malformed;
        if (header.size() < kind.headerBytes) return Verdict::Truncated;
        if (u16(h + 6) != kind.headerBytes) return Verdict::Malformed;
        const uint32_t count = u32(h + 12);
        if (count > MaxSections) return Verdict::Malformed;
        bytes = kind.headerBytes + count * SectionBytes + Ed25519::SignatureBytes;
        return Verdict::Ok;
}

SignedContainer::Verdict SignedContainer::parse(Span<const uint8_t> head, uint64_t fileBytes, const Kind &kind,
                                                Head &out) {
        size_t        need = 0;
        const Verdict v = headBytes(head, kind, need);
        if (v != Verdict::Ok) return v;
        if (head.size() < need || fileBytes < need) return Verdict::Truncated;
        const uint8_t *h = head.data();
        // Flags this code doesn't know may change what the file means: refused, not ignored.
        if (u32(h + 8) != 0) return Verdict::NewerFormat;
        out.sectionCount = u32(h + 12);
        std::memcpy(out.keyId, h + 16, KeyIdBytes);
        out.signedBytes = kind.headerBytes + out.sectionCount * SectionBytes;
        out.headBytes = need;
        for (size_t i = 0; i < out.sectionCount; ++i) {
                const uint8_t *e = h + kind.headerBytes + i * SectionBytes;
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

Error SignedContainer::parseKey(StringView line, Key &key, StringView &comment) {
        constexpr StringView Kind("ed25519 ");
        if (line.endsWith('\n')) line = line.left(line.size() - 1);
        if (line.endsWith('\r')) line = line.left(line.size() - 1);
        if (!line.startsWith(Kind) || line.size() < Kind.size() + 2 * sizeof(key.bytes)) return Error::CorruptData;
        const StringView hexText = line.mid(Kind.size(), 2 * sizeof(key.bytes));
        for (size_t i = 0; i < sizeof(key.bytes); ++i) {
                const int hi = hexDigit(hexText[2 * i]);
                const int lo = hexDigit(hexText[2 * i + 1]);
                if (hi < 0 || lo < 0) return Error::CorruptData;
                key.bytes[i] = uint8_t(hi * 16 + lo);
        }
        StringView rest = line.mid(Kind.size() + hexText.size());
        if (!rest.isEmpty() && rest[0] != ' ') return Error::CorruptData;
        rest = rest.trimmed();
        if (rest.size() > MaxKeyCommentBytes || !PkgManifest::isCleanText(rest)) return Error::CorruptData;
        comment = rest;
        return Error::Ok;
}

bool SignedContainer::isUnsigned(const Head &parsed) {
        return allZero(parsed.keyId, sizeof(parsed.keyId)) && allZero(parsed.signature, sizeof(parsed.signature));
}

Error SignedContainer::buildUnsigned(const Kind &kind, ByteSpan body, Span<const Part> parts, List<uint8_t> &out) {
        out.clear();
        if (kind.headerBytes < PrefixBytes || body.size() != kind.headerBytes - PrefixBytes ||
            parts.size() > MaxSections)
                return Error::Invalid;
        const size_t head = kind.headerBytes + parts.size() * SectionBytes + Ed25519::SignatureBytes;
        uint64_t     total = head;
        for (size_t i = 0; i < parts.size(); ++i) {
                if (parts[i].type == 0) return Error::Invalid;
                for (size_t j = 0; j < i; ++j) {
                        if (parts[j].type == parts[i].type) return Error::Invalid;
                }
                total += parts[i].bytes.size();
        }
        if (total > UINT32_MAX) return Error::Invalid;
        Error err = out.reserve(size_t(total));
        for (size_t i = 0; err.isOk() && i < head; ++i) err = out.append(uint8_t(0));
        if (err.isError()) return err;
        uint8_t *h = out.data();
        std::memcpy(h, kind.magic, 4);
        put16(h + 4, kind.format);
        put16(h + 6, kind.headerBytes);
        put32(h + 12, uint32_t(parts.size()));
        if (!body.isEmpty()) std::memcpy(h + PrefixBytes, body.data(), body.size());
        uint32_t offset = uint32_t(head);
        for (size_t i = 0; i < parts.size(); ++i) {
                uint8_t *e = h + kind.headerBytes + i * SectionBytes;
                put32(e, parts[i].type);
                put32(e + 8, offset);
                put32(e + 12, uint32_t(parts[i].bytes.size()));
                Blake2s hash;
                hash.update(parts[i].bytes.data(), parts[i].bytes.size());
                uint8_t digest[HashBytes];
                hash.final(digest);
                std::memcpy(e + 16, digest, HashBytes);
                offset += uint32_t(parts[i].bytes.size());
        }
        for (const Part &part : parts) {
                err = out.append(part.bytes);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

SignedContainer::Verdict SignedContainer::verify(const Head &parsed, Span<const uint8_t> head, Span<const Key> keys,
                                                 size_t &index) {
        if (head.size() < parsed.signedBytes) return Verdict::Truncated;
        for (size_t i = 0; i < keys.size(); ++i) {
                uint8_t id[KeyIdBytes];
                keyId(keys[i], id);
                if (std::memcmp(id, parsed.keyId, KeyIdBytes) != 0) continue;
                if (Ed25519::verify(parsed.signature, keys[i].bytes, head.data(), parsed.signedBytes)) {
                        index = i;
                        return Verdict::Ok;
                }
        }
        return Verdict::BadSignature;
}

BMDS_NAMESPACE_END
