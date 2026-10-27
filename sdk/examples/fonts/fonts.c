/**
 * @file      fonts.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's fonts (devplan/fonts.md §4e): `fonts -l` lists the installed faces; `fonts` opens
 * a window showing each face in itself, a sample at several sizes, smoothed and sharp, over the
 * picture and on a box. Escape or closing the window quits.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>

#define WIDTH 640
#define HEIGHT 420

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_CLOSE || (e->type == BMDS_EVENT_KEY && e->data[0] == 0x00070029u))
                bmds_game_quit(game); /* closed, or Escape */
}

/* One line in @p name at (@p x, @p y) with @p flags (BMDS_TEXT_...), on @p background if opaque. */
static int32_t line(bmds_canvas_t *c, const char *name, int32_t x, int32_t y, const char *text, uint32_t rgb,
                    uint32_t flags, uint32_t background) {
        const int32_t font = bmds_game_font(name);
        if (font < 0) return 0;
        bmds_text_draw_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.font = font;
        d.pixels = c->pixels;
        d.format = c->format;
        d.width = (uint32_t)c->width;
        d.height = (uint32_t)c->height;
        d.stride = c->stride;
        d.x = x;
        d.y = y;
        d.color = rgb;
        d.background = background;
        d.flags = flags;
        d.text = text;
        d.length = (uint32_t)strlen(text);
        return bmds_text_draw(&d);
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        int *drawn = bmds_game_arg(game);
        if (*drawn) return;
        *drawn = 1;
        bmds_canvas_clear(c, bmds_canvas_pixel(c, 0xf4f4f0));
        int32_t y = 8;
        /* Each face, named in itself. */
        bmds_font_face_t face = {sizeof(face), 0, 0, 0, 0, 0, {0}, {0}, {0}};
        for (uint32_t i = 0; bmds_font_list(i, &face) == 0 && y < 200; ++i) {
                char name[BMDS_FONT_NAME_MAX];
                snprintf(name, sizeof(name), "%s %s%s%s", face.family, face.bitmap_size != 0 ? "16" : "18",
                         (face.style & BMDS_FONT_BOLD) != 0 ? " bold" : "",
                         (face.style & BMDS_FONT_ITALIC) != 0 ? " italic" : "");
                char text[160];
                snprintf(text, sizeof(text), "%s %s: the quick brown fox, 0123", face.family, face.style_name);
                line(c, name, 10, y, text, 0x202020, 0, 0);
                y += 26;
        }
        /* One face at several sizes. */
        static const char *const sizes[] = {"IBM Plex Sans 11", "IBM Plex Sans 13", "IBM Plex Sans 16",
                                            "IBM Plex Sans 24", "IBM Plex Serif 32"};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
                const int32_t font = bmds_game_font(sizes[i]);
                line(c, sizes[i], 10, y, sizes[i], 0x103060, 0, 0);
                y += bmds_game_line_height(font);
        }
        /* Smoothed and sharp; over the picture and on a box. */
        y += 6;
        const int32_t w =
                line(c, "IBM Plex Sans 13", 10, y, "Smoothed (font/smooth's way)", 0x000000, BMDS_TEXT_SMOOTH, 0);
        line(c, "IBM Plex Sans 13", 20 + w, y, "Sharp, one bit a pixel", 0x000000, BMDS_TEXT_SHARP, 0);
        y += 24;
        bmds_canvas_fill(c, 0, y - 4, WIDTH, 30, bmds_canvas_pixel(c, 0x3060a0));
        line(c, "IBM Plex Sans 16 bold", 10, y, "White over blue", 0xffffff, 0, 0);
        line(c, "IBM Plex Sans 16", 220, y, "On a yellow box", 0x000000, BMDS_TEXT_OPAQUE, 0xffe060);
        line(c, "unscii 16", 420, y, "unscii, too", 0xffffff, 0, 0);
}

int main(int argc, char **argv) {
        if (argc == 2 && strcmp(argv[1], "-l") == 0) {
                bmds_font_face_t face = {sizeof(face), 0, 0, 0, 0, 0, {0}, {0}, {0}};
                for (uint32_t i = 0; bmds_font_list(i, &face) == 0; ++i)
                        printf("%2u  %s %s  weight %u%s%s  %s\n", (unsigned)i, face.family, face.style_name,
                               (unsigned)face.weight, (face.flags & BMDS_FONT_FIXED) != 0 ? ", fixed" : "",
                               (face.flags & BMDS_FONT_BITMAP) != 0 ? ", bitmap" : "", face.path);
                return 0;
        }
        if (argc != 1) {
                fprintf(stderr, "usage: fonts [-l]\n");
                return 2;
        }
        static int         drawn;
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = WIDTH;
        config.height = HEIGHT;
        config.format = BMDS_PIXEL_BGR24;
        config.title = "Fonts";
        config.event = on_event;
        config.draw = on_draw;
        config.arg = &drawn;
        return bmds_game_run(&config);
}
