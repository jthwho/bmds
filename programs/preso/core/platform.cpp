/**
 * @file      platform.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "platform.h"
#include "transitions.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace preso {

        // Here, not with the transitions: a platform's pieces need nothing else of them.
        void applyPieces(Canvas &out, const Canvas &from, const Canvas &to, const Piece *pieces, uint32_t count) {
                uint32_t bpp = bytesPerPixel(out.format());
                out.clearClip();
                for (uint32_t i = 0; i < count; i++) {
                        const Piece &p = pieces[i];
                        Rect         r = Rect{p.x, p.y, p.width, p.height}.intersected(out.clip());
                        if (r.isEmpty()) continue;
                        if (p.source == Piece::Fill) {
                                out.fill(r, p.color);
                                continue;
                        }
                        const Canvas &src = p.source == Piece::From ? from : to;
                        int32_t       sx = p.fromX + (r.x - p.x), sy = p.fromY + (r.y - p.y);
                        if (sx < 0 || sy < 0 || sx + r.width > src.width() || sy + r.height > src.height()) continue;
                        for (int32_t y = 0; y < r.height; y++) {
                                memcpy((out.pixels() + size_t(r.y + y) * out.stride()) + size_t(r.x) * bpp,
                                       (src.pixels() + size_t(sy + y) * src.stride()) + size_t(sx) * bpp,
                                       size_t(r.width) * bpp);
                        }
                }
                return;
        }


        namespace {

                // A PNG's pieces: big-endian numbers and the chunks' CRC-32.
                void putBig(uint8_t *out, uint32_t v) {
                        out[0] = uint8_t(v >> 24);
                        out[1] = uint8_t(v >> 16);
                        out[2] = uint8_t(v >> 8);
                        out[3] = uint8_t(v);
                        return;
                }

                struct Crc {
                                uint32_t table[256];
                                Crc() {
                                        for (uint32_t n = 0; n < 256; n++) {
                                                uint32_t c = n;
                                                for (int k = 0; k < 8; k++)
                                                        c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                                                table[n] = c;
                                        }
                                }
                                uint32_t add(uint32_t crc, const uint8_t *data, size_t n) const {
                                        for (size_t i = 0; i < n; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
                                        return crc;
                                }
                };

                // A chunk written in pieces: its length first, then its type and data through the CRC.
                class Chunk {
                        public:
                                Chunk(FILE *f, const Crc &crc, const char *type, uint32_t length) : _f(f), _crc(crc) {
                                        uint8_t n[4];
                                        putBig(n, length);
                                        (void)fwrite(n, 1, 4, f);
                                        put(reinterpret_cast<const uint8_t *>(type), 4);
                                }
                                void put(const uint8_t *data, size_t n) {
                                        _sum = _crc.add(_sum, data, n);
                                        (void)fwrite(data, 1, n, _f);
                                        return;
                                }
                                void end() {
                                        uint8_t n[4];
                                        putBig(n, _sum ^ 0xFFFFFFFFu);
                                        (void)fwrite(n, 1, 4, _f);
                                        return;
                                }

                        private:
                                FILE      *_f;
                                const Crc &_crc;
                                uint32_t   _sum = 0xFFFFFFFFu;
                };

        } // namespace

        Error Platform::writePicture(const char *path, const Canvas &canvas, const Rect &area) {
                // RGB, 8 bits, its pixel data in deflate's stored blocks: no compressor needed.
                Rect r = area.intersected(Rect{0, 0, canvas.width(), canvas.height()});
                if (r.isEmpty()) return Error::Invalid;
                FILE *f = fopen(path, "wb");
                if (f == nullptr) return Error::Io;
                Crc           crc;
                const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
                (void)fwrite(signature, 1, 8, f);
                uint8_t header[13] = {};
                putBig(header, uint32_t(r.width));
                putBig(header + 4, uint32_t(r.height));
                header[8] = 8; // bits
                header[9] = 2; // RGB
                Chunk ihdr(f, crc, "IHDR", 13);
                ihdr.put(header, 13);
                ihdr.end();
                const uint32_t rowBytes = 1 + 3 * uint32_t(r.width); // its filter (none), then RGB
                const uint64_t raw = uint64_t(rowBytes) * uint32_t(r.height);
                const uint64_t blocks = (raw + 65534) / 65535;
                const uint64_t length = 2 + blocks * 5 + raw + 4;
                if (length > 0x7FFFFFFFu) {
                        fclose(f);
                        return Error::TooBig;
                }
                uint8_t *row = static_cast<uint8_t *>(malloc(rowBytes));
                if (row == nullptr) {
                        fclose(f);
                        return Error::NoMemory;
                }
                Chunk         idat(f, crc, "IDAT", uint32_t(length));
                const uint8_t zlib[2] = {0x78, 0x01};
                idat.put(zlib, 2);
                uint32_t a = 1, b = 0;  // Adler-32
                uint64_t left = raw;    // bytes still to go
                uint32_t blockLeft = 0; // in the block being written
                for (int32_t y = 0; y < r.height; y++) {
                        row[0] = 0;
                        for (int32_t x = 0; x < r.width; x++) {
                                Color c = canvas.colorAt(r.x + x, r.y + y);
                                row[1 + 3 * x] = uint8_t(c >> 16);
                                row[2 + 3 * x] = uint8_t(c >> 8);
                                row[3 + 3 * x] = uint8_t(c);
                        }
                        for (uint32_t i = 0; i < rowBytes; i++) {
                                a = (a + row[i]) % 65521;
                                b = (b + a) % 65521;
                        }
                        // The row into blocks of at most 65535 bytes, each with its header.
                        uint32_t done = 0;
                        while (done < rowBytes) {
                                if (blockLeft == 0) {
                                        uint32_t n = left > 65535 ? 65535 : uint32_t(left);
                                        uint8_t  h[5] = {uint8_t(left <= 65535 ? 1 : 0), uint8_t(n), uint8_t(n >> 8),
                                                         uint8_t(~n), uint8_t(~n >> 8)};
                                        idat.put(h, 5);
                                        blockLeft = n;
                                }
                                uint32_t n = rowBytes - done < blockLeft ? rowBytes - done : blockLeft;
                                idat.put(row + done, n);
                                done += n;
                                blockLeft -= n;
                                left -= n;
                        }
                }
                uint8_t adler[4];
                putBig(adler, (b << 16) | a);
                idat.put(adler, 4);
                idat.end();
                free(row);
                Chunk iend(f, crc, "IEND", 0);
                iend.end();
                bool ok = ferror(f) == 0;
                if (fclose(f) != 0) ok = false;
                return ok ? Error::Ok : Error::Io;
        }

        Error Platform::readFile(const char *path, uint32_t maxLength, char **data, uint32_t *length) {
                *data = nullptr;
                *length = 0;
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return errno == ENOENT ? Error::NotFound : Error::Io;
                // Read in pieces rather than trusting a size asked first: a pipe or a growing
                // file has none, and the limit holds either way.
                char    *buffer = nullptr;
                uint32_t used = 0;
                uint32_t capacity = 0;
                Error    error = Error::Ok;
                for (;;) {
                        if (used + 4096 + 1 > capacity) {
                                uint32_t want = capacity == 0 ? 16384 : capacity * 2;
                                char    *grown = static_cast<char *>(realloc(buffer, want));
                                if (grown == nullptr) {
                                        error = Error::NoMemory;
                                        break;
                                }
                                buffer = grown;
                                capacity = want;
                        }
                        size_t n = fread(buffer + used, 1, capacity - used - 1, f);
                        used += uint32_t(n);
                        if (used > maxLength) {
                                error = Error::TooBig;
                                break;
                        }
                        if (n == 0) {
                                if (ferror(f)) error = Error::Io;
                                break;
                        }
                }
                fclose(f);
                if (error != Error::Ok) {
                        free(buffer);
                        return error;
                }
                buffer[used] = '\0';
                *data = buffer;
                *length = used;
                return Error::Ok;
        }

        Error Platform::imageSize(const char *path, int32_t *width, int32_t *height) {
                *width = *height = 0;
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return errno == ENOENT ? Error::NotFound : Error::Io;
                fclose(f);
                return Error::NotSupported;
        }

        int64_t Platform::now() {
                struct timespec t;
                timespec_get(&t, TIME_UTC);
                return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
        }

        bool Platform::beginPieces(const Canvas &from, const Canvas &to) {
                _piecesFrom = &from;
                _piecesTo = &to;
                return true;
        }

        bool Platform::presentPieces(const Piece *pieces, uint32_t count) {
                Canvas screen;
                if (_piecesFrom == nullptr || !beginFrame(screen)) return false;
                applyPieces(screen, *_piecesFrom, *_piecesTo, pieces, count);
                endFrame();
                return true;
        }

        void Platform::endPieces() {
                _piecesFrom = _piecesTo = nullptr;
                return;
        }

        Action defaultAction(uint32_t usage, InputMode mode) {
                // As Controls' defaults (the BMDS platform's declaration), by HID usage.
                usage &= 0xFFFF;
                if (mode == InputMode::Script) {
                        switch (usage) {
                                case 0x29: return Action::Leave;
                                case 0x4E: return Action::Next;
                                case 0x4B: return Action::Previous;
                                default: return Action::None;
                        }
                }
                if (mode == InputMode::Overview) {
                        switch (usage) {
                                case 0x50: return Action::Left;
                                case 0x4F: return Action::Right;
                                case 0x52: return Action::Up;
                                case 0x51: return Action::Down;
                                case 0x28:
                                case 0x2C: return Action::Choose;
                                case 0x2B:
                                case 0x29: return Action::Close;
                                default: return Action::None;
                        }
                }
                switch (usage) {
                        case 0x4F: // Right
                        case 0x51: // Down
                        case 0x2C: // Space
                        case 0x4E: return Action::Next;
                        case 0x50: // Left
                        case 0x52: // Up
                        case 0x2A: // Backspace
                        case 0x4B: return Action::Previous;
                        case 0x4A: return Action::First;
                        case 0x4D: return Action::Last;
                        case 0x29: return Action::Quit;
                        case 0x05: // B
                        case 0x37: return Action::Black;
                        case 0x1A: return Action::White;
                        case 0x2B: return Action::Overview;
                        case 0x0C: return Action::Into;     // I
                        case 0x06: return Action::ClearInk; // C
                        default: return Action::None;
                }
        }

        bool Platform::fileExists(const char *path) {
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return false;
                fclose(f);
                return true;
        }

        void *Platform::allocatePixels(uint32_t bytes) {
                return malloc(bytes);
        }

        void Platform::freePixels(void *pixels) {
                free(pixels);
                return;
        }

} // namespace preso
