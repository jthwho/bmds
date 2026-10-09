/**
 * @file      sampleformat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Sample formats to and from the engine's float (devplan/audio.md §17).
 * G.711 follows the ITU's reference arithmetic (as in Sun's public g711.c).
 */

#include <array>
#include <cmath>
#include <cstring>
#include <bmds/sampleformat.h>

BMDS_NAMESPACE_BEGIN

namespace SampleFormats {

        namespace {

                struct FormatInfo {
                                const char *name;
                                uint8_t     bytes;
                };

                // In SampleFormat's order.
                constexpr FormatInfo Formats[Count] = {
                        {"u8", 1},      {"s8", 1},    {"s16le", 2}, {"s16be", 2},  {"s24le", 4},
                        {"s24-3le", 3}, {"s32le", 4}, {"f32le", 4}, {"mu-law", 1}, {"a-law", 1},
                };

                constexpr int16_t muLawDecode(uint8_t code) {
                        const uint8_t u = static_cast<uint8_t>(~code);
                        int           t = ((u & 0x0f) << 3) + 0x84;
                        t <<= (u & 0x70) >> 4;
                        return static_cast<int16_t>((u & 0x80) != 0 ? 0x84 - t : t - 0x84);
                }

                constexpr int16_t aLawDecode(uint8_t code) {
                        const uint8_t a = static_cast<uint8_t>(code ^ 0x55);
                        int           t = (a & 0x0f) << 4;
                        const int     seg = (a & 0x70) >> 4;
                        if (seg == 0) {
                                t += 8;
                        } else {
                                t += 0x108;
                                if (seg > 1) t <<= seg - 1;
                        }
                        return static_cast<int16_t>((a & 0x80) != 0 ? t : -t);
                }

                template <int16_t (*Decode)(uint8_t)> constexpr std::array<float, 256> decodeTable() {
                        std::array<float, 256> t{};
                        for (size_t i = 0; i < 256; ++i)
                                t[i] = static_cast<float>(Decode(static_cast<uint8_t>(i))) / 32768.0f;
                        return t;
                }

                constexpr std::array<float, 256> MuLawTable = decodeTable<muLawDecode>();
                constexpr std::array<float, 256> ALawTable = decodeTable<aLawDecode>();

                constexpr float Scale8 = 1.0f / 128.0f;
                constexpr float Scale16 = 1.0f / 32768.0f;
                constexpr float Scale24 = 1.0f / 8388608.0f;
                constexpr float Scale32 = 1.0f / 2147483648.0f;

                inline uint32_t le32(const uint8_t *p) {
                        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                               static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
                }

                inline int32_t signExtend24(uint32_t v) {
                        return static_cast<int32_t>(v << 8) >> 8;
                }

                // One sample of @p format at @p p, as float.
                inline float sample(SampleFormat format, const uint8_t *p) {
                        switch (format) {
                                case SampleFormat::U8: return static_cast<float>(static_cast<int>(p[0]) - 128) * Scale8;
                                case SampleFormat::S8: return static_cast<float>(static_cast<int8_t>(p[0])) * Scale8;
                                case SampleFormat::S16Le:
                                        return static_cast<float>(static_cast<int16_t>(p[0] | p[1] << 8)) * Scale16;
                                case SampleFormat::S16Be:
                                        return static_cast<float>(static_cast<int16_t>(p[0] << 8 | p[1])) * Scale16;
                                case SampleFormat::S24Le: return static_cast<float>(signExtend24(le32(p))) * Scale24;
                                case SampleFormat::S24_3Le:
                                        return static_cast<float>(signExtend24(static_cast<uint32_t>(p[0]) |
                                                                               static_cast<uint32_t>(p[1]) << 8 |
                                                                               static_cast<uint32_t>(p[2]) << 16)) *
                                               Scale24;
                                case SampleFormat::S32Le:
                                        return static_cast<float>(static_cast<int32_t>(le32(p))) * Scale32;
                                case SampleFormat::F32Le: {
                                        const uint32_t bits = le32(p);
                                        float          f;
                                        std::memcpy(&f, &bits, sizeof(f));
                                        return f;
                                }
                                case SampleFormat::MuLaw: return MuLawTable[p[0]];
                                case SampleFormat::ALaw: return ALawTable[p[0]];
                        }
                        return 0.0f;
                }

                // @p x scaled to @p range (2^(bits-1)), rounded and clamped. Float is exact up
                // to 24 bits; 32 bits go through double (not on a hot path).
                inline int32_t quantize(float x, float range, int32_t lo, int32_t hi) {
                        const float v = x * range;
                        if (!(v > static_cast<float>(lo))) return lo; // NaN too
                        if (v >= static_cast<float>(hi)) return hi;
                        return static_cast<int32_t>(std::lrintf(v));
                }

                inline int32_t quantize32(float x) {
                        const double v = static_cast<double>(x) * 2147483648.0;
                        if (!(v > -2147483648.0)) return INT32_MIN;
                        if (v >= 2147483647.0) return INT32_MAX;
                        return static_cast<int32_t>(std::lrint(v));
                }

                inline void put32(uint8_t *p, uint32_t v) {
                        p[0] = static_cast<uint8_t>(v);
                        p[1] = static_cast<uint8_t>(v >> 8);
                        p[2] = static_cast<uint8_t>(v >> 16);
                        p[3] = static_cast<uint8_t>(v >> 24);
                        return;
                }

                inline void putSample(SampleFormat format, float x, uint8_t *p) {
                        switch (format) {
                                case SampleFormat::U8:
                                        p[0] = static_cast<uint8_t>(quantize(x, 128.0f, -128, 127) + 128);
                                        return;
                                case SampleFormat::S8:
                                        p[0] = static_cast<uint8_t>(quantize(x, 128.0f, -128, 127));
                                        return;
                                case SampleFormat::S16Le:
                                case SampleFormat::S16Be: {
                                        const auto v = static_cast<uint16_t>(quantize(x, 32768.0f, -32768, 32767));
                                        const bool le = format == SampleFormat::S16Le;
                                        p[le ? 0 : 1] = static_cast<uint8_t>(v);
                                        p[le ? 1 : 0] = static_cast<uint8_t>(v >> 8);
                                        return;
                                }
                                case SampleFormat::S24Le:
                                        put32(p, static_cast<uint32_t>(quantize(x, 8388608.0f, -8388608, 8388607)));
                                        return;
                                case SampleFormat::S24_3Le: {
                                        const auto v =
                                                static_cast<uint32_t>(quantize(x, 8388608.0f, -8388608, 8388607));
                                        p[0] = static_cast<uint8_t>(v);
                                        p[1] = static_cast<uint8_t>(v >> 8);
                                        p[2] = static_cast<uint8_t>(v >> 16);
                                        return;
                                }
                                case SampleFormat::S32Le: put32(p, static_cast<uint32_t>(quantize32(x))); return;
                                case SampleFormat::F32Le: {
                                        uint32_t bits;
                                        std::memcpy(&bits, &x, sizeof(bits));
                                        put32(p, bits);
                                        return;
                                }
                                case SampleFormat::MuLaw:
                                        p[0] = linearToMuLaw(
                                                static_cast<int16_t>(quantize(x, 32768.0f, -32768, 32767)));
                                        return;
                                case SampleFormat::ALaw:
                                        p[0] = linearToALaw(static_cast<int16_t>(quantize(x, 32768.0f, -32768, 32767)));
                                        return;
                        }
                        return;
                }

        } // namespace

        size_t bytesPerSample(SampleFormat format) {
                const auto i = static_cast<size_t>(format);
                return i < Count ? Formats[i].bytes : 0;
        }

        const char *name(SampleFormat format) {
                const auto i = static_cast<size_t>(format);
                return i < Count ? Formats[i].name : "?";
        }

        Result<SampleFormat> parse(StringView text) {
                for (size_t i = 0; i < Count; ++i)
                        if (text == StringView(Formats[i].name)) return makeResult(static_cast<SampleFormat>(i));
                return makeError<SampleFormat>(Error::Invalid);
        }

        void toFloat(SampleFormat format, unsigned channels, const uint8_t *src, size_t frames, float *stereo) {
                const size_t step = bytesPerSample(format);
                // The common case first: 16-bit stereo, little-endian.
                if (format == SampleFormat::S16Le && channels == 2) {
                        for (size_t i = 0; i < 2 * frames; ++i, src += 2)
                                stereo[i] = static_cast<float>(static_cast<int16_t>(src[0] | src[1] << 8)) * Scale16;
                        return;
                }
                if (channels == 2) {
                        for (size_t i = 0; i < 2 * frames; ++i, src += step) stereo[i] = sample(format, src);
                        return;
                }
                for (size_t i = 0; i < frames; ++i, src += step) {
                        const float v = sample(format, src);
                        stereo[2 * i] = v;
                        stereo[2 * i + 1] = v;
                }
                return;
        }

        void fromFloat(SampleFormat format, unsigned channels, const float *stereo, size_t frames, uint8_t *dst) {
                const size_t step = bytesPerSample(format);
                if (channels == 2) {
                        for (size_t i = 0; i < 2 * frames; ++i, dst += step) putSample(format, stereo[i], dst);
                        return;
                }
                for (size_t i = 0; i < frames; ++i, dst += step)
                        putSample(format, 0.5f * (stereo[2 * i] + stereo[2 * i + 1]), dst);
                return;
        }

        int16_t muLawToLinear(uint8_t code) {
                return muLawDecode(code);
        }

        int16_t aLawToLinear(uint8_t code) {
                return aLawDecode(code);
        }

        uint8_t linearToMuLaw(int16_t sample) {
                constexpr int Bias = 0x84;
                constexpr int Clip = 32635;
                int           pcm = sample;
                const int     sign = pcm < 0 ? 0x80 : 0x00;
                if (pcm < 0) pcm = -pcm;
                if (pcm > Clip) pcm = Clip;
                pcm += Bias;
                int exponent = 7;
                for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; mask >>= 1) --exponent;
                const int mantissa = (pcm >> (exponent + 3)) & 0x0f;
                return static_cast<uint8_t>(~(sign | exponent << 4 | mantissa));
        }

        uint8_t linearToALaw(int16_t sample) {
                constexpr int SegmentEnds[8] = {0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff};
                int           pcm = sample >> 3;
                int           mask = 0xd5;
                if (pcm < 0) {
                        mask = 0x55;
                        pcm = -pcm - 1;
                }
                int seg = 0;
                while (seg < 8 && pcm > SegmentEnds[seg]) ++seg;
                if (seg >= 8) return static_cast<uint8_t>(0x7f ^ mask);
                int a = seg << 4;
                a |= seg < 2 ? (pcm >> 1) & 0x0f : (pcm >> seg) & 0x0f;
                return static_cast<uint8_t>(a ^ mask);
        }

} // namespace SampleFormats

BMDS_NAMESPACE_END
