/**
 * @file      solitaire.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Solitaire (devplan/games.md §21): Klondike in a window on libbmdsgame. The rules are rules.c's;
 * this is the picture (640×480 RGB565, cards from the folder's cards.png, each frame redrawing
 * only where a card or mark differs from the last), cards flying where the game sends them, the
 * mouse and the keyboard, the sounds, the options, and the bouncing cards of a win.
 *
 *     solitaire [-F] [-W]     -F full screen; -W a game played out, finishing by itself (the win's demo)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/app.h>
#include <bmds/game.h>
#include "rules.h"

#define WIDTH 640
#define HEIGHT 480
#define CW 71 /* a card */
#define CH 96
#define COL_X(c) (20 + (c) * 88)
#define TOP_Y 10
#define TABLEAU_Y 120
#define STATUS_Y 460
#define FAN_X 14    /* the waste's fan, drawing three */
#define DOWN_STEP 6 /* a column's face-down cards */
#define UP_STEP 24  /* its face-up cards, closer when it would reach the status line */
#define MIN_STEP 10
#define FLIGHT_TICKS 9
#define DRAG_START 3 /* pixels moved before a press drags */
#define DOUBLE_NS 400000000LL

/* Tiles of cards.png: the faces (suit × 13 + rank), then these in its last row. */
#define TILE_BACK 52
#define TILE_EMPTY 53
#define TILE_HOME 54
#define TILE_AGAIN 55
#define TILE_DONE 56

#define FELT 0x0e7a36
#define BAND 0x0a5a28
#define TEXT 0xe8f4e8
#define DIM 0xa0c8a8
#define CURSOR 0x40e0ff
#define HELD 0xffd020
#define BOX 0x103a20
#define BOX_EDGE 0x70c090

/* HID keys (page 7). */
#define KEY(u) (0x00070000u | (u))
#define K_A KEY(0x04)
#define K_D KEY(0x07)
#define K_F KEY(0x09)
#define K_H KEY(0x0b)
#define K_M KEY(0x10)
#define K_N KEY(0x11)
#define K_O KEY(0x12)
#define K_S KEY(0x16)
#define K_U KEY(0x18)
#define K_Z KEY(0x1d)
#define K_1 KEY(0x1e)
#define K_3 KEY(0x20)
#define K_ENTER KEY(0x28)
#define K_ESCAPE KEY(0x29)
#define K_BACKSPACE KEY(0x2a)
#define K_SPACE KEY(0x2c)
#define K_F1 KEY(0x3a)
#define K_F2 KEY(0x3b)
#define K_RIGHT KEY(0x4f)
#define K_LEFT KEY(0x50)
#define K_DOWN KEY(0x51)
#define K_UP KEY(0x52)
#define K_KP_ENTER KEY(0x58)
#define MOD_CTRL 0x11u

enum {
        S_PLACE,
        S_FLIP,
        S_DEAL,
        S_WIN,
        SOUNDS
};
static const char *const SOUND_FILES[SOUNDS] = {"place.wav", "flip.wav", "deal.wav", "win.wav"};

/* What a frame shows: items by key (the cards, then the piles' places), drawn in `order`. */
#define KEY_PLACE SOL_CARDS /* + the pile */
#define KEYS (KEY_PLACE + SOL_PILES)

enum {
        LAYER_PLACE,
        LAYER_PILE,
        LAYER_DRAG,
        LAYER_FLIGHT
};

typedef struct {
                int16_t x, y;
                uint8_t on, tile, layer, pile, index;
} item_t;

typedef struct {
                item_t  item[KEYS];
                uint8_t order[KEYS];
                uint8_t count;
} view_t;

typedef struct {
                uint8_t  card;   /* its key */
                uint8_t  back;   /* shown face down until it lands */
                uint16_t delay;  /* ticks before it leaves */
                uint16_t ticks;  /* flown */
                int16_t  fx, fy; /* from */
} flight_t;

enum {
        BOX_NONE,
        BOX_HELP,
        BOX_OPTIONS,
        BOX_WON
};

#define MAX_DIRTY 24

typedef struct {
                solitaire_t       game;
                bmds_game_image_t cards;
                char              dir[256];
                char              settings[256]; /* "": none kept */
                uint32_t          draw, scoring, sound, played, won;
                /* What the last frame showed, and what to redraw. */
                view_t      shown;
                int         redraw_all;
                bmds_rect_t dirty[MAX_DIRTY];
                uint32_t    dirty_count;
                bmds_rect_t shown_cursor, shown_held; /* width 0: none */
                int         shown_box;
                uint32_t    box_version, shown_box_version;
                char        shown_status[96];
                /* Motion. */
                flight_t flight[SOL_CARDS];
                uint32_t flights;
                int      auto_play; /* sending everything up, a card every few ticks */
                uint32_t auto_ticks;
                uint32_t second_ticks;
                /* The mouse. */
                uint32_t buttons;
                int32_t  px, py;
                int      press_pile, press_index; /* -1: none */
                int32_t  press_x, press_y, grab_x, grab_y;
                int      dragging;
                int64_t  last_press_ns;
                int      last_press_card;
                /* The keyboard. */
                int cursor_on;
                int cursor_pile, cursor_index;
                int held_pile, held_count; /* held_pile -1: none */
                int box;
                int demo;    /* the next deal played out (-W) */
                int counted; /* this game counts in the games played and won (not a demo) */
                /* The win: cards leaving the foundations, bouncing. */
                int      winning;
                uint32_t launched;
                int      flying_card;      /* -1: none */
                int32_t  wx, wy, wvx, wvy; /* 1/16ths of a pixel */
                int16_t  trail[16][3];     /* x, y, the card */
                uint32_t trail_count;
                int      launch_under; /* the foundation whose new top to draw, or -1 */
                /* Sound. */
                bmds_game_audio_t *audio;
                bmds_game_sound_t  sfx[SOUNDS];
} state_t;

/* ---- Settings: "NAME VALUE" a line. */

static void load_settings(state_t *s) {
        s->draw = 1;
        s->scoring = SOL_SCORE_STANDARD;
        s->sound = 1;
        if (s->settings[0] == '\0') return;
        FILE *f = fopen(s->settings, "r");
        if (f == NULL) return;
        char     line[64], name[32];
        unsigned value;
        while (fgets(line, sizeof(line), f) != NULL) {
                if (sscanf(line, "%31s %u", name, &value) != 2) continue;
                if (strcmp(name, "draw") == 0)
                        s->draw = value == 3 ? 3 : 1;
                else if (strcmp(name, "scoring") == 0)
                        s->scoring = value < SOL_SCORINGS ? value : SOL_SCORE_STANDARD;
                else if (strcmp(name, "sound") == 0)
                        s->sound = value != 0;
                else if (strcmp(name, "played") == 0)
                        s->played = value;
                else if (strcmp(name, "won") == 0)
                        s->won = value;
        }
        fclose(f);
        return;
}

static void save_settings(const state_t *s) {
        if (s->settings[0] == '\0') return;
        FILE *f = fopen(s->settings, "w");
        if (f == NULL) return;
        fprintf(f, "draw %u\nscoring %u\nsound %u\nplayed %u\nwon %u\n", (unsigned)s->draw, (unsigned)s->scoring,
                (unsigned)s->sound, (unsigned)s->played, (unsigned)s->won);
        fclose(f);
        return;
}

/* ---- Sound */

static void play(state_t *s, int sound, float volume) {
        if (s->audio != NULL && s->sound && s->sfx[sound].samples != NULL)
                (void)bmds_game_play(s->audio, &s->sfx[sound], volume, 0.0f, 0);
        return;
}

/* ---- Rectangles */

static bmds_rect_t rect(int32_t x, int32_t y, int32_t w, int32_t h) {
        const bmds_rect_t r = {x, y, (uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)};
        return r;
}

static int meets(const bmds_rect_t *a, const bmds_rect_t *b) {
        return a->width > 0 && b->width > 0 && a->x < b->x + (int32_t)b->width && b->x < a->x + (int32_t)a->width &&
               a->y < b->y + (int32_t)b->height && b->y < a->y + (int32_t)a->height;
}

static bmds_rect_t bounds(const bmds_rect_t *a, const bmds_rect_t *b) {
        const int32_t x0 = a->x < b->x ? a->x : b->x, y0 = a->y < b->y ? a->y : b->y;
        const int32_t ax1 = a->x + (int32_t)a->width, bx1 = b->x + (int32_t)b->width;
        const int32_t ay1 = a->y + (int32_t)a->height, by1 = b->y + (int32_t)b->height;
        return rect(x0, y0, (ax1 > bx1 ? ax1 : bx1) - x0, (ay1 > by1 ? ay1 : by1) - y0);
}

static int contains(const bmds_rect_t *r, int32_t x, int32_t y) {
        return x >= r->x && y >= r->y && x < r->x + (int32_t)r->width && y < r->y + (int32_t)r->height;
}

/* Adds @p r to what the next frame redraws (merged with one it meets; full: with the last). */
static void dirty(state_t *s, bmds_rect_t r) {
        if (r.width == 0 || r.height == 0) return;
        for (uint32_t i = 0; i < s->dirty_count; ++i) {
                if (!meets(&s->dirty[i], &r)) continue;
                r = bounds(&s->dirty[i], &r);
                s->dirty[i] = s->dirty[--s->dirty_count];
                i = (uint32_t)-1; /* the bigger one may meet others now */
        }
        if (s->dirty_count == MAX_DIRTY) {
                s->dirty[MAX_DIRTY - 1] = bounds(&s->dirty[MAX_DIRTY - 1], &r);
                return;
        }
        s->dirty[s->dirty_count++] = r;
        return;
}

/* ---- Where things are */

static int is_column(int pile) {
        return pile >= SOL_TABLEAU && pile < SOL_PILES;
}

static int is_foundation(int pile) {
        return pile >= SOL_FOUNDATION && pile < SOL_TABLEAU;
}

/* A pile's place (its first card's). */
static void pile_origin(int pile, int32_t *x, int32_t *y) {
        if (is_column(pile)) {
                *x = COL_X(pile - SOL_TABLEAU);
                *y = TABLEAU_Y;
        } else {
                *x = COL_X(pile == SOL_STOCK ? 0 : pile == SOL_WASTE ? 1 : pile - SOL_FOUNDATION + 3);
                *y = TOP_Y;
        }
        return;
}

/* A column's step between face-up cards: closer when it would reach the status line. */
static int32_t up_step(const sol_pile_t *p) {
        uint32_t down = 0, up = 0;
        for (uint32_t i = 0; i < p->count; ++i) {
                if ((p->cards[i] & SOL_UP) != 0)
                        ++up;
                else
                        ++down;
        }
        if (up <= 1) return UP_STEP;
        const int32_t room = STATUS_Y - 4 - TABLEAU_Y - CH - (int32_t)down * DOWN_STEP;
        int32_t       step = room / (int32_t)(up - 1);
        if (step > UP_STEP) step = UP_STEP;
        return step < MIN_STEP ? MIN_STEP : step;
}

/* Where card @p index of @p pile lies (the waste's fan aside: see layout()). */
static void card_place(const solitaire_t *g, int pile, int index, int32_t *x, int32_t *y) {
        pile_origin(pile, x, y);
        if (!is_column(pile)) return;
        const sol_pile_t *p = &g->pile[pile];
        const int32_t     step = up_step(p);
        for (int i = 0; i < index; ++i) *y += (p->cards[i] & SOL_UP) != 0 ? step : DOWN_STEP;
        return;
}

static int flying(const state_t *s, uint8_t card) {
        for (uint32_t i = 0; i < s->flights; ++i)
                if (s->flight[i].card == card) return 1;
        return 0;
}

/* The cards of @p pile not lifted by a drag or flying (a pile's lowest run of them). */
static int resting(const state_t *s, int pile) {
        const sol_pile_t *p = &s->game.pile[pile];
        int               n = p->count;
        if (s->dragging && s->press_pile == pile) n = s->press_index;
        while (n > 0 && flying(s, p->cards[n - 1] & (uint8_t)~SOL_UP)) --n;
        return n;
}

/* The waste's cards fanned (drawing three), of @p n resting. */
static int fanned(const state_t *s, int n) {
        if (s->game.draw != 3) return n > 0 ? 1 : 0;
        int f = (int)s->game.fan;
        if (s->game.pile[SOL_WASTE].count > n) f -= s->game.pile[SOL_WASTE].count - n; /* lifted from the fan */
        if (f < 1) f = 1;
        return f < n ? f : n;
}

static void put(view_t *v, int key, int32_t x, int32_t y, int tile, int layer, int pile, int index) {
        item_t *it = &v->item[key];
        it->on = 1;
        it->x = (int16_t)x;
        it->y = (int16_t)y;
        it->tile = (uint8_t)tile;
        it->layer = (uint8_t)layer;
        it->pile = (uint8_t)pile;
        it->index = (uint8_t)index;
        v->order[v->count++] = (uint8_t)key;
        return;
}

static int tile_of(uint8_t card) {
        return (card & SOL_UP) != 0 ? (card & ~SOL_UP) : TILE_BACK;
}

/* Where card @p index of @p pile is drawn when at rest, the waste's fan included. */
static void rest_place(const state_t *s, int pile, int index, int32_t *x, int32_t *y) {
        card_place(&s->game, pile, index, x, y);
        if (pile == SOL_WASTE) {
                const int n = s->game.pile[SOL_WASTE].count, f = fanned(s, n);
                if (index >= n - f) *x += (index - (n - f)) * FAN_X;
        }
        return;
}

static int ease(int32_t from, int32_t to, uint32_t ticks) {
        const int32_t t = (int32_t)(ticks > FLIGHT_TICKS ? FLIGHT_TICKS : ticks), n = FLIGHT_TICKS;
        return from + (to - from) * (2 * n * t - t * t) / (n * n); /* easing out */
}

/* Works out every item of the frame: the places, the piles, a drag, the flights. */
static void layout(const state_t *s, view_t *v) {
        memset(v, 0, sizeof(*v));
        const solitaire_t *g = &s->game;
        int32_t            x, y;
        for (int pile = 0; pile < SOL_PILES; ++pile) {
                int tile = TILE_EMPTY;
                if (pile == SOL_WASTE) continue;
                if (is_foundation(pile)) tile = TILE_HOME;
                if (pile == SOL_STOCK)
                        tile = g->pile[SOL_STOCK].count > 0 || sol_can_draw(g) || g->pile[SOL_WASTE].count == 0
                                       ? TILE_AGAIN
                                       : TILE_DONE;
                if (pile == SOL_STOCK && g->pile[SOL_WASTE].count == 0 && g->pile[SOL_STOCK].count == 0)
                        tile = TILE_EMPTY;
                pile_origin(pile, &x, &y);
                put(v, KEY_PLACE + pile, x, y, tile, LAYER_PLACE, pile, 0);
        }
        for (int pile = 0; pile < SOL_PILES; ++pile) {
                const sol_pile_t *p = &g->pile[pile];
                const int         n = resting(s, pile);
                int               first = 0;
                if (pile == SOL_WASTE)
                        first = n - fanned(s, n);
                else if (!is_column(pile))
                        first = n - 1; /* the top only */
                for (int i = first < 0 ? 0 : first; i < n; ++i) {
                        rest_place(s, pile, i, &x, &y);
                        put(v, p->cards[i] & ~SOL_UP, x, y, tile_of(p->cards[i]), LAYER_PILE, pile, i);
                }
        }
        if (s->dragging) {
                const sol_pile_t *p = &g->pile[s->press_pile];
                const int32_t     step = is_column(s->press_pile) ? up_step(p) : UP_STEP;
                for (int i = s->press_index; i < p->count; ++i)
                        put(v, p->cards[i] & ~SOL_UP, s->px - s->grab_x,
                            s->py - s->grab_y + (i - s->press_index) * step, tile_of(p->cards[i]), LAYER_DRAG,
                            s->press_pile, i);
        }
        for (uint32_t f = 0; f < s->flights; ++f) {
                const flight_t *fl = &s->flight[f];
                /* Where it is going: its place in the rules now. */
                for (int pile = 0; pile < SOL_PILES; ++pile) {
                        const sol_pile_t *p = &g->pile[pile];
                        for (int i = 0; i < p->count; ++i) {
                                if ((p->cards[i] & ~SOL_UP) != fl->card) continue;
                                rest_place(s, pile, i, &x, &y);
                                const int moving = fl->ticks > 0;
                                put(v, fl->card, moving ? ease(fl->fx, x, fl->ticks) : fl->fx,
                                    moving ? ease(fl->fy, y, fl->ticks) : fl->fy,
                                    fl->back || fl->delay > 0 ? TILE_BACK : tile_of(p->cards[i]), LAYER_FLIGHT, pile,
                                    i);
                        }
                }
        }
        return;
}

/* ---- Flights */

/* Sends @p card flying from (@p x, @p y) to its place, after @p delay ticks. */
static void fly(state_t *s, uint8_t card, int32_t x, int32_t y, uint32_t delay, int back) {
        for (uint32_t i = 0; i < s->flights; ++i) /* flying already: from where it is */
                if (s->flight[i].card == card) s->flight[i] = s->flight[--s->flights];
        if (s->flights == SOL_CARDS) return;
        flight_t *f = &s->flight[s->flights++];
        f->card = card;
        f->back = (uint8_t)back;
        f->delay = (uint16_t)delay;
        f->ticks = 0;
        f->fx = (int16_t)x;
        f->fy = (int16_t)y;
        return;
}

static void land_all(state_t *s) {
        s->flights = 0;
        return;
}

/* Where each card shows now (the last frame's), for flights from there. */
static int shown_at(const state_t *s, uint8_t card, int32_t *x, int32_t *y) {
        const item_t *it = &s->shown.item[card];
        if (!it->on) return 0;
        *x = it->x;
        *y = it->y;
        return 1;
}

/* Moves the top @p count of @p from onto @p to, the cards flying from where they show. */
static int move_flying(state_t *s, int from, int count, int to) {
        const sol_pile_t *p = &s->game.pile[from];
        uint8_t           cards[SOL_CARDS];
        int32_t           xs[SOL_CARDS], ys[SOL_CARDS];
        int               known[SOL_CARDS];
        for (int i = 0; i < count; ++i) {
                cards[i] = p->cards[p->count - count + i] & (uint8_t)~SOL_UP;
                known[i] = shown_at(s, cards[i], &xs[i], &ys[i]);
        }
        if (sol_move(&s->game, (uint32_t)from, (uint32_t)count, (uint32_t)to) != 0) return -1;
        for (int i = 0; i < count; ++i)
                if (known[i]) fly(s, cards[i], xs[i], ys[i], 0, 0);
        const sol_move_t *m = &s->game.history[(s->game.history_first + s->game.history_count - 1) % SOL_HISTORY];
        play(s, S_PLACE, 0.8f);
        if (m->flipped) play(s, S_FLIP, 0.5f);
        return 0;
}

static void draw_cards(state_t *s) {
        land_all(s);
        int32_t x, y;
        pile_origin(SOL_STOCK, &x, &y);
        const int n = sol_draw(&s->game);
        if (n < 0) return;
        play(s, S_FLIP, 0.6f);
        if (n == 0) return; /* turned again: the waste goes back at once */
        const sol_pile_t *w = &s->game.pile[SOL_WASTE];
        for (int i = 0; i < n; ++i) fly(s, w->cards[w->count - n + i] & (uint8_t)~SOL_UP, x, y, (uint32_t)i * 2, 0);
        return;
}

/* A card that can go up (sol_auto_step()'s order), without moving it. @return its pile, or -1. */
static int auto_find(const solitaire_t *g, int *to) {
        if (g->won) return -1;
        for (int pile = SOL_WASTE; pile < SOL_PILES; ++pile) {
                if (is_foundation(pile)) continue;
                *to = sol_foundation_for(g, (uint32_t)pile);
                if (*to >= 0) return pile;
        }
        return -1;
}

static void send_up(state_t *s, int pile) {
        const int f = sol_foundation_for(&s->game, (uint32_t)pile);
        if (f >= 0) (void)move_flying(s, pile, 1, f);
        return;
}

/* ---- Games */

static void deal(state_t *s) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        land_all(s);
        s->auto_play = 0;
        s->counted = !s->demo;
        if (s->demo) {
                sol_deal_finished(&s->game, s->draw, s->scoring);
                s->game.started = 1;
                s->auto_play = 1;
                s->demo = 0;
        } else {
                sol_deal(&s->game, s->draw, s->scoring, (uint32_t)ns ^ (uint32_t)(ns >> 32));
        }
        s->dragging = 0;
        s->press_pile = -1;
        s->held_pile = -1;
        s->winning = 0;
        s->second_ticks = 0;
        s->box = BOX_NONE;
        s->cursor_pile = SOL_STOCK;
        s->cursor_index = 0;
        s->redraw_all = 1;
        /* The deal flies out a card at a time, face down until it lands. */
        int32_t x, y;
        pile_origin(SOL_STOCK, &x, &y);
        uint32_t k = 0;
        for (int row = 0; row < 7 && s->counted; ++row)
                for (int col = row; col < 7; ++col)
                        fly(s, s->game.pile[SOL_TABLEAU + col].cards[row] & (uint8_t)~SOL_UP, x, y, k++ * 2, 1);
        play(s, S_DEAL, 0.8f);
        return;
}

static void count_played(state_t *s, int was_started) {
        if (!was_started && s->game.started) {
                ++s->played;
                save_settings(s);
        }
        return;
}

/* After a move: the win, or everything up by itself once nothing is hidden. */
static void after_move(state_t *s) {
        if (s->game.won) return;
        const solitaire_t *g = &s->game;
        if (g->pile[SOL_STOCK].count != 0 || g->pile[SOL_WASTE].count != 0) return;
        for (int c = SOL_TABLEAU; c < SOL_PILES; ++c)
                for (int i = 0; i < g->pile[c].count; ++i)
                        if ((g->pile[c].cards[i] & SOL_UP) == 0) return;
        s->auto_play = 1;
        return;
}

static void start_win(state_t *s) {
        s->winning = 1;
        s->launched = 0;
        s->flying_card = -1;
        s->trail_count = 0;
        s->launch_under = -1;
        if (s->counted) {
                ++s->won;
                save_settings(s);
        }
        play(s, S_WIN, 0.8f);
        return;
}

/* ---- The win's bouncing cards */

static void win_tick(state_t *s) {
        if (s->flying_card < 0) {
                if (s->launched == SOL_CARDS) {
                        if (s->box != BOX_WON) {
                                s->box = BOX_WON;
                                ++s->box_version;
                        }
                        return;
                }
                const int   f = SOL_FOUNDATION + (int)(s->launched % 4);
                sol_pile_t *p = &s->game.pile[f];
                if (p->count == 0) {
                        ++s->launched;
                        return;
                }
                s->flying_card = p->cards[p->count - 1] & ~SOL_UP;
                --p->count; /* the game is over: the foundations are only drawn from now */
                ++s->launched;
                int32_t x, y;
                pile_origin(f, &x, &y);
                s->wx = x * 16;
                s->wy = y * 16;
                uint32_t r = (uint32_t)rand();
                s->wvx = (int32_t)(40 + r % 72) * ((r >> 8) & 1 ? 1 : -1); /* 2.5-7 pixels a step */
                s->wvy = -(int32_t)((r >> 12) % 128);
                s->launch_under = f;
        }
        /* Two steps a tick: gravity, and a bounce off the bottom losing a quarter. */
        for (int step = 0; step < 2 && s->flying_card >= 0; ++step) {
                s->wvy += 6;
                s->wx += s->wvx;
                s->wy += s->wvy;
                if (s->wy > (HEIGHT - CH) * 16) {
                        s->wy = (HEIGHT - CH) * 16;
                        s->wvy = -s->wvy * 3 / 4;
                }
                if (s->trail_count < 16) {
                        s->trail[s->trail_count][0] = (int16_t)(s->wx / 16);
                        s->trail[s->trail_count][1] = (int16_t)(s->wy / 16);
                        s->trail[s->trail_count][2] = (int16_t)s->flying_card;
                        ++s->trail_count;
                }
                if (s->wx / 16 + CW <= 0 || s->wx / 16 >= WIDTH) s->flying_card = -1;
        }
        return;
}

/* ---- Keyboard */

static const int TOP_ROW[] = {SOL_STOCK,          SOL_WASTE,          SOL_FOUNDATION,
                              SOL_FOUNDATION + 1, SOL_FOUNDATION + 2, SOL_FOUNDATION + 3};

static int column_of(int pile) {
        if (is_column(pile)) return pile - SOL_TABLEAU;
        return pile == SOL_STOCK ? 0 : pile == SOL_WASTE ? 1 : pile - SOL_FOUNDATION + 3;
}

static int top_pile_at(int col) {
        return col == 0 ? SOL_STOCK : col <= 2 ? SOL_WASTE : SOL_FOUNDATION + col - 3;
}

/* The cursor on @p pile, at its top card. */
static void cursor_to(state_t *s, int pile) {
        s->cursor_pile = pile;
        const int n = s->game.pile[pile].count;
        s->cursor_index = n > 0 ? n - 1 : 0;
        return;
}

static int first_up(const sol_pile_t *p) {
        int i = p->count;
        while (i > 0 && (p->cards[i - 1] & SOL_UP) != 0) --i;
        return i;
}

static void key_move(state_t *s, uint32_t usage) {
        const int pile = s->cursor_pile;
        if (usage == K_LEFT || usage == K_RIGHT) {
                const int d = usage == K_LEFT ? -1 : 1;
                if (is_column(pile)) {
                        cursor_to(s, SOL_TABLEAU + (column_of(pile) + d + 7) % 7);
                } else {
                        int at = 0;
                        for (int i = 0; i < 6; ++i)
                                if (TOP_ROW[i] == pile) at = i;
                        cursor_to(s, TOP_ROW[(at + d + 6) % 6]);
                }
        } else if (usage == K_UP) {
                const sol_pile_t *p = &s->game.pile[pile];
                if (is_column(pile) && p->count > 0 && s->cursor_index > first_up(p))
                        --s->cursor_index;
                else if (is_column(pile))
                        cursor_to(s, top_pile_at(column_of(pile)));
        } else if (usage == K_DOWN) {
                if (!is_column(pile))
                        cursor_to(s, SOL_TABLEAU + column_of(pile));
                else if (s->cursor_index + 1 < s->game.pile[pile].count)
                        ++s->cursor_index;
        }
        return;
}

/* Space or Enter: draw, pick up, or put down. */
static void key_act(state_t *s) {
        const int pile = s->cursor_pile;
        if (s->held_pile >= 0) {
                const int from = s->held_pile, count = s->held_count;
                s->held_pile = -1;
                if (pile != from && move_flying(s, from, count, pile) == 0) cursor_to(s, pile);
                return;
        }
        if (pile == SOL_STOCK) {
                draw_cards(s);
                return;
        }
        const int n = s->game.pile[pile].count;
        if (n == 0) return;
        int index = is_column(pile) ? s->cursor_index : n - 1;
        if (index >= n) index = n - 1;
        if (sol_can_take(&s->game, (uint32_t)pile, (uint32_t)(n - index))) {
                s->held_pile = pile;
                s->held_count = n - index;
        }
        return;
}

static void set_option(state_t *s, uint32_t usage) {
        if (usage == K_1 || usage == K_3) {
                const uint32_t draw = usage == K_1 ? 1 : 3;
                if (draw == s->draw) return;
                s->draw = draw;
        } else if (usage == K_S) {
                s->scoring = (s->scoring + 1) % SOL_SCORINGS;
        } else {
                return;
        }
        save_settings(s);
        deal(s);
        s->box = BOX_OPTIONS;
        ++s->box_version;
        return;
}

static void on_key(state_t *s, const bmds_surface_event_t *e) {
        const uint32_t usage = e->data[0];
        const int      ctrl = (e->data[1] & MOD_CTRL) != 0;
        if (s->winning) {
                if ((e->flags & BMDS_KEY_REPEAT) == 0 && (usage & 0xffffu) < 0xe0) deal(s); /* not a modifier */
                return;
        }
        s->cursor_on = 1;
        if (usage == K_M) {
                s->sound = !s->sound;
                save_settings(s);
                ++s->box_version;
                return;
        }
        if (s->box != BOX_NONE) {
                if (s->box == BOX_OPTIONS && usage != K_O && usage != K_ESCAPE) {
                        set_option(s, usage);
                        return;
                }
                if (usage == K_ESCAPE || usage == K_O || usage == K_F1 || usage == K_H || usage == K_ENTER)
                        s->box = BOX_NONE;
                return;
        }
        land_all(s);
        s->auto_play = 0;
        const int was_started = s->game.started;
        switch (usage) {
                case K_LEFT:
                case K_RIGHT:
                case K_UP:
                case K_DOWN: key_move(s, usage); break;
                case K_SPACE:
                case K_ENTER:
                case K_KP_ENTER: key_act(s); break;
                case K_ESCAPE: s->held_pile = -1; break;
                case K_D: draw_cards(s); break;
                case K_F:
                        s->held_pile = -1;
                        send_up(s, s->cursor_pile);
                        break;
                case K_A: s->auto_play = 1; break;
                case K_U:
                case K_BACKSPACE:
                case K_Z:
                        if (usage == K_Z && !ctrl) break;
                        s->held_pile = -1;
                        if (sol_undo(&s->game) == 0) play(s, S_FLIP, 0.5f);
                        break;
                case K_N:
                case K_F2: deal(s); return;
                case K_O: s->box = BOX_OPTIONS; break;
                case K_H:
                case K_F1: s->box = BOX_HELP; break;
                default: break;
        }
        /* The cursor stays on a card that is there. */
        const int n = s->game.pile[s->cursor_pile].count;
        if (s->cursor_index >= n) s->cursor_index = n > 0 ? n - 1 : 0;
        count_played(s, was_started);
        after_move(s);
        return;
}

/* ---- Mouse */

/* The pile and card under (@p x, @p y) as shown: the card -1 for a place with none. @return the pile, or -1. */
static int hit(const state_t *s, int32_t x, int32_t y, int *index) {
        for (int k = s->shown.count - 1; k >= 0; --k) {
                const item_t *it = &s->shown.item[s->shown.order[k]];
                if (it->layer == LAYER_FLIGHT || it->layer == LAYER_DRAG) continue;
                const bmds_rect_t r = rect(it->x, it->y, CW, CH);
                if (!contains(&r, x, y)) continue;
                *index = it->layer == LAYER_PLACE ? -1 : it->index;
                return it->pile;
        }
        return -1;
}

/* The pile a drop of the dragged cards goes to: the most overlap among those that take them. */
static int drop_target(const state_t *s) {
        const int         from = s->press_pile, count = s->game.pile[from].count - s->press_index;
        const bmds_rect_t moved = rect(s->px - s->grab_x, s->py - s->grab_y, CW, CH);
        int               best = -1;
        int64_t           best_area = 0;
        for (int pile = SOL_FOUNDATION; pile < SOL_PILES; ++pile) {
                if (!sol_can_move(&s->game, (uint32_t)from, (uint32_t)count, (uint32_t)pile)) continue;
                int32_t   x, y;
                const int n = s->game.pile[pile].count;
                rest_place(s, pile, n > 0 ? n - 1 : 0, &x, &y);
                const bmds_rect_t r = rect(x, y, CW, CH);
                if (!meets(&r, &moved)) continue;
                const int32_t w = (r.x < moved.x ? r.x + CW - moved.x : moved.x + CW - r.x);
                const int32_t h = (r.y < moved.y ? r.y + CH - moved.y : moved.y + CH - r.y);
                if ((int64_t)w * h > best_area) {
                        best_area = (int64_t)w * h;
                        best = pile;
                }
        }
        return best;
}

static void drop(state_t *s) {
        const int from = s->press_pile, count = s->game.pile[from].count - s->press_index;
        const int to = drop_target(s);
        s->dragging = 0;
        s->press_pile = -1;
        if (to >= 0 && move_flying(s, from, count, to) == 0) return;
        /* Back where they came from, flying from where they were let go. */
        const sol_pile_t *p = &s->game.pile[from];
        for (int i = p->count - count; i < p->count; ++i) {
                int32_t x, y;
                if (shown_at(s, p->cards[i] & (uint8_t)~SOL_UP, &x, &y))
                        fly(s, p->cards[i] & (uint8_t)~SOL_UP, x, y, 0, 0);
        }
        return;
}

static void on_press(state_t *s, int64_t when) {
        land_all(s);
        s->auto_play = 0;
        s->held_pile = -1;
        int       index;
        const int pile = hit(s, s->px, s->py, &index);
        if (pile == SOL_STOCK) {
                draw_cards(s);
                return;
        }
        if (pile < 0 || index < 0) return;
        const sol_pile_t *p = &s->game.pile[pile];
        const int         card = p->cards[index] & ~SOL_UP;
        if (index == p->count - 1 && card == s->last_press_card && when - s->last_press_ns < DOUBLE_NS) {
                s->last_press_card = -1;
                send_up(s, pile);
                return;
        }
        s->last_press_card = card;
        s->last_press_ns = when;
        if (!sol_can_take(&s->game, (uint32_t)pile, (uint32_t)(p->count - index))) return;
        s->press_pile = pile;
        s->press_index = index;
        s->press_x = s->px;
        s->press_y = s->py;
        const item_t *it = &s->shown.item[card];
        s->grab_x = s->px - it->x;
        s->grab_y = s->py - it->y;
        return;
}

static void on_pointer(state_t *s, const bmds_surface_event_t *e) {
        const uint32_t buttons = e->data[2], pressed = buttons & ~s->buttons, released = s->buttons & ~buttons;
        const int32_t  x = (int32_t)e->data[0], y = (int32_t)e->data[1];
        if (x != s->px || y != s->py) s->cursor_on = 0;
        s->px = x < -CW ? -CW : x > WIDTH + CW ? WIDTH + CW : x;
        s->py = y < -CH ? -CH : y > HEIGHT + CH ? HEIGHT + CH : y;
        s->buttons = buttons;
        if (s->winning) {
                if (pressed != 0) deal(s);
                return;
        }
        if (s->box != BOX_NONE) {
                if (pressed != 0) s->box = BOX_NONE;
                return;
        }
        const int was_started = s->game.started;
        if ((pressed & 1u) != 0) on_press(s, e->time_ns);
        if ((pressed & 2u) != 0) {
                land_all(s);
                s->auto_play = 1;
        }
        if (s->press_pile >= 0 && !s->dragging && (buttons & 1u) != 0) {
                const int32_t dx = s->px - s->press_x, dy = s->py - s->press_y;
                if (dx > DRAG_START || dx < -DRAG_START || dy > DRAG_START || dy < -DRAG_START) s->dragging = 1;
        }
        if ((released & 1u) != 0) {
                if (s->dragging)
                        drop(s);
                else
                        s->press_pile = -1;
        }
        count_played(s, was_started);
        after_move(s);
        return;
}

/* ---- Events and ticks */

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        state_t *s = bmds_game_arg(game);
        switch (e->type) {
                case BMDS_EVENT_CLOSE: bmds_game_quit(game); break;
                case BMDS_EVENT_PRESENTED: s->redraw_all = 1; break;
                case BMDS_EVENT_ACTIVE:
                        if (s->audio != NULL) bmds_game_audio_pause(s->audio, e->data[0] == 0);
                        break;
                case BMDS_EVENT_POINTER: on_pointer(s, e); break;
                case BMDS_EVENT_KEY:
                        if ((e->flags & BMDS_KEY_DOWN) != 0) on_key(s, e);
                        break;
                default: break;
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        state_t *s = bmds_game_arg(game);
        if (s->winning) {
                win_tick(s);
                return;
        }
        /* Flights move on; those landed end. */
        for (uint32_t i = 0; i < s->flights;) {
                flight_t *f = &s->flight[i];
                if (f->delay > 0) {
                        --f->delay;
                } else if (++f->ticks > FLIGHT_TICKS) {
                        *f = s->flight[--s->flights];
                        continue;
                }
                ++i;
        }
        if (s->auto_play && ++s->auto_ticks >= 4) {
                s->auto_ticks = 0;
                int       to = -1;
                const int from = auto_find(&s->game, &to);
                if (from < 0)
                        s->auto_play = 0;
                else
                        (void)move_flying(s, from, 1, to);
        }
        if (s->game.won && s->flights == 0 && !s->auto_play) start_win(s);
        if (++s->second_ticks >= 60) {
                s->second_ticks = 0;
                sol_second(&s->game);
        }
        return;
}

/* ---- Drawing */

static void blit_tile(bmds_canvas_t *c, const state_t *s, int32_t x, int32_t y, int tile) {
        const bmds_rect_t r = rect((tile % 13) * CW, (tile / 13) * CH, CW, CH);
        bmds_canvas_blit(c, x, y, &s->cards, &r);
        return;
}

static void status_text(const state_t *s, char *out, size_t size) {
        const solitaire_t *g = &s->game;
        char               score[24] = "";
        if (g->scoring == SOL_SCORE_STANDARD)
                snprintf(score, sizeof(score), "Score: %d", (int)g->score);
        else if (g->scoring == SOL_SCORE_VEGAS)
                snprintf(score, sizeof(score), "Score: $%d", (int)g->score);
        snprintf(out, size, "%-16s Time: %u:%02u   Moves: %u", score, (unsigned)(g->seconds / 60),
                 (unsigned)(g->seconds % 60), (unsigned)g->moves);
        return;
}

/* The cursor's rectangle (the card under it, or the place), and the held run's. */
static bmds_rect_t cursor_rect(const state_t *s) {
        if (!s->cursor_on || s->winning || s->box != BOX_NONE) return rect(0, 0, 0, 0);
        int32_t   x, y;
        const int n = s->game.pile[s->cursor_pile].count;
        int       index = is_column(s->cursor_pile) ? s->cursor_index : n - 1;
        if (index < 0) index = 0;
        rest_place(s, s->cursor_pile, index, &x, &y);
        return rect(x - 5, y - 5, CW + 10, CH + 10); /* outside a held run's */
}

static bmds_rect_t held_rect(const state_t *s) {
        if (s->held_pile < 0 || s->winning) return rect(0, 0, 0, 0);
        const int n = s->game.pile[s->held_pile].count;
        int32_t   x0, y0, x1, y1;
        rest_place(s, s->held_pile, n - s->held_count, &x0, &y0);
        rest_place(s, s->held_pile, n - 1, &x1, &y1);
        return rect(x0 - 2, y0 - 2, x1 - x0 + CW + 4, y1 - y0 + CH + 4);
}

static void outline(bmds_canvas_t *c, bmds_rect_t r, uint32_t rgb) {
        const uint32_t p = bmds_canvas_pixel(c, rgb);
        bmds_canvas_rect(c, r.x, r.y, (int32_t)r.width, (int32_t)r.height, p);
        bmds_canvas_rect(c, r.x + 1, r.y + 1, (int32_t)r.width - 2, (int32_t)r.height - 2, p);
        return;
}

static bmds_rect_t box_rect(void) {
        return rect(110, 100, 420, 260);
}

static void box_line(bmds_canvas_t *c, int32_t y, const char *text, uint32_t rgb) {
        bmds_canvas_text(c, 130, y, text, bmds_canvas_pixel(c, rgb), 1);
        return;
}

static void draw_box(bmds_canvas_t *c, const state_t *s) {
        const bmds_rect_t r = box_rect();
        char              line[80];
        bmds_canvas_fill(c, r.x, r.y, (int32_t)r.width, (int32_t)r.height, bmds_canvas_pixel(c, BOX));
        outline(c, r, BOX_EDGE);
        if (s->box == BOX_HELP) {
                box_line(c, 116, "SOLITAIRE: the keys", HELD);
                box_line(c, 144, "Arrows      move the cursor", TEXT);
                box_line(c, 162, "Space/Enter draw, pick up, put down", TEXT);
                box_line(c, 180, "D  draw     F  this card up", TEXT);
                box_line(c, 198, "A  all up   Esc  let go", TEXT);
                box_line(c, 216, "Ctrl+Z/U    undo", TEXT);
                box_line(c, 234, "F2/N  deal  O  options  M  sound", TEXT);
                box_line(c, 262, "Mouse: drag cards; double click", DIM);
                box_line(c, 280, "sends one up, right click all.", DIM);
                box_line(c, 324, "Esc: close", DIM);
        } else if (s->box == BOX_OPTIONS) {
                static const char *const scoring[] = {"standard", "Vegas", "none"};
                box_line(c, 116, "OPTIONS", HELD);
                snprintf(line, sizeof(line), "1 / 3    draw %s", s->draw == 3 ? "three" : "one");
                box_line(c, 150, line, TEXT);
                snprintf(line, sizeof(line), "S        scoring: %s", scoring[s->scoring]);
                box_line(c, 172, line, TEXT);
                snprintf(line, sizeof(line), "M        sound %s", s->sound ? "on" : "off");
                box_line(c, 194, line, TEXT);
                box_line(c, 222, "Changing the draw or scoring deals again.", DIM);
                snprintf(line, sizeof(line), "Games won: %u of %u", (unsigned)s->won, (unsigned)s->played);
                box_line(c, 260, line, TEXT);
                box_line(c, 324, "Esc: close", DIM);
        } else {
                bmds_canvas_text(c, r.x + ((int32_t)r.width - bmds_game_text_width("YOU WON!", 3)) / 2, 140, "YOU WON!",
                                 bmds_canvas_pixel(c, HELD), 3);
                if (s->game.scoring != SOL_SCORE_NONE) {
                        snprintf(line, sizeof(line), "Score %s%d in %u:%02u",
                                 s->game.scoring == SOL_SCORE_VEGAS ? "$" : "", (int)s->game.score,
                                 (unsigned)(s->game.seconds / 60), (unsigned)(s->game.seconds % 60));
                        box_line(c, 220, line, TEXT);
                }
                snprintf(line, sizeof(line), "Games won: %u of %u", (unsigned)s->won, (unsigned)s->played);
                box_line(c, 244, line, TEXT);
                box_line(c, 324, "Click or press a key to deal again", DIM);
        }
        return;
}

/* Everything inside @p r, from the felt up. */
static void draw_scene(bmds_canvas_t *c, const state_t *s, const view_t *v, bmds_rect_t r) {
        bmds_canvas_set_clip(c, &r);
        bmds_canvas_fill(c, r.x, r.y, (int32_t)r.width, (int32_t)r.height, bmds_canvas_pixel(c, FELT));
        const bmds_rect_t band = rect(0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y);
        if (meets(&r, &band)) {
                char text[96];
                status_text(s, text, sizeof(text));
                bmds_canvas_fill(c, 0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y, bmds_canvas_pixel(c, BAND));
                bmds_canvas_text(c, 12, STATUS_Y + 2, text, bmds_canvas_pixel(c, TEXT), 1);
                const char *keys = "F1: keys   O: options";
                bmds_canvas_text(c, WIDTH - 12 - bmds_game_text_width(keys, 1), STATUS_Y + 2, keys,
                                 bmds_canvas_pixel(c, DIM), 1);
        }
        for (int k = 0; k < v->count; ++k) {
                const item_t     *it = &v->item[v->order[k]];
                const bmds_rect_t cr = rect(it->x, it->y, CW, CH);
                if (meets(&cr, &r)) blit_tile(c, s, it->x, it->y, it->tile);
        }
        const bmds_rect_t held = held_rect(s), cursor = cursor_rect(s);
        if (held.width > 0 && meets(&held, &r)) outline(c, held, HELD);
        if (cursor.width > 0 && meets(&cursor, &r)) outline(c, cursor, CURSOR);
        const bmds_rect_t box = box_rect();
        if (s->box != BOX_NONE && meets(&box, &r)) draw_box(c, s);
        bmds_canvas_set_clip(c, NULL);
        return;
}

static int same(const item_t *a, const item_t *b) {
        return a->on == b->on && (!a->on || (a->x == b->x && a->y == b->y && a->tile == b->tile &&
                                             a->layer == b->layer && a->pile == b->pile && a->index == b->index));
}

static int same_rect(const bmds_rect_t *a, const bmds_rect_t *b) {
        return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static void draw_win(bmds_canvas_t *c, state_t *s) {
        if (s->redraw_all) { /* the trails are lost: the felt and the foundations again */
                bmds_canvas_clear(c, bmds_canvas_pixel(c, FELT));
                for (int f = SOL_FOUNDATION; f < SOL_TABLEAU; ++f) {
                        const sol_pile_t *p = &s->game.pile[f];
                        int32_t           x, y;
                        pile_origin(f, &x, &y);
                        blit_tile(c, s, x, y, p->count > 0 ? (p->cards[p->count - 1] & ~SOL_UP) : TILE_HOME);
                }
                s->shown_box_version = s->box_version - 1;
        }
        s->redraw_all = 0;
        if (s->launch_under >= 0) { /* the foundation's new top, or its place */
                const sol_pile_t *p = &s->game.pile[s->launch_under];
                int32_t           x, y;
                pile_origin(s->launch_under, &x, &y);
                bmds_canvas_fill(c, x, y, CW, CH, bmds_canvas_pixel(c, FELT));
                blit_tile(c, s, x, y, p->count > 0 ? (p->cards[p->count - 1] & ~SOL_UP) : TILE_HOME);
                s->launch_under = -1;
        }
        for (uint32_t i = 0; i < s->trail_count; ++i) /* every place it passed, never erased: Windows' trails */
                blit_tile(c, s, s->trail[i][0], s->trail[i][1], s->trail[i][2]);
        s->trail_count = 0;
        if (s->box == BOX_WON && s->shown_box_version != s->box_version) {
                draw_box(c, s);
                s->shown_box_version = s->box_version;
        }
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t *s = bmds_game_arg(game);
        if (s->winning) {
                draw_win(c, s);
                return;
        }
        static view_t now;
        layout(s, &now);
        if (s->redraw_all) {
                s->dirty_count = 0;
                dirty(s, rect(0, 0, WIDTH, HEIGHT));
                s->redraw_all = 0;
        } else {
                for (int key = 0; key < KEYS; ++key) {
                        const item_t *a = &s->shown.item[key], *b = &now.item[key];
                        if (same(a, b)) continue;
                        if (a->on) dirty(s, rect(a->x, a->y, CW, CH));
                        if (b->on) dirty(s, rect(b->x, b->y, CW, CH));
                }
        }
        const bmds_rect_t cursor = cursor_rect(s), held = held_rect(s);
        if (!same_rect(&cursor, &s->shown_cursor)) {
                dirty(s, s->shown_cursor);
                dirty(s, cursor);
                s->shown_cursor = cursor;
        }
        if (!same_rect(&held, &s->shown_held)) {
                dirty(s, s->shown_held);
                dirty(s, held);
                s->shown_held = held;
        }
        if (s->box != s->shown_box || (s->box != BOX_NONE && s->box_version != s->shown_box_version)) {
                dirty(s, box_rect());
                s->shown_box = s->box;
                s->shown_box_version = s->box_version;
        }
        char text[96];
        status_text(s, text, sizeof(text));
        if (strcmp(text, s->shown_status) != 0) {
                dirty(s, rect(0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y));
                snprintf(s->shown_status, sizeof(s->shown_status), "%s", text);
        }
        for (uint32_t i = 0; i < s->dirty_count; ++i) draw_scene(c, s, &now, s->dirty[i]);
        s->dirty_count = 0;
        s->shown = now;
        return;
}

/* ---- Start */

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
                } else if (strcmp(argv[i], "-W") == 0) {
                        s.demo = 1;
                } else {
                        fprintf(stderr, "usage: solitaire [-F] [-W]\n");
                        return 2;
                }
        }
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
        if (bmds_app_data_dir(data, sizeof(data)) != 0 ||
            path_in(s.settings, sizeof(s.settings), data, "settings") != 0)
                s.settings[0] = '\0';
        load_settings(&s);

        char                   path[300];
        const bmds_game_load_t how = {sizeof(how), BMDS_PIXEL_RGB565, 0, 0, NULL, 0, 0, 0};
        int err = path_in(path, sizeof(path), s.dir, "cards.png") == 0 ? bmds_game_image_load(path, &how, &s.cards)
                                                                       : -BMDS_ENAMETOOLONG;
        if (err < 0) {
                fprintf(stderr, "solitaire: %s: %s\n", path, strerror(-err));
                return 1;
        }
        if (bmds_game_audio_open(NULL, &s.audio) == 0) {
                for (int i = 0; i < SOUNDS; ++i)
                        if (path_in(path, sizeof(path), s.dir, SOUND_FILES[i]) != 0 ||
                            bmds_game_sound_load(s.audio, path, &s.sfx[i]) < 0)
                                fprintf(stderr, "solitaire: %s: not loaded\n", path);
        }
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        srand((unsigned)ns);
        s.last_press_card = -1;
        deal(&s);

        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = WIDTH;
        config.height = HEIGHT;
        config.format = BMDS_PIXEL_RGB565;
        config.flags = full ? BMDS_SURFACE_FULL_SCREEN : 0;
        config.title = "Solitaire";
        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        err = bmds_game_run(&config);
        bmds_game_audio_close(s.audio);
        for (int i = 0; i < SOUNDS; ++i) bmds_game_sound_free(&s.sfx[i]);
        bmds_game_image_free(&s.cards);
        if (err < 0) {
                fprintf(stderr, "solitaire: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
