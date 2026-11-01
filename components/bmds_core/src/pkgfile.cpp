/**
 * @file      pkgfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/inflate.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgversion.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t u32(const uint8_t *p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        uint64_t u64(const uint8_t *p) {
                return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32);
        }

        // A NUL-padded field of N bytes: printable ASCII, then only NULs.
        template <size_t N> bool text(const uint8_t *p, char (&out)[N + 1]) {
                size_t n = 0;
                while (n < N && p[n] != 0) {
                        if (p[n] < 0x20 || p[n] > 0x7e) return false;
                        ++n;
                }
                for (size_t i = n; i < N; ++i) {
                        if (p[i] != 0) return false;
                }
                std::memcpy(out, p, n);
                out[n] = '\0';
                return true;
        }

        uint16_t u16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | (p[1] << 8));
        }

        uint32_t be32(const uint8_t *p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        // A PNG's signature and its IHDR chunk first, saying @p side by @p side.
        bool isPng(const uint8_t *p, size_t bytes, uint16_t side) {
                constexpr uint8_t Signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
                if (bytes < 24 || std::memcmp(p, Signature, 8) != 0 || std::memcmp(p + 12, "IHDR", 4) != 0) {
                        return false;
                }
                return be32(p + 16) == side && be32(p + 20) == side;
        }

        bool isLowerOrDigit(char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        }

} // namespace

bool PkgFile::isValidName(StringView name) {
        if (name.isEmpty() || name.size() > NameBytes || !isLowerOrDigit(name[0])) return false;
        for (char c : name) {
                if (!isLowerOrDigit(c) && c != '+' && c != '-' && c != '.') return false;
        }
        return true;
}

bool PkgFile::isValidArch(StringView arch) {
        if (arch.isEmpty() || arch.size() > ArchBytes) return false;
        for (char c : arch) {
                if (!isLowerOrDigit(c)) return false;
        }
        return true;
}

const char *PkgFile::verdictText(Verdict v) {
        switch (v) {
                case Verdict::NotAPackage: return "not a package";
                case Verdict::NewerFormat: return "a newer package format than this firmware reads";
                case Verdict::Malformed: return "a damaged package";
                default: return SignedContainer::verdictText(v);
        }
}

PkgFile::Verdict PkgFile::parsePackage(Span<const uint8_t> head, uint64_t fileBytes, Package &out) {
        out = Package{};
        const Verdict v = SignedContainer::parse(head, fileBytes, PackageKind, out);
        if (v != Verdict::Ok) return v;
        const uint8_t *h = head.data() + SignedContainer::PrefixBytes;
        if (!text<NameBytes>(h, out.name) || !text<VersionBytes>(h + 32, out.version) ||
            !text<ArchBytes>(h + 64, out.arch)) {
                return Verdict::Malformed;
        }
        if (!isValidName(out.name) || !PkgVersion::isValid(out.version) || !isValidArch(out.arch)) {
                return Verdict::Malformed;
        }
        out.abiVersion = u32(h + 80);
        if (u32(h + 84) != 0) return Verdict::Malformed;
        out.installedBytes = u64(h + 88);
        out.madeAt = static_cast<int64_t>(u64(h + 96));
        return Verdict::Ok;
}

PkgFile::Verdict PkgFile::parseIndex(Span<const uint8_t> head, uint64_t fileBytes, Index &out) {
        out = Index{};
        const Verdict v = SignedContainer::parse(head, fileBytes, IndexKind, out);
        if (v != Verdict::Ok) return v;
        const uint8_t *h = head.data() + SignedContainer::PrefixBytes;
        out.madeAt = static_cast<int64_t>(u64(h));
        out.packageCount = u32(h + 8);
        if (u32(h + 12) != 0) return Verdict::Malformed;
        if (!text<LabelBytes>(h + 16, out.label)) return Verdict::Malformed;
        return Verdict::Ok;
}

Error PkgFile::buildIndex(StringView stanzas, ByteSpan icons, uint32_t count, int64_t madeAt, StringView label,
                          List<uint8_t> &out) {
        if (label.size() > LabelBytes) return Error::Invalid;
        for (char c : label) {
                if (c < 0x20 || c > 0x7e) return Error::Invalid;
        }
        uint8_t body[IndexHeaderBytes - SignedContainer::PrefixBytes] = {};
        for (int i = 0; i < 8; ++i) body[i] = uint8_t(uint64_t(madeAt) >> (8 * i));
        for (int i = 0; i < 4; ++i) body[8 + i] = uint8_t(count >> (8 * i));
        if (!label.isEmpty()) std::memcpy(body + 16, label.data(), label.size());
        const SignedContainer::Part parts[] = {
                {static_cast<uint32_t>(IndexSectionType::Stanzas),
                 ByteSpan(reinterpret_cast<const uint8_t *>(stanzas.data()), stanzas.size())},
                {static_cast<uint32_t>(IndexSectionType::Icons), icons}};
        return SignedContainer::buildUnsigned(IndexKind, ByteSpan(body, sizeof(body)),
                                              Span<const SignedContainer::Part>(parts, icons.isEmpty() ? 1 : 2), out);
}

Error PkgFile::appendIcon(List<uint8_t> &section, StringView name, uint16_t side, ByteSpan png) {
        if (name.size() > 255 || side == 0 || side > MaxIconSide || png.size() > MaxIconBytes) return Error::Invalid;
        uint8_t    head[8];
        const auto bytes = uint32_t(png.size());
        Error      err = section.append(uint8_t(name.size()));
        if (err.isOk())
                err = section.append(Span<const uint8_t>(reinterpret_cast<const uint8_t *>(name.data()), name.size()));
        head[0] = uint8_t(side);
        head[1] = uint8_t(side >> 8);
        head[2] = uint8_t(side);
        head[3] = uint8_t(side >> 8);
        for (int i = 0; i < 4; ++i) head[4 + i] = uint8_t(bytes >> (8 * i));
        if (err.isOk()) err = section.append(Span<const uint8_t>(head, 8));
        if (err.isOk()) err = section.append(png);
        return err;
}

Error PkgFile::readBlock(uint32_t word, size_t decodedBytes, Block &out) {
        if (decodedBytes == 0 || decodedBytes > BlockBytes) return Error::Invalid;
        out.stored = (word & BlockStored) != 0;
        out.bytes = word & ~BlockStored;
        if (out.stored ? out.bytes != decodedBytes : (out.bytes == 0 || out.bytes > BlockBytes)) {
                return Error::CorruptData;
        }
        return Error::Ok;
}

Error PkgFile::decodeBlock(const Block &block, ByteSpan in, WritableByteSpan out) {
        if (in.size() != block.bytes || out.size() == 0 || out.size() > BlockBytes) return Error::Invalid;
        if (block.stored) {
                if (in.size() != out.size()) return Error::CorruptData;
                std::memcpy(out.data(), in.data(), in.size());
                return Error::Ok;
        }
        const Result<size_t> n = Inflate::raw(in, out);
        if (n.isError()) return n.error().code() == Error::BufferTooSmall ? Error::CorruptData : n.error();
        return n.value() == out.size() ? Error::Ok : Error::CorruptData;
}

Error PkgFile::IconReader::next(Icon &out) {
        if (_pos >= _section.size()) return Error::EndOfFile;
        const uint8_t *p = _section.data() + _pos;
        const size_t   left = _section.size() - _pos;
        const size_t   nameBytes = p[0];
        if (left < 1 + nameBytes + 8) return Error::CorruptData;
        out = Icon{};
        out.name = StringView(reinterpret_cast<const char *>(p + 1), nameBytes);
        if (_named ? !isValidName(out.name) : nameBytes != 0) return Error::CorruptData;
        const uint8_t *h = p + 1 + nameBytes;
        const uint16_t width = u16(h);
        const uint16_t height = u16(h + 2);
        const uint32_t png = u32(h + 4);
        if (width != height || width == 0 || width > MaxIconSide || png > MaxIconBytes) return Error::CorruptData;
        if (png > left - 1 - nameBytes - 8) return Error::CorruptData;
        if (!isPng(h + 8, png, width)) return Error::CorruptData;
        out.side = width;
        out.png = ByteSpan(h + 8, png);
        _pos += 1 + nameBytes + 8 + png;
        return Error::Ok;
}

BMDS_NAMESPACE_END
