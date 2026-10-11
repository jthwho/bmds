/**
 * @file      pixels.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Rows of pixels from and to 0xAARRGGBB (devplan/images.md §3a).
 */

#include <bmds/image.h>

BMDS_NAMESPACE_BEGIN

namespace {

        uint32_t over(uint32_t argb, uint32_t rgb) {
                const uint32_t a = argb >> 24;
                if (a == 0xff) return argb & 0xffffff;
                if (a == 0) return rgb & 0xffffff;
                uint32_t out = 0;
                for (int shift = 0; shift < 24; shift += 8) {
                        const uint32_t s = argb >> shift & 0xff;
                        const uint32_t d = rgb >> shift & 0xff;
                        out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
                }
                return out;
        }

} // namespace

void Pixels::swap24(const uint8_t *in, uint8_t *out, size_t count, Simd::Kernel kernel) {
        size_t done = 0;
#if BMDS_HAS_PIE
        if (Simd::usable(kernel) == Simd::Kernel::Pie && count >= 16) {
                // 16 pixels a step: their 48 bytes split into three planes (vunzipt) and woven back in the
                // other order (vzipt). Misaligned loads and stores by the hardware (CFG).
                uint32_t       cfg;
                const size_t   steps = count / 16;
                const uint8_t *src = in;
                uint8_t       *dst = out;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfg));
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg | 3u));
                for (size_t i = 0; i < steps; ++i)
                        asm volatile("esp.vld.128.ip q0, %0, 16\n"
                                     "esp.vld.128.ip q1, %0, 16\n"
                                     "esp.vld.128.ip q2, %0, 16\n"
                                     "esp.vunzipt.8 q0, q1, q2\n"
                                     "esp.vzipt.8 q2, q1, q0\n"
                                     "esp.vst.128.ip q2, %1, 16\n"
                                     "esp.vst.128.ip q1, %1, 16\n"
                                     "esp.vst.128.ip q0, %1, 16\n"
                                     : "+cr"(src), "+cr"(dst)
                                     :
                                     : "memory");
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg));
                done = steps * 16;
        }
#endif
        (void)kernel;
        for (size_t i = done; i < count; ++i) {
                const uint8_t a = in[3 * i], b = in[3 * i + 1], c = in[3 * i + 2];
                out[3 * i] = c;
                out[3 * i + 1] = b;
                out[3 * i + 2] = a;
        }
        return;
}

void Pixels::fromArgb(const uint32_t *in, size_t count, uint8_t *out, PixelFormat format, uint32_t background) {
        switch (format) {
                case PixelFormat::Bgr24:
                        for (size_t i = 0; i < count; ++i, out += 3) {
                                const uint32_t c = over(in[i], background);
                                out[0] = uint8_t(c);
                                out[1] = uint8_t(c >> 8);
                                out[2] = uint8_t(c >> 16);
                        }
                        break;
                case PixelFormat::Rgb24:
                        for (size_t i = 0; i < count; ++i, out += 3) {
                                const uint32_t c = over(in[i], background);
                                out[0] = uint8_t(c >> 16);
                                out[1] = uint8_t(c >> 8);
                                out[2] = uint8_t(c);
                        }
                        break;
                case PixelFormat::Bgra32:
                        for (size_t i = 0; i < count; ++i, out += 4) {
                                const uint32_t c = in[i];
                                out[0] = uint8_t(c);
                                out[1] = uint8_t(c >> 8);
                                out[2] = uint8_t(c >> 16);
                                out[3] = uint8_t(c >> 24);
                        }
                        break;
                case PixelFormat::Rgb565:
                        for (size_t i = 0; i < count; ++i, out += 2) {
                                const uint32_t c = over(in[i], background);
                                const uint16_t v = uint16_t((c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x001f));
                                out[0] = uint8_t(v);
                                out[1] = uint8_t(v >> 8);
                        }
                        break;
                case PixelFormat::Gray8:
                        for (size_t i = 0; i < count; ++i) out[i] = luma(over(in[i], background));
                        break;
        }
        return;
}

void Pixels::toArgb(const uint8_t *in, size_t count, uint32_t *out, PixelFormat format) {
        switch (format) {
                case PixelFormat::Bgr24:
                        for (size_t i = 0; i < count; ++i, in += 3)
                                out[i] = 0xff000000u | uint32_t(in[2]) << 16 | uint32_t(in[1]) << 8 | in[0];
                        break;
                case PixelFormat::Rgb24:
                        for (size_t i = 0; i < count; ++i, in += 3)
                                out[i] = 0xff000000u | uint32_t(in[0]) << 16 | uint32_t(in[1]) << 8 | in[2];
                        break;
                case PixelFormat::Bgra32:
                        for (size_t i = 0; i < count; ++i, in += 4)
                                out[i] = uint32_t(in[3]) << 24 | uint32_t(in[2]) << 16 | uint32_t(in[1]) << 8 | in[0];
                        break;
                case PixelFormat::Rgb565:
                        for (size_t i = 0; i < count; ++i, in += 2) {
                                const uint32_t v = uint32_t(in[0]) | uint32_t(in[1]) << 8;
                                const uint32_t r = v >> 11, g = v >> 5 & 0x3f, b = v & 0x1f;
                                out[i] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) |
                                         (b << 3 | b >> 2);
                        }
                        break;
                case PixelFormat::Gray8:
                        for (size_t i = 0; i < count; ++i) out[i] = 0xff000000u | uint32_t(in[i]) * 0x010101u;
                        break;
        }
        return;
}

BMDS_NAMESPACE_END
