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
#define K_C KEY(0x06)
#define K_M KEY(0x10)
#define K_P KEY(0x13)
#define K_Q KEY(0x14)
#define K_X KEY(0x1b)
#define K_Z KEY(0x1d)
#define K_ENTER KEY(0x28)
#define K_ESCAPE KEY(0x29)
#define K_BACKSPACE KEY(0x2a)
#define K_SPACE KEY(0x2c)
#define K_RIGHT KEY(0x4f)
#define K_LEFT KEY(0x50)
#define K_DOWN KEY(0x51)
#define K_UP KEY(0x52)
#define K_KP_ENTER KEY(0x58)
#define K_LCTRL KEY(0xe0)
#define K_LSHIFT KEY(0xe1)
#define K_RCTRL KEY(0xe4)
#define K_RSHIFT KEY(0xe5)

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
                uint32_t          stick;         /* BLOCKS_LEFT or _RIGHT: the left stick past half way */
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

static void show(state_t *s, int screen) {
        s->screen = screen;
        s->redraw_all = 1;
        return;
}

static void start(state_t *s) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        blocks_start(&s->game, 1, (uint32_t)ns ^ (uint32_t)(ns >> 32));
        s->held = s->pressed = s->stick = 0;
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

/* ---- Keys */

static uint32_t action_of(uint32_t usage) {
        switch (usage) {
                case K_LEFT: return BLOCKS_LEFT;
                case K_RIGHT: return BLOCKS_RIGHT;
                case K_DOWN:
                case K_SPACE: return BLOCKS_HARD; /* all the way */
                case K_UP:
                case K_X: return BLOCKS_CW;
                case K_Z:
                case K_LCTRL:
                case K_RCTRL: return BLOCKS_CCW;
                case K_C:
                case K_LSHIFT:
                case K_RSHIFT: return BLOCKS_HOLD;
                default: return 0;
        }
}

/* ---- Pads (devplan/controls.md §7b): the D-pad's left and right move, down drops, up holds */

static uint32_t pad_action_of(uint32_t button) {
        switch (button) {
                case BMDS_PAD_LEFT: return BLOCKS_LEFT;
                case BMDS_PAD_RIGHT: return BLOCKS_RIGHT;
                case BMDS_PAD_DOWN: return BLOCKS_HARD;
                case BMDS_PAD_UP: return BLOCKS_HOLD;
                case BMDS_PAD_SOUTH:
                case BMDS_PAD_WEST:
                case BMDS_PAD_L1: return BLOCKS_CCW;
                case BMDS_PAD_EAST:
                case BMDS_PAD_NORTH:
                case BMDS_PAD_R1: return BLOCKS_CW;
                default: return 0;
        }
}

static void pad_event(bmds_game_t *game, state_t *s, const bmds_surface_event_t *e) {
        uint32_t button, down;
        if (e->flags == BMDS_PAD_AXIS && e->data[1] == BMDS_PAD_LEFT_X) {
                /* The left stick as the D-pad's left and right: past half way. */
                const int32_t  x = (int32_t)e->data[2];
                const uint32_t now = x < -16384 ? BLOCKS_LEFT : x > 16384 ? BLOCKS_RIGHT : 0;
                if (s->screen == PLAYING) s->pressed |= now & ~s->stick;
                s->held = (s->held & ~s->stick) | now;
                s->stick = now;
                return;
        }
        if (e->flags != BMDS_PAD_BUTTON) return;
        button = e->data[1];
        down = e->data[2] != 0;
        if (!down) {
                s->held &= ~pad_action_of(button);
                return;
        }
        switch (s->screen) {
                case PLAYING: {
                        const uint32_t a = pad_action_of(button);
                        s->held |= a;
                        s->pressed |= a;
                        if (button == BMDS_PAD_START) {
                                show(s, PAUSED);
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 1);
                        }
                        break;
                }
                case PAUSED:
                        if (button == BMDS_PAD_START) {
                                show(s, PLAYING);
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                        } else if (button == BMDS_PAD_SELECT) {
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                                music(s, 0);
                                show(s, TITLE);
                        }
                        break;
                case TITLE:
                case OVER:
                        if (s->screen == OVER && s->entering) {
                                if (button == BMDS_PAD_START) { /* the name as typed */
                                        s->entering = 0;
                                        record_score(s);
                                        show(s, TITLE);
                                        s->overlay_dirty = 1;
                                }
                        } else if (button == BMDS_PAD_START || button == BMDS_PAD_SOUTH) {
                                start(s);
                        }
                        break;
                default: break;
        }
        (void)game;
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
                s->held = s->stick = 0; /* keys let go while away aren't seen */
                if (s->audio != NULL) bmds_game_audio_pause(s->audio, e->data[0] == 0);
                return;
        }
        if (e->type == BMDS_EVENT_PRESENTED) {
                s->redraw_all = 1;
                return;
        }
        if (e->type == BMDS_EVENT_PAD) {
                pad_event(game, s, e);
                return;
        }
        if (e->type != BMDS_EVENT_KEY) return;
        const uint32_t usage = e->data[0];
        if ((e->flags & BMDS_KEY_UP) != 0) {
                s->held &= ~action_of(usage);
                return;
        }
        if ((e->flags & BMDS_KEY_DOWN) == 0) return;
        const int repeat = (e->flags & BMDS_KEY_REPEAT) != 0;
        if (s->screen == OVER && s->entering) {
                type_name(s, e);
                return;
        }
        if (repeat) return; /* the game repeats its own moves */
        if (usage == K_M) {
                s->music = !s->music;
                if (s->screen == PLAYING) music(s, s->music);
                return;
        }
        switch (s->screen) {
                case PLAYING: {
                        const uint32_t a = action_of(usage);
                        s->held |= a;
                        s->pressed |= a;
                        if (usage == K_ESCAPE || usage == K_P) {
                                show(s, PAUSED);
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 1);
                        }
                        break;
                }
                case PAUSED:
                        if (usage == K_ESCAPE || usage == K_P || usage == K_ENTER) {
                                show(s, PLAYING);
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                        } else if (usage == K_Q) {
                                if (s->audio != NULL) bmds_game_audio_pause(s->audio, 0);
                                music(s, 0);
                                show(s, TITLE);
                        }
                        break;
                case TITLE:
                case OVER:
                        if (usage == K_ENTER || usage == K_KP_ENTER || usage == K_SPACE)
                                start(s);
                        else if (usage == K_Q || usage == K_ESCAPE)
                                bmds_game_quit(game);
                        break;
                default: break;
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
                centred(c, 192, "Esc: play on", LABEL, 1);
                centred(c, 210, "Q: give up", LABEL, 1);
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
                        centred(c, 300, "Enter: play again   Q: quit", LABEL, 1);
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
        centred(c, 288, "Enter: play   M: music   Q: quit", LABEL, 1);
        centred(c, 304, "Arrows, Down/Space: drop, Z/X: turn, C: hold", LABEL, 1);
        centred(c, 320, "Pad: D-pad, down drops, up holds", LABEL, 1);
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
