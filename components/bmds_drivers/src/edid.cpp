/**
 * @file      edid.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Layout from VESA E-EDID 1.4 (base block) and CEA-861 (extension).
 */

#include <bmds/edid.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t  Header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
        constexpr size_t   DescriptorsAt = 54;
        constexpr size_t   DescriptorBytes = 18;
        constexpr uint8_t  NameTag = 0xfc;
        constexpr uint8_t  CeaTag = 0x02;
        constexpr uint8_t  VendorBlockTag = 3;
        constexpr uint32_t HdmiOui = 0x000c03;

        bool checksumOk(ByteSpan block) {
                uint8_t sum = 0;
                for (uint8_t b : block) sum = static_cast<uint8_t>(sum + b);
                return sum == 0;
        }

        // A descriptor's text: up to 13 bytes, ended by LF, padded with spaces.
        void copyText(const uint8_t *text, char (&out)[14]) {
                size_t n = 0;
                while (n < 13 && text[n] != '\n' && text[n] != 0) {
                        const uint8_t c = text[n];
                        out[n] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
                        ++n;
                }
                while (n > 0 && out[n - 1] == ' ') --n;
                out[n] = '\0';
                return;
        }

} // namespace

bool Edid::parseDetailedTiming(const uint8_t *d, VideoTiming &out) {
        const uint32_t clock = d[0] | uint32_t(d[1]) << 8; // 10 kHz units; 0: a display descriptor
        if (clock == 0) return false;
        VideoTiming t;
        t.pixelKhz = clock * 10;
        t.hActive = static_cast<uint16_t>(d[2] | (d[4] >> 4) << 8);
        const uint16_t hBlank = static_cast<uint16_t>(d[3] | (d[4] & 0x0f) << 8);
        t.vActive = static_cast<uint16_t>(d[5] | (d[7] >> 4) << 8);
        const uint16_t vBlank = static_cast<uint16_t>(d[6] | (d[7] & 0x0f) << 8);
        t.hFront = static_cast<uint16_t>(d[8] | ((d[11] >> 6) & 3) << 8);
        t.hSync = static_cast<uint16_t>(d[9] | ((d[11] >> 4) & 3) << 8);
        t.vFront = static_cast<uint16_t>((d[10] >> 4) | ((d[11] >> 2) & 3) << 4);
        t.vSync = static_cast<uint16_t>((d[10] & 0x0f) | (d[11] & 3) << 4);
        if (hBlank < t.hFront + t.hSync || vBlank < t.vFront + t.vSync) return false;
        t.hBack = static_cast<uint16_t>(hBlank - t.hFront - t.hSync);
        t.vBack = static_cast<uint16_t>(vBlank - t.vFront - t.vSync);
        const uint8_t flags = d[17];
        t.interlaced = (flags & 0x80) != 0;
        if ((flags & 0x18) == 0x18) { // digital separate sync: the polarities
                t.vSyncHigh = (flags & 0x04) != 0;
                t.hSyncHigh = (flags & 0x02) != 0;
        }
        out = t;
        return true;
}

Result<Edid> Edid::parse(ByteSpan block) {
        if (block.size() != BlockBytes) return makeError<Edid>(Error::Invalid);
        for (size_t i = 0; i < sizeof(Header); ++i) {
                if (block[i] != Header[i]) return makeError<Edid>(Error::CorruptData);
        }
        if (!checksumOk(block)) return makeError<Edid>(Error::CorruptData);
        Edid           e;
        const uint16_t id = static_cast<uint16_t>(block[8] << 8 | block[9]);
        for (int i = 0; i < 3; ++i) {
                const uint8_t letter = (id >> (10 - 5 * i)) & 0x1f;
                e.maker[i] = letter >= 1 && letter <= 26 ? static_cast<char>('A' + letter - 1) : '?';
        }
        e.product = static_cast<uint16_t>(block[10] | block[11] << 8);
        e.version = block[18];
        e.revision = block[19];
        e.digital = (block[20] & 0x80) != 0;
        for (size_t i = 0; i < 4; ++i) {
                const uint8_t *d = block.data() + DescriptorsAt + i * DescriptorBytes;
                VideoTiming    t;
                if (parseDetailedTiming(d, t)) {
                        if (e.preferred.pixelKhz == 0) e.preferred = t;
                } else if (d[3] == NameTag) {
                        copyText(d + 5, e.name);
                }
        }
        e.extensions = block[126];
        return makeResult(e);
}

Error Edid::parseExtension(ByteSpan block) {
        if (block.size() != BlockBytes) return Error::Invalid;
        if (!checksumOk(block)) return Error::CorruptData;
        if (block[0] != CeaTag) return Error::Ok;
        // Data blocks from byte 4 up to the detailed timings at block[2].
        const size_t end = block[2] >= 4 && block[2] < BlockBytes ? block[2] : 4;
        for (size_t at = 4; at < end;) {
                const uint8_t tag = block[at] >> 5;
                const size_t  len = block[at] & 0x1f;
                if (at + 1 + len > end) break;
                if (tag == VendorBlockTag && len >= 3) {
                        const uint32_t oui =
                                block[at + 1] | uint32_t(block[at + 2]) << 8 | uint32_t(block[at + 3]) << 16;
                        if (oui == HdmiOui) hdmi = true;
                }
                at += 1 + len;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
