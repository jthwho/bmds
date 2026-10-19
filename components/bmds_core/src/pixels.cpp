/**
 * @file      pixels.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Rows of pixels from and to 0xAARRGGBB (devplan/images.md §3a).
 */

#include <cstdint>
#include <cstring>
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

#if BMDS_HAS_PIE
        // @p steps × 16 pixels blended over the background into B, G, R (or R, G, B), the same bytes as
        // over(): with t = s × a + d × (255 − a), (t + 127) / 255 is (t + 128 + ((t + 128) >> 8)) >> 8.
        // Four loads and two levels of vunzip make planes B, G, R and A; each channel is two passes of
        // QACC (16 lanes of u8 products): the first (128 to start, mov.u8.qacc) gives (t + 128) >> 8,
        // the second starts from that and adds t + 128 again, shifted out by 8. G and R wait in
        // @p scratch (48 bytes, 16-byte aligned) for registers, as does B's result; vzipt weaves the
        // three. @p d holds the background's channels in plane order (B, G, R).
        void blendPie(const uint32_t *in, size_t steps, uint8_t *out, const uint8_t *d, uint8_t *scratch, bool rgb) {
                static constexpr uint8_t Constants[2] = {1, 128};
                const uint8_t           *c = Constants;
                const uint32_t           sh = 8;
                asm volatile("esp.vldbc.8.ip q5, %0, 1\n" // ones
                             "esp.vldbc.8.ip q7, %0, 1\n" // 128
                             : "+cr"(c)
                             :
                             : "memory");
                for (size_t i = 0; i < steps; ++i) {
                        // One channel: the plane in qX, the background in q6, A in q3, ~A in q4; qT a temporary.
#define BMDS_BLEND_CHANNEL(qX, qT)                                                                                     \
        "esp.mov.u8.qacc q7\n"                                                                                         \
        "esp.vmulas.u8.qacc q6, q4\n"                                                                                  \
        "esp.vmulas.u8.qacc " qX ", q3\n"                                                                              \
        "esp.srcmb.u8.qacc " qT ", %[sh], 1\n"                                                                         \
        "esp.mov.u8.qacc " qT "\n"                                                                                     \
        "esp.vmulas.u8.qacc q7, q5\n"                                                                                  \
        "esp.vmulas.u8.qacc q6, q4\n"                                                                                  \
        "esp.vmulas.u8.qacc " qX ", q3\n"                                                                              \
        "esp.srcmb.u8.qacc " qX ", %[sh], 1\n"
                        asm volatile("esp.vld.128.ip q0, %[in], 16\n"
                                     "esp.vld.128.ip q1, %[in], 16\n"
                                     "esp.vld.128.ip q2, %[in], 16\n"
                                     "esp.vld.128.ip q3, %[in], 16\n"
                                     "esp.vunzip.8 q0, q1\n" // B, R and G, A: pixels 0-7
                                     "esp.vunzip.8 q2, q3\n" // pixels 8-15
                                     "esp.vunzip.8 q0, q2\n" // B, R
                                     "esp.vunzip.8 q1, q3\n" // G, A
                                     "esp.vst.128.ip q1, %[s], 16\n"
                                     "esp.vst.128.ip q2, %[s], 16\n"
                                     "esp.notq q4, q3\n"
                                     "esp.vldbc.8.ip q6, %[d], 1\n" BMDS_BLEND_CHANNEL("q0", "q1") // B
                                     "esp.vst.128.ip q0, %[s], -32\n"
                                     "esp.vld.128.ip q0, %[s], 16\n"
                                     "esp.vldbc.8.ip q6, %[d], 1\n" BMDS_BLEND_CHANNEL("q0", "q1") // G
                                     "esp.vld.128.ip q2, %[s], 16\n"
                                     "esp.vldbc.8.ip q6, %[d], -2\n" BMDS_BLEND_CHANNEL("q2", "q1") // R
                                     "esp.vld.128.ip q1, %[s], -32\n"
                                     "beqz %[rgb], 1f\n"
                                     "esp.vzipt.8 q2, q0, q1\n" // R, G, B
                                     "esp.vst.128.ip q2, %[out], 16\n"
                                     "esp.vst.128.ip q0, %[out], 16\n"
                                     "esp.vst.128.ip q1, %[out], 16\n"
                                     "j 2f\n"
                                     "1:\n"
                                     "esp.vzipt.8 q1, q0, q2\n" // B, G, R
                                     "esp.vst.128.ip q1, %[out], 16\n"
                                     "esp.vst.128.ip q0, %[out], 16\n"
                                     "esp.vst.128.ip q2, %[out], 16\n"
                                     "2:\n"
                                     : [in] "+cr"(in), [out] "+cr"(out), [s] "+cr"(scratch), [d] "+cr"(d)
                                     : [sh] "cr"(sh), [rgb] "r"(rgb ? 1u : 0u)
                                     : "memory");
#undef BMDS_BLEND_CHANNEL
                }
                return;
        }
#endif

        // Blended over @p background into B, G, R or R, G, B (@p rgb).
        void blend24(const uint32_t *in, size_t count, uint8_t *out, uint32_t background, bool rgb,
                     Simd::Kernel kernel) {
                size_t done = 0;
#if BMDS_HAS_PIE
                if (Simd::usable(kernel) == Simd::Kernel::Pie && count >= 16) {
                        alignas(16) uint8_t scratch[48];
                        const uint8_t d[3] = {uint8_t(background), uint8_t(background >> 8), uint8_t(background >> 16)};
                        const size_t  steps = count / 16;
                        blendPie(in, steps, out, d, scratch, rgb);
                        done = steps * 16;
                }
#endif
                (void)kernel;
                out += 3 * done;
                for (size_t i = done; i < count; ++i, out += 3) {
                        const uint32_t c = over(in[i], background);
                        out[rgb ? 2 : 0] = uint8_t(c);
                        out[1] = uint8_t(c >> 8);
                        out[rgb ? 0 : 2] = uint8_t(c >> 16);
                }
                return;
        }

} // namespace

void Pixels::swap24(const uint8_t *in, uint8_t *out, size_t count, Simd::Kernel kernel) {
        size_t done = 0;
#if BMDS_HAS_PIE
        if (Simd::usable(kernel) == Simd::Kernel::Pie && count >= 16) {
                // 16 pixels a step: their 48 bytes split into three planes (vunzipt) and woven back in the
                // other order (vzipt). Misaligned loads and stores by the hardware (CFG, set at boot).
                const size_t   steps = count / 16;
                const uint8_t *src = in;
                uint8_t       *dst = out;
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

void Pixels::fromArgb(const uint32_t *in, size_t count, uint8_t *out, PixelFormat format, uint32_t background,
                      Simd::Kernel kernel) {
        switch (format) {
                case PixelFormat::Bgr24: blend24(in, count, out, background, false, kernel); break;
                case PixelFormat::Rgb24: blend24(in, count, out, background, true, kernel); break;
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

void Pixels::repeat16(const uint16_t *in, size_t count, uint16_t *out, unsigned times, Simd::Kernel kernel) {
        size_t done = 0;
#if BMDS_HAS_PIE
        if (Simd::usable(kernel) == Simd::Kernel::Pie && times == 2 && count >= 8 &&
            (reinterpret_cast<uintptr_t>(in) & 15) == 0 && (reinterpret_cast<uintptr_t>(out) & 15) == 0) {
                // 8 pixels a step: loaded twice and zipped by 16 bits, each pixel beside itself.
                const size_t    steps = count / 8;
                const uint16_t *a = in;
                const uint16_t *b = in;
                uint16_t       *o = out;
                for (size_t i = 0; i < steps; ++i)
                        asm volatile("esp.vld.128.ip q0, %[a], 16\n"
                                     "esp.vld.128.ip q1, %[b], 16\n"
                                     "esp.vzip.16 q0, q1\n"
                                     "esp.vst.128.ip q0, %[o], 16\n"
                                     "esp.vst.128.ip q1, %[o], 16\n"
                                     : [a] "+cr"(a), [b] "+cr"(b), [o] "+cr"(o)
                                     :
                                     : "memory");
                done = steps * 8;
        }
#endif
        (void)kernel;
        uint16_t *o = out + done * times;
        for (size_t i = done; i < count; ++i) {
                const uint16_t v = in[i];
                for (unsigned t = 0; t < times; ++t) *o++ = v;
        }
        return;
}

void Pixels::repeat24(const uint8_t *in, size_t count, uint8_t *out, unsigned times) {
        for (size_t i = 0; i < count; ++i) {
                const uint8_t b = in[3 * i], g = in[3 * i + 1], r = in[3 * i + 2];
                for (unsigned t = 0; t < times; ++t) {
                        out[0] = b;
                        out[1] = g;
                        out[2] = r;
                        out += 3;
                }
        }
        return;
}

namespace {

        // Four pixels @p c (B, G, R in their low 24 bits) each @p Times times over, as 3 × Times
        // words: the 3-byte units packed in order (a unit split across words gives its low bytes
        // to the first).
        template <unsigned Times> inline void expandFour(const uint32_t c[4], uint32_t *o);

        template <> inline void expandFour<1>(const uint32_t c[4], uint32_t *o) {
                o[0] = c[0] | c[1] << 24;
                o[1] = c[1] >> 8 | c[2] << 16;
                o[2] = c[2] >> 16 | c[3] << 8;
                return;
        }

        template <> inline void expandFour<2>(const uint32_t c[4], uint32_t *o) {
                o[0] = c[0] | c[0] << 24;
                o[1] = c[0] >> 8 | c[1] << 16;
                o[2] = c[1] >> 16 | c[1] << 8;
                o[3] = c[2] | c[2] << 24;
                o[4] = c[2] >> 8 | c[3] << 16;
                o[5] = c[3] >> 16 | c[3] << 8;
                return;
        }

        template <> inline void expandFour<3>(const uint32_t c[4], uint32_t *o) {
                o[0] = c[0] | c[0] << 24;
                o[1] = c[0] >> 8 | c[0] << 16;
                o[2] = c[0] >> 16 | c[1] << 8;
                o[3] = c[1] | c[1] << 24;
                o[4] = c[1] >> 8 | c[2] << 16;
                o[5] = c[2] >> 16 | c[2] << 8;
                o[6] = c[2] | c[3] << 24;
                o[7] = c[3] >> 8 | c[3] << 16;
                o[8] = c[3] >> 16 | c[3] << 8;
                return;
        }

        template <> inline void expandFour<4>(const uint32_t c[4], uint32_t *o) {
                for (unsigned i = 0; i < 4; ++i, o += 3) {
                        o[0] = c[i] | c[i] << 24;
                        o[1] = c[i] >> 8 | c[i] << 16;
                        o[2] = c[i] >> 16 | c[i] << 8;
                }
                return;
        }

        template <unsigned Times>
        size_t expandWords(const uint8_t *in, size_t count, const uint32_t *palette, uint8_t *out) {
                auto        *o = reinterpret_cast<uint32_t *>(out);
                const size_t groups = count / 4;
                for (size_t g = 0; g < groups; ++g, in += 4, o += 3 * Times) {
                        uint32_t v; // four indices in one load (PSRAM's reads are the dear part)
                        std::memcpy(&v, in, 4);
                        const uint32_t c[4] = {palette[v & 0xff] & 0xffffffu, palette[(v >> 8) & 0xff] & 0xffffffu,
                                               palette[(v >> 16) & 0xff] & 0xffffffu, palette[v >> 24] & 0xffffffu};
                        expandFour<Times>(c, o);
                }
                return groups * 4;
        }

} // namespace

void Pixels::stretch(const uint8_t *in, unsigned bytes, const uint32_t *palette, uint32_t first, uint32_t count,
                     uint32_t from, uint32_t to, uint8_t *out) {
        if (to == 0) return;
        // The input column steps by from / to: a whole part and a remainder carried.
        const uint64_t at = uint64_t(first) * from;
        uint32_t       col = uint32_t(at / to), rem = uint32_t(at % to);
        const uint32_t whole = from / to, part = from % to;
        for (uint32_t i = 0; i < count; ++i) {
                if (bytes == 1) {
                        const uint32_t rgb = palette[in[col]];
                        out[0] = uint8_t(rgb);
                        out[1] = uint8_t(rgb >> 8);
                        out[2] = uint8_t(rgb >> 16);
                        out += 3;
                } else if (bytes == 2) {
                        std::memcpy(out, in + size_t(col) * 2, 2);
                        out += 2;
                } else {
                        std::memcpy(out, in + size_t(col) * 3, 3);
                        out += 3;
                }
                col += whole;
                rem += part;
                if (rem >= to) {
                        rem -= to;
                        ++col;
                }
        }
        return;
}

void Pixels::expandIndexed(const uint8_t *in, size_t count, const uint32_t *palette, uint8_t *out, unsigned times) {
        // Whole words where the row allows (four pixels make whole words at any factor): byte
        // stores a pixel took ~12 cycles an output pixel (games.md §20).
        size_t done = 0;
        if ((reinterpret_cast<uintptr_t>(out) & 3) == 0) {
                switch (times) {
                        case 1: done = expandWords<1>(in, count, palette, out); break;
                        case 2: done = expandWords<2>(in, count, palette, out); break;
                        case 3: done = expandWords<3>(in, count, palette, out); break;
                        case 4: done = expandWords<4>(in, count, palette, out); break;
                        default: break;
                }
        }
        in += done;
        out += done * 3 * times;
        count -= done;
        for (size_t i = 0; i < count; ++i) {
                const uint32_t c = palette[in[i]];
                const uint8_t  b = uint8_t(c), g = uint8_t(c >> 8), r = uint8_t(c >> 16);
                for (unsigned t = 0; t < times; ++t) {
                        out[0] = b;
                        out[1] = g;
                        out[2] = r;
                        out += 3;
                }
        }
        return;
}

// The table's words for color c (B, G, R in its low bytes, the top byte 0), in the order
// expandTriple() loads them. Four pixels times three are 36 bytes, nine words; each word is one
// of these or two of them ORed: c c c over 12 bytes is A B C, and the words where two pixels
// meet take one's tail and the other's head.
namespace {
        enum TripleWord {
                TA,
                TB,
                TC,
                TD,
                TE,
                TF,
                TG,
                TPlain
        };
}

void Pixels::makeTripleTable(const uint32_t *palette, uint32_t *table) {
        for (size_t i = 0; i < 256; ++i, table += 8) {
                const uint32_t c = palette[i] & 0xffffffu;
                table[TA] = c | c << 24;      // b g r b
                table[TB] = c >> 8 | c << 16; // g r b g
                table[TC] = c >> 16 | c << 8; // r b g r
                table[TD] = c << 8;           // 0 b g r: a pixel starting a word's second byte
                table[TE] = c >> 8;           // g r 0 0: one ending at a word's second byte
                table[TF] = c << 16;          // 0 0 b g: one starting at its third
                table[TG] = c << 24;          // 0 0 0 b: one starting at its last
                table[TPlain] = c;            // b g r 0
        }
        return;
}

void Pixels::expandTriple(const uint8_t *in, size_t count, const uint32_t *table, uint8_t *out) {
        size_t done = 0;
        if ((reinterpret_cast<uintptr_t>(out) & 3) == 0) {
                auto        *o = reinterpret_cast<uint32_t *>(out);
                const size_t groups = count / 4;
                // Entry i at table + i * 8: the byte's index shifted into place and masked.
                const auto *t = reinterpret_cast<const uint8_t *>(table);
                for (size_t g = 0; g < groups; ++g, in += 4, o += 9) {
                        uint32_t v;
                        std::memcpy(&v, in, 4);
                        const auto *e0 = reinterpret_cast<const uint32_t *>(t + (v << 5 & 0x1fe0u));
                        const auto *e1 = reinterpret_cast<const uint32_t *>(t + (v >> 3 & 0x1fe0u));
                        const auto *e2 = reinterpret_cast<const uint32_t *>(t + (v >> 11 & 0x1fe0u));
                        const auto *e3 = reinterpret_cast<const uint32_t *>(t + (v >> 19 & 0x1fe0u));
                        o[0] = e0[TA];
                        o[1] = e0[TB];
                        o[2] = (e0[TC] & 0xffu) | e1[TD];
                        o[3] = e1[TA];
                        o[4] = e1[TE] | e2[TF];
                        o[5] = e2[TC];
                        o[6] = e2[TPlain] | e3[TG];
                        o[7] = e3[TB];
                        o[8] = e3[TC];
                }
                done = groups * 4;
        }
        out += done * 9;
        for (size_t i = done; i < count; ++i) {
                const uint32_t c = table[size_t(in[i - done]) * 8 + TPlain];
                for (unsigned k = 0; k < 3; ++k, out += 3) {
                        out[0] = uint8_t(c);
                        out[1] = uint8_t(c >> 8);
                        out[2] = uint8_t(c >> 16);
                }
        }
        return;
}

BMDS_NAMESPACE_END
