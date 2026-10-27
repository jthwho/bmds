/**
 * @file      text.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's text (devplan/games.md §13, devplan/fonts.md §4e): the system's fonts, drawn by
 * the OS into the canvas (text_draw), a line at a time; unscii 16 when no font is given.
 */

#include <string.h>
#include "game_internal.h"

/* The fonts opened by name, kept for the program's life (the OS closes them at its end): at most
   the program's 16 font handles. */
#define FONTS_KEPT 16
#define FONT_NAME_KEPT 40

static struct {
                char    name[FONT_NAME_KEPT];
                int32_t fd;
} kept[FONTS_KEPT];
static uint32_t kept_count;

int32_t bmds_game_font(const char *name) {
        if (name == NULL) return -BMDS_EINVAL;
        const size_t n = strlen(name);
        for (uint32_t i = 0; i < kept_count; ++i)
                if (strcmp(kept[i].name, name) == 0) return kept[i].fd;
        const int32_t fd = bmds_font_open_name(name);
        if (fd < 0) return fd;
        if (kept_count < FONTS_KEPT && n < FONT_NAME_KEPT) {
                memcpy(kept[kept_count].name, name, n + 1);
                kept[kept_count].fd = fd;
                kept_count++;
        }
        return fd;
}

int32_t bmds_game_font_scaled(int32_t scale) {
        static const char digits[] = "0123456789";
        char              name[16] = "unscii ";
        if (scale < 1) scale = 1;
        if (scale > 15) scale = 15;
        uint32_t size = (uint32_t)scale * 16u, at = 7;
        if (size >= 100) name[at++] = digits[size / 100];
        if (size >= 10) name[at++] = digits[size / 10 % 10];
        name[at++] = digits[size % 10];
        name[at] = 0;
        return bmds_game_font(name);
}

/* @p font, or unscii 16 for a negative one. */
static int32_t font_or_default(int32_t font) {
        return font >= 0 ? font : bmds_game_font("unscii 16");
}

/* The bytes of @p text's line starting at @p at (to a newline or the end). */
static uint32_t line_length(const char *at) {
        const char *end = strchr(at, '\n');
        return end != NULL ? (uint32_t)(end - at) : (uint32_t)strlen(at);
}

int32_t bmds_game_line_height(int32_t font) {
        bmds_font_info_t info = {sizeof(bmds_font_info_t), 0, 0, 0, 0, 0, 0, 0, 0, {0}};
        const int32_t    fd = font_or_default(font);
        if (fd < 0 || bmds_font_info(fd, &info) < 0) return BMDS_GAME_GLYPH_HEIGHT;
        return info.line_height;
}

int32_t bmds_game_text_width(int32_t font, const char *text) {
        const int32_t fd = font_or_default(font);
        int32_t       widest = 0;
        if (fd < 0) return 0;
        for (const char *at = text;;) {
                const uint32_t     n = line_length(at);
                bmds_text_extent_t e = {sizeof(bmds_text_extent_t), 0, 0, 0};
                if (n > 0 && bmds_text_measure(fd, at, n, 0, &e) == 0 && (int32_t)e.width > widest)
                        widest = (int32_t)e.width;
                if (at[n] == 0) break;
                at += n + 1;
        }
        return widest;
}

int32_t bmds_canvas_text(bmds_canvas_t *canvas, int32_t font, int32_t x, int32_t y, const char *text, uint32_t pixel) {
        return bmds_canvas_text_flags(canvas, font, x, y, text, pixel, 0, 0);
}

int32_t bmds_canvas_text_flags(bmds_canvas_t *canvas, int32_t font, int32_t x, int32_t y, const char *text,
                               uint32_t pixel, uint32_t background, uint32_t flags) {
        const int32_t fd = font_or_default(font);
        if (fd < 0) return 0;
        const int32_t    line = bmds_game_line_height(fd);
        bmds_text_draw_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.font = fd;
        d.pixels = canvas->pixels;
        d.format = canvas->format;
        d.width = (uint32_t)canvas->width;
        d.height = (uint32_t)canvas->height;
        d.stride = canvas->stride;
        d.clip_x = canvas->clip.x;
        d.clip_y = canvas->clip.y;
        d.clip_width = canvas->clip.width;
        d.clip_height = canvas->clip.height;
        d.color = bmds_canvas_rgb(canvas, pixel);
        d.background = bmds_canvas_rgb(canvas, background);
        d.flags = flags;
        int32_t widest = 0, cy = y;
        for (const char *at = text;;) {
                const uint32_t n = line_length(at);
                if (n > 0 && canvas->clip.width != 0 && canvas->clip.height != 0) {
                        d.x = x;
                        d.y = cy;
                        d.text = at;
                        d.length = n;
                        const int32_t w = bmds_text_draw(&d);
                        if (w > widest) widest = w;
                }
                if (at[n] == 0) break;
                at += n + 1;
                cy += line;
        }
        if (widest > 0) {
                /* Smoothed ink may reach a pixel or two past the advance (a bearing, an overhang). */
                const bmds_rect_t r = {x - 2, y, (uint32_t)widest + 4, (uint32_t)(cy - y + line)};
                bmds_game_damage_in_clip(canvas, &r);
        }
        return widest;
}
