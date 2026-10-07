/**
 * @file      inflate.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * DEFLATE decompression (RFC 1951): canonical Huffman codes decoded a bit at a
 * time from their counts (as tinf and puff do), the output its own window.
 */

#include <cstring>
#include <bmds/inflate.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr int MaxBits = 15;
        constexpr int MaxLiterals = 288; // 286 used, 288 in the fixed code
        constexpr int MaxDistances = 30;

        // A canonical Huffman code: how many codes of each length, and the symbols in code order.
        struct Huffman {
                        uint16_t counts[MaxBits + 1];
                        uint16_t symbols[MaxLiterals];
        };

        struct Reader {
                        const uint8_t *in;
                        size_t         size;
                        size_t         at = 0;
                        uint32_t       bits = 0; // waiting, least significant first
                        int            count = 0;
                        bool           overrun = false;

                        // @p n bits (up to 16), least significant first; zeros past the end (overrun set).
                        uint32_t take(int n) {
                                while (count < n) {
                                        uint32_t byte = 0;
                                        if (at < size) {
                                                byte = in[at++];
                                        } else {
                                                overrun = true;
                                        }
                                        bits |= byte << count;
                                        count += 8;
                                }
                                const uint32_t v = bits & ((1u << n) - 1);
                                bits >>= n;
                                count -= n;
                                return v;
                        }

                        void toByte() {
                                bits = 0;
                                count = 0;
                                return;
                        }
        };

        // A code from @p lengths of @p n symbols; false if it is over-subscribed.
        bool build(Huffman &h, const uint8_t *lengths, int n) {
                std::memset(h.counts, 0, sizeof(h.counts));
                for (int i = 0; i < n; ++i) ++h.counts[lengths[i]];
                h.counts[0] = 0;
                int left = 1;
                for (int len = 1; len <= MaxBits; ++len) {
                        left <<= 1;
                        left -= h.counts[len];
                        if (left < 0) return false;
                }
                uint16_t offsets[MaxBits + 1];
                offsets[1] = 0;
                for (int len = 1; len < MaxBits; ++len) offsets[len + 1] = uint16_t(offsets[len] + h.counts[len]);
                for (int i = 0; i < n; ++i)
                        if (lengths[i] != 0) h.symbols[offsets[lengths[i]]++] = uint16_t(i);
                return true;
        }

        // The next symbol of @p h, or -1 (no such code).
        int decode(Reader &r, const Huffman &h) {
                int code = 0;
                int first = 0;
                int index = 0;
                for (int len = 1; len <= MaxBits; ++len) {
                        code |= int(r.take(1));
                        const int count = h.counts[len];
                        if (code - first < count) return h.symbols[index + (code - first)];
                        index += count;
                        first += count;
                        first <<= 1;
                        code <<= 1;
                }
                return -1;
        }

        constexpr uint16_t LengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t  LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t DistanceBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                               33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t  DistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        // One block's symbols, with @p lit and @p dist, into @p out from @p n.
        Error codes(Reader &r, const Huffman &lit, const Huffman &dist, WritableByteSpan out, size_t &n) {
                for (;;) {
                        const int sym = decode(r, lit);
                        if (sym < 0 || r.overrun) return Error::CorruptData;
                        if (sym < 256) {
                                if (n == out.size()) return Error::BufferTooSmall;
                                out[n++] = uint8_t(sym);
                                continue;
                        }
                        if (sym == 256) return Error();
                        const int li = sym - 257;
                        if (li >= 29) return Error::CorruptData;
                        const size_t length = LengthBase[li] + r.take(LengthExtra[li]);
                        const int    di = decode(r, dist);
                        if (di < 0 || di >= MaxDistances || r.overrun) return Error::CorruptData;
                        const size_t distance = DistanceBase[di] + r.take(DistanceExtra[di]);
                        if (distance > n) return Error::CorruptData;
                        if (length > out.size() - n) return Error::BufferTooSmall;
                        // Byte by byte: a copy may overlap itself (a run).
                        for (size_t i = 0; i < length; ++i, ++n) out[n] = out[n - distance];
                }
        }

        Error stored(Reader &r, WritableByteSpan out, size_t &n) {
                r.toByte();
                if (r.size - r.at < 4) return Error::CorruptData;
                const uint16_t len = uint16_t(r.in[r.at] | r.in[r.at + 1] << 8);
                const uint16_t nlen = uint16_t(r.in[r.at + 2] | r.in[r.at + 3] << 8);
                r.at += 4;
                if (len != uint16_t(~nlen)) return Error::CorruptData;
                if (r.size - r.at < len) return Error::CorruptData;
                if (len > out.size() - n) return Error::BufferTooSmall;
                std::memcpy(out.data() + n, r.in + r.at, len);
                r.at += len;
                n += len;
                return Error();
        }

        Error fixed(Reader &r, WritableByteSpan out, size_t &n) {
                uint8_t lengths[MaxLiterals];
                int     i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                Huffman lit, dist;
                (void)build(lit, lengths, MaxLiterals);
                for (i = 0; i < MaxDistances; ++i) lengths[i] = 5;
                (void)build(dist, lengths, MaxDistances);
                return codes(r, lit, dist, out, n);
        }

        Error dynamic(Reader &r, WritableByteSpan out, size_t &n) {
                constexpr uint8_t Order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const int         nlen = int(r.take(5)) + 257;
                const int         ndist = int(r.take(5)) + 1;
                const int         ncode = int(r.take(4)) + 4;
                if (nlen > 286 || ndist > MaxDistances) return Error::CorruptData;
                uint8_t lengths[MaxLiterals + MaxDistances] = {};
                for (int i = 0; i < ncode; ++i) lengths[Order[i]] = uint8_t(r.take(3));
                Huffman lencode;
                if (!build(lencode, lengths, 19)) return Error::CorruptData;
                int i = 0;
                while (i < nlen + ndist) {
                        const int sym = decode(r, lencode);
                        if (sym < 0 || r.overrun) return Error::CorruptData;
                        if (sym < 16) {
                                lengths[i++] = uint8_t(sym);
                                continue;
                        }
                        uint8_t value = 0;
                        int     repeat = 0;
                        if (sym == 16) {
                                if (i == 0) return Error::CorruptData;
                                value = lengths[i - 1];
                                repeat = 3 + int(r.take(2));
                        } else if (sym == 17) {
                                repeat = 3 + int(r.take(3));
                        } else {
                                repeat = 11 + int(r.take(7));
                        }
                        if (i + repeat > nlen + ndist) return Error::CorruptData;
                        while (repeat-- > 0) lengths[i++] = value;
                }
                if (lengths[256] == 0) return Error::CorruptData; // no end of block
                Huffman lit, dist;
                if (!build(lit, lengths, nlen) || !build(dist, lengths + nlen, ndist)) return Error::CorruptData;
                return codes(r, lit, dist, out, n);
        }

        Result<size_t> run(Reader &r, WritableByteSpan out) {
                size_t n = 0;
                bool   last = false;
                while (!last) {
                        last = r.take(1) != 0;
                        const uint32_t type = r.take(2);
                        Error          err;
                        if (type == 0) {
                                err = stored(r, out, n);
                        } else if (type == 1) {
                                err = fixed(r, out, n);
                        } else if (type == 2) {
                                err = dynamic(r, out, n);
                        } else {
                                err = Error::CorruptData;
                        }
                        if (err.isOk() && r.overrun) err = Error::CorruptData;
                        if (err.isError()) return makeError<size_t>(err);
                }
                return makeResult(n);
        }

} // namespace

Result<size_t> Inflate::raw(ByteSpan in, WritableByteSpan out) {
        Reader r{in.data(), in.size()};
        return run(r, out);
}

Result<size_t> Inflate::zlib(ByteSpan in, WritableByteSpan out) {
        if (in.size() < 6) return makeError<size_t>(Error::CorruptData);
        const uint8_t cmf = in[0];
        const uint8_t flg = in[1];
        if ((uint32_t(cmf) << 8 | flg) % 31 != 0) return makeError<size_t>(Error::CorruptData);
        if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || (flg & 0x20) != 0) return makeError<size_t>(Error::NotSupported);
        Reader r{in.data() + 2, in.size() - 2};
        auto [n, err] = run(r, out);
        if (err.isError()) return makeError<size_t>(err);
        // The Adler-32 after the data, from the next whole byte.
        const size_t at = 2 + r.at;
        if (in.size() - at < 4) return makeError<size_t>(Error::CorruptData);
        const uint32_t want =
                uint32_t(in[at]) << 24 | uint32_t(in[at + 1]) << 16 | uint32_t(in[at + 2]) << 8 | uint32_t(in[at + 3]);
        if (adler32(ByteSpan(out.data(), n)) != want) return makeError<size_t>(Error::CorruptData);
        return makeResult(n);
}

uint32_t Inflate::adler32(ByteSpan data, uint32_t adler) {
        uint32_t a = adler & 0xffff;
        uint32_t b = adler >> 16;
        size_t   i = 0;
        while (i < data.size()) {
                // 5552 bytes keep the sums from overflowing before the modulo (zlib's NMAX).
                const size_t end = data.size() - i < 5552 ? data.size() : i + 5552;
                for (; i < end; ++i) {
                        a += data[i];
                        b += a;
                }
                a %= 65521;
                b %= 65521;
        }
        return b << 16 | a;
}

BMDS_NAMESPACE_END
