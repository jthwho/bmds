/**
 * @file      png.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PNG files, deflated by the ROM's miniz (devplan/display.md §5d).
 */

#include <cstring>
#include <esp_rom_crc.h>
#include <miniz.h>
#include <bmds/memory.h>
#include <bmds/png.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  OutBytes = 16 * 1024; // an IDAT chunk's data
        constexpr uint8_t Signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        // miniz's probes by level (its tdefl_create_comp_flags_from_zip_params()).
        constexpr int Probes[10] = {0, 1, 6, 32, 16, 32, 128, 256, 512, 768};

        void putBe32(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v >> 24);
                p[1] = static_cast<uint8_t>(v >> 16);
                p[2] = static_cast<uint8_t>(v >> 8);
                p[3] = static_cast<uint8_t>(v);
                return;
        }

        struct Writer {
                        Png::Sink sink = nullptr;
                        void     *arg = nullptr;
                        Error     err;
                        uint8_t  *out = nullptr; // the IDAT data gathering
                        size_t    used = 0;
                        size_t    bytes = 0;

                        void send(const uint8_t *data, size_t n) {
                                if (err.isError() || n == 0) return;
                                err = sink(arg, ByteSpan(data, n));
                                bytes += n;
                                return;
                        }

                        // A whole chunk: length, type, data, CRC (of type and data).
                        void chunk(const char *type, const uint8_t *data, size_t n) {
                                uint8_t head[8];
                                putBe32(head, static_cast<uint32_t>(n));
                                std::memcpy(head + 4, type, 4);
                                uint32_t crc = esp_rom_crc32_le(0, head + 4, 4);
                                if (n > 0) crc = esp_rom_crc32_le(crc, data, static_cast<uint32_t>(n));
                                uint8_t tail[4];
                                putBe32(tail, crc);
                                send(head, sizeof(head));
                                send(data, n);
                                send(tail, sizeof(tail));
                                return;
                        }

                        void flush() {
                                if (used > 0) chunk("IDAT", out, used);
                                used = 0;
                                return;
                        }
        };

        // The compressor's output, gathered into IDAT chunks.
        mz_bool put(const void *buf, int len, void *user) {
                auto       *w = static_cast<Writer *>(user);
                const auto *p = static_cast<const uint8_t *>(buf);
                size_t      n = static_cast<size_t>(len);
                while (n > 0 && w->err.isOk()) {
                        const size_t take = n < OutBytes - w->used ? n : OutBytes - w->used;
                        std::memcpy(w->out + w->used, p, take);
                        w->used += take;
                        p += take;
                        n -= take;
                        if (w->used == OutBytes) w->flush();
                }
                return w->err.isOk() ? MZ_TRUE : MZ_FALSE;
        }

        uint32_t magnitude(uint8_t v) {
                const int s = static_cast<int8_t>(v);
                return static_cast<uint32_t>(s < 0 ? -s : s);
        }

        // B, G, R to R, G, B, a row.
        void toRgb(const uint8_t *src, uint8_t *dst, size_t bytes) {
                for (size_t i = 0; i < bytes; i += 3) {
                        const uint8_t b = src[i], g = src[i + 1], r = src[i + 2];
                        dst[i] = r;
                        dst[i + 1] = g;
                        dst[i + 2] = b;
                }
                return;
        }

        // Filters @p cur (after @p prev) into @p out: a filter byte, then the
        // row. None, Sub or Up, by the smallest sum of absolute values,
        // scored in a pass that only reads; a row like the last one (the
        // common case on a desktop) is Up's zeros at once.
        void filterRow(const uint8_t *cur, const uint8_t *prev, size_t bytes, uint8_t *out) {
                uint8_t *row = out + 1;
                if (std::memcmp(cur, prev, bytes) == 0) {
                        out[0] = 2;
                        std::memset(row, 0, bytes);
                        return;
                }
                uint32_t none = 0, sub = 0, up = 0;
                for (size_t i = 0; i < 3; ++i) {
                        none += magnitude(cur[i]);
                        sub += magnitude(cur[i]);
                        up += magnitude(static_cast<uint8_t>(cur[i] - prev[i]));
                }
                for (size_t i = 3; i < bytes; ++i) {
                        const uint8_t c = cur[i];
                        none += magnitude(c);
                        sub += magnitude(static_cast<uint8_t>(c - cur[i - 3]));
                        up += magnitude(static_cast<uint8_t>(c - prev[i]));
                }
                if (up <= sub && up <= none) {
                        out[0] = 2;
                        for (size_t i = 0; i < bytes; ++i) row[i] = static_cast<uint8_t>(cur[i] - prev[i]);
                } else if (sub < none) {
                        out[0] = 1;
                        std::memcpy(row, cur, 3);
                        for (size_t i = 3; i < bytes; ++i) row[i] = static_cast<uint8_t>(cur[i] - cur[i - 3]);
                } else {
                        out[0] = 0;
                        std::memcpy(row, cur, bytes);
                }
                return;
        }

} // namespace

Error Png::write(const uint8_t *bgr, uint16_t width, uint16_t height, size_t stride, int level, Sink sink, void *arg,
                 Stats *stats) {
        if (bgr == nullptr || sink == nullptr || width == 0 || height == 0 || stride < size_t(width) * 3 || level < 0 ||
            level > 9)
                return Error::Invalid;
        const size_t rowBytes = size_t(width) * 3;
        const size_t lineBytes = 1 + rowBytes; // a filter byte, then the row

        // The compressor's state: internal RAM is much faster for its hash chains.
        bool  internalState = true;
        auto *d = static_cast<tdefl_compressor *>(Memory::alloc(MemSpace::Internal, sizeof(tdefl_compressor)));
        if (d == nullptr) {
                internalState = false;
                d = static_cast<tdefl_compressor *>(Memory::alloc(MemSpace::Psram, sizeof(tdefl_compressor)));
        }
        // Rows: the last and this one (RGB), and this one filtered.
        auto *rows = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, 2 * rowBytes + lineBytes + OutBytes));
        if (d == nullptr || rows == nullptr) {
                Memory::free(d);
                Memory::free(rows);
                return Error::NoMem;
        }
        uint8_t *prev = rows;
        uint8_t *cur = prev + rowBytes;
        uint8_t *filtered = cur + rowBytes;
        Writer   w;
        w.sink = sink;
        w.arg = arg;
        w.out = filtered + lineBytes;
        std::memset(prev, 0, rowBytes);

        w.send(Signature, sizeof(Signature));
        uint8_t ihdr[13] = {};
        putBe32(ihdr, width);
        putBe32(ihdr + 4, height);
        ihdr[8] = 8; // bits a channel
        ihdr[9] = 2; // truecolor
        w.chunk("IHDR", ihdr, sizeof(ihdr));

        int flags = Probes[level] | TDEFL_WRITE_ZLIB_HEADER;
        if (level <= 3) flags |= TDEFL_GREEDY_PARSING_FLAG;
        if (level == 0) flags |= TDEFL_FORCE_ALL_RAW_BLOCKS;
        uint64_t filterUs = 0, compressUs = 0, readUs = 0;
        if (tdefl_init(d, put, &w, flags) != TDEFL_STATUS_OKAY) w.err = Error::LibraryFailure;
        for (uint16_t y = 0; y < height && w.err.isOk(); ++y) {
                const uint64_t t0 = System::uptimeUs();
                toRgb(bgr + size_t(y) * stride, cur, rowBytes);
                readUs += System::uptimeUs() - t0;
                filterRow(cur, prev, rowBytes, filtered);
                const uint64_t t1 = System::uptimeUs();
                if (tdefl_compress_buffer(d, filtered, lineBytes, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY && w.err.isOk())
                        w.err = Error::LibraryFailure;
                const uint64_t t2 = System::uptimeUs();
                filterUs += t1 - t0;
                compressUs += t2 - t1;
                uint8_t *t = prev;
                prev = cur;
                cur = t;
        }
        if (w.err.isOk()) {
                const uint64_t t0 = System::uptimeUs();
                if (tdefl_compress_buffer(d, nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE && w.err.isOk())
                        w.err = Error::LibraryFailure;
                w.flush();
                w.chunk("IEND", nullptr, 0);
                compressUs += System::uptimeUs() - t0;
        }
        Memory::free(d);
        Memory::free(rows);
        if (stats != nullptr) {
                stats->bytes = w.bytes;
                stats->filterUs = static_cast<uint32_t>(filterUs);
                stats->readUs = static_cast<uint32_t>(readUs);
                stats->compressUs = static_cast<uint32_t>(compressUs);
                stats->internalState = internalState;
        }
        return w.err;
}

BMDS_NAMESPACE_END
