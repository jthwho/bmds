/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck (devplan/luckyduck.md): the casino. One 1280×720 surface for the whole run, the loop
 * forwarding to the scene playing (the lobby or a game); between scenes a line of ducklings
 * waddles across, the new scene drawn behind them; leaving a game under way asks first.
 *
 *     luckyduck [GAME [-W]]     GAME: solitaire (-W: a game played out)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/app.h>
#include "ui.h"

app_t app;

static const scene_t *const SCENES[] = {&lobby_scene, &solitaire_scene, &blackjack_scene};

static const char *const SOUND_FILES[SFX_COUNT] = {"place.wav",  "flip.wav",   "deal.wav",  "win.wav", "quack.wav",
                                                   "quack2.wav", "quacks.wav", "click.wav", "sad.wav"};

enum {
        BOX_NONE,
        BOX_CONFIRM,
        BOX_INFO
};

static struct {
                const scene_t     *scene;
                bmds_game_audio_t *audio;
                bmds_game_sound_t  sfx[SFX_COUNT];
                int                sound;
                int                full_redraw;
                /* The transition: the new scene shown left of `edge`, the ducklings leading. */
                int         moving;
                int32_t     edge, shown_edge;
                uint32_t    move_ticks;
                bmds_rect_t ducklings[5], shown_ducklings[5];
                int         duckling_frame[5];
                int         ducklings_shown;
                /* Where to start, once the surface says its size. */
                const scene_t *start;
                int            start_argc;
                char         **start_argv;
                /* The box up: a question or something to read. */
                int         box;
                char        box_title[40], box_line[96];
                const char *box_yes;
                void (*box_yes_fn)(void);
                const char *const *box_lines;
                int                box_count, box_shown, box_hover, box_hover_shown;
} m;

/* ---- Sound */

void sfx(int which, float volume) {
        if (m.audio != NULL && m.sound && m.sfx[which].samples != NULL)
                (void)bmds_game_play(m.audio, &m.sfx[which], volume, 0.0f, 0);
        return;
}

int sound_on(void) {
        return m.sound;
}

void sound_toggle(void) {
        m.sound = !m.sound;
        set_setting("sound", m.sound);
        return;
}

/* ---- Rectangles */

bmds_rect_t rect(int32_t x, int32_t y, int32_t w, int32_t h) {
        const bmds_rect_t r = {x, y, (uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)};
        return r;
}

int meets(const bmds_rect_t *a, const bmds_rect_t *b) {
        return a->width > 0 && b->width > 0 && a->height > 0 && b->height > 0 && a->x < b->x + (int32_t)b->width &&
               b->x < a->x + (int32_t)a->width && a->y < b->y + (int32_t)b->height && b->y < a->y + (int32_t)a->height;
}

bmds_rect_t bounds(const bmds_rect_t *a, const bmds_rect_t *b) {
        if (a->width == 0 || a->height == 0) return *b;
        if (b->width == 0 || b->height == 0) return *a;
        const int32_t x0 = a->x < b->x ? a->x : b->x, y0 = a->y < b->y ? a->y : b->y;
        const int32_t ax1 = a->x + (int32_t)a->width, bx1 = b->x + (int32_t)b->width;
        const int32_t ay1 = a->y + (int32_t)a->height, by1 = b->y + (int32_t)b->height;
        return rect(x0, y0, (ax1 > bx1 ? ax1 : bx1) - x0, (ay1 > by1 ? ay1 : by1) - y0);
}

int contains(const bmds_rect_t *r, int32_t x, int32_t y) {
        return x >= r->x && y >= r->y && x < r->x + (int32_t)r->width && y < r->y + (int32_t)r->height;
}

bmds_rect_t intersection(bmds_rect_t a, bmds_rect_t b) {
        if (!meets(&a, &b)) return rect(0, 0, 0, 0);
        const int32_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
        const int32_t ax1 = a.x + (int32_t)a.width, bx1 = b.x + (int32_t)b.width;
        const int32_t ay1 = a.y + (int32_t)a.height, by1 = b.y + (int32_t)b.height;
        return rect(x0, y0, (ax1 < bx1 ? ax1 : bx1) - x0, (ay1 < by1 ? ay1 : by1) - y0);
}

int same_rect(const bmds_rect_t *a, const bmds_rect_t *b) {
        return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

/* ---- What to redraw */

static int64_t area(const bmds_rect_t *r) {
        return (int64_t)r->width * r->height;
}

/* The area @p a and @p b cover together. */
static int64_t covered(const bmds_rect_t *a, const bmds_rect_t *b) {
        int64_t overlap = 0;
        if (meets(a, b)) {
                const int32_t x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
                const int32_t ax1 = a->x + (int32_t)a->width, bx1 = b->x + (int32_t)b->width;
                const int32_t ay1 = a->y + (int32_t)a->height, by1 = b->y + (int32_t)b->height;
                overlap = (int64_t)((ax1 < bx1 ? ax1 : bx1) - x0) * ((ay1 < by1 ? ay1 : by1) - y0);
        }
        return area(a) + area(b) - overlap;
}

void dirty_add(bmds_rect_t *list, uint32_t *count, uint32_t max, bmds_rect_t r) {
        if (r.width == 0 || r.height == 0) return;
        /* Merged with one it meets when their bounds add at most a quarter to what they cover:
           merging whatever meets chains into one rectangle around everything (a deal's flying
           cards redrawing the whole table). */
        for (uint32_t i = 0; i < *count; ++i) {
                const bmds_rect_t b = bounds(&list[i], &r);
                if (!meets(&list[i], &r) || area(&b) * 4 > covered(&list[i], &r) * 5) continue;
                r = b;
                list[i] = list[--*count];
                i = (uint32_t)-1; /* the bigger one may merge with others now */
        }
        if (*count < max) {
                list[(*count)++] = r;
                return;
        }
        /* Full: the two (the new one among them) whose bounds add the least, merged. */
        uint32_t pa = 0, pb = max;
        int64_t  best = INT64_MAX;
        for (uint32_t a = 0; a <= max; ++a) {
                for (uint32_t b = a + 1; b <= max; ++b) {
                        const bmds_rect_t *x = a < max ? &list[a] : &r, *y = b < max ? &list[b] : &r;
                        const bmds_rect_t  u = bounds(x, y);
                        const int64_t      added = area(&u) - covered(x, y);
                        if (added < best) best = added, pa = a, pb = b;
                }
        }
        const bmds_rect_t merged = bounds(&list[pa], pb < max ? &list[pb] : &r);
        if (pb < max) {
                list[pa] = merged;
                list[pb] = r;
        } else {
                list[pa] = merged;
        }
        return;
}

/* ---- The felt */

void felt_restore(bmds_canvas_t *c, bmds_rect_t r) {
        bmds_canvas_blit(c, r.x, r.y, &app.felt, &r);
        return;
}

/* The felt's shading: an ordered dither (smooth shades in RGB565 without bands) and a little
   grain (felt isn't flat), together a 64×64 tile of one of 128 "texels" (8 grains × 16
   thresholds); a shade by its distance from the middle (65 steps). A finished RGB565 pixel for
   each shade and texel is a table made first, so a pixel is three lookups: PSRAM, not arithmetic,
   is then what a felt costs (devplan/luckyduck.md §10). */

#define SHADES 65 /* the distance in 1/64ths of the reach, 0 to 64 */
#define TEXELS 128

static struct {
                uint8_t texel[64][64];
                int     made;
} tex;

static void make_texture(void) {
        static const uint8_t m4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        if (tex.made) return;
        for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                        uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
                        h ^= h >> 13;
                        h *= 0x5bd1e995u;
                        tex.texel[y][x] = (uint8_t)(((h >> 24) & 7) * 16 + m4[(y & 3) * 4 + (x & 3)]);
                }
        }
        tex.made = 1;
        return;
}

static int32_t channel(uint32_t rgb, int shift) {
        return (int32_t)((rgb >> shift) & 0xffu);
}

/* 8-bit @p r, @p g, @p b with texel @p texel's grain and dither, to RGB565. */
static uint16_t pixel565(int32_t r, int32_t g, int32_t b, uint32_t texel) {
        const int32_t grain = (int32_t)(texel / 16) - 3, t = (int32_t)(texel % 16);
        r += grain;
        g += grain;
        b += grain;
        r = ((r < 0 ? 0 : r > 255 ? 255 : r) + t / 2) >> 3;
        g = ((g < 0 ? 0 : g > 255 ? 255 : g) + t / 4) >> 2;
        b = ((b < 0 ? 0 : b > 255 ? 255 : b) + t / 2) >> 3;
        return (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

/* Each pixel's shade: its distance from the middle, @p wide and @p high percent of the picture
   the reach, squared, in 1/64ths (across + down). */
static void reach(uint8_t *across, uint8_t *down, int wide, int high) {
        for (int32_t x = 0; x < WIDTH; ++x) {
                const int32_t d = (x - WIDTH / 2) * 64 / (WIDTH * wide / 100);
                across[x] = (uint8_t)(d * d / 64 > 64 ? 64 : d * d / 64);
        }
        for (int32_t y = 0; y < HEIGHT; ++y) {
                const int32_t d = (y - HEIGHT / 2) * 64 / (HEIGHT * high / 100);
                down[y] = (uint8_t)(d * d / 64 > 64 ? 64 : d * d / 64);
        }
        return;
}

static uint16_t look[4][SHADES][TEXELS]; /* the carpet's four colours; the baize uses the first */

void felt_baize(uint32_t middle, uint32_t edge) {
        static uint8_t across[MAX_WIDTH], down[MAX_HEIGHT];
        make_texture();
        reach(across, down, 62, 72);
        for (int t = 0; t < SHADES; ++t) {
                int32_t c[3];
                for (int i = 0; i < 3; ++i) {
                        const int32_t a = channel(middle, 16 - 8 * i), b = channel(edge, 16 - 8 * i);
                        c[i] = a + (b - a) * t / (SHADES - 1);
                }
                for (uint32_t k = 0; k < TEXELS; ++k) look[0][t][k] = pixel565(c[0], c[1], c[2], k);
        }
        uint16_t *px = (uint16_t *)(void *)app.felt.pixels;
        for (int32_t y = 0; y < HEIGHT; ++y) {
                const uint8_t *k = tex.texel[y & 63];
                const uint8_t  dy = down[y];
                for (int32_t x = 0; x < WIDTH; ++x) {
                        const uint32_t t = (uint32_t)across[x] + dy;
                        *px++ = look[0][t > SHADES - 1 ? SHADES - 1 : t][k[x & 63]];
                }
        }
        return;
}

void felt_carpet(void) {
        static uint8_t        across[MAX_WIDTH], down[MAX_HEIGHT];
        static uint8_t        pattern[72][72];
        static const uint32_t colours[4] = {0x7a1420, 0xb08028, 0xd0a040, 0x5a0c16};
        make_texture();
        reach(across, down, 70, 80);
        for (int c = 0; c < 4; ++c)
                for (int t = 0; t < SHADES; ++t) {
                        const int32_t k = 256 - t * 154 / (SHADES - 1); /* darker towards the walls */
                        for (uint32_t x = 0; x < TEXELS; ++x)
                                look[c][t][x] =
                                        pixel565(channel(colours[c], 16) * k / 256, channel(colours[c], 8) * k / 256,
                                                 channel(colours[c], 0) * k / 256, x);
                }
        /* Diamonds 72 across (repeating every 72 pixels each way): a gold line, a rose in each. */
        for (int y = 0; y < 72; ++y)
                for (int x = 0; x < 72; ++x) {
                        const int u = (x + y) % 72, v = (x - y + 72) % 72;
                        const int r = (u - 36) * (u - 36) + (v - 36) * (v - 36);
                        pattern[y][x] = (uint8_t)(u < 2 || v < 2 ? 1 : r < 40 ? 2 : r < 120 ? 3 : 0);
                }
        uint16_t *px = (uint16_t *)(void *)app.felt.pixels;
        for (int32_t y = 0; y < HEIGHT; ++y) {
                const uint8_t *k = tex.texel[y & 63];
                const uint8_t *p = pattern[y % 72];
                const uint8_t  dy = down[y];
                int32_t        px72 = 0;
                for (int32_t x = 0; x < WIDTH; ++x) {
                        const uint32_t t = (uint32_t)across[x] + dy;
                        *px++ = look[p[px72]][t > SHADES - 1 ? SHADES - 1 : t][k[x & 63]];
                        if (++px72 == 72) px72 = 0;
                }
        }
        return;
}

/* ---- Scenes */

static void enter(const scene_t *scene, int argc, char **argv) {
        if (scene->enter(argc, argv) != 0 && scene != &lobby_scene) {
                scene = &lobby_scene;
                (void)scene->enter(0, NULL);
        }
        m.scene = scene;
        return;
}

void app_switch(const scene_t *scene, int argc, char **argv) {
        if (m.scene != NULL && m.scene->leave != NULL) m.scene->leave();
        m.box = BOX_NONE;
        enter(scene, argc, argv);
        m.moving = 1;
        m.edge = m.shown_edge = 0;
        m.move_ticks = 0;
        m.ducklings_shown = 0;
        sfx(SFX_QUACKS, 0.5f);
        return;
}

static void to_lobby(void) {
        app_switch(&lobby_scene, 0, NULL);
        return;
}

void app_leave(void) {
        if (m.scene == &lobby_scene) return;
        if (m.scene->busy != NULL && m.scene->busy()) {
                app_confirm("LEAVE THE TABLE?", "The game under way ends if you go back to the lobby.", "LEAVE",
                            to_lobby);
                return;
        }
        to_lobby();
        return;
}

bmds_rect_t app_system_button(int i) {
        return rect(WIDTH - 158, 10 + i * 42, 146, 36);
}

int app_asking(void) {
        return m.box != BOX_NONE;
}

/* ---- The boxes: a question, or something to read (rules, help) */

#define INFO_LINES 22

void app_confirm(const char *title, const char *line, const char *yes, void (*yes_fn)(void)) {
        m.box = BOX_CONFIRM;
        snprintf(m.box_title, sizeof(m.box_title), "%s", title);
        snprintf(m.box_line, sizeof(m.box_line), "%s", line);
        m.box_yes = yes;
        m.box_yes_fn = yes_fn;
        m.box_shown = 0;
        m.box_hover = -1;
        return;
}

void app_info(const char *title, const char *const *lines, int count) {
        m.box = BOX_INFO;
        snprintf(m.box_title, sizeof(m.box_title), "%s", title);
        m.box_lines = lines;
        m.box_count = count < INFO_LINES ? count : INFO_LINES;
        m.box_shown = 0;
        m.box_hover = -1;
        return;
}

static bmds_rect_t box_rect(void) {
        if (m.box == BOX_INFO) {
                const int32_t h = 150 + m.box_count * 20;
                return rect(WIDTH / 2 - 330, HEIGHT / 2 - h / 2, 660, h);
        }
        return rect(WIDTH / 2 - 300, HEIGHT / 2 - 120, 600, 240);
}

/* The box's buttons: its answer and "keep playing", or "close". */
static int box_buttons(ui_button_t *out) {
        const bmds_rect_t r = box_rect();
        const int32_t     y = r.y + (int32_t)r.height - 66;
        if (m.box == BOX_INFO) {
                const ui_button_t close = {{WIDTH / 2 - 80, y, 160, 40}, "CLOSE", K_ESCAPE};
                out[0] = close;
                return 1;
        }
        const ui_button_t yes = {{WIDTH / 2 - 240, y, 220, 40}, m.box_yes, K_Y};
        const ui_button_t no = {{WIDTH / 2 + 20, y, 220, 40}, "KEEP PLAYING", K_ESCAPE};
        out[0] = yes;
        out[1] = no;
        return 2;
}

static void box_answer(int yes) {
        void (*fn)(void) = m.box == BOX_CONFIRM && yes ? m.box_yes_fn : NULL;
        m.box = BOX_NONE;
        sfx(SFX_CLICK, 0.6f);
        m.full_redraw = 1;
        if (fn != NULL) fn();
        return;
}

static void box_event(const bmds_surface_event_t *e) {
        ui_button_t b[2];
        const int   n = box_buttons(b);
        if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                const uint32_t usage = e->data[0];
                if (m.box == BOX_INFO) {
                        if (usage == K_ESCAPE || usage == K_ENTER || usage == K_KP_ENTER || usage == K_SPACE)
                                box_answer(0);
                } else if (usage == K_Y) { /* only Y ends the game: a key hit by chance keeps playing */
                        box_answer(1);
                } else if (usage == K_N || usage == K_ESCAPE || usage == K_ENTER || usage == K_KP_ENTER ||
                           usage == K_SPACE) {
                        box_answer(0);
                }
        } else if (e->type == BMDS_EVENT_POINTER) {
                const uint32_t pressed = e->data[2] & ~app.buttons;
                m.box_hover = ui_button_at(b, n, (int32_t)e->data[0], (int32_t)e->data[1]);
                if ((pressed & 1u) != 0 && m.box_hover >= 0) box_answer(m.box_hover == 0 && m.box == BOX_CONFIRM);
        }
        return;
}

static void box_draw(bmds_canvas_t *c) {
        ui_button_t b[2];
        const int   n = box_buttons(b);
        if (!m.box_shown) {
                bmds_canvas_fill_blend(c, 0, 0, WIDTH, HEIGHT, 0x000000, BMDS_GAME_OPAQUE / 2);
                const bmds_rect_t r = box_rect();
                ui_panel(c, r);
                ui_text_centred(c, ART_FONT_MED, WIDTH / 2, r.y + 28, m.box_title);
                if (m.box == BOX_INFO) {
                        for (int i = 0; i < m.box_count; ++i)
                                ui_line(c, r.x + 44, r.y + 80 + i * 20, m.box_lines[i], C_TEXT);
                } else {
                        ui_line(c, WIDTH / 2 - (int32_t)strlen(m.box_line) * 4, r.y + 92, m.box_line, C_TEXT);
                }
                m.box_shown = 1;
                m.box_hover_shown = -2;
        }
        if (m.box_hover != m.box_hover_shown) {
                /* Lit: the one under the pointer, else the safe one (keep playing, close): Enter's. */
                const int lit = m.box_hover >= 0 ? m.box_hover : n - 1;
                for (int i = 0; i < n; ++i) ui_button(c, &b[i], lit == i ? UI_HOVER : UI_NORMAL);
                m.box_hover_shown = m.box_hover;
        }
        return;
}

/* ---- The transition */

#define MOVE_STEP 22 /* pixels a tick */
#define DUCKLING 56

static void move_tick(void) {
        if (!m.moving) return;
        ++m.move_ticks;
        m.edge += MOVE_STEP;
        for (int i = 0; i < 5; ++i) {
                m.ducklings[i] =
                        rect(m.edge - DUCKLING - (i % 2) * 18, 40 + i * 136 + (i % 2) * 10, DUCKLING, DUCKLING);
                m.duckling_frame[i] = ART_DUCKLING_WALK0 + (int)((m.move_ticks / 4 + (uint32_t)i) % 4);
        }
        return;
}

static void move_draw(bmds_canvas_t *c) {
        const int32_t right = m.edge < WIDTH ? m.edge : WIDTH;
        bmds_canvas_t sub;
        if (right <= 0 || bmds_canvas_init(&sub, c->pixels, c->stride, right, HEIGHT, c->format) != 0) return;
        /* The scene as usual, clipped to what is shown of it; what the ducklings uncovered; where
           they were; then they, a step on. */
        m.scene->draw(&sub);
        if (right > m.shown_edge) m.scene->redraw(&sub, rect(m.shown_edge, 0, right - m.shown_edge, HEIGHT));
        if (m.ducklings_shown)
                for (int i = 0; i < 5; ++i) m.scene->redraw(&sub, m.shown_ducklings[i]);
        if (m.edge < WIDTH + 2 * DUCKLING) {
                for (int i = 0; i < 5; ++i) {
                        ui_sprite(&sub, m.ducklings[i].x, m.ducklings[i].y, m.duckling_frame[i], 0);
                        m.shown_ducklings[i] = m.ducklings[i];
                }
                m.ducklings_shown = 1;
        } else {
                m.moving = 0;
        }
        m.shown_edge = right;
        for (uint32_t i = 0; i < sub.damage_count; ++i) bmds_canvas_damage(c, &sub.damage[i]);
        return;
}

/* ---- The loop */

static void quit(void) {
        bmds_game_quit(app.game);
        return;
}

/* The first callback: the picture's size known (a maximized window's), the felt fitted to it and
   the first scene entered. */
static void begin(bmds_game_t *game) {
        if (m.scene != NULL) return;
        app.game = game;
        app.width = (int32_t)bmds_game_width(game);
        app.height = (int32_t)bmds_game_height(game);
        app.felt.stride = (uint32_t)WIDTH * 2;
        app.felt.width = WIDTH;
        app.felt.height = HEIGHT;
        (void)bmds_canvas_init(&app.felt_canvas, (void *)app.felt.pixels, app.felt.stride, WIDTH, HEIGHT,
                               BMDS_PIXEL_RGB565);
        enter(m.start, m.start_argc, m.start_argv);
        m.full_redraw = 1;
        return;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        int64_t now = 0;
        begin(game);
        switch (e->type) {
                case BMDS_EVENT_CLOSE: /* the window's close: a game under way asks first */
                        if (m.scene->busy != NULL && m.scene->busy() && m.box == BOX_NONE)
                                app_confirm("QUIT LUCKY DUCK?", "The game under way ends if you quit.", "QUIT", quit);
                        else
                                bmds_game_quit(game);
                        return;
                case BMDS_EVENT_PRESENTED: m.full_redraw = 1; return;
                case BMDS_EVENT_ACTIVE:
                        if (m.audio != NULL) bmds_game_audio_pause(m.audio, e->data[0] == 0);
                        break;
                case BMDS_EVENT_KEY:
                case BMDS_EVENT_POINTER:
                case BMDS_EVENT_WHEEL:
                        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
                        app.idle_ns = now;
                        break;
                default: break;
        }
        if (m.box != BOX_NONE) {
                box_event(e);
        } else if (!m.moving || e->type != BMDS_EVENT_POINTER || (e->data[2] & ~app.buttons) == 0) {
                m.scene->event(e); /* a click while the ducklings cross is dropped */
        }
        if (e->type == BMDS_EVENT_POINTER) {
                app.px = (int32_t)e->data[0];
                app.py = (int32_t)e->data[1];
                app.buttons = e->data[2];
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        begin(game);
        move_tick();
        if (m.box == BOX_NONE && !m.moving) m.scene->tick(); /* a still picture to uncover: cheaper */
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        begin(game);
        if (m.full_redraw) {
                m.full_redraw = 0;
                m.moving = 0;
                m.box_shown = 0;
                m.scene->redraw(c, rect(0, 0, WIDTH, HEIGHT));
        }
        if (m.box != BOX_NONE) {
                box_draw(c);
                return;
        }
        if (m.moving)
                move_draw(c);
        else
                m.scene->draw(c);
        return;
}

/* ---- Start */

static int load_image(const char *dir, const char *name, bmds_game_image_t *image) {
        char                   path[300];
        const bmds_game_load_t how = {sizeof(how), BMDS_PIXEL_RGB565, 0, 0, NULL, 0, 0, 0};
        const int              n = snprintf(path, sizeof(path), "%s/%s", dir, name);
        int err = n >= 0 && (size_t)n < sizeof(path) ? bmds_game_image_load(path, &how, image) : -BMDS_ENAMETOOLONG;
        if (err < 0) fprintf(stderr, "luckyduck: %s: %s\n", path, strerror(-err));
        return err;
}

static void usage(void) {
        fprintf(stderr, "usage: luckyduck [GAME [-W]]\n  GAME:");
        for (size_t i = 0; i < sizeof(SCENES) / sizeof(SCENES[0]); ++i) fprintf(stderr, " %s", SCENES[i]->name);
        fprintf(stderr, "\n");
        return;
}

int main(int argc, char **argv) {
        int            first = 1;
        const scene_t *start = &lobby_scene;
        if (first < argc) {
                start = NULL;
                for (size_t i = 0; i < sizeof(SCENES) / sizeof(SCENES[0]); ++i)
                        if (strcmp(argv[first], SCENES[i]->name) == 0) start = SCENES[i];
                if (start == NULL) {
                        usage();
                        return 2;
                }
        }
        char        dir[256];
        const char *app_dir = bmds_app_dir();
        if (app_dir != NULL) {
                snprintf(dir, sizeof(dir), "%s", app_dir);
        } else {
                snprintf(dir, sizeof(dir), "%s", argv[0]);
                char *slash = strrchr(dir, '/');
                if (slash != NULL)
                        *slash = '\0';
                else
                        snprintf(dir, sizeof(dir), ".");
        }
        if (load_image(dir, "cards.png", &app.cards) < 0 || load_image(dir, "sprites.png", &app.sprites) < 0 ||
            load_image(dir, "font.png", &app.font) < 0)
                return 1;
        /* The felt: a picture the scenes paint, under every redraw (fitted to the picture by begin()). */
        void *felt = bmds_mem_alloc((uint32_t)MAX_WIDTH * MAX_HEIGHT * 2, 16, 0);
        if (felt == NULL) {
                fprintf(stderr, "luckyduck: out of memory\n");
                return 1;
        }
        app.felt.pixels = felt;
        app.felt.format = BMDS_PIXEL_RGB565;
        m.sound = setting("sound", 1) != 0;
        if (bmds_game_audio_open(NULL, &m.audio) == 0) {
                for (int i = 0; i < SFX_COUNT; ++i) {
                        char path[300];
                        if (snprintf(path, sizeof(path), "%s/%s", dir, SOUND_FILES[i]) >= (int)sizeof(path) ||
                            bmds_game_sound_load(m.audio, path, &m.sfx[i]) < 0)
                                fprintf(stderr, "luckyduck: %s: not loaded\n", path);
                }
        }
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        srand((unsigned)ns ^ (unsigned)(ns >> 32));
        app.idle_ns = ns;
        m.start = start;
        m.start_argc = argc - first - 1;
        m.start_argv = argv + first + 1;

        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = 0; /* a maximized window's */
        config.height = 0;
        config.format = BMDS_PIXEL_RGB565;
        config.flags = BMDS_SURFACE_MAXIMIZED;
        config.title = "Lucky Duck";
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        const int err = bmds_game_run(&config);
        if (m.scene != NULL && m.scene->leave != NULL) m.scene->leave();
        bmds_game_audio_close(m.audio);
        for (int i = 0; i < SFX_COUNT; ++i) bmds_game_sound_free(&m.sfx[i]);
        bmds_game_image_free(&app.cards);
        bmds_game_image_free(&app.sprites);
        bmds_game_image_free(&app.font);
        bmds_mem_free(felt);
        if (err < 0) {
                fprintf(stderr, "luckyduck: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
