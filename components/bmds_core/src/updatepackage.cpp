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

} // namespace

bool UpdatePackage::parseVersion(const char *text, Version &out) {
        uint16_t parts[3] = {};
        for (int i = 0; i < 3; ++i) {
                if (i > 0 && *text++ != '.') return false;
                if (*text < '0' || *text > '9') return false;
                uint32_t n = 0;
                while (*text >= '0' && *text <= '9') {
                        n = n * 10 + uint32_t(*text++ - '0');
                        if (n > 0xffff) return false;
                }
                parts[i] = uint16_t(n);
        }
        if (*text != '\0' && *text != '+' && *text != '-') return false;
        out = Version{parts[0], parts[1], parts[2]};
        return true;
}

const char *UpdatePackage::verdictText(Verdict v) {
        switch (v) {
                case Verdict::NotAPackage: return "not an update package";
                case Verdict::NewerFormat: return "a newer package format than this firmware reads";
                case Verdict::Malformed: return "a damaged package";
                default: return SignedContainer::verdictText(v);
        }
}

UpdatePackage::Verdict UpdatePackage::headBytes(Span<const uint8_t> header, size_t &bytes) {
        return SignedContainer::headBytes(header, ContainerKind, bytes);
}

UpdatePackage::Verdict UpdatePackage::parse(Span<const uint8_t> head, uint64_t fileBytes, Package &out) {
        out = Package{};
        const Verdict v = SignedContainer::parse(head, fileBytes, ContainerKind, out);
        if (v != Verdict::Ok) return v;
        const uint8_t *h = head.data() + SignedContainer::PrefixBytes;
        out.abiVersion = u32(h);
        out.minChipRevision = u16(h + 4);
        if (u16(h + 6) != 0) return Verdict::Malformed;
        if (!text(h + 8, out.board) || !text(h + 40, out.versionText)) return Verdict::Malformed;
        std::memcpy(out.buildId, h + 72, sizeof(out.buildId));
        out.madeAt = i64(h + 104);
        out.version = Version{u16(h + 112), u16(h + 114), u16(h + 116)};
        if (u16(h + 118) != 0) return Verdict::Malformed;
        return Verdict::Ok;
}

UpdatePackage::Verdict UpdatePackage::verify(const Package &package, Span<const uint8_t> head, Span<const Key> keys,
                                             size_t &index) {
        return SignedContainer::verify(package, head, keys, index);
}

bool UpdatePackage::appRevisions(Span<const uint8_t> image, uint16_t chipId, ChipRevisions &out) {
        constexpr uint8_t AppMagic = 0xe9;
        if (image.size() < AppHeaderBytes) return false;
        const uint8_t *h = image.data();
        if (h[0] != AppMagic || u16(h + 12) != chipId) return false;
        out.min = u16(h + 15);
        out.max = u16(h + 17);
        return true;
}

bool UpdatePackage::runsOn(const ChipRevisions &revisions, uint16_t revision, bool ignoreMax) {
        if (revision < revisions.min) return false;
        const bool maxSet = revisions.max != 0 && revisions.max != 0xffff;
        return !maxSet || ignoreMax || revision <= revisions.max;
}

BMDS_NAMESPACE_END
