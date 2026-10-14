/**
 * @file      surfdemo.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Surfaces (devplan/games.md §11): bouncing squares drawn with the CPU into a surface's buffers
 * and presented each frame; in L8 the palette cycles. With -d only the squares are redrawn and
 * presented as damage (the palette stays). Escape or closing the window ends it; the frames shown
 * and dropped are printed.
 *
 *     surfdemo [-f l8|565|888] [-s WIDTHxHEIGHT] [-b BUFFERS] [-t SECONDS] [-F] [-d] [-v]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

#define SQUARES 12
#define SIDE 24

typedef struct {
                int      x, y, dx, dy;
                uint32_t color; /* 0xRRGGBB, or an index in L8 */
} square_t;

static uint32_t format = BMDS_PIXEL_RGB565;
static int      width = 320, height = 180;

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

/* A rectangle of one color: its first row written pixel by pixel, the rest copied from it. */
static void fill(uint8_t *pixels, uint32_t stride, int x, int y, int w, int h, uint32_t color) {
        const size_t bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
        uint8_t     *first = pixels + (size_t)y * stride + (size_t)x * bpp;
        if (format == BMDS_PIXEL_L8) {
                memset(first, (int)color, (size_t)w);
        } else if (format == BMDS_PIXEL_RGB565) {
                const uint16_t v =
                        (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f));
                uint16_t *p = (uint16_t *)first;
                for (int c = 0; c < w; ++c) p[c] = v;
        } else {
                for (int c = 0; c < w; ++c) {
                        first[3 * c] = (uint8_t)color;
                        first[3 * c + 1] = (uint8_t)(color >> 8);
                        first[3 * c + 2] = (uint8_t)(color >> 16);
                }
        }
        for (int r = 1; r < h; ++r) memcpy(first + (size_t)r * stride, first, (size_t)w * bpp);
}

/* Adds the square at (@p x, @p y) to @p list (of @p n). */
static void add_rect(bmds_rect_t *list, uint32_t *n, int x, int y) {
        if (x < 0) return; /* not drawn yet */
        list[*n].x = x;
        list[*n].y = y;
        list[*n].width = SIDE;
        list[*n].height = SIDE;
        ++*n;
}

static const char *event_name(uint32_t type) {
        switch (type) {
                case BMDS_EVENT_KEY: return "key";
                case BMDS_EVENT_POINTER: return "pointer";
                case BMDS_EVENT_WHEEL: return "wheel";
                case BMDS_EVENT_ACTIVE: return "active";
                case BMDS_EVENT_PRESENTED: return "presented";
                case BMDS_EVENT_CLOSE: return "close";
                default: return "?";
        }
}

int main(int argc, char **argv) {
        int      buffers = 2, seconds = 0, verbose = 0, damage = 0, opt;
        uint32_t flags = 0;
        while ((opt = getopt(argc, argv, "f:s:b:t:Fdv")) != -1) {
                if (opt == 'f') {
                        format = strcmp(optarg, "l8") == 0    ? BMDS_PIXEL_L8
                                 : strcmp(optarg, "888") == 0 ? BMDS_PIXEL_BGR24
                                                              : BMDS_PIXEL_RGB565;
                } else if (opt == 's') {
                        if (sscanf(optarg, "%dx%d", &width, &height) != 2) return 2;
                } else if (opt == 'b') {
                        buffers = atoi(optarg);
                } else if (opt == 't') {
                        seconds = atoi(optarg);
                } else if (opt == 'F') {
                        flags |= BMDS_SURFACE_FULL_SCREEN;
                } else if (opt == 'd') {
                        damage = 1;
                } else if (opt == 'v') {
                        verbose = 1;
                } else {
                        fprintf(stderr,
                                "usage: surfdemo [-f l8|565|888] [-s WxH] [-b BUFFERS] [-t SECONDS] [-F] [-d] [-v]\n");
                        return 2;
                }
        }
        if (!bmds_sys_has(BMDS_SYS_SURFACE_OPEN)) {
                fprintf(stderr, "surfdemo: this system has no surfaces\n");
                return 1;
        }
        bmds_surface_config_t config = {sizeof(config)};
        config.width = (uint32_t)width;
        config.height = (uint32_t)height;
        config.format = format;
        config.buffers = (uint32_t)buffers;
        config.flags = flags;
        config.title = "Surface demo";
        const int32_t fd = bmds_surface_open(&config);
        if (fd < 0) {
                fprintf(stderr, "surfdemo: surface_open: %s\n", strerror(-fd));
                return 1;
        }
        uint8_t *pixels[3] = {0};
        uint32_t stride = 0;
        for (int i = 0; i < buffers; ++i) {
                bmds_surface_buffer_t b = {sizeof(b), (uint32_t)i};
                const int32_t         r = bmds_surface_buffer(fd, &b);
                if (r < 0) {
                        fprintf(stderr, "surfdemo: surface_buffer: %s\n", strerror(-r));
                        return 1;
                }
                pixels[i] = b.pixels;
                stride = b.stride;
        }
        uint32_t palette[256];
        if (format == BMDS_PIXEL_L8) {
                for (int i = 0; i < 256; ++i) palette[i] = (uint32_t)i * 0x010101u;
        }
        square_t sq[SQUARES];
        for (int i = 0; i < SQUARES; ++i) {
                sq[i].x = (i * 37) % (width - SIDE);
                sq[i].y = (i * 53) % (height - SIDE);
                sq[i].dx = 1 + i % 3;
                sq[i].dy = 1 + (i + 1) % 2;
                sq[i].color = format == BMDS_PIXEL_L8 ? (uint32_t)(16 + i * 20)
                                                      : (uint32_t)((i * 0x3a1f79u) | 0x404040u) & 0xffffffu;
        }
        /* Where each buffer's squares were drawn (-d: erased there before drawing again), and the
           last present's. */
        int      drawn[3][SQUARES][2], last[SQUARES][2];
        uint32_t background = format == BMDS_PIXEL_L8 ? 0 : 0x101828;
        for (int b = 0; b < 3; ++b)
                for (int i = 0; i < SQUARES; ++i) drawn[b][i][0] = -1, drawn[b][i][1] = -1;
        for (int i = 0; i < SQUARES; ++i) last[i][0] = -1, last[i][1] = -1;
        int           fresh[3] = {1, 1, 1}; /* the buffer's background not drawn yet */
        bmds_rect_t   rects[2 * SQUARES];
        const int64_t start = now_ns();
        uint32_t      frames = 0, phase = 0;
        int32_t       shown = -1; /* the frame the last present goes out in */
        int           running = 1;
        while (running) {
                /* Events, without waiting. */
                bmds_pollfd_t p = {sizeof(p), fd, BMDS_POLL_IN, 0};
                while (running && bmds_io_poll(&p, 1, 0, NULL) > 0 && (p.revents & BMDS_POLL_IN) != 0) {
                        bmds_surface_event_t e[8];
                        const int32_t        n = bmds_read(fd, e, sizeof(e));
                        if (n <= 0) break;
                        for (int i = 0; i < n / (int)sizeof(e[0]); ++i) {
                                if (!verbose) {
                                        /* printed with -v only: a terminal scrolling them costs time */
                                } else if (e[i].type == BMDS_EVENT_POINTER || e[i].type == BMDS_EVENT_KEY ||
                                           e[i].type == BMDS_EVENT_WHEEL) {
                                        printf("%s %08x %d %d %u flags %u\n", event_name(e[i].type),
                                               (unsigned)e[i].data[0], (int)e[i].data[0], (int)e[i].data[1],
                                               (unsigned)e[i].data[2], (unsigned)e[i].flags);
                                } else {
                                        printf("%s %u %08x %08x\n", event_name(e[i].type), (unsigned)e[i].data[0],
                                               (unsigned)e[i].data[1], (unsigned)e[i].data[2]);
                                }
                                if (e[i].type == BMDS_EVENT_CLOSE ||
                                    (e[i].type == BMDS_EVENT_KEY && e[i].data[0] == 0x00070029u))
                                        running = 0; /* the window closed, or Escape */
                        }
                        p.revents = 0;
                }
                if (seconds > 0 && now_ns() - start >= (int64_t)seconds * 1000000000) running = 0;
                if (!running) break;
                const int32_t index = bmds_surface_acquire(fd, -1);
                if (index < 0) {
                        fprintf(stderr, "surfdemo: surface_acquire: %s\n", strerror(-index));
                        break;
                }
                uint8_t *px = pixels[index];
                uint32_t nrects = 0;
                if (!damage || fresh[index]) {
                        fill(px, stride, 0, 0, width, height, background);
                } else {
                        /* This buffer's squares from its last frame, back to the background. */
                        for (int i = 0; i < SQUARES; ++i)
                                fill(px, stride, drawn[index][i][0], drawn[index][i][1], SIDE, SIDE, background);
                }
                for (int i = 0; i < SQUARES; ++i) {
                        square_t *s = &sq[i];
                        s->x += s->dx;
                        s->y += s->dy;
                        if (s->x < 0 || s->x > width - SIDE) s->dx = -s->dx, s->x += 2 * s->dx;
                        if (s->y < 0 || s->y > height - SIDE) s->dy = -s->dy, s->y += 2 * s->dy;
                }
                /* Drawn after all are erased: one square's erase never cuts another. */
                for (int i = 0; i < SQUARES; ++i) {
                        square_t *s = &sq[i];
                        fill(px, stride, s->x, s->y, SIDE, SIDE, s->color);
                        drawn[index][i][0] = s->x, drawn[index][i][1] = s->y;
                        /* Changed since the last present: where the square was, and is. */
                        add_rect(rects, &nrects, last[i][0], last[i][1]);
                        add_rect(rects, &nrects, s->x, s->y);
                        last[i][0] = s->x, last[i][1] = s->y;
                }
                const int whole = !damage || fresh[index];
                fresh[index] = 0;
                if (format == BMDS_PIXEL_L8 && !damage) {
                        /* The squares' entries cycle through the hues. */
                        for (int i = 0; i < SQUARES; ++i) {
                                const uint32_t k = (phase + (uint32_t)i * 21) % 192, t = k % 64 * 4;
                                palette[16 + i * 20] = k < 64    ? (0xff0000u | t << 8)
                                                       : k < 128 ? (0x00ff00u | t)
                                                                 : (0x0000ffu | t << 16);
                        }
                        bmds_surface_palette_t pal = {sizeof(pal), 0, 256, palette};
                        bmds_surface_control(fd, BMDS_SURFACE_PALETTE, &pal, sizeof(pal));
                        ++phase;
                }
                /* Drawn while the last one went out: now its frame has begun, this one follows. */
                if (shown >= 0) {
                        bmds_surface_wait_t wait = {sizeof(wait), (uint32_t)shown, 100};
                        bmds_surface_control(fd, BMDS_SURFACE_WAIT, &wait, sizeof(wait));
                }
                bmds_surface_present_t present = {sizeof(present), (uint32_t)index, whole ? NULL : rects,
                                                  whole ? 0 : nrects};
                const int32_t          frame = bmds_surface_present(fd, &present);
                if (frame < 0) {
                        fprintf(stderr, "surfdemo: surface_present: %s\n", strerror(-frame));
                        break;
                }
                shown = frame;
                ++frames;
        }
        bmds_surface_info_t info = {sizeof(info)};
        bmds_surface_control(fd, BMDS_SURFACE_INFO, &info, sizeof(info));
        const double secs = (double)(now_ns() - start) / 1e9;
        printf("surfdemo: %u presents in %.1f s (%.1f a second), %u dropped; %ux%u at %ux on screen\n",
               (unsigned)frames, secs, secs > 0 ? frames / secs : 0.0, (unsigned)info.dropped, (unsigned)info.width,
               (unsigned)info.height, (unsigned)(info.scale_x16 / 16));
        bmds_close(fd);
        return 0;
}
