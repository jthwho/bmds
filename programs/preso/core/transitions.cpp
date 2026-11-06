/**
 * @file      transitions.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Every frame is rows copied from the two pictures, or blocks filled: on the board each pixel is
 * a PSRAM read and a write, and a frame must come in a display frame's time.
 */

#include "transitions.h"
#include "deck.h"
#include <stdlib.h>

namespace preso {

        namespace {

                constexpr uint8_t Bayer8[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
                                                  {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
                                                  {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
                                                  {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};

                inline uint8_t *rowOf(const Canvas &c, int32_t y) {
                        return c.pixels() + size_t(y) * c.stride();
                }

                // Pixels [x, x + n) of row y of @p from into @p out.
                inline void copySpan(const Canvas &from, Canvas &out, int32_t y, int32_t x, int32_t n) {
                        if (n <= 0) return;
                        uint32_t bpp = bytesPerPixel(out.format());
                        memcpy(rowOf(out, y) + size_t(x) * bpp, rowOf(from, y) + size_t(x) * bpp, size_t(n) * bpp);
                        return;
                }

                // Eased: slow at both ends.
                uint32_t ease(uint32_t p) {
                        // 3p² - 2p³, in 16.16.
                        uint64_t x = p;
                        uint64_t v = (3 * x * x * 65536 - 2 * x * x * x) >> 32;
                        return uint32_t(v > 65536 ? 65536 : v);
                }

                struct Out {
                                Piece   *pieces;
                                uint32_t count = 0;

                                void add(Piece::Source source, int32_t x, int32_t y, int32_t w, int32_t h, int32_t fx,
                                         int32_t fy, Color color = 0) {
                                        if (w <= 0 || h <= 0 || count >= MaxPieces) return;
                                        Piece &p = pieces[count++];
                                        p.source = source;
                                        p.x = x;
                                        p.y = y;
                                        p.width = w;
                                        p.height = h;
                                        p.fromX = fx;
                                        p.fromY = fy;
                                        p.color = color;
                                }
                                void same(Piece::Source source, int32_t x, int32_t y, int32_t w, int32_t h) {
                                        add(source, x, y, w, h, x, y);
                                }
                };

                int32_t wipeEdge(uint32_t p, int32_t size, bool columns) {
                        p = ease(p);
                        if (p >= 65536) return size;
                        int32_t edge = int32_t((int64_t(size) * p) >> 16);
                        // Across, on four-pixel columns (the DMA's into the display's buffers).
                        return columns ? edge & ~3 : edge;
                }

                void wipe(Direction d, uint32_t p, uint32_t since, int32_t w, int32_t h, Out &out) {
                        bool    across = d == Direction::Left || d == Direction::Right;
                        int32_t size = across ? w : h;
                        int32_t edge = wipeEdge(p, size, across);
                        // Since an earlier frame: only the strip between its edge and this one.
                        int32_t was = since == UINT32_MAX ? 0 : wipeEdge(since, size, across);
                        if (since == UINT32_MAX) {
                                if (d == Direction::Right) out.same(Piece::From, edge, 0, w - edge, h);
                                if (d == Direction::Left) out.same(Piece::From, 0, 0, w - edge, h);
                                if (d == Direction::Down) out.same(Piece::From, 0, edge, w, h - edge);
                                if (d == Direction::Up) out.same(Piece::From, 0, 0, w, h - edge);
                        }
                        if (edge < was) was = 0; // went back (can't): whole
                        switch (d) {
                                case Direction::Right: out.same(Piece::To, was, 0, edge - was, h); break;
                                case Direction::Left: out.same(Piece::To, w - edge, 0, edge - was, h); break;
                                case Direction::Down: out.same(Piece::To, 0, was, w, edge - was); break;
                                case Direction::Up: out.same(Piece::To, 0, h - edge, w, edge - was); break;
                        }
                        return;
                }

                void dissolve(uint32_t p, uint32_t since, int32_t w, int32_t h, Out &out) {
                        // Square cells take the next slide as the threshold map passes them: the
                        // slide whole, then runs of cells across each row of cells. Since an earlier
                        // frame: only the cells turned since.
                        // 32 pixels at 1280 (16 at 640): at 16 a frame was ~600 pieces and 22 frames a
                        // second on the board; blocky is the old programs' look anyway.
                        int32_t  cell = w >= 1000 ? 32 : 16;
                        uint32_t level = uint32_t((uint64_t(p) * 64 + 32768) >> 16);
                        uint32_t floor = since == UINT32_MAX ? 0 : uint32_t((uint64_t(since) * 64 + 32768) >> 16);
                        if (since == UINT32_MAX) out.same(Piece::From, 0, 0, w, h);
                        auto turns = [&](uint32_t m) {
                                return m < level && m >= floor;
                        };
                        for (int32_t cy = 0; cy * cell < h; cy++) {
                                const uint8_t *map = Bayer8[cy & 7];
                                int32_t        y = cy * cell, ch = y + cell <= h ? cell : h - y;
                                int32_t        cx = 0, cells = (w + cell - 1) / cell;
                                while (cx < cells) {
                                        if (!turns(map[cx & 7])) {
                                                cx++;
                                                continue;
                                        }
                                        int32_t start = cx;
                                        while (cx < cells && turns(map[cx & 7])) cx++;
                                        int32_t x = start * cell, end = cx * cell < w ? cx * cell : w;
                                        out.same(Piece::To, x, y, end - x, ch);
                                }
                        }
                        return;
                }

                void crt(uint32_t p, int32_t w, int32_t h, Out &out) {
                        // Off: the picture squeezed to a line, the line to a dot, the dot gone; then
                        // the same backwards with the next slide.
                        out.add(Piece::Fill, 0, 0, w, h, 0, 0, 0x000000);
                        bool          off = p < 32768;
                        Piece::Source source = off ? Piece::From : Piece::To;
                        uint32_t      q = off ? p * 2 : (65536 - p) * 2; // 0: the picture whole; 65536: nothing
                        if (q < 40000) {
                                // Squeezing: rows sampled into a band about the middle, a row a piece; at
                                // its thinnest a white line.
                                int32_t band = int32_t(int64_t(h) * (40000 - q) / 40000);
                                if (band < 4) {
                                        out.add(Piece::Fill, 0, h / 2 - 2, w, 4, 0, 0, 0xFFFFFF);
                                        return;
                                }
                                int32_t top = (h - band) / 2;
                                for (int32_t y = 0; y < band; y++) {
                                        int32_t sy = int32_t(int64_t(y) * h / band);
                                        out.add(source, 0, top + y, w, 1, 0, sy);
                                }
                        } else if (q < 58000) {
                                // The line drawing in to a dot.
                                int32_t len = int32_t(int64_t(w) * (58000 - q) / 18000) & ~3;
                                if (len < 4) len = 4;
                                out.add(Piece::Fill, ((w - len) / 2) & ~3, h / 2 - 2, len, 4, 0, 0, 0xFFFFFF);
                        } else {
                                // The dot dimming.
                                uint32_t v = uint32_t((uint64_t(65536 - q) * 255) / 7536);
                                if (v > 255) v = 255;
                                out.add(Piece::Fill, (w / 2 - 4) & ~3, h / 2 - 2, 4, 4, 0, 0, (v << 16) | (v << 8) | v);
                        }
                        return;
                }

        } // namespace

        uint32_t pixelateStage(uint32_t p, int32_t width) {
                // The first half the slide coarsens into blocks up to a 40th of its width; the
                // second, the next slide sharpens out of them.
                uint32_t half = p < 32768 ? p * 2 : (65536 - p) * 2;
                int32_t  most = width / 40 > 2 ? width / 40 : 2;
                int32_t  block = 1 + int32_t((int64_t(most - 1) * ease(half)) >> 16);
                return uint32_t(block) | (p < 32768 ? 0u : 0x10000u);
        }

        namespace {

                void pixelate(uint32_t p, const Canvas &from, const Canvas &to, Canvas &out) {
                        uint32_t      stage = pixelateStage(p, out.width());
                        const Canvas &source = (stage & 0x10000u) == 0 ? from : to;
                        int32_t       block = int32_t(stage & 0xFFFF);
                        if (block <= 1) {
                                for (int32_t y = 0; y < out.height(); y++) copySpan(source, out, y, 0, out.width());
                                return;
                        }
                        out.clearClip();
                        for (int32_t by = 0; by < out.height(); by += block) {
                                int32_t cy = by + block / 2 < out.height() ? by + block / 2 : out.height() - 1;
                                for (int32_t bx = 0; bx < out.width(); bx += block) {
                                        int32_t cx = bx + block / 2 < out.width() ? bx + block / 2 : out.width() - 1;
                                        out.fill(Rect{bx, by, block, block}, source.colorAt(cx, cy));
                                }
                        }
                        return;
                }

        } // namespace

        bool parseTransition(StringView text, Transition *t, const char **why) {
                *t = Transition();
                *why = "is none, cut, wipe, dissolve, pixelate, crt or type, a direction, a time";
                uint32_t word = 0;
                bool     ok = true;
                bool     timed = false;
                forEachWord(text, [&](StringView w) {
                        if (word++ == 0) {
                                static const char *const names[] = {"cut",      "wipe", "dissolve",
                                                                    "pixelate", "crt",  "type"};
                                if (w.equals("none")) {
                                        t->kind = TransitionKind::Cut;
                                        return;
                                }
                                for (uint32_t i = 0; i < 6; i++) {
                                        if (w.equals(names[i])) {
                                                t->kind = TransitionKind(i);
                                                return;
                                        }
                                }
                                ok = false;
                                return;
                        }
                        if (w.equals("left")) {
                                t->direction = Direction::Left;
                        } else if (w.equals("right")) {
                                t->direction = Direction::Right;
                        } else if (w.equals("up")) {
                                t->direction = Direction::Up;
                        } else if (w.equals("down")) {
                                t->direction = Direction::Down;
                        } else {
                                uint32_t ms = 0;
                                if (!timed && parseDuration(w, 1000, &ms) && ms <= 10000) {
                                        t->durationMs = ms;
                                        timed = true;
                                } else {
                                        ok = false;
                                }
                        }
                });
                if (word == 0) ok = false;
                if (!timed && t->kind == TransitionKind::Crt) t->durationMs = 700;
                if (!timed && t->kind == TransitionKind::Type) t->durationMs = 2000;
                return ok;
        }

        uint32_t transitionPieces(const Transition &t, uint32_t progress, int32_t width, int32_t height, Piece *pieces,
                                  uint32_t since) {
                if (progress > 65536) progress = 65536;
                if (since != UINT32_MAX && since > progress) since = UINT32_MAX;
                Out out{pieces};
                switch (t.kind) {
                        case TransitionKind::Wipe: wipe(t.direction, progress, since, width, height, out); break;
                        case TransitionKind::Dissolve: dissolve(progress, since, width, height, out); break;
                        case TransitionKind::Crt: crt(progress, width, height, out); break;
                        case TransitionKind::Pixelate: return UINT32_MAX;
                        case TransitionKind::Cut:
                        case TransitionKind::Type: out.same(Piece::To, 0, 0, width, height); break;
                }
                return out.count;
        }

        void composeTransition(const Transition &t, uint32_t progress, const Canvas &from, const Canvas &to,
                               Canvas &out) {
                if (from.width() != out.width() || to.width() != out.width() || from.height() != out.height() ||
                    to.height() != out.height() || from.format() != out.format() || to.format() != out.format()) {
                        return;
                }
                if (progress > 65536) progress = 65536;
                if (t.kind == TransitionKind::Pixelate) {
                        pixelate(progress, from, to, out);
                        return;
                }
                Piece *pieces = static_cast<Piece *>(malloc(MaxPieces * sizeof(Piece)));
                if (pieces == nullptr) return;
                uint32_t n = transitionPieces(t, progress, out.width(), out.height(), pieces);
                applyPieces(out, from, to, pieces, n);
                free(pieces);
                return;
        }

} // namespace preso
