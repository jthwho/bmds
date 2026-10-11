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
                        Simd::Kernel       kernel = Simd::Kernel::Scalar;
        };

        size_t input(JDEC *jd, uint8_t *buf, size_t n) {
                auto        *s = static_cast<Session *>(jd->device);
                const size_t left = s->file.size() - s->at;
                if (n > left) n = left;
                if (buf != nullptr) std::memcpy(buf, s->file.data() + s->at, n);
                s->at += n;
                return n;
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

        int unused(JDEC *, void *, JRECT *) {
                return 1;
        }

        // JFIF's YCbCr to RGB (BT.601), TJpgDec's constants (×1024), each sum rounded.
        constexpr int CrR = 1435, CbG = 352, CrG = 731, CbB = 1814;

        inline uint8_t clip(int v) {
                return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
        }

        // A row of planar Y, Cb, Cr (16-bit, Cb and Cr offset by 128) to @p n pixels at @p d.
        void convertScalar(const int16_t *y, const int16_t *cb, const int16_t *cr, unsigned n, uint8_t *d,
                           PixelFormat f) {
                const unsigned bpp = bytesPerPixel(f);
                for (unsigned i = 0; i < n; ++i, d += bpp) {
                        const int     yy = y[i], b0 = cb[i] - 128, r0 = cr[i] - 128;
                        const uint8_t r = clip(yy + ((CrR * r0 + 512) >> 10));
                        const uint8_t g = clip(yy - ((CbG * b0 + CrG * r0 + 512) >> 10));
                        const uint8_t b = clip(yy + ((CbB * b0 + 512) >> 10));
                        switch (f) {
                                case PixelFormat::Bgr24: d[0] = b, d[1] = g, d[2] = r; break;
                                case PixelFormat::Rgb24: d[0] = r, d[1] = g, d[2] = b; break;
                                default: {
                                        const uint32_t argb = 0xff000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b;
                                        Pixels::fromArgb(&argb, 1, d, f, 0);
                                        break;
                                }
                        }
                }
                return;
        }

#if BMDS_HAS_PIE
        // The same, 8 pixels a step into B, G, R or R, G, B: each sum (with its rounding, 512 × 1) in
        // QACC, shifted out by 10, added to Y, clamped by vsat, narrowed to bytes (vunzip against a zero
        // register, which stays zero), woven by vzipt; 24 bytes stored. The rest scalar. Expects CFG's
        // misaligned loads and stores on, q6 all 512, q7 all ones.
        // Broadcast in this order by the kernel: the chroma offset, then the products' constants.
        constexpr int16_t Coefficients[5] = {128, CrR, CbG, CrG, CbB};

        void convertPie(const int16_t *y, const int16_t *cb, const int16_t *cr, unsigned n, uint8_t *d, bool rgb) {
                const unsigned steps = n / 8;
                const uint32_t lo = 0, hi = 255, sh = 10;
                for (unsigned i = 0; i < steps; ++i) {
                        const int16_t *k = Coefficients;
                        if (!rgb)
                                asm volatile("esp.vld.128.ip q0, %[y], 16\n"
                                             "esp.vld.128.ip q1, %[cb], 16\n"
                                             "esp.vld.128.ip q2, %[cr], 16\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n" // 128
                                             "esp.vsub.s16 q1, q1, q3\n"
                                             "esp.vsub.s16 q2, q2, q3\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n" // crR
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q2, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q4, %[sh], 1\n"
                                             "esp.vadd.s16 q4, q0, q4\n"     // R
                                             "esp.vldbc.16.ip q3, %[k], 2\n" // cbG
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q1, q3\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n" // crG
                                             "esp.vmulas.s16.qacc q2, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q5, %[sh], 1\n"
                                             "esp.vsub.s16 q5, q0, q5\n"     // G
                                             "esp.vldbc.16.ip q3, %[k], 2\n" // cbB
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q1, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q2, %[sh], 1\n"
                                             "esp.vadd.s16 q2, q0, q2\n" // B
                                             "esp.vsat.s16 q4, q4, %[lo], %[hi]\n"
                                             "esp.vsat.s16 q5, q5, %[lo], %[hi]\n"
                                             "esp.vsat.s16 q2, q2, %[lo], %[hi]\n"
                                             "esp.zero.q q1\n"
                                             "esp.vunzip.8 q4, q1\n"
                                             "esp.vunzip.8 q5, q1\n"
                                             "esp.vunzip.8 q2, q1\n"
                                             "esp.vzipt.8 q2, q5, q4\n" // B, G, R
                                             "esp.vst.128.ip q2, %[d], 16\n"
                                             "esp.vst.l.64.ip q5, %[d], 8\n"
                                             : [y] "+cr"(y), [cb] "+cr"(cb), [cr] "+cr"(cr), [d] "+cr"(d), [k] "+cr"(k)
                                             : [lo] "cr"(lo), [hi] "cr"(hi), [sh] "cr"(sh)
                                             : "memory");
                        else
                                asm volatile("esp.vld.128.ip q0, %[y], 16\n"
                                             "esp.vld.128.ip q1, %[cb], 16\n"
                                             "esp.vld.128.ip q2, %[cr], 16\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n"
                                             "esp.vsub.s16 q1, q1, q3\n"
                                             "esp.vsub.s16 q2, q2, q3\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n"
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q2, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q4, %[sh], 1\n"
                                             "esp.vadd.s16 q4, q0, q4\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n"
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q1, q3\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n"
                                             "esp.vmulas.s16.qacc q2, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q5, %[sh], 1\n"
                                             "esp.vsub.s16 q5, q0, q5\n"
                                             "esp.vldbc.16.ip q3, %[k], 2\n"
                                             "esp.zero.qacc\n"
                                             "esp.vmulas.s16.qacc q1, q3\n"
                                             "esp.vmulas.s16.qacc q6, q7\n"
                                             "esp.srcmb.s16.qacc q2, %[sh], 1\n"
                                             "esp.vadd.s16 q2, q0, q2\n"
                                             "esp.vsat.s16 q4, q4, %[lo], %[hi]\n"
                                             "esp.vsat.s16 q5, q5, %[lo], %[hi]\n"
                                             "esp.vsat.s16 q2, q2, %[lo], %[hi]\n"
                                             "esp.zero.q q1\n"
                                             "esp.vunzip.8 q4, q1\n"
                                             "esp.vunzip.8 q5, q1\n"
                                             "esp.vunzip.8 q2, q1\n"
                                             "esp.vzipt.8 q4, q5, q2\n" // R, G, B
                                             "esp.vst.128.ip q4, %[d], 16\n"
                                             "esp.vst.l.64.ip q5, %[d], 8\n"
                                             : [y] "+cr"(y), [cb] "+cr"(cb), [cr] "+cr"(cr), [d] "+cr"(d), [k] "+cr"(k)
                                             : [lo] "cr"(lo), [hi] "cr"(hi), [sh] "cr"(sh)
                                             : "memory");
                }
                if (n % 8 != 0) convertScalar(y, cb, cr, n % 8, d, rgb ? PixelFormat::Rgb24 : PixelFormat::Bgr24);
                return;
        }
#endif

} // namespace

// TJpgDec's MCU, decoded (Y blocks, then Cb and Cr; 16-bit, level-shifted, unclipped), gathered into
// row-major planes (at full size Cb and Cr repeated over the pixels they cover; reduced, each pixel
// the rounded average of its 2^scale square), then converted a row at a time into the picture.
extern "C" JRESULT bmds_softjpeg_mcu_output(JDEC *jd, int (*)(JDEC *, void *, JRECT *), unsigned int x,
                                            unsigned int y) {
        const auto        *s = static_cast<const Session *>(jd->device);
        const ImageBuffer &out = *s->out;
        const unsigned     msx = jd->msx, msy = jd->msy, mx = msx * 8, my = msy * 8, sc = jd->scale;
        unsigned           rx = x + mx <= jd->width ? mx : jd->width - x;
        unsigned           ry = y + my <= jd->height ? my : jd->height - y;
        rx >>= sc;
        ry >>= sc;
        if (rx == 0 || ry == 0) return JDR_OK;
        x >>= sc;
        y >>= sc;
        const unsigned rw = mx >> sc, rh = my >> sc; // the reduced MCU
        const bool     colour = jd->ncomp == 3;
        const int16_t *ys = jd->mcubuf;
        const int16_t *cbs = ys + mx * my;
        const int16_t *crs = cbs + 64;
        int16_t        yp[256], cbp[256], crp[256];
        if (sc == 0) {
                for (unsigned py = 0; py < my; ++py) {
                        for (unsigned bx = 0; bx < msx; ++bx)
                                std::memcpy(yp + py * mx + bx * 8, ys + ((py >> 3) * msx + bx) * 64 + (py & 7) * 8, 16);
                        if (!colour) continue;
                        const int16_t *cbRow = cbs + (py >> (msy - 1)) * 8, *crRow = crs + (py >> (msy - 1)) * 8;
                        for (unsigned px = 0; px < mx; ++px) {
                                cbp[py * mx + px] = cbRow[px >> (msx - 1)];
                                crp[py * mx + px] = crRow[px >> (msx - 1)];
                        }
                }
        } else {
                const unsigned w = 1u << sc, shift = 2 * sc;
                const int      half = int(1u << shift) >> 1;
                for (unsigned oy = 0; oy < rh; ++oy)
                        for (unsigned ox = 0; ox < rw; ++ox) {
                                int sy = 0, scb = 0, scr = 0;
                                for (unsigned py = oy * w; py < oy * w + w; ++py) {
                                        const int16_t *yr = ys + ((py >> 3) * msx) * 64 + (py & 7) * 8;
                                        const unsigned cy = (py >> (msy - 1)) * 8;
                                        for (unsigned px = ox * w; px < ox * w + w; ++px) {
                                                sy += yr[(px >> 3) * 64 + (px & 7)];
                                                if (colour) {
                                                        scb += cbs[cy + (px >> (msx - 1))];
                                                        scr += crs[cy + (px >> (msx - 1))];
                                                }
                                        }
                                }
                                yp[oy * rw + ox] = int16_t((sy + half) >> shift);
                                cbp[oy * rw + ox] = int16_t((scb + half) >> shift);
                                crp[oy * rw + ox] = int16_t((scr + half) >> shift);
                        }
        }
        if (!colour)
                for (unsigned i = 0; i < rw * rh; ++i) cbp[i] = crp[i] = 128;
        const unsigned n = x + rx <= out.width ? rx : (x < out.width ? out.width - x : 0);
        const unsigned bpp = bytesPerPixel(out.format);
#if BMDS_HAS_PIE
        const bool pie = s->kernel == Simd::Kernel::Pie &&
                         (out.format == PixelFormat::Bgr24 || out.format == PixelFormat::Rgb24) && n >= 8;
        static const int16_t Round[8] __attribute__((aligned(16))) = {512, 512, 512, 512, 512, 512, 512, 512};
        static const int16_t Ones[8] __attribute__((aligned(16))) = {1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t             cfg = 0;
        if (pie) {
                const int16_t *rp = Round, *op = Ones;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfg));
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg | 3u));
                asm volatile("esp.vld.128.ip q6, %0, 0\n"
                             "esp.vld.128.ip q7, %1, 0\n" ::"cr"(rp),
                             "cr"(op)
                             : "memory");
        }
#endif
        for (unsigned oy = 0; oy < ry && y + oy < out.height; ++oy) {
                uint8_t *row = out.row(y + oy) + size_t(x) * bpp;
#if BMDS_HAS_PIE
                if (pie) {
                        convertPie(yp + oy * rw, cbp + oy * rw, crp + oy * rw, n, row,
                                   out.format == PixelFormat::Rgb24);
                        continue;
                }
#endif
                convertScalar(yp + oy * rw, cbp + oy * rw, crp + oy * rw, n, row, out.format);
        }
#if BMDS_HAS_PIE
        if (pie) asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg));
#endif
        return JDR_OK;
}

Error SoftJpeg::decode(ByteSpan file, const ImageBuffer &out, unsigned shift, MemSpace work, Simd::Kernel kernel) {
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
        s.kernel = Simd::usable(kernel);
        JDEC    jd;
        JRESULT r = jd_prepare(&jd, input, pool, WorkBytes, &s);
        if (r == JDR_OK) r = jd_decomp(&jd, unused, uint8_t(shift)); // the MCUs go to bmds_softjpeg_mcu_output()
        Memory::free(pool);
        return errorOf(r);
}

BMDS_NAMESPACE_END
