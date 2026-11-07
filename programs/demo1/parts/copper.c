/**
 * @file      copper.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 1 (devplan/demo.md §3): copper bars sliding behind a sine scroller, the copper way: every
 * pair of rows of the picture is its own palette entry (a still picture, copied in by the 2D-DMA
 * each frame), and the bars and the sky are those entries' colours, worked out each frame. The
 * message is drawn once into a strip in unscii at four times its size; each frame each column of
 * the screen copies a column of the strip moved up or down by a wave, its rows coloured by a band
 * of the palette that cycles. On the beat (sync_t): the kick brightens the bars and the sky and
 * widens the bars' swing, the clap lifts the wave, each kick jolts the text's colours on. The
 * pointer across bends the wave; a click flashes the palette.
 */

#include <stdlib.h>
#include <string.h>
#include <bmds/palette.h>
#include "../demo.h"

#define BARS 8
#define ROWS (DEMO_HEIGHT / 2) /* palette entries, a pair of rows each */
#define ROW_FIRST 1
#define BAR_ROWS 14                       /* a bar's entries (28 rows) */
#define TEXT_FIRST (ROW_FIRST + ROWS + 3) /* 184: the scroller's rows */
#define TEXT_COUNT 64
#define SCALE 4 /* unscii 16 × 4 */
#define STRIP_HEIGHT (16 * SCALE)

static const char message[] =
        "      BMDS DEMO #1  ***  BARE METAL, DO STUFF  ***  AN ESP32-P4: TWO RISC-V CORES AT 360 MHZ, "
        "NO GPU  ***  EVERY PIXEL DRAWN IN SOFTWARE, EVERY COLOUR LOOKED UP BY THE PPA  ***  THESE BARS "
        "ARE ONLY THE PALETTE: THE PICTURE BEHIND THEM NEVER CHANGES  ***  "
        "PRESS SPACE FOR THE NEXT PART, F FOR THE NUMBERS  ***      ";

static struct {
                uint8_t          *strip; /* the message: 1 where a glyph's pixel is */
                int32_t           width; /* its width in pixels */
                bmds_canvas_t     canvas;
                bmds_game_image_t rows; /* the still picture: each pair of rows its entry */
                uint32_t          palette[PAL_PARTS];
                uint32_t          sky[ROWS];
                uint32_t          bars[BARS][BAR_ROWS];
                uint32_t          row_rgb[ROWS]; /* this frame's */
                uint32_t          lit[BAR_ROWS]; /* a bar this frame, the kick's light in it */
                float             scroll;
} s;

static int init(demo_t *demo) {
        (void)demo;
        const int32_t font = bmds_game_font_scaled(SCALE);
        if (font < 0) return font;
        s.width = bmds_game_text_width(font, message);
        s.strip = malloc((size_t)s.width * STRIP_HEIGHT);
        uint8_t *rows = bmds_mem_alloc(DEMO_WIDTH * DEMO_HEIGHT, 64, 0); /* the 2D-DMA reads it */
        if (s.strip == NULL || rows == NULL ||
            bmds_canvas_init(&s.canvas, s.strip, (uint32_t)s.width, s.width, STRIP_HEIGHT, BMDS_PIXEL_L8) != 0)
                return -BMDS_ENOMEM;
        memset(s.strip, 0, (size_t)s.width * STRIP_HEIGHT);
        (void)bmds_canvas_text(&s.canvas, font, 0, 0, message, 1);
        for (int32_t y = 0; y < DEMO_HEIGHT; ++y) memset(rows + (size_t)y * DEMO_WIDTH, ROW_FIRST + y / 2, DEMO_WIDTH);
        s.rows = (bmds_game_image_t){rows, DEMO_WIDTH, DEMO_WIDTH, DEMO_HEIGHT, BMDS_PIXEL_L8, 0, 0};

        /* The sky: deep blue to black, top to bottom. The bars: each its own hue, dark-bright-dark. */
        bmds_palette_ramp(s.sky, 0, ROWS, 0x101850, 0x000008);
        for (uint32_t b = 0; b < BARS; ++b) {
                uint32_t ramp[BAR_ROWS / 2];
                bmds_palette_ramp3(ramp, 0, BAR_ROWS / 2, 0x000000, bmds_palette_hsv((float)b / BARS, 0.85f, 1.0f),
                                   0xffffff);
                for (uint32_t r = 0; r < BAR_ROWS / 2; ++r) s.bars[b][r] = s.bars[b][BAR_ROWS - 1 - r] = ramp[r];
        }
        memcpy(s.palette + ROW_FIRST, s.sky, sizeof(s.sky));
        /* The text: a band of warm and cold colours, cycled. */
        for (uint32_t i = 0; i < TEXT_COUNT; ++i)
                s.palette[TEXT_FIRST + i] = bmds_palette_hsv(
                        0.08f + 0.5f * (0.5f + 0.5f * dsin(6.28318531f * (float)i / TEXT_COUNT)), 0.7f, 1.0f);
        return 0;
}

static void enter(demo_t *demo) {
        (void)bmds_game_dma_image(demo->game, 0, &s.rows);
        pal_set_base(&demo->pal, s.palette, 0.6f);
        s.scroll = -DEMO_WIDTH;
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER && (e->data[2] & 1u) != 0 && !demo->button)
                pal_flash(&demo->pal, 0xffffff, 0.7f, 2.5f); /* a click: a flash */
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        /* The picture: the same every frame, by the 2D-DMA. */
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT};
        bmds_game_dma_copy(demo->game, 0, &whole, 1);

        /* The bars, back to front, as the rows' colours over the sky (the kick lights both). */
        const sync_t  *y = &demo->sync;
        const uint32_t light = (uint32_t)(110.0f * y->kick);
        bmds_palette_mix_rgb(s.row_rgb, s.sky, ROWS, 0x4030a0, light);
        const float swing = 55.0f + 25.0f * y->kick;
        for (int b = 0; b < BARS; ++b) {
                const float   phase = t * 1.3f + (float)b * 0.55f;
                const int32_t top = (int32_t)(ROWS / 2 - BAR_ROWS / 2 + swing * dsin(phase) * dcos(t * 0.37f));
                bmds_palette_mix_rgb(s.lit, s.bars[b], BAR_ROWS, 0xffffff, light);
                for (int32_t r = 0; r < BAR_ROWS; ++r)
                        if (top + r >= 0 && top + r < ROWS) s.row_rgb[top + r] = s.lit[r];
        }
        pal_set_entries(&demo->pal, ROW_FIRST, ROWS, s.row_rgb);

        /* The scroller: column by column, each moved by the wave (the pointer across bends it). */
        s.scroll += 150.0f * dt;
        if (s.scroll >= (float)s.width) s.scroll -= (float)(s.width + DEMO_WIDTH);
        const float   bend = demo->pointer_x >= 0 ? 0.004f + 0.02f * (float)demo->pointer_x / DEMO_WIDTH : 0.012f;
        const int32_t base = DEMO_HEIGHT / 2 - STRIP_HEIGHT / 2;
        const float   height = 62.0f + 28.0f * y->clap; /* at most 90: the strip stays on the screen */
        uint8_t      *px = c->pixels;
        for (int32_t x = 0; x < DEMO_WIDTH; ++x) {
                const int32_t u = x + (int32_t)s.scroll;
                if (u < 0 || u >= s.width) continue;
                const int32_t  y0 = base + (int32_t)(height * dsin((float)x * bend + t * 2.4f));
                const uint8_t *col = s.strip + u;
                uint8_t       *out = px + (size_t)y0 * c->stride + x;
                for (int32_t r = 0; r < STRIP_HEIGHT; ++r, col += s.width, out += c->stride)
                        if (*col != 0) *out = (uint8_t)(TEXT_FIRST + r);
        }
        /* The text's band cycles. */
        pal_rotate(&demo->pal, TEXT_FIRST, TEXT_COUNT, 1 + 6 * (int32_t)y->kicks);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_copper = {"Copper and scroller", init, enter, frame, event};
