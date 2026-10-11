/**
 * @file      jpegheader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A JPEG's markers up to its first scan (devplan/images.md §3b).
 */

#include <cstring>
#include <bmds/jpegheader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t be16(const uint8_t *p) {
                return uint32_t(p[0]) << 8 | p[1];
        }

        // EXIF's orientation (tag 0x0112 in IFD0) from APP1's data after "Exif\0\0", or 1.
        uint8_t exifOrientation(const uint8_t *tiff, size_t size) {
                if (size < 8) return 1;
                const bool little = tiff[0] == 'I' && tiff[1] == 'I';
                if (!little && !(tiff[0] == 'M' && tiff[1] == 'M')) return 1;
                const auto u16 = [&](size_t at) -> uint32_t {
                        return little ? uint32_t(tiff[at]) | uint32_t(tiff[at + 1]) << 8 : be16(tiff + at);
                };
                const auto u32 = [&](size_t at) -> uint32_t {
                        return little ? u16(at) | u16(at + 2) << 16 : u16(at) << 16 | u16(at + 2);
                };
                if (u16(2) != 42) return 1;
                const uint32_t ifd = u32(4);
                if (ifd > size - 2) return 1;
                const uint32_t entries = u16(ifd);
                for (uint32_t i = 0; i < entries; ++i) {
                        const size_t e = ifd + 2 + size_t(i) * 12;
                        if (e + 12 > size) break;
                        if (u16(e) != 0x0112) continue;
                        const uint32_t v = u16(e + 8); // a SHORT, its value in the entry
                        return v >= 1 && v <= 8 ? uint8_t(v) : 1;
                }
                return 1;
        }

} // namespace

Result<JpegHeader> JpegHeader::parse(ByteSpan file) {
        const uint8_t *p = file.data();
        const size_t   n = file.size();
        if (n < 4 || p[0] != 0xff || p[1] != 0xd8) return makeError<JpegHeader>(Error::CorruptData);
        JpegHeader h;
        bool       framed = false;
        size_t     at = 2;
        while (at + 4 <= n) {
                if (p[at] != 0xff) return makeError<JpegHeader>(Error::CorruptData);
                const uint8_t marker = p[at + 1];
                if (marker == 0xff) { // fill
                        ++at;
                        continue;
                }
                if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) { // TEM, RSTn: no length
                        at += 2;
                        continue;
                }
                if (marker == 0xd9 || marker == 0xd8)
                        return makeError<JpegHeader>(Error::CorruptData); // EOI, SOI: no scan
                const size_t length = be16(p + at + 2);
                if (length < 2 || at + 2 + length > n) return makeError<JpegHeader>(Error::CorruptData);
                const uint8_t *d = p + at + 4;
                const size_t   size = length - 2;
                if (marker == 0xda) { // SOS: the first scan
                        if (!framed) return makeError<JpegHeader>(Error::CorruptData);
                        h.scanAt = at;
                        return makeResult(h);
                }
                if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                        if (size < 6) return makeError<JpegHeader>(Error::CorruptData);
                        h.coding = marker == 0xc0   ? Coding::Baseline
                                   : marker == 0xc1 ? Coding::Extended
                                   : marker == 0xc2 ? Coding::Progressive
                                                    : Coding::Other;
                        h.precision = d[0];
                        h.height = be16(d + 1);
                        h.width = be16(d + 3);
                        h.components = d[5];
                        if (size < 6 + 3 * size_t(h.components) || h.width == 0 || h.height == 0)
                                return makeError<JpegHeader>(Error::CorruptData);
                        h.mcuWidth = uint8_t(8 * (d[6 + 1] >> 4 != 0 ? d[6 + 1] >> 4 : 1));
                        h.mcuHeight = uint8_t(8 * ((d[6 + 1] & 15) != 0 ? d[6 + 1] & 15 : 1));
                        h.sampling = Sampling::Other;
                        if (h.components == 1) {
                                h.sampling = Sampling::Gray;
                        } else if (h.components == 3 && d[6 + 4] == 0x11 && d[6 + 7] == 0x11) {
                                const uint8_t y = d[6 + 1];
                                h.sampling = y == 0x11   ? Sampling::Yuv444
                                             : y == 0x21 ? Sampling::Yuv422
                                             : y == 0x22 ? Sampling::Yuv420
                                                         : Sampling::Other;
                        }
                        framed = true;
                } else if (marker == 0xdd && size >= 2) { // DRI
                        h.restartInterval = uint16_t(be16(d));
                } else if (marker == 0xe1 && size >= 6 && std::memcmp(d, "Exif\0\0", 6) == 0) {
                        h.orientation = exifOrientation(d + 6, size - 6);
                } else if (marker == 0xee && size >= 5 && std::memcmp(d, "Adobe", 5) == 0) {
                        h.adobe = true;
                }
                at += 2 + length;
        }
        return makeError<JpegHeader>(Error::CorruptData);
}

ImageInfo JpegHeader::info() const {
        ImageInfo i;
        i.codec = ImageCodec::Jpeg;
        i.width = width;
        i.height = height;
        i.flags = uint8_t((coding == Coding::Progressive ? ImageInfo::Progressive : 0) |
                          (isHardware() ? ImageInfo::Hardware : 0));
        i.orientation = orientation;
        return i;
}

BMDS_NAMESPACE_END
