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
        constexpr uint8_t  SerialTag = 0xff;
        constexpr uint8_t  RangeTag = 0xfd;
        constexpr uint8_t  VideoBlockTag = 2;
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

        // Established timings: byte 35 bit 7 first (bit 23 of `Edid::established`).
        constexpr Edid::Mode Established[17] = {
                {720, 400, 70, false},  {720, 400, 88, false},  {640, 480, 60, false},  {640, 480, 67, false},
                {640, 480, 72, false},  {640, 480, 75, false},  {800, 600, 56, false},  {800, 600, 60, false},
                {800, 600, 72, false},  {800, 600, 75, false},  {832, 624, 75, false},  {1024, 768, 87, true},
                {1024, 768, 60, false}, {1024, 768, 70, false}, {1024, 768, 75, false}, {1280, 1024, 75, false},
                {1152, 870, 75, false},
        };

        struct Vic {
                        uint8_t    vic;
                        Edid::Mode mode;
        };
        // The CEA-861 video identification codes BMDS names by size.
        constexpr Vic Vics[] = {
                {1, {640, 480, 60, false}},     {2, {720, 480, 60, false}},    {3, {720, 480, 60, false}},
                {4, {1280, 720, 60, false}},    {5, {1920, 1080, 60, true}},   {16, {1920, 1080, 60, false}},
                {17, {720, 576, 50, false}},    {18, {720, 576, 50, false}},   {19, {1280, 720, 50, false}},
                {20, {1920, 1080, 50, true}},   {31, {1920, 1080, 50, false}}, {32, {1920, 1080, 24, false}},
                {33, {1920, 1080, 25, false}},  {34, {1920, 1080, 30, false}}, {60, {1280, 720, 24, false}},
                {61, {1280, 720, 25, false}},   {62, {1280, 720, 30, false}},  {63, {1920, 1080, 120, false}},
                {64, {1920, 1080, 100, false}}, {93, {3840, 2160, 24, false}}, {94, {3840, 2160, 25, false}},
                {95, {3840, 2160, 30, false}},  {96, {3840, 2160, 50, false}}, {97, {3840, 2160, 60, false}},
        };

        // Range limits (EDID 1.4 adds 255 to a limit by flag bits in byte 4).
        void parseRange(const uint8_t *d, Edid::Range &r) {
                const uint8_t flags = d[4];
                r.present = true;
                r.minHz = static_cast<uint16_t>(d[5] + ((flags & 0x03) == 0x03 ? 255 : 0));
                r.maxHz = static_cast<uint16_t>(d[6] + ((flags & 0x02) != 0 ? 255 : 0));
                r.minKhz = static_cast<uint16_t>(d[7] + ((flags & 0x0c) == 0x0c ? 255 : 0));
                r.maxKhz = static_cast<uint16_t>(d[8] + ((flags & 0x08) != 0 ? 255 : 0));
                r.maxPixelKhz = uint32_t(d[9]) * 10000;
                return;
        }

        // A standard timing: (width / 8) - 31, then the aspect ratio and rate - 60.
        bool parseStandard(const uint8_t *b, Edid::Mode &out) {
                if (b[0] == 0x01 && b[1] == 0x01) return false; // unused
                if (b[0] == 0) return false;
                const uint16_t w = static_cast<uint16_t>((b[0] + 31) * 8);
                uint16_t       h = 0;
                switch (b[1] >> 6) {
                        case 0: h = static_cast<uint16_t>(w * 10 / 16); break;
                        case 1: h = static_cast<uint16_t>(w * 3 / 4); break;
                        case 2: h = static_cast<uint16_t>(w * 4 / 5); break;
                        default: h = static_cast<uint16_t>(w * 9 / 16); break;
                }
                out.width = w;
                out.height = h;
                out.hz = static_cast<uint16_t>((b[1] & 0x3f) + 60);
                out.interlaced = false;
                return true;
        }

} // namespace

bool Edid::takes(const VideoTiming &t) const {
        if (t.interlaced || t.pixelKhz == 0) return false;
        const uint32_t milliHz = t.refreshMilliHz();
        const uint16_t hz = static_cast<uint16_t>((milliHz + 500) / 1000);
        const auto     same = [&](const Mode &m) {
                return !m.interlaced && m.width == t.hActive && m.height == t.vActive && m.hz == hz;
        };
        Mode m;
        for (int bit = 23; bit >= 0; --bit)
                if ((established >> bit & 1) != 0 && establishedMode(bit, m) && same(m)) return true;
        for (uint8_t i = 0; i < standardCount; ++i)
                if (same(standard[i])) return true;
        for (uint8_t i = 0; i < videoModeCount; ++i) {
                uint8_t vic = 0;
                bool    native = false;
                if (videoMode(videoModes[i], m, vic, native) && same(m)) return true;
        }
        const auto listed = [&](const VideoTiming &d) {
                const uint32_t dHz = (d.refreshMilliHz() + 500) / 1000;
                return !d.interlaced && d.hActive == t.hActive && d.vActive == t.vActive && dHz == hz;
        };
        for (uint8_t i = 0; i < detailedCount; ++i)
                if (listed(detailed[i])) return true;
        for (uint8_t i = 0; i < extDetailedCount; ++i)
                if (listed(extDetailed[i])) return true;
        if (!range.present) return false;
        const uint32_t lineHz = static_cast<uint32_t>(uint64_t(t.pixelKhz) * 1000 / t.hTotal());
        return milliHz >= uint32_t(range.minHz) * 1000 && milliHz <= uint32_t(range.maxHz) * 1000 + 999 &&
               lineHz >= uint32_t(range.minKhz) * 1000 && lineHz <= uint32_t(range.maxKhz) * 1000 + 999 &&
               (range.maxPixelKhz == 0 || t.pixelKhz <= range.maxPixelKhz);
}

bool Edid::establishedMode(int bit, Mode &out) {
        const int index = 23 - bit;
        if (index < 0 || index >= int(sizeof(Established) / sizeof(Established[0]))) return false;
        out = Established[index];
        return true;
}

bool Edid::videoMode(uint8_t svd, Mode &out, uint8_t &vic, bool &native) {
        // CEA-861-F: 129-192 are VICs 1-64 marked native; above, the byte is the VIC.
        native = svd >= 129 && svd <= 192;
        vic = native ? static_cast<uint8_t>(svd & 0x7f) : svd;
        for (const Vic &v : Vics) {
                if (v.vic != vic) continue;
                out = v.mode;
                return true;
        }
        return false;
}

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
        e.week = block[16];
        e.year = static_cast<uint16_t>(1990 + block[17]);
        e.digital = (block[20] & 0x80) != 0;
        e.widthCm = block[21];
        e.heightCm = block[22];
        e.established = uint32_t(block[35]) << 16 | uint32_t(block[36]) << 8 | block[37];
        for (size_t i = 0; i < MaxStandard; ++i) {
                if (parseStandard(block.data() + 38 + i * 2, e.standard[e.standardCount])) ++e.standardCount;
        }
        for (size_t i = 0; i < 4; ++i) {
                const uint8_t *d = block.data() + DescriptorsAt + i * DescriptorBytes;
                VideoTiming    t;
                if (parseDetailedTiming(d, t)) {
                        if (e.preferred.pixelKhz == 0) e.preferred = t;
                        if (e.detailedCount < MaxDetailed) e.detailed[e.detailedCount++] = t;
                } else if (d[3] == NameTag) {
                        copyText(d + 5, e.name);
                } else if (d[3] == SerialTag) {
                        copyText(d + 5, e.serial);
                } else if (d[3] == RangeTag) {
                        parseRange(d, e.range);
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
                if (tag == VideoBlockTag) {
                        for (size_t i = 0; i < len && videoModeCount < MaxVideoModes; ++i)
                                videoModes[videoModeCount++] = block[at + 1 + i];
                }
                if (tag == VendorBlockTag && len >= 3) {
                        const uint32_t oui =
                                block[at + 1] | uint32_t(block[at + 2]) << 8 | uint32_t(block[at + 3]) << 16;
                        if (oui == HdmiOui) hdmi = true;
                }
                at += 1 + len;
        }
        // Detailed timings from block[2] on, until a zero clock or the checksum.
        // (block[2] 0: none.)
        for (size_t at = block[2] >= 4 ? end : BlockBytes;
             at + DescriptorBytes <= BlockBytes - 1 && extDetailedCount < MaxExtDetailed; at += DescriptorBytes) {
                VideoTiming t;
                if (!parseDetailedTiming(block.data() + at, t)) break;
                extDetailed[extDetailedCount++] = t;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
