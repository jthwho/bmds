/**
 * @file      demo1.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1 (devplan/demo.md): a demoscene-style show of the system. The skeleton (step 1): a
 * 640×360 L8 picture full screen, the parts in turn, the show's palette, and the overlay.
 *
 * Keys: Space the next part; 1-9 a part by number; P pause; F the overlay (frames a second, the
 * time a frame and its drawing, each core's busy share); Escape ends it.
 *
 *     demo1 [-w]      (-w: in a window)
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>
#include <bmds/palette.h>
#include "demo.h"

#define KEY_ESCAPE 0x00070029u
#define KEY_SPACE 0x0007002cu
#define KEY_P 0x00070013u
#define KEY_F 0x00070009u
#define KEY_1 0x0007001eu
#define KEY_9 0x00070026u

static const part_t *const parts[] = {&part_copper, &part_plasma, &part_stars};
#define PARTS ((int)(sizeof(parts) / sizeof(parts[0])))

static struct {
                demo_t  demo;
                int     part, next; /* shown, and asked for (-1: none) */
                float   t;          /* seconds into the part */
                int     paused, overlay;
                int64_t last_ns;
                /* The overlay's figures, a second at a time. */
                int64_t  second_ns, busy_us[2];
                uint32_t frames, fps;
                float    frame_ms, draw_ms, busy[2];
                uint32_t counters[2];
                int      have_counters;
} show;

float demo_sines[DEMO_SINES];

void demo_init_sines(void) {
        for (int i = 0; i < DEMO_SINES; ++i) demo_sines[i] = sinf(6.28318531f * (float)i / DEMO_SINES);
        return;
}

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

/* Each core's busy time so far (µs), from the system's counters; 0 if they can't be read. */
static int busy_now(int64_t *us) {
        if (!show.have_counters) {
                for (uint32_t i = 0;; ++i) {
                        bmds_counter_t c;
                        memset(&c, 0, sizeof(c));
                        c.size = sizeof(c);
                        if (bmds_stats_list(i, &c) != 0) break;
                        if (strcmp(c.name, "cpu.core0.busy_us") == 0) show.counters[0] = c.id;
                        if (strcmp(c.name, "cpu.core1.busy_us") == 0) show.counters[1] = c.id;
                }
                show.have_counters = show.counters[0] != 0 && show.counters[1] != 0 ? 1 : -1;
        }
        return show.have_counters > 0 && bmds_stats_read(show.counters, 2, us, NULL, 0) == 2;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        demo_t *d = &show.demo;
        if (e->type == BMDS_EVENT_CLOSE) {
                bmds_game_quit(game);
                return;
        }
        if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                const uint32_t key = e->data[0];
                if (key == KEY_ESCAPE)
                        bmds_game_quit(game);
                else if (key == KEY_SPACE)
                        show.next = (show.part + 1) % PARTS;
                else if (key >= KEY_1 && key <= KEY_9 && (int)(key - KEY_1) < PARTS)
                        show.next = (int)(key - KEY_1);
                else if (key == KEY_P)
                        show.paused = !show.paused;
                else if (key == KEY_F)
                        show.overlay = !show.overlay;
                else if (parts[show.part]->event != NULL)
                        parts[show.part]->event(d, e);
                return;
        }
        if (parts[show.part]->event != NULL) parts[show.part]->event(d, e);
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                d->pointer_x = x == BMDS_POINTER_GONE ? -1 : x;
                d->pointer_y = x == BMDS_POINTER_GONE ? -1 : y;
                d->button = (e->data[2] & 1u) != 0;
        }
        return;
}

/* A tick a frame (60 a second) is what makes the loop draw; the show's time is the clock's. */
static void on_tick(bmds_game_t *game) {
        (void)game;
        return;
}

static void draw_overlay(bmds_canvas_t *c) {
        char line[160];
        snprintf(line, sizeof(line), " %u fps  frame %.1f ms  draw %.1f ms  core 0 %.0f%%  core 1 %.0f%%  %d/%d %s%s ",
                 (unsigned)show.fps, (double)show.frame_ms, (double)show.draw_ms, (double)show.busy[0],
                 (double)show.busy[1], show.part + 1, PARTS, parts[show.part]->name, show.paused ? "  (paused)" : "");
        (void)bmds_canvas_text_flags(c, BMDS_GAME_FONT_DEFAULT, 0, 0, line, PAL_TEXT, PAL_PANEL, BMDS_TEXT_OPAQUE);
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        demo_t       *d = &show.demo;
        const int64_t start = now_ns();
        const float   dt = show.last_ns != 0 ? (float)(start - show.last_ns) / 1e9f : 0.0f;
        d->game = game;
        show.last_ns = start;
        if (show.next >= 0) {
                show.part = show.next;
                show.next = -1;
                show.t = 0.0f;
                parts[show.part]->enter(d);
        }
        const float step = show.paused ? 0.0f : (dt > 0.1f ? 0.1f : dt); /* a stall doesn't jump the show */
        show.t += step;

        /* Every pixel repainted: the loop copies nothing forward. */
        const bmds_rect_t whole = {0, 0, (uint32_t)c->width, (uint32_t)c->height};
        bmds_game_repainted(game, &whole);
        parts[show.part]->frame(d, c, show.t, step);
        if (show.overlay) draw_overlay(c);
        const uint32_t *palette = pal_update(&d->pal, step);
        if (palette != NULL) (void)bmds_game_palette(game, 0, 256, palette);

        /* The figures: smoothed a frame at a time, the cores' a second at a time. */
        const int64_t end = now_ns();
        show.draw_ms += ((float)(end - start) / 1e6f - show.draw_ms) * 0.05f;
        show.frame_ms += (dt * 1000.0f - show.frame_ms) * 0.05f;
        ++show.frames;
        if (end - show.second_ns >= 1000000000) {
                int64_t us[2];
                if (busy_now(us)) {
                        const float span = (float)(end - show.second_ns) / 1000.0f;
                        for (int k = 0; k < 2; ++k) {
                                show.busy[k] = 100.0f * (float)(us[k] - show.busy_us[k]) / span;
                                show.busy_us[k] = us[k];
                        }
                }
                show.fps = show.frames;
                show.frames = 0;
                show.second_ns = end;
        }
        return;
}

int main(int argc, char **argv) {
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = DEMO_WIDTH;
        config.height = DEMO_HEIGHT;
        config.format = BMDS_PIXEL_L8;
        config.flags = BMDS_SURFACE_FULL_SCREEN | BMDS_SURFACE_WHOLE;
        config.title = "BMDS Demo #1";
        config.options = BMDS_GAME_REPAINTS;
        int opt;
        while ((opt = getopt(argc, argv, "w")) != -1) {
                if (opt == 'w') {
                        config.flags &= ~BMDS_SURFACE_FULL_SCREEN;
                } else {
                        fprintf(stderr, "usage: demo1 [-w]\n");
                        return 2;
                }
        }
        demo_t *d = &show.demo;
        d->pointer_x = d->pointer_y = -1;
        pal_init(&d->pal);
        demo_init_sines();
        /* The parts' 3D: a helper on the other core draws a band of the solids. */
        bmds_3d_config_t c3;
        memset(&c3, 0, sizeof(c3));
        c3.size = sizeof(c3);
        c3.cores = 2;
        int e3 = bmds_3d_create(&c3, &d->ctx);
        if (e3 < 0) {
                fprintf(stderr, "demo1: 3D: %s\n", strerror(-e3));
                return 1;
        }
        bmds_mat4_t projection;
        bmds_mat4_perspective(&projection, 1.0472f, (float)DEMO_WIDTH / (float)DEMO_HEIGHT, 0.5f, 120.0f);
        bmds_3d_set_projection(d->ctx, &projection);
        for (int i = 0; i < PARTS; ++i) {
                const int err = parts[i]->init(d);
                if (err < 0) {
                        fprintf(stderr, "demo1: %s: %s\n", parts[i]->name, strerror(-err));
                        return 1;
                }
        }
        show.next = 0;
        show.second_ns = now_ns();
        int64_t us[2];
        if (busy_now(us)) show.busy_us[0] = us[0], show.busy_us[1] = us[1];

        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        const int err = bmds_game_run(&config);
        if (err < 0) {
                fprintf(stderr, "demo1: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
