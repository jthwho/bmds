/**
 * @file      credits.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 9 (devplan/demo.md §3): the credits. A starfield flying past, and over it a roll of what
 * made the demo: who, the music and which synth plays it, the libraries, the chip's parts used;
 * greetings picked at random; the duo chat over it (demo1.c: the outro's cue). Underneath, the
 * system's numbers live: frames a second, each core's load, the memory free. The kick pushes the
 * stars; the clap lights the headings.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/palette.h>
#include "../demo.h"

#define STARS 420
#define STAR_FIRST 1 /* 16 greys, far to near */
#define STAR_COUNT 16
#define INK_TEXT 20
#define INK_DIM 21
#define INK_PANEL 22
#define HEAD_FIRST 32 /* the headings' cycling colours */
#define HEAD_COUNT 32
#define ITEMS 48
#define SPEED 78.0f /* pixels a second */
#define PANEL_H 28

typedef struct {
                char text[72];
                int  heading;
                /* Drawn once (text is a system call a string), blitted each frame: keyed on 0. */
                bmds_game_image_t image;
                int32_t           x;
} item_t;

static const char *const greetings[] = {
        "the demoscene, then and now",
        "everyone who wrote a datasheet",
        "the people still keeping floppies",
        "copper bars everywhere",
        "the SID chip, forever",
        "all who count cycles",
        "anyone reading the source",
        "Paula, Agnus and Denise",
        "the 16 colours of EGA",
        "whoever invented the palette",
        "you, for watching",
};
#define GREETINGS ((uint32_t)(sizeof(greetings) / sizeof(greetings[0])))

static struct {
                float             x[STARS], y[STARS], z[STARS];
                item_t            items[ITEMS];
                int               count;
                float             roll;   /* pixels rolled */
                float             length; /* pixels the roll takes to end, its last line at the middle */
                uint32_t          palette[PAL_PARTS];
                char              numbers[96];
                uint8_t          *panel_pixels; /* the numbers, drawn once a second */
                bmds_canvas_t     panel;
                bmds_game_image_t panel_image;
                float             numbers_age;
} s;

static void star(int i, float z) {
        s.x[i] = (demo_frand() - 0.5f) * 40.0f;
        s.y[i] = (demo_frand() - 0.5f) * 24.0f;
        s.z[i] = z;
        return;
}

static int init(demo_t *demo) {
        (void)demo;
        for (int i = 0; i < STARS; ++i) star(i, 1.0f + 39.0f * demo_frand());
        s.panel_pixels = malloc(DEMO_WIDTH * PANEL_H);
        if (s.panel_pixels == NULL ||
            bmds_canvas_init(&s.panel, s.panel_pixels, DEMO_WIDTH, DEMO_WIDTH, PANEL_H, BMDS_PIXEL_L8) != 0)
                return -BMDS_ENOMEM;
        bmds_canvas_clear(&s.panel, INK_PANEL);
        (void)bmds_canvas_image(&s.panel, NULL, &s.panel_image);
        s.palette[0] = 0x000006;
        for (uint32_t i = 0; i < STAR_COUNT; ++i) {
                const uint32_t v = 40 + 215 * i / (STAR_COUNT - 1);
                s.palette[STAR_FIRST + i] = v << 16 | v << 8 | (v + (255 - v) / 3);
        }
        s.palette[INK_TEXT] = 0xf0f0ff;
        s.palette[INK_DIM] = 0x8890b0;
        s.palette[INK_PANEL] = 0x101828;
        for (uint32_t i = 0; i < HEAD_COUNT; ++i)
                s.palette[HEAD_FIRST + i] = bmds_palette_hsv(0.5f + 0.5f * (float)i / HEAD_COUNT, 0.6f, 1.0f);
        return 0;
}

static void item(const char *text, int heading) {
        if (s.count == ITEMS) return;
        item_t *it = &s.items[s.count++];
        snprintf(it->text, sizeof(it->text), "%s", text);
        it->heading = heading;
        return;
}

/* Item @p i's text drawn into its own image, centred. */
static void render(int i, int32_t big) {
        item_t       *it = &s.items[i];
        const int32_t font = it->heading && big >= 0 ? big : BMDS_GAME_FONT_DEFAULT;
        const int32_t w = it->text[0] != '\0' ? bmds_game_text_width(font, it->text) : 0;
        const int32_t h = bmds_game_line_height(font);
        memset(&it->image, 0, sizeof(it->image));
        if (w <= 0 || h <= 0) return;
        uint8_t      *pixels = malloc((size_t)w * (size_t)h);
        bmds_canvas_t canvas;
        if (pixels == NULL || bmds_canvas_init(&canvas, pixels, (uint32_t)w, w, h, BMDS_PIXEL_L8) != 0) {
                free(pixels);
                return;
        }
        bmds_canvas_clear(&canvas, 0);
        const uint32_t ink = it->heading ? HEAD_FIRST + (uint32_t)(i * 5) % HEAD_COUNT : i <= 1 ? INK_TEXT : INK_DIM;
        (void)bmds_canvas_text(&canvas, font, 0, 0, it->text, ink);
        (void)bmds_canvas_image(&canvas, NULL, &it->image);
        it->image.flags = BMDS_GAME_IMAGE_KEYED;
        it->image.key = 0;
        it->x = (DEMO_WIDTH - w) / 2;
        return;
}

/* The roll, made afresh each time: the synth playing, the greetings picked. */
static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.6f);
        for (int i = 0; i < s.count; ++i) free((void *)s.items[i].image.pixels);
        s.count = 0;
        s.roll = 0.0f;
        bmds_system_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (bmds_system_info(&info) != 0) info.cpu_mhz = 360;
        char line[72];
        item("BMDS DEMO #1", 1);
        item("bare metal, do stuff", 0);
        item("", 0);
        item("CODE", 1);
        item("Jason Howard", 0);
        item("and Claude", 0);
        item("", 0);
        item("MUSIC", 1);
        item("an original track at 140 BPM,", 0);
        item("written as sequencer data", 0);
        item(demo->wavetable ? "played live on the wavetable synth" : "played live on the FM synth", 0);
        item("", 0);
        item("GRAPHICS", 1);
        item("libbmds3d: wire, flat, Gouraud, textured", 0);
        item("palettes, tables, and a lot of loops", 0);
        item("", 0);
        item("SPEECH", 1);
        item("SVOX Pico, talking on the board", 0);
        item("", 0);
        item("POWERED BY", 1);
        snprintf(line, sizeof(line), "an ESP32-P4: two RISC-V cores at %u MHz", info.cpu_mhz);
        item(line, 0);
        item("no GPU: the PPA's colour lookup,", 0);
        item("the 2D-DMA, and PIE's vectors", 0);
        item("FreeRTOS, ESP-IDF, unscii", 0);
        item("", 0);
        item("GREETINGS", 1);
        uint32_t order[GREETINGS];
        demo_shuffle(order, GREETINGS);
        for (uint32_t i = 0; i < 4; ++i) item(greetings[order[i]], 0);
        item("", 0);
        item("THANKS FOR WATCHING", 1);
        const int32_t big = bmds_game_font_scaled(2);
        for (int i = 0; i < s.count; ++i) render(i, big);
        s.length = (float)(DEMO_HEIGHT / 2);
        for (int i = 0; i < s.count; ++i) s.length += s.items[i].heading ? 40.0f : 24.0f;
        s.numbers[0] = '\0';
        s.numbers_age = 1.0f;
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        (void)t;
        const sync_t     *y = &demo->sync;
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT - PANEL_H};
        bmds_game_dma_fill(demo->game, &whole, 1, 0);
        /* The stars, flying at the viewer, faster on the kick. */
        const float fly = dt * (6.0f + 22.0f * y->kick);
        for (int i = 0; i < STARS; ++i) {
                s.z[i] -= fly;
                if (s.z[i] < 0.5f) star(i, 40.0f);
                const float   k = 240.0f / s.z[i];
                const int32_t px = DEMO_WIDTH / 2 + (int32_t)(s.x[i] * k), py = DEMO_HEIGHT / 2 + (int32_t)(s.y[i] * k);
                if (px < 0 || py < 0 || px >= DEMO_WIDTH - 1 || py >= DEMO_HEIGHT - 1) {
                        star(i, 40.0f);
                        continue;
                }
                uint32_t shade = (uint32_t)((40.0f - s.z[i]) * (STAR_COUNT / 40.0f));
                if (shade > STAR_COUNT - 1) shade = STAR_COUNT - 1;
                uint8_t *p = c->pixels + (size_t)py * c->stride + px;
                p[0] = (uint8_t)(STAR_FIRST + shade);
                if (s.z[i] < 10.0f) p[1] = p[c->stride] = p[c->stride + 1] = (uint8_t)(STAR_FIRST + shade);
        }

        /* The roll: each line 24 pixels (a heading 40), from below the screen up; with the music it
           ends with the outro's eight bars (a little before), without at its own pace. */
        const float speed = y->known ? s.length / (8.0f * y->bar_seconds * 0.95f) : SPEED;
        s.roll += dt * speed;
        int32_t top = DEMO_HEIGHT - (int32_t)s.roll;
        for (int i = 0; i < s.count; ++i) {
                const item_t *it = &s.items[i];
                const int32_t h = it->heading ? 40 : 24;
                if (top > -h && top < DEMO_HEIGHT - 44 && it->image.pixels != NULL)
                        bmds_canvas_blit(c, it->x, top, &it->image, NULL);
                top += h;
        }
        if (top < 0) s.roll = 0.0f; /* round again (without music the part holds) */

        /* The numbers, live (once a second). */
        s.numbers_age += dt;
        if (s.numbers_age >= 1.0f) {
                s.numbers_age = 0.0f;
                bmds_mem_info_t m;
                memset(&m, 0, sizeof(m));
                m.size = sizeof(m);
                (void)bmds_mem_info(0, &m);
                const figures_t *f = &demo->figures;
                snprintf(s.numbers, sizeof(s.numbers), "now: %u fps   core 0 %.0f%%   core 1 %.0f%%   %u KB free",
                         f->fps, (double)f->busy[0], (double)f->busy[1], m.free_bytes / 1024);
                bmds_canvas_clear(&s.panel, INK_PANEL);
                const int32_t w = bmds_game_text_width(BMDS_GAME_FONT_DEFAULT, s.numbers);
                (void)bmds_canvas_text(&s.panel, BMDS_GAME_FONT_DEFAULT, (DEMO_WIDTH - w) / 2, 6, s.numbers, INK_TEXT);
        }
        bmds_canvas_blit(c, 0, DEMO_HEIGHT - PANEL_H, &s.panel_image, NULL);
        pal_rotate(&demo->pal, HEAD_FIRST, HEAD_COUNT, y->claps != 0 ? 4 : 1);
        if (y->claps != 0) pal_flash(&demo->pal, 0x202040, 0.2f, 6.0f);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_credits = {"Credits", init, enter, frame, NULL};
