/**
 * @file      blittest.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A board test of `BMDS_SURFACE_BLIT` (devplan/games.md §25): random CPU fills and 2D-DMA copies
 * and fills into a surface's buffers, each buffer checked pixel by pixel against a model after
 * every blit. A mismatch is told with where it is and where it falls in its 128-byte cache line.
 *
 *     blittest [ROUNDS] [SEED] [KINDS] [PRECHECK]
 *         KINDS: a mask of 1 image, 2 fill, 4 buffer (7: all); PRECHECK: also check before each blit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/sys.h>

#define W 640
#define H 400
#define LINE 128 /* the L2's cache line */

static uint16_t *model[2], *image;
static uint8_t  *pixels[2];
static uint32_t  stride;
static uint32_t  seed = 1;
static uint32_t  bad_runs, bad_pixels, reported;

static uint32_t next(void) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
}

static uint32_t below(uint32_t n) {
        return n == 0 ? 0 : next() % n;
}

static uint16_t *at(int b, int32_t x, int32_t y) {
        return (uint16_t *)(void *)(pixels[b] + (size_t)y * stride) + x;
}

/* A random piece: mostly small and narrow (where cache lines and bursts are shared), some big. */
static void random_piece(bmds_surface_piece_t *p) {
        const uint32_t kind = below(4);
        p->width = kind == 0 ? 1 + below(8) : kind == 1 ? 1 + below(70) : kind == 2 ? 1 + below(200) : 1 + below(W);
        p->height = kind == 3 ? 1 + below(H) : 1 + below(60);
        p->x = (int32_t)below(W + 20) - 10;
        p->y = (int32_t)below(H + 20) - 10;
        p->from_x = (int32_t)below(W);
        p->from_y = (int32_t)below(H);
        return;
}

/* Pieces side by side on the same rows, a few pixels apart (the gaps sharing cache lines). */
static uint32_t neighbours(bmds_surface_piece_t *p, uint32_t max) {
        uint32_t       n = 0;
        int32_t        x = (int32_t)below(64), y = (int32_t)below(H - 40);
        const uint32_t h = 1 + below(40);
        while (n < max && x < W) {
                bmds_surface_piece_t *q = &p[n++];
                q->x = x;
                q->y = y;
                q->width = 1 + below(40);
                q->height = h;
                q->from_x = (int32_t)below(W / 2);
                q->from_y = (int32_t)below(H / 2);
                x += (int32_t)q->width + 1 + (int32_t)below(20);
        }
        return n;
}

/* The model of what a blit's pieces do (cut as the kernel cuts them). */
static void model_blit(int b, uint32_t source, uint32_t from, const bmds_surface_piece_t *p, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
                for (int32_t y = 0; y < (int32_t)p[i].height; ++y)
                        for (int32_t x = 0; x < (int32_t)p[i].width; ++x) {
                                const int32_t dx = p[i].x + x, dy = p[i].y + y;
                                if (dx < 0 || dy < 0 || dx >= W || dy >= H) continue;
                                if (source == BMDS_BLIT_FILL) {
                                        model[b][dy * W + dx] = (uint16_t)from;
                                        continue;
                                }
                                const int32_t sx = p[i].from_x + x, sy = p[i].from_y + y;
                                if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
                                model[b][dy * W + dx] =
                                        source == BMDS_BLIT_IMAGE ? image[sy * W + sx] : model[from][sy * W + sx];
                        }
        }
        return;
}

static const bmds_surface_piece_t *last_pieces;
static uint32_t                    last_count;

/* A piece on row @p y whose bytes share the 64-byte block holding column @p x, or -1. */
static int edge_of(int32_t x, int32_t y) {
        const int32_t block0 = x / 32 * 32, block1 = block0 + 32;
        for (int i = (int)last_count - 1; i >= 0; --i) {
                const bmds_surface_piece_t *p = &last_pieces[i];
                if (y < p->y || y >= p->y + (int32_t)p->height) continue;
                if (p->x < block1 && p->x + (int32_t)p->width > block0) return i;
        }
        return -1;
}

/* The last of the blit's pieces that writes (x, y) (the kernel's order), or -1. */
static int piece_of(int32_t x, int32_t y) {
        for (int i = (int)last_count - 1; i >= 0; --i) {
                const bmds_surface_piece_t *p = &last_pieces[i];
                if (x >= p->x && y >= p->y && x < p->x + (int32_t)p->width && y < p->y + (int32_t)p->height) return i;
        }
        return -1;
}

static void check(int b, const char *after, uint32_t round) {
        for (int32_t y = 0; y < H; ++y) {
                const uint16_t *row = at(b, 0, y), *want = &model[b][y * W];
                for (int32_t x = 0; x < W;) {
                        if (row[x] == want[x]) {
                                ++x;
                                continue;
                        }
                        int32_t end = x;
                        while (end < W && row[end] != want[end]) ++end;
                        ++bad_runs;
                        bad_pixels += (uint32_t)(end - x);
                        if (reported < 40) {
                                ++reported;
                                const uintptr_t             addr = (uintptr_t)at(b, x, y);
                                const int                   pi = piece_of(x, y);
                                const bmds_surface_piece_t *pp = pi >= 0 ? &last_pieces[pi] : NULL;
                                printf("round %u after %s: buffer %d row %d x %d-%d (%d px), line offset %u, "
                                       "have %04x want %04x, piece %d of %u (rows %d-%d), block shared with %d\n",
                                       round, after, b, (int)y, (int)x, (int)end - 1, (int)(end - x),
                                       (unsigned)(addr % LINE), row[x], want[x], pi, last_count,
                                       pp != NULL ? (int)pp->y : -1,
                                       pp != NULL ? (int)(pp->y + (int32_t)pp->height - 1) : -1);
                        }
                        /* The model takes what is there, so one fault is told once. */
                        for (int32_t k = x; k < end; ++k) model[b][y * W + k] = row[k];
                        x = end;
                }
        }
        return;
}

int main(int argc, char **argv) {
        const uint32_t rounds = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 2000;
        if (argc > 2) seed = (uint32_t)strtoul(argv[2], NULL, 0) | 1u;
        const uint32_t kinds = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) & 7u : 7u;
        if (kinds == 0) return 2;
        bmds_surface_config_t sc;
        memset(&sc, 0, sizeof(sc));
        sc.size = sizeof(sc);
        sc.width = W;
        sc.height = H;
        sc.format = BMDS_PIXEL_RGB565;
        sc.buffers = 2;
        sc.title = "blittest";
        const int32_t fd = bmds_surface_open(&sc);
        if (fd < 0) {
                fprintf(stderr, "blittest: surface_open: %d\n", (int)fd);
                return 1;
        }
        for (int b = 0; b < 2; ++b) {
                bmds_surface_buffer_t info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                info.index = (uint32_t)b;
                if (bmds_surface_buffer(fd, &info) != 0) return 1;
                pixels[b] = info.pixels;
                stride = info.stride;
                model[b] = bmds_mem_alloc(W * H * 2, 16, 0);
                if (model[b] == NULL) return 1;
                memset(model[b], 0, W * H * 2); /* the buffers start black */
        }
        image = bmds_mem_alloc(W * H * 2, 16, 0);
        if (image == NULL) return 1;
        for (uint32_t i = 0; i < W * H; ++i) image[i] = (uint16_t)next();
        const bmds_surface_image_t im = {sizeof(im), 0, image, W, H, W * 2};
        if (bmds_surface_control(fd, BMDS_SURFACE_IMAGE, (void *)&im, sizeof(im)) != 0) {
                fprintf(stderr, "blittest: no BMDS_SURFACE_IMAGE\n");
                return 1;
        }
        static bmds_surface_piece_t pieces[BMDS_SURFACE_BLIT_MAX];
        uint32_t                    blits = 0;
        for (uint32_t round = 0; round < rounds; ++round) {
                const int32_t b = bmds_surface_acquire(fd, -1);
                if (b < 0 || b > 1) {
                        fprintf(stderr, "blittest: acquire: %d\n", (int)b);
                        return 1;
                }
                /* What this buffer holds was checked when it was last drawn. */
                for (int op = 0; op < 6; ++op) {
                        /* Some CPU drawing first, in rows a blit may then share. */
                        for (int f = 0; f < 3; ++f) {
                                bmds_surface_piece_t p;
                                random_piece(&p);
                                const uint16_t c = (uint16_t)next();
                                for (int32_t y = p.y < 0 ? 0 : p.y; y < p.y + (int32_t)p.height && y < H; ++y)
                                        for (int32_t x = p.x < 0 ? 0 : p.x; x < p.x + (int32_t)p.width && x < W; ++x) {
                                                *at(b, x, y) = c;
                                                model[b][y * W + x] = c;
                                        }
                        }
                        uint32_t count;
                        uint32_t source = below(3);
                        while ((kinds & (source == BMDS_BLIT_IMAGE ? 1u : source == BMDS_BLIT_FILL ? 2u : 4u)) == 0)
                                source = below(3);
                        if (below(2) == 0) {
                                count = neighbours(pieces, BMDS_SURFACE_BLIT_MAX);
                        } else {
                                count = 1 + below(BMDS_SURFACE_BLIT_MAX);
                                for (uint32_t i = 0; i < count; ++i) random_piece(&pieces[i]);
                        }
                        const uint32_t from = source == BMDS_BLIT_FILL     ? (uint32_t)(next() & 0xffffu)
                                              : source == BMDS_BLIT_BUFFER ? (uint32_t)(1 - b)
                                                                           : 0u;
                        last_count = 0;
                        /* A check here would read every line (writing the CPU's back by eviction): it would
                           hide a blit that loses them (games.md §25g). Only with a fourth argument. */
                        if (argc > 4) check(b, "cpu", round);
                        const bmds_surface_blit_t bl = {sizeof(bl), (uint32_t)b, source, from, count, pieces};
                        const int32_t r = bmds_surface_control(fd, BMDS_SURFACE_BLIT, (void *)&bl, sizeof(bl));
                        if (r != 0) {
                                fprintf(stderr, "blittest: blit: %d\n", (int)r);
                                return 1;
                        }
                        ++blits;
                        last_pieces = pieces;
                        last_count = count;
                        model_blit(b, source, from, pieces, count);
                        check(b,
                              source == BMDS_BLIT_FILL    ? "fill"
                              : source == BMDS_BLIT_IMAGE ? "image"
                                                          : "buffer",
                              round);
                }
                const bmds_surface_present_t pr = {sizeof(pr), (uint32_t)b, NULL, 0};
                if (bmds_surface_present(fd, &pr) < 0) return 1;
        }
        printf("blittest: %u rounds, %u blits: %u bad runs, %u bad pixels\n", rounds, blits, bad_runs, bad_pixels);
        bmds_close(fd);
        return bad_runs != 0;
}
