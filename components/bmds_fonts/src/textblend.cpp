/**
 * @file      textblend.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/textblend.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // One channel: (c × a + d × (255 − a) + 127) / 255, as the icons' blend rounds.
        inline uint8_t mix(uint32_t c, uint32_t d, uint32_t a) {
                return uint8_t((c * a + d * (255 - a) + 127) / 255);
        }

        void blend24Scalar(uint8_t *p, const uint8_t *alpha, unsigned count, uint8_t c0, uint8_t c1, uint8_t c2) {
                for (unsigned i = 0; i < count; ++i, p += 3) {
                        const uint32_t a = alpha[i];
                        if (a == 0) continue;
                        if (a == 255) {
                                p[0] = c0;
                                p[1] = c1;
                                p[2] = c2;
                                continue;
                        }
                        p[0] = mix(c0, p[0], a);
                        p[1] = mix(c1, p[1], a);
                        p[2] = mix(c2, p[2], a);
                }
                return;
        }

        void blend8Scalar(uint8_t *p, const uint8_t *alpha, unsigned count, uint8_t c) {
                for (unsigned i = 0; i < count; ++i)
                        if (alpha[i] != 0) p[i] = alpha[i] == 255 ? c : mix(c, p[i], alpha[i]);
                return;
        }

#if BMDS_HAS_PIE
        // One channel of 16 pixels, as mix(): the plane in qX, the color's channel in q6, alpha in q3,
        // 255 − alpha in q4, ones in q5, 128s in q7; qT a temporary. With t = c × a + d × (255 − a),
        // (t + 127) / 255 is (t + 128 + ((t + 128) >> 8)) >> 8: two QACC passes (docs/platforms/esp32p4.md).
#define BMDS_TEXT_CHANNEL(qX, qT)                                                                                      \
        "esp.mov.u8.qacc q7\n"                                                                                         \
        "esp.vmulas.u8.qacc q6, q3\n"                                                                                  \
        "esp.vmulas.u8.qacc " qX ", q4\n"                                                                              \
        "esp.srcmb.u8.qacc " qT ", %[sh], 1\n"                                                                         \
        "esp.mov.u8.qacc " qT "\n"                                                                                     \
        "esp.vmulas.u8.qacc q7, q5\n"                                                                                  \
        "esp.vmulas.u8.qacc q6, q3\n"                                                                                  \
        "esp.vmulas.u8.qacc " qX ", q4\n"                                                                              \
        "esp.srcmb.u8.qacc " qX ", %[sh], 1\n"

        void loadConstants() {
                static constexpr uint8_t Constants[2] = {1, 128};
                const uint8_t           *k = Constants;
                asm volatile("esp.vldbc.8.ip q5, %0, 1\n" // ones
                             "esp.vldbc.8.ip q7, %0, 1\n" // 128
                             : "+cr"(k)
                             :
                             : "memory");
                return;
        }

        // @p steps × 16 pixels of three bytes: their 48 bytes split into planes (vunzipt), each channel
        // blended, woven back (vzipt). @p channels holds the color in byte order; G and R wait in
        // @p scratch (48 bytes, 16-byte aligned) for registers, as does the first channel's result.
        void blend24Pie(uint8_t *p, const uint8_t *alpha, size_t steps, const uint8_t *channels, uint8_t *scratch) {
                loadConstants();
                const uint32_t sh = 8;
                uint8_t       *in = p;
                for (size_t i = 0; i < steps; ++i) {
                        const uint8_t *c = channels;
                        asm volatile(
                                "esp.vld.128.ip q0, %[in], 16\n"
                                "esp.vld.128.ip q1, %[in], 16\n"
                                "esp.vld.128.ip q2, %[in], 16\n"
                                "esp.vunzipt.8 q0, q1, q2\n" // the planes: bytes 0, 1, 2 of each pixel
                                "esp.vld.128.ip q3, %[a], 16\n"
                                "esp.notq q4, q3\n"
                                "esp.vst.128.ip q1, %[s], 16\n"
                                "esp.vst.128.ip q2, %[s], 16\n"
                                "esp.vldbc.8.ip q6, %[c], 1\n" BMDS_TEXT_CHANNEL(
                                        "q0", "q1") "esp.vst.128.ip q0, %[s], -32\n"
                                                    "esp.vld.128.ip q0, %[s], 16\n"
                                                    "esp.vldbc.8.ip q6, %[c], 1\n" BMDS_TEXT_CHANNEL(
                                                            "q0",
                                                            "q1") "esp.vld.128.ip q2, %[s], 16\n"
                                                                  "esp.vldbc.8.ip q6, %[c], 1\n" BMDS_TEXT_CHANNEL(
                                                                          "q2", "q1") "esp.vld.128.ip q1, %[s], -32\n"
                                                                                      "esp.vzipt.8 q1, q0, q2\n"
                                                                                      "esp.vst.128.ip q1, %[out], 16\n"
                                                                                      "esp.vst.128.ip q0, %[out], 16\n"
                                                                                      "esp.vst.128.ip q2, %[out], 16\n"
                                : [in] "+cr"(in), [out] "+cr"(p), [a] "+cr"(alpha), [s] "+cr"(scratch), [c] "+cr"(c)
                                : [sh] "cr"(sh)
                                : "memory");
                }
                return;
        }

        // @p steps × 16 one-byte pixels.
        void blend8Pie(uint8_t *p, const uint8_t *alpha, size_t steps, const uint8_t *channel) {
                loadConstants();
                const uint32_t sh = 8;
                const uint8_t *c = channel;
                asm volatile("esp.vldbc.8.ip q6, %0, 0\n" : "+cr"(c) : : "memory");
                uint8_t *in = p;
                for (size_t i = 0; i < steps; ++i)
                        asm volatile("esp.vld.128.ip q0, %[in], 16\n"
                                     "esp.vld.128.ip q3, %[a], 16\n"
                                     "esp.notq q4, q3\n" BMDS_TEXT_CHANNEL("q0", "q1") "esp.vst.128.ip q0, %[out], 16\n"
                                     : [in] "+cr"(in), [out] "+cr"(p), [a] "+cr"(alpha)
                                     : [sh] "cr"(sh)
                                     : "memory");
                return;
        }
#undef BMDS_TEXT_CHANNEL
#endif

} // namespace

void TextBlend::makeRamp(Format format, uint32_t color, uint32_t background, Ramp &out) {
        out.format = format;
        const uint32_t fr = color >> 16 & 0xff, fg = color >> 8 & 0xff, fb = color & 0xff;
        const uint32_t br = background >> 16 & 0xff, bg = background >> 8 & 0xff, bb = background & 0xff;
        for (unsigned l = 0; l < 16; ++l) {
                const uint32_t a = l * 17u;
                const uint32_t r = mix(fr, br, a), g = mix(fg, bg, a), b = mix(fb, bb, a);
                uint8_t       *p = out.bytes[l];
                switch (format) {
                        case Format::Bgr24:
                                p[0] = uint8_t(b);
                                p[1] = uint8_t(g);
                                p[2] = uint8_t(r);
                                break;
                        case Format::Rgb24:
                                p[0] = uint8_t(r);
                                p[1] = uint8_t(g);
                                p[2] = uint8_t(b);
                                break;
                        case Format::Bgra32:
                                p[0] = uint8_t(b);
                                p[1] = uint8_t(g);
                                p[2] = uint8_t(r);
                                p[3] = 255;
                                break;
                        case Format::Rgb565: {
                                const uint32_t w = (r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3;
                                p[0] = uint8_t(w);
                                p[1] = uint8_t(w >> 8);
                                break;
                        }
                        case Format::Gray8: p[0] = mix(luma(color), luma(background), a); break;
                        case Format::Index8: p[0] = uint8_t(l >= 8 ? color : background); break;
                }
        }
        return;
}

void TextBlend::paint(uint8_t *pixels, const uint8_t *alpha, unsigned count, const Ramp &ramp) {
        // alpha = level × 17, so alpha >> 4 is the level again.
        switch (ramp.format) {
                case Format::Bgr24:
                case Format::Rgb24:
                        for (unsigned i = 0; i < count; ++i, pixels += 3)
                                if (alpha[i] != 0) {
                                        const uint8_t *c = ramp.bytes[alpha[i] >> 4];
                                        pixels[0] = c[0];
                                        pixels[1] = c[1];
                                        pixels[2] = c[2];
                                }
                        break;
                case Format::Bgra32:
                        for (unsigned i = 0; i < count; ++i, pixels += 4)
                                if (alpha[i] != 0) std::memcpy(pixels, ramp.bytes[alpha[i] >> 4], 4);
                        break;
                case Format::Rgb565:
                        for (unsigned i = 0; i < count; ++i, pixels += 2)
                                if (alpha[i] != 0) {
                                        pixels[0] = ramp.bytes[alpha[i] >> 4][0];
                                        pixels[1] = ramp.bytes[alpha[i] >> 4][1];
                                }
                        break;
                case Format::Gray8:
                case Format::Index8:
                        for (unsigned i = 0; i < count; ++i)
                                if (alpha[i] != 0) pixels[i] = ramp.bytes[alpha[i] >> 4][0];
                        break;
        }
        return;
}

void TextBlend::blend(uint8_t *pixels, Format format, const uint8_t *alpha, unsigned count, uint32_t color,
                      Simd::Kernel kernel) {
        const uint8_t r = uint8_t(color >> 16), g = uint8_t(color >> 8), b = uint8_t(color);
        switch (format) {
                case Format::Bgr24:
                case Format::Rgb24: {
                        const uint8_t channels[3] = {format == Format::Bgr24 ? b : r, g,
                                                     format == Format::Bgr24 ? r : b};
                        unsigned      done = 0;
#if BMDS_HAS_PIE
                        if (Simd::usable(kernel) == Simd::Kernel::Pie && count >= 16) {
                                alignas(16) uint8_t scratch[48];
                                const size_t        steps = count / 16;
                                blend24Pie(pixels, alpha, steps, channels, scratch);
                                done = unsigned(steps * 16);
                        }
#endif
                        (void)kernel;
                        blend24Scalar(pixels + 3 * done, alpha + done, count - done, channels[0], channels[1],
                                      channels[2]);
                        break;
                }
                case Format::Gray8: {
                        const uint8_t y = luma(color);
                        unsigned      done = 0;
#if BMDS_HAS_PIE
                        if (Simd::usable(kernel) == Simd::Kernel::Pie && count >= 16) {
                                const size_t steps = count / 16;
                                blend8Pie(pixels, alpha, steps, &y);
                                done = unsigned(steps * 16);
                        }
#endif
                        blend8Scalar(pixels + done, alpha + done, count - done, y);
                        break;
                }
                case Format::Bgra32:
                        for (unsigned i = 0; i < count; ++i, pixels += 4) {
                                const uint32_t a = alpha[i];
                                if (a == 0) continue;
                                pixels[0] = mix(b, pixels[0], a);
                                pixels[1] = mix(g, pixels[1], a);
                                pixels[2] = mix(r, pixels[2], a);
                                pixels[3] = mix(255, pixels[3], a);
                        }
                        break;
                case Format::Rgb565:
                        for (unsigned i = 0; i < count; ++i, pixels += 2) {
                                const uint32_t a = alpha[i];
                                if (a == 0) continue;
                                const uint32_t v = uint32_t(pixels[0]) | uint32_t(pixels[1]) << 8;
                                // Each channel widened to 8 bits as the decoders do (the top bits repeated).
                                const uint32_t pr = (v >> 11 & 31) << 3 | (v >> 13 & 7);
                                const uint32_t pg = (v >> 5 & 63) << 2 | (v >> 9 & 3);
                                const uint32_t pb = (v & 31) << 3 | (v >> 2 & 7);
                                const uint32_t nr = mix(r, pr, a), ng = mix(g, pg, a), nb = mix(b, pb, a);
                                const uint32_t w = (nr & 0xf8) << 8 | (ng & 0xfc) << 3 | nb >> 3;
                                pixels[0] = uint8_t(w);
                                pixels[1] = uint8_t(w >> 8);
                        }
                        break;
                case Format::Index8:
                        for (unsigned i = 0; i < count; ++i)
                                if (alpha[i] >= 128) pixels[i] = uint8_t(color);
                        break;
        }
        return;
}

void TextBlend::fill(uint8_t *pixels, Format format, unsigned count, uint32_t color) {
        const uint8_t r = uint8_t(color >> 16), g = uint8_t(color >> 8), b = uint8_t(color);
        switch (format) {
                case Format::Bgr24:
                case Format::Rgb24: {
                        const uint8_t c0 = format == Format::Bgr24 ? b : r, c2 = format == Format::Bgr24 ? r : b;
                        unsigned      i = 0;
                        for (; i < count && (reinterpret_cast<uintptr_t>(pixels) & 3) != 0; ++i, pixels += 3) {
                                pixels[0] = c0;
                                pixels[1] = g;
                                pixels[2] = c2;
                        }
                        // Aligned: four pixels as three words.
                        const uint32_t w0 = uint32_t(c0) | uint32_t(g) << 8 | uint32_t(c2) << 16 | uint32_t(c0) << 24;
                        const uint32_t w1 = uint32_t(g) | uint32_t(c2) << 8 | uint32_t(c0) << 16 | uint32_t(g) << 24;
                        const uint32_t w2 = uint32_t(c2) | uint32_t(c0) << 8 | uint32_t(g) << 16 | uint32_t(c2) << 24;
                        for (; i + 4 <= count; i += 4, pixels += 12) {
                                auto *w = reinterpret_cast<uint32_t *>(pixels);
                                w[0] = w0;
                                w[1] = w1;
                                w[2] = w2;
                        }
                        for (; i < count; ++i, pixels += 3) {
                                pixels[0] = c0;
                                pixels[1] = g;
                                pixels[2] = c2;
                        }
                        break;
                }
                case Format::Bgra32:
                        for (unsigned i = 0; i < count; ++i, pixels += 4) {
                                pixels[0] = b;
                                pixels[1] = g;
                                pixels[2] = r;
                                pixels[3] = 255;
                        }
                        break;
                case Format::Rgb565: {
                        const uint32_t w = (uint32_t(r) & 0xf8) << 8 | (uint32_t(g) & 0xfc) << 3 | b >> 3;
                        for (unsigned i = 0; i < count; ++i, pixels += 2) {
                                pixels[0] = uint8_t(w);
                                pixels[1] = uint8_t(w >> 8);
                        }
                        break;
                }
                case Format::Gray8: std::memset(pixels, luma(color), count); break;
                case Format::Index8: std::memset(pixels, uint8_t(color), count); break;
        }
        return;
}

BMDS_NAMESPACE_END
