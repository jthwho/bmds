/**
 * @file      softjpeg.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Baseline JPEGs by TJpgDec (devplan/images.md §3b). It hands out each MCU as R, G, B bytes in a
 * rectangle of the (reduced) picture; they are stored in the format asked for.
 */

#include <cstring>
#include <tjpgd.h>
#include <bmds/jpegheader.h>
#include <bmds/memory.h>
#include <bmds/softjpeg.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct Session {
                        ByteSpan           file;
                        size_t             at = 0;
                        const ImageBuffer *out = nullptr;
        };

        size_t input(JDEC *jd, uint8_t *buf, size_t n) {
                auto        *s = static_cast<Session *>(jd->device);
                const size_t left = s->file.size() - s->at;
                if (n > left) n = left;
                if (buf != nullptr) std::memcpy(buf, s->file.data() + s->at, n);
                s->at += n;
                return n;
        }

        int output(JDEC *jd, void *bitmap, JRECT *rect) {
                const auto        *s = static_cast<Session *>(jd->device);
                const ImageBuffer &out = *s->out;
                const uint8_t     *src = static_cast<const uint8_t *>(bitmap);
                const uint32_t     w = uint32_t(rect->right - rect->left + 1);
                const unsigned     bpp = bytesPerPixel(out.format);
                for (uint32_t y = rect->top; y <= rect->bottom; ++y, src += 3 * w) {
                        if (y >= out.height || rect->left >= out.width) continue;
                        const uint32_t n = rect->left + w <= out.width ? w : out.width - rect->left;
                        uint8_t       *d = out.row(y) + size_t(rect->left) * bpp;
                        switch (out.format) {
                                case PixelFormat::Rgb24: std::memcpy(d, src, size_t(n) * 3); break;
                                case PixelFormat::Bgr24:
                                        for (uint32_t x = 0; x < n; ++x, d += 3) {
                                                d[0] = src[3 * x + 2];
                                                d[1] = src[3 * x + 1];
                                                d[2] = src[3 * x];
                                        }
                                        break;
                                default: {
                                        uint32_t argb[64];
                                        for (uint32_t x = 0; x < n; x += 64) {
                                                const uint32_t k = n - x < 64 ? n - x : 64;
                                                for (uint32_t i = 0; i < k; ++i) {
                                                        const uint8_t *p = src + 3 * (x + i);
                                                        argb[i] = 0xff000000u | uint32_t(p[0]) << 16 |
                                                                  uint32_t(p[1]) << 8 | p[2];
                                                }
                                                Pixels::fromArgb(argb, k, d + size_t(x) * bpp, out.format, 0);
                                        }
                                        break;
                                }
                        }
                }
                return 1;
        }

        Error errorOf(JRESULT r) {
                switch (r) {
                        case JDR_OK: return Error::Ok;
                        case JDR_MEM1:
                        case JDR_MEM2: return Error::NoMem;
                        case JDR_FMT2:
                        case JDR_FMT3: return Error::NotSupported;
                        default: return Error::CorruptData;
                }
        }

} // namespace

Error SoftJpeg::decode(ByteSpan file, const ImageBuffer &out, unsigned shift, MemSpace work) {
        if (shift > MaxShift) return Error::Invalid;
        auto [h, herr] = JpegHeader::parse(file);
        if (herr.isError()) return herr;
        if (!h.isSoftware()) return Error::NotSupported;
        if (h.width > 65535 || h.height > 65535) return Error::NotSupported;
        if (out.pixels == nullptr || out.width < reduced(h.width, shift) || out.height < reduced(h.height, shift) ||
            out.stride < size_t(reduced(h.width, shift)) * bytesPerPixel(out.format))
                return Error::BufferTooSmall;
        void *pool = Memory::alloc(work, WorkBytes);
        if (pool == nullptr) return Error::NoMem;
        Session s;
        s.file = file;
        s.out = &out;
        JDEC    jd;
        JRESULT r = jd_prepare(&jd, input, pool, WorkBytes, &s);
        if (r == JDR_OK) r = jd_decomp(&jd, output, uint8_t(shift));
        Memory::free(pool);
        return errorOf(r);
}

BMDS_NAMESPACE_END
