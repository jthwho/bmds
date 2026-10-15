/**
 * @file      blocks.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Blocks (devplan/games.md §17): the guideline's falling-blocks game on libbmdsgame. The rules
 * are rules.c's; this is the picture (640×360 RGB565, tiles from the folder's tiles.png, each
 * frame drawing only the cells and figures that changed), the keys, the sounds and music, the
 * title, pause and game-over screens, and the high scores (in the data folder).
 *
 *     blocks [-F]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/app.h>
#include <bmds/game.h>
#include "rules.h"

#define WIDTH 640
#define HEIGHT 360
#define CELL 16
#define ROWS (BLOCKS_HEIGHT - BLOCKS_HIDDEN)
#define FIELD_X ((WIDTH - BLOCKS_WIDTH * CELL) / 2)
#define FIELD_Y 20
#define LEFT_X 112
#define RIGHT_X 432
#define PANEL_W 96

/* Tiles in tiles.png. */
#define TILE_EMPTY 0
#define TILE_FLASH 8
#define TILE_GHOST 8 /* + the piece */
#define TILES 16

#define BACKGROUND 0x0b1020
#define FRAME 0x3a4560
#define LABEL 0x8090b0
#define VALUE 0xf0f4ff
#define BOX 0x161e33
#define ACCENT 0xf0d000

#define SCORES 10
#define NAME_MAX 12

/* HID keys (page 7). */
#define KEY(u) (0x00070000u | (u))
#define K_ENTER KEY(0x28)
#define K_ESCAPE KEY(0x29)
#define K_BACKSPACE KEY(0x2a)
#define K_KP_ENTER KEY(0x58)

/* The actions' Ids (controls.conf). */
enum {
        A_LEFT = 1,
        A_RIGHT,
        A_DROP,
        A_TURN_LEFT,
        A_TURN_RIGHT,
        A_HOLD,
        A_PAUSE,
        A_RESUME,
        A_GIVE_UP,
        A_PLAY,
        A_QUIT,
        A_MUSIC
};

enum {
        TITLE,
        PLAYING,
        PAUSED,
        OVER
};

enum {
        S_MOVE,
        S_TURN,
        S_LOCK,
        S_DROP,
        S_HOLD,
        S_CLEAR,
        S_FOUR,
        S_LEVEL,
        S_OVER,
        SOUNDS
};
static const char *const SOUND_FILES[SOUNDS] = {"move.wav",  "turn.wav", "lock.wav",  "drop.wav", "hold.wav",
                                                "clear.wav", "four.wav", "level.wav", "over.wav"};

typedef struct {
                uint32_t score, level, lines;
                char     name[NAME_MAX + 1];
} entry_t;

typedef struct {
                blocks_t          game;
                int               screen;
                uint32_t          held, pressed; /* BLOCKS_... */
                char              hints[5][160]; /* the title's and pause's lines, from the bindings */
                bmds_game_image_t tiles;
                char              dir[256];    /* the folder's */
                char              scores[256]; /* the high scores' file ("": none) */
                entry_t           table[SCORES];
                uint32_t          entries;
                int               entering; /* typing a name for a high score */
                int               placed;   /* the new entry's place, or -1 */
                char              name[NAME_MAX + 1];
                /* Sound. */
                bmds_game_audio_t *audio;
                bmds_game_sound_t  sound[SOUNDS];
                int                music;
                char               music_path[300];
                /* What is on the screen (drawn when it differs). */
                int      redraw_all;
                uint8_t  shown[ROWS][BLOCKS_WIDTH]; /* a tile, 0xff unknown */
                uint8_t  shown_hold, shown_hold_dim, shown_next[BLOCKS_NEXT];
                uint32_t shown_score, shown_level, shown_lines;
                int      overlay_dirty;
} state_t;

/* ---- High scores: "SCORE LEVEL LINES NAME" a line, best first. */

static void load_scores(state_t *s) {
        s->entries = 0;
        if (s->scores[0] == '\0') return;
        FILE *f = fopen(s->scores, "r");
        if (f == NULL) return;
        char line[96];
        while (s->entries < SCORES && fgets(line, sizeof(line), f) != NULL) {
                entry_t *e = &s->table[s->entries];
                unsigned score, level, lines;
                int      used = 0;
                if (sscanf(line, "%u %u %u %n", &score, &level, &lines, &used) < 3) continue;
                e->score = score;
                e->level = level;
                e->lines = lines;
                snprintf(e->name, sizeof(e->name), "%s", line + used);
                e->name[strcspn(e->name, "\r\n")] = '\0';
                ++s->entries;
        }
        fclose(f);
        return;
}

static void save_scores(const state_t *s) {
        if (s->scores[0] == '\0') return;
        FILE *f = fopen(s->scores, "w");
        if (f == NULL) return;
        for (uint32_t i = 0; i < s->entries; ++i)
                fprintf(f, "%u %u %u %s\n", (unsigned)s->table[i].score, (unsigned)s->table[i].level,
                        (unsigned)s->table[i].lines, s->table[i].name);
        fclose(f);
        return;
}

/* The place @p score would take in the table, or -1. */
static int place_of(const state_t *s, uint32_t score) {
        if (score == 0) return -1;
        for (uint32_t i = 0; i < s->entries; ++i)
                if (score > s->table[i].score) return (int)i;
        return s->entries < SCORES ? (int)s->entries : -1;
}

static void record_score(state_t *s) {
        const int at = place_of(s, s->game.score);
        if (at < 0) return;
        const uint32_t keep = s->entries < SCORES ? s->entries : SCORES - 1;
        memmove(&s->table[at + 1], &s->table[at], sizeof(entry_t) * (keep - (uint32_t)at));
        entry_t *e = &s->table[at];
        e->score = s->game.score;
        e->level = s->game.level;
        e->lines = s->game.lines;
        snprintf(e->name, sizeof(e->name), "%s", s->name[0] != '\0' ? s->name : "?");
        if (s->entries < SCORES) ++s->entries;
        s->placed = at;
        save_scores(s);
        return;
}

/* ---- Sound */

static void play(state_t *s, int sound, float volume) {
        if (s->audio != NULL && s->sound[sound].samples != NULL)
                (void)bmds_game_play(s->audio, &s->sound[sound], volume, 0.0f, 0);
        return;
}

static void music(state_t *s, int on) {
        if (s->audio == NULL) return;
        if (on && s->music)
                (void)bmds_game_music_play(s->audio, s->music_path, 0.6f, BMDS_GAME_PLAY_LOOP);
        else
                bmds_game_music_stop(s->audio);
        return;
}

static void sounds_for(state_t *s, uint32_t events) {
        if ((events & BLOCKS_OVER) != 0) {
                play(s, S_OVER, 0.8f);
                return;
        }
        if ((events & BLOCKS_CLEARED) != 0)
                play(s, s->game.clearing_count == 4 ? S_FOUR : S_CLEAR, 0.7f);
        else if ((events & BLOCKS_DROPPED) != 0)
                play(s, S_DROP, 0.8f);
        else if ((events & BLOCKS_LOCKED) != 0)
                play(s, S_LOCK, 0.6f);
        if ((events & BLOCKS_SETTLED) != 0) play(s, S_LOCK, 0.35f);
        if ((events & BLOCKS_LEVELLED) != 0) play(s, S_LEVEL, 0.7f);
        if ((events & BLOCKS_HELD) != 0) play(s, S_HOLD, 0.6f);
        if ((events & BLOCKS_TURNED) != 0) play(s, S_TURN, 0.5f);
        if ((events & BLOCKS_MOVED) != 0) play(s, S_MOVE, 0.4f);
        return;
}

/* ---- Screens */

/* The screen, and the actions that go with it (typing a name: none, the keys raw). */
static void show(state_t *s, int screen) {
        static const char *const SETS[] = {"title", "play", "paused", "title"};
        const char              *set = screen == OVER && s->entering ? NULL : SETS[screen];
        s->screen = screen;
        s->redraw_all = 1;
        (void)bmds_controls(BMDS_CONTROLS_SET, (void *)set, set != NULL ? (uint32_t)strlen(set) : 0);
        return;
}

static void start(state_t *s) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        blocks_start(&s->game, 1, (uint32_t)ns ^ (uint32_t)(ns >> 32));
        s->held = s->pressed = 0;
        s->placed = -1;
        show(s, PLAYING);
        music(s, 1);
        return;
}

static void game_over(state_t *s) {
        s->entering = place_of(s, s->game.score) >= 0;
        show(s, OVER);
        music(s, 0);
        return;
}

/* ---- Actions (controls.conf): what each does on each screen */

static uint32_t move_of(uint32_t id) {
        switch (id) {
                case A_LEFT: return BLOCKS_LEFT;
                case A_RIGHT: return BLOCKS_RIGHT;
                case A_DROP: return BLOCKS_HARD; /* all the way */
                case A_TURN_LEFT: return BLOCKS_CCW;
                case A_TURN_RIGHT: return BLOCKS_CW;
                case A_HOLD: return BLOCKS_HOLD;
                default: return 0;
        }
}

static void action(bmds_game_t *game, state_t *s, const bmds_surface_event_t *e) {
        const uint32_t id = e->data[0];
        if (e->flags == BMDS_ACTION_RELEASE) {
                s->held &= ~move_of(id);
                return;
        }
        if (e->flags != BMDS_ACTION_PRESS) return;
        switch (id) {
                case A_MUSIC:
                        s->music = !s->music;
                        if (s->screen == PLAYING) music(s, s->music);
                        break;
                case A_PAUSE:
                        show(s, PAUSED);
                        if (s->audio != NULL) bmds_game_audio_pause(s->audio, 1);
                        break;
                case A_RESUME:
                        show(s, PLAYING);
                        if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                        break;
                case A_GIVE_UP:
                        if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                        music(s, 0);
                        show(s, TITLE);
                        break;
                case A_PLAY: start(s); break;
                case A_QUIT: bmds_game_quit(game); break;
                default:
                        s->held |= move_of(id);
                        s->pressed |= move_of(id);
                        break;
        }
        return;
}

/* An action's first binding as text, without a pad's "Pad." ("" if none). */
static void first_binding(uint32_t id, uint32_t which, char *out, size_t size) {
        bmds_controls_binding_t b = {sizeof(b), id, which, out, (uint32_t)size};
        out[0] = '\0';
        if (bmds_controls(BMDS_CONTROLS_BINDING, &b, sizeof(b)) < 0) out[0] = '\0';
        char *semicolon = strchr(out, ';');
        if (semicolon != NULL) *semicolon = '\0';
        if (strncmp(out, "Pad.", 4) == 0) memmove(out, out + 4, strlen(out + 4) + 1);
        return;
}

/* The title's and the pause's help, from the bindings as they are now. */
static void make_hints(state_t *s) {
        char k[12][16], p[4][16];
        for (uint32_t i = 0; i < 12; ++i) first_binding(i + 1, BMDS_BINDING_KEYS, k[i], sizeof(k[i]));
        first_binding(A_LEFT, BMDS_BINDING_PAD, p[0], sizeof(p[0]));
        first_binding(A_RIGHT, BMDS_BINDING_PAD, p[1], sizeof(p[1]));
        first_binding(A_DROP, BMDS_BINDING_PAD, p[2], sizeof(p[2]));
        first_binding(A_HOLD, BMDS_BINDING_PAD, p[3], sizeof(p[3]));
        snprintf(s->hints[0], sizeof(s->hints[0]), "%s: play   %s: music   %s: quit", k[A_PLAY - 1], k[A_MUSIC - 1],
                 k[A_QUIT - 1]);
        snprintf(s->hints[1], sizeof(s->hints[1]), "%s/%s: move  %s: drop  %s/%s: turn  %s: hold", k[A_LEFT - 1],
                 k[A_RIGHT - 1], k[A_DROP - 1], k[A_TURN_LEFT - 1], k[A_TURN_RIGHT - 1], k[A_HOLD - 1]);
        snprintf(s->hints[2], sizeof(s->hints[2]), "Pad: %s/%s move, %s drops, %s holds", p[0], p[1], p[2], p[3]);
        snprintf(s->hints[3], sizeof(s->hints[3]), "%s: play on", k[A_RESUME - 1]);
        snprintf(s->hints[4], sizeof(s->hints[4]), "%s: give up", k[A_GIVE_UP - 1]);
        return;
}

static void type_name(state_t *s, const bmds_surface_event_t *e) {
        const uint32_t usage = e->data[0];
        size_t         n = strlen(s->name);
        if (usage == K_ENTER || usage == K_KP_ENTER) {
                s->entering = 0;
                record_score(s);
                show(s, TITLE);
        } else if (usage == K_BACKSPACE) {
                if (n > 0) s->name[n - 1] = '\0';
        } else if (usage == K_ESCAPE) {
                s->entering = 0; /* not kept */
                show(s, TITLE);
        } else if (e->data[2] >= 0x20 && e->data[2] < 0x7f && n < NAME_MAX) {
                s->name[n] = (char)e->data[2];
                s->name[n + 1] = '\0';
        }
        s->overlay_dirty = 1;
        return;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        state_t *s = bmds_game_arg(game);
        if (e->type == BMDS_EVENT_CLOSE) {
                bmds_game_quit(game);
                return;
        }
        if (e->type == BMDS_EVENT_ACTIVE) {
                if (e->data[0] == 0 && s->screen == PLAYING) {
                        show(s, PAUSED);
                        bmds_game_redraw(game); /* drawn though inactive: not a frozen game */
                }
                s->held = 0; /* the system lets go of what was held */
                if (s->audio != NULL) bmds_game_audio_pause(s->audio, e->data[0] == 0);
                return;
        }
        if (e->type == BMDS_EVENT_PRESENTED) {
                s->redraw_all = 1;
                return;
        }
        if (e->type == BMDS_EVENT_ACTION) {
                action(game, s, e);
                return;
        }
        /* Raw keys and pads: only while typing a name (no actions then); a pad's Start keeps it. */
        if (s->screen != OVER || !s->entering) return;
        if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) type_name(s, e);
        if (e->type == BMDS_EVENT_PAD && e->flags == BMDS_PAD_BUTTON && e->data[1] == BMDS_PAD_START &&
            e->data[2] != 0) {
                s->entering = 0;
                record_score(s);
                show(s, TITLE);
                s->overlay_dirty = 1;
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        state_t *s = bmds_game_arg(game);
        if (s->screen != PLAYING) return;
        const blocks_input_t in = {s->held, s->pressed};
        s->pressed = 0;
        blocks_tick(&s->game, &in);
        sounds_for(s, s->game.events);
        if (s->game.over) game_over(s);
        return;
}

/* ---- Drawing */

static void tile(bmds_canvas_t *c, const state_t *s, int32_t x, int32_t y, uint32_t t) {
        const bmds_rect_t r = {t * CELL, 0, CELL, CELL};
        bmds_canvas_blit(c, x, y, &s->tiles, &r);
        return;
}

/* What each visible cell shows (a tile, or TILE_GHOST + the piece over the empty one) into @p out. */
static void cells_now(const state_t *s, uint8_t out[ROWS][BLOCKS_WIDTH]) {
        const blocks_t *g = &s->game;
        if (s->screen == TITLE || s->screen == PAUSED) { /* the field hidden */
                memset(out, TILE_EMPTY, ROWS * BLOCKS_WIDTH);
                return;
        }
        for (int row = 0; row < ROWS; ++row) memcpy(out[row], g->field[row + BLOCKS_HIDDEN], BLOCKS_WIDTH);
        if (g->clearing_count > 0) { /* full rows blinking, then the rows above dropping */
                if (blocks_flashing(g))
                        for (uint32_t i = 0; i < g->clearing_count; ++i)
                                memset(out[g->clearing[i] - BLOCKS_HIDDEN], TILE_FLASH, BLOCKS_WIDTH);
                return;
        }
        if (s->screen != PLAYING) return;
        /* The ghost, then the piece over it. */
        blocks_piece_t ghost = g->piece;
        ghost.y = (int8_t)(ghost.y + blocks_drop_distance(g));
        int8_t c[4][2];
        blocks_cells(&ghost, c);
        for (int i = 0; i < 4; ++i)
                if (c[i][1] >= BLOCKS_HIDDEN && out[c[i][1] - BLOCKS_HIDDEN][c[i][0]] == BLOCKS_NONE)
                        out[c[i][1] - BLOCKS_HIDDEN][c[i][0]] = (uint8_t)(TILE_GHOST + g->piece.type);
        blocks_cells(&g->piece, c);
        for (int i = 0; i < 4; ++i)
                if (c[i][1] >= BLOCKS_HIDDEN) out[c[i][1] - BLOCKS_HIDDEN][c[i][0]] = g->piece.type;
        return;
}

/* A piece in a 4 × 2 cell box at (@p x, @p y), centred (dim: drawn as ghosts). */
static void draw_preview(bmds_canvas_t *c, const state_t *s, int32_t x, int32_t y, uint8_t type, int dim) {
        bmds_canvas_fill(c, x, y, 4 * CELL, 2 * CELL, bmds_canvas_pixel(c, BACKGROUND));
        if (type == BLOCKS_NONE) return;
        const blocks_piece_t p = {type, 0, 0, 0};
        int8_t               cells[4][2];
        blocks_cells(&p, cells);
        int minx = 9, maxx = -9, miny = 9, maxy = -9;
        for (int i = 0; i < 4; ++i) {
                if (cells[i][0] < minx) minx = cells[i][0];
                if (cells[i][0] > maxx) maxx = cells[i][0];
                if (cells[i][1] < miny) miny = cells[i][1];
                if (cells[i][1] > maxy) maxy = cells[i][1];
        }
        const int32_t ox = x + (4 * CELL - (maxx - minx + 1) * CELL) / 2 - minx * CELL;
        const int32_t oy = y + (2 * CELL - (maxy - miny + 1) * CELL) / 2 - miny * CELL;
        for (int i = 0; i < 4; ++i)
                tile(c, s, ox + cells[i][0] * CELL, oy + cells[i][1] * CELL, dim ? TILE_GHOST + type : type);
        return;
}

static void draw_number(bmds_canvas_t *c, int32_t x, int32_t y, uint32_t value) {
        char text[16];
        snprintf(text, sizeof(text), "%u", (unsigned)value);
        bmds_canvas_fill(c, x, y, PANEL_W, 32, bmds_canvas_pixel(c, BACKGROUND));
        bmds_canvas_text(c, x + PANEL_W - bmds_game_text_width(text, 2), y, text, bmds_canvas_pixel(c, VALUE), 2);
        return;
}

static void centred(bmds_canvas_t *c, int32_t y, const char *text, uint32_t rgb, int32_t scale) {
        bmds_canvas_text(c, (WIDTH - bmds_game_text_width(text, scale)) / 2, y, text, bmds_canvas_pixel(c, rgb), scale);
        return;
}

/* The box over the middle: the title's scores, pause, game over. */
static void draw_overlay(bmds_canvas_t *c, state_t *s) {
        char line[64];
        if (s->screen == PAUSED) {
                /* Over the field (hidden) only. */
                bmds_canvas_fill(c, FIELD_X, 132, BLOCKS_WIDTH * CELL, 104, bmds_canvas_pixel(c, BOX));
                bmds_canvas_rect(c, FIELD_X, 132, BLOCKS_WIDTH * CELL, 104, bmds_canvas_pixel(c, FRAME));
                centred(c, 148, "PAUSED", VALUE, 2);
                centred(c, 192, s->hints[3], LABEL, 1);
                centred(c, 210, s->hints[4], LABEL, 1);
                return;
        }
        /* Over the field and both panels. */
        bmds_canvas_fill(c, LEFT_X - 16, 10, WIDTH - 2 * (LEFT_X - 16), 340, bmds_canvas_pixel(c, BOX));
        bmds_canvas_rect(c, LEFT_X - 16, 10, WIDTH - 2 * (LEFT_X - 16), 340, bmds_canvas_pixel(c, FRAME));
        if (s->screen == OVER) {
                centred(c, 44, "GAME OVER", VALUE, 3);
                snprintf(line, sizeof(line), "Score %u   level %u   %u lines", (unsigned)s->game.score,
                         (unsigned)s->game.level, (unsigned)s->game.lines);
                centred(c, 112, line, VALUE, 1);
                if (s->entering) {
                        centred(c, 160, "A high score! Your name:", ACCENT, 1);
                        snprintf(line, sizeof(line), "%s_", s->name);
                        centred(c, 192, line, VALUE, 2);
                        centred(c, 300, "Enter: keep it", LABEL, 1);
                } else {
                        char again[160];
                        snprintf(again, sizeof(again), "%s", s->hints[0]);
                        centred(c, 300, again, LABEL, 1);
                }
                return;
        }
        centred(c, 40, "BLOCKS", ACCENT, 3);
        centred(c, 100, "High scores", LABEL, 1);
        if (s->entries == 0) centred(c, 180, "None yet", LABEL, 1);
        for (uint32_t i = 0; i < s->entries; ++i) {
                const entry_t *e = &s->table[i];
                snprintf(line, sizeof(line), "%2u %-12s %8u  L%-2u", (unsigned)(i + 1), e->name, (unsigned)e->score,
                         (unsigned)e->level);
                bmds_canvas_text(c, (WIDTH - 29 * BMDS_GAME_GLYPH_WIDTH) / 2, 124 + (int32_t)i * 16, line,
                                 bmds_canvas_pixel(c, (int)i == s->placed ? ACCENT : VALUE), 1);
        }
        centred(c, 288, s->hints[0], LABEL, 1);
        centred(c, 304, s->hints[1], LABEL, 1);
        centred(c, 320, s->hints[2], LABEL, 1);
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t *s = bmds_game_arg(game);
        if (s->redraw_all) {
                bmds_canvas_clear(c, bmds_canvas_pixel(c, BACKGROUND));
                bmds_canvas_rect(c, FIELD_X - 2, FIELD_Y - 2, BLOCKS_WIDTH * CELL + 4, ROWS * CELL + 4,
                                 bmds_canvas_pixel(c, FRAME));
                const uint32_t label = bmds_canvas_pixel(c, LABEL);
                bmds_canvas_text(c, LEFT_X, FIELD_Y, "HOLD", label, 1);
                bmds_canvas_text(c, LEFT_X, 120, "SCORE", label, 1);
                bmds_canvas_text(c, LEFT_X, 190, "LEVEL", label, 1);
                bmds_canvas_text(c, LEFT_X, 260, "LINES", label, 1);
                bmds_canvas_text(c, RIGHT_X, FIELD_Y, "NEXT", label, 1);
                memset(s->shown, 0xff, sizeof(s->shown));
                s->shown_hold = s->shown_hold_dim = 0xff;
                memset(s->shown_next, 0xff, sizeof(s->shown_next));
                s->shown_score = s->shown_level = s->shown_lines = UINT32_MAX;
                s->overlay_dirty = 1;
        }
        const int playing = s->screen == PLAYING;
        /* The field: the cells that changed (under a box, only when it is drawn whole). */
        if (playing || s->redraw_all) {
                uint8_t now[ROWS][BLOCKS_WIDTH];
                cells_now(s, now);
                for (int row = 0; row < ROWS; ++row)
                        for (int x = 0; x < BLOCKS_WIDTH; ++x) {
                                const uint8_t t = now[row][x];
                                if (s->shown[row][x] == t) continue;
                                s->shown[row][x] = t;
                                const int32_t px = FIELD_X + x * CELL, py = FIELD_Y + row * CELL;
                                if (t > TILE_GHOST) tile(c, s, px, py, TILE_EMPTY); /* a ghost is an outline */
                                tile(c, s, px, py, t);
                        }
                /* The panels. */
                const blocks_t *g = &s->game;
                const uint8_t   hold = s->screen == TITLE ? 0 : g->hold, dim = !g->can_hold;
                if (hold != s->shown_hold || dim != s->shown_hold_dim) {
                        draw_preview(c, s, LEFT_X, FIELD_Y + 24, hold, dim);
                        s->shown_hold = hold;
                        s->shown_hold_dim = dim;
                }
                for (int i = 0; i < BLOCKS_NEXT; ++i) {
                        const uint8_t t = s->screen == TITLE ? 0 : g->next[i];
                        if (t == s->shown_next[i]) continue;
                        draw_preview(c, s, RIGHT_X, FIELD_Y + 24 + i * 44, t, 0);
                        s->shown_next[i] = t;
                }
                if (g->score != s->shown_score) draw_number(c, LEFT_X, 140, s->shown_score = g->score);
                if (g->level != s->shown_level) draw_number(c, LEFT_X, 210, s->shown_level = g->level);
                if (g->lines != s->shown_lines) draw_number(c, LEFT_X, 280, s->shown_lines = g->lines);
        }
        if (!playing && s->overlay_dirty) draw_overlay(c, s);
        s->overlay_dirty = 0;
        s->redraw_all = 0;
        return;
}

/* ---- Start */

/* @p dir/@p name into @p out. @return 0, or -1 (too long). */
static int path_in(char *out, size_t size, const char *dir, const char *name) {
        const int n = snprintf(out, size, "%s/%s", dir, name);
        return n >= 0 && (size_t)n < size ? 0 : -1;
}

int main(int argc, char **argv) {
        static state_t s;
        int            full = 0;
        for (int i = 1; i < argc; ++i) {
                if (strcmp(argv[i], "-F") == 0) {
                        full = 1;
                } else {
                        fprintf(stderr, "usage: blocks [-F]\n");
                        return 2;
                }
        }
        /* The folder: the application's, or where the program is. */
        const char *dir = bmds_app_dir();
        if (dir != NULL) {
                snprintf(s.dir, sizeof(s.dir), "%s", dir);
        } else {
                snprintf(s.dir, sizeof(s.dir), "%s", argv[0]);
                char *slash = strrchr(s.dir, '/');
                if (slash != NULL)
                        *slash = '\0';
                else
                        snprintf(s.dir, sizeof(s.dir), ".");
        }
        char data[256];
        if (bmds_app_data_dir(data, sizeof(data)) != 0 || path_in(s.scores, sizeof(s.scores), data, "scores") != 0)
                s.scores[0] = '\0'; /* not an application: no high scores kept */
        load_scores(&s);
        const char *user = getenv("USER");
        snprintf(s.name, sizeof(s.name), "%s", user != NULL && strcmp(user, "root") != 0 ? user : "");
        s.placed = -1;

        char                   path[300];
        const bmds_game_load_t how = {sizeof(how), BMDS_PIXEL_RGB565, 0, 0, NULL, 0, 0, BMDS_MEM_FAST};
        int err = path_in(path, sizeof(path), s.dir, "tiles.png") == 0 ? bmds_game_image_load(path, &how, &s.tiles)
                                                                       : -BMDS_ENAMETOOLONG;
        if (err < 0) {
                fprintf(stderr, "blocks: %s: %s\n", path, strerror(-err));
                return 1;
        }
        /* Sound if there is a device; quiet otherwise. */
        if (bmds_game_audio_open(NULL, &s.audio) == 0) {
                for (int i = 0; i < SOUNDS; ++i) {
                        if (path_in(path, sizeof(path), s.dir, SOUND_FILES[i]) != 0 ||
                            bmds_game_sound_load(s.audio, path, &s.sound[i]) < 0)
                                fprintf(stderr, "blocks: %s: not loaded\n", path);
                }
                s.music = path_in(s.music_path, sizeof(s.music_path), s.dir, "music.mid") == 0;
        }

        /* The actions: the folder's controls.conf (the system reads it), else the one beside the program. */
        err = bmds_controls(BMDS_CONTROLS_DECLARE, NULL, 0);
        if (err == -BMDS_ENOENT && path_in(path, sizeof(path), s.dir, "controls.conf") == 0) {
                static char text[8192];
                FILE       *f = fopen(path, "r");
                size_t      n = f != NULL ? fread(text, 1, sizeof(text), f) : 0;
                if (f != NULL) fclose(f);
                err = n > 0 ? bmds_controls(BMDS_CONTROLS_DECLARE, text, (uint32_t)n) : -BMDS_ENOENT;
        }
        if (err < 0) {
                fprintf(stderr, "blocks: its controls: %s\n", strerror(-err));
                return 1;
        }
        make_hints(&s);
        show(&s, TITLE);
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = WIDTH;
        config.height = HEIGHT;
        config.format = BMDS_PIXEL_RGB565;
        config.flags = full ? BMDS_SURFACE_FULL_SCREEN : 0;
        config.title = "Blocks";
        config.tick_hz = BLOCKS_TICK_HZ;
        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        err = bmds_game_run(&config);
        bmds_game_audio_close(s.audio);
        for (int i = 0; i < SOUNDS; ++i) bmds_game_sound_free(&s.sound[i]);
        bmds_game_image_free(&s.tiles);
        if (err < 0) {
                fprintf(stderr, "blocks: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
