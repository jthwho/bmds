/**
 * @file      bounce.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's frame loop (bmds/game.h, devplan/games.md §13): squares bouncing at 60 ticks a
 * second, each frame drawing only what changed (a square erased where it was, drawn where it is;
 * once a second, the rate in text). The loop brings each buffer up to the last frame shown, so
 * nothing else is drawn. With -i, each square is a picture (a PNG's transparency kept). Escape
 * or closing the window ends it.
 *
 *     bounce [-s WIDTHxHEIGHT] [-f l8|565|888] [-n SQUARES] [-i PICTURE] [-F]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>

#define MAX_SQUARES 64
#define SIDE 16

typedef struct {
                int32_t  x, y, dx, dy, old_x, old_y;
                uint32_t rgb;
} square_t;

typedef struct {
                int32_t  width, height, count;
                square_t sq[MAX_SQUARES];
                int      drawn; /* the background drawn */
                uint64_t second_start, frames_in_second, frames;
                int64_t  second_ns;
                char     rate[32];
                int32_t           side_x, side_y; /* a square's size */
                bmds_game_image_t image;          /* with -i: drawn as each square */
                uint32_t          palette[256];   /* L8's */
} state_t;

#define L8_BACKGROUND 0
#define L8_KEY 1 /* no color: a picture's transparent pixels */
#define L8_INK 15
#define L8_CUBE 16 /* 6 x 6 x 6 colors */

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_CLOSE || (e->type == BMDS_EVENT_KEY && e->data[0] == 0x00070029u))
                bmds_game_quit(game); /* closed, or Escape */
}

static void on_tick(bmds_game_t *game) {
        state_t *s = bmds_game_arg(game);
        for (int32_t i = 0; i < s->count; ++i) {
                square_t *q = &s->sq[i];
                q->x += q->dx;
                q->y += q->dy;
                if (q->x < 0 || q->x > s->width - s->side_x) q->dx = -q->dx, q->x += 2 * q->dx;
                if (q->y < 20 || q->y > s->height - s->side_y) q->dy = -q->dy, q->y += 2 * q->dy;
        }
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t       *s = bmds_game_arg(game);
        const uint32_t background = bmds_canvas_pixel(c, c->format == BMDS_PIXEL_L8 ? L8_BACKGROUND : 0x101828);
        const uint32_t ink = bmds_canvas_pixel(c, c->format == BMDS_PIXEL_L8 ? L8_INK : 0xe0e0e0);
        if (!s->drawn) {
                if (c->format == BMDS_PIXEL_L8) {
                        bmds_surface_palette_t p = {sizeof(p), 0, 256, s->palette};
                        (void)bmds_surface_control(bmds_game_fd(game), BMDS_SURFACE_PALETTE, &p, sizeof(p));
                }
                bmds_canvas_clear(c, background);
                s->drawn = 1;
        } else {
                for (int32_t i = 0; i < s->count; ++i)
                        bmds_canvas_fill(c, s->sq[i].old_x, s->sq[i].old_y, s->side_x, s->side_y, background);
        }
        for (int32_t i = 0; i < s->count; ++i) {
                square_t *q = &s->sq[i];
                if (s->image.pixels != NULL)
                        bmds_canvas_blit(c, q->x, q->y, &s->image, NULL);
                else
                        bmds_canvas_fill(c, q->x, q->y, SIDE, SIDE, bmds_canvas_pixel(c, q->rgb));
                q->old_x = q->x, q->old_y = q->y;
        }
        /* Once a second: the frames drawn in it. */
        ++s->frames;
        const int64_t now = now_ns();
        if (now - s->second_ns >= 1000000000) {
                snprintf(s->rate, sizeof(s->rate), "%llu frames a second",
                         (unsigned long long)(s->frames - s->frames_in_second));
                s->frames_in_second = s->frames;
                s->second_ns = now;
                bmds_canvas_fill(c, 0, 0, s->width, 18, background);
                bmds_canvas_text(c, 4, 1, s->rate, ink, 1);
        }
}

int main(int argc, char **argv) {
        static state_t     s;
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = 320;
        config.height = 180;
        config.format = BMDS_PIXEL_RGB565;
        config.title = "Bounce";
        s.count = 12;
        const char *picture = NULL;
        int         opt;
        while ((opt = getopt(argc, argv, "s:f:n:i:F")) != -1) {
                if (opt == 's') {
                        if (sscanf(optarg, "%ux%u", &config.width, &config.height) != 2) return 2;
                } else if (opt == 'f') {
                        config.format = strcmp(optarg, "l8") == 0    ? BMDS_PIXEL_L8
                                        : strcmp(optarg, "888") == 0 ? BMDS_PIXEL_BGR24
                                                                     : BMDS_PIXEL_RGB565;
                } else if (opt == 'n') {
                        s.count = atoi(optarg);
                        if (s.count < 1 || s.count > MAX_SQUARES) return 2;
                } else if (opt == 'i') {
                        picture = optarg;
                } else if (opt == 'F') {
                        config.flags |= BMDS_SURFACE_FULL_SCREEN;
                } else {
                        fprintf(stderr, "usage: bounce [-s WxH] [-f l8|565|888] [-n SQUARES] [-i PICTURE] [-F]\n");
                        return 2;
                }
        }
        s.width = (int32_t)config.width;
        s.height = (int32_t)config.height;
        for (uint32_t i = 0; i < 256; ++i) s.palette[i] = i * 0x010101u;
        s.palette[L8_BACKGROUND] = 0x101828;
        s.palette[L8_INK] = 0xe0e0e0;
        for (uint32_t i = 0; i < 216; ++i)
                s.palette[L8_CUBE + i] = (i / 36 * 51) << 16 | (i / 6 % 6 * 51) << 8 | (i % 6 * 51);
        s.side_x = s.side_y = SIDE;
        if (picture != NULL) {
                const bmds_game_load_t how = {sizeof(how), config.format, 0, 0, s.palette, 256, L8_KEY, 0};
                const int              err = bmds_game_image_load(picture, &how, &s.image);
                if (err < 0) {
                        fprintf(stderr, "bounce: %s: %s\n", picture, strerror(-err));
                        return 1;
                }
                printf("bounce: %s: %dx%d%s\n", picture, (int)s.image.width, (int)s.image.height,
                       (s.image.flags & BMDS_GAME_IMAGE_KEYED) != 0 ? ", keyed" : "");
                s.side_x = s.image.width;
                s.side_y = s.image.height;
                if (s.side_x > s.width / 2 || s.side_y > (s.height - 20) / 2) {
                        fprintf(stderr, "bounce: %s: too big for the picture\n", picture);
                        return 1;
                }
        }
        for (int32_t i = 0; i < s.count; ++i) {
                s.sq[i].x = (i * 37) % (s.width - s.side_x);
                s.sq[i].y = 20 + (i * 53) % (s.height - s.side_y - 20);
                s.sq[i].dx = 1 + i % 3;
                s.sq[i].dy = 1 + (i + 1) % 2;
                s.sq[i].rgb = config.format == BMDS_PIXEL_L8 ? (uint32_t)(L8_CUBE + (i * 37) % 216)
                                                             : ((uint32_t)i * 0x3a1f79u | 0x404040u) & 0xffffffu;
        }
        s.second_ns = now_ns();
        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        const int err = bmds_game_run(&config);
        bmds_game_image_free(&s.image);
        if (err < 0) {
                fprintf(stderr, "bounce: %s\n", strerror(-err));
                return 1;
        }
        printf("bounce: %llu frames, %s\n", (unsigned long long)s.frames, s.rate);
        return 0;
}
