/**
 * @file      solitaire.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Solitaire (devplan/games.md §21, devplan/luckyduck.md §8a): Klondike, as it was on
 * its own, at the casino's size (a maximized window's) with its cards. The rules are solitaire_rules.c's; this is the
 * picture (each frame redrawing only where a card or mark differs from the last, from the felt),
 * cards flying where the game sends them, the mouse and the keyboard, the options, the bouncing
 * cards of a win, and Vegas scoring played from the bank.
 *
 *     luckyduck solitaire [-W]     -W a game played out, finishing by itself (the win's demo)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solitaire_rules.h"
#include "ui.h"

#define CW ART_CARD_W
#define CH ART_CARD_H
/* The columns centred between the left edge and the system buttons. */
#define COL_X(c) ((WIDTH - 158) / 2 - 500 + (c) * 150)
#define TOP_Y 16
#define TABLEAU_Y 180
#define STATUS_Y (HEIGHT - 44)
#define FAN_X 22     /* the waste's fan, drawing three */
#define DOWN_STEP 10 /* a column's face-down cards */
#define UP_STEP 36   /* its face-up cards, closer when it would reach the status line */
#define MIN_STEP 14
#define FLIGHT_TICKS 9
#define DRAG_START 4 /* pixels moved before a press drags */
#define DOUBLE_NS 400000000LL

#define TILE_BACK ART_CARD_BACK
#define TILE_EMPTY ART_CARD_EMPTY
#define TILE_HOME ART_CARD_HOME
#define TILE_AGAIN ART_CARD_AGAIN
#define TILE_DONE ART_CARD_DONE

#define BAND 0x08301c
#define TEXT C_TEXT
#define DIM C_DIM
#define CURSOR C_CURSOR
#define HELD 0xffd020

#define S_PLACE SFX_PLACE
#define S_FLIP SFX_FLIP
#define S_DEAL SFX_DEAL
#define S_WIN SFX_WIN

/* The buttons: the system's stacked at the top right, the game's (undo) in the status band. */
enum {
        B_LOBBY,
        B_NEW,
        B_OPTIONS,
        B_HELP,
        B_UNDO,
        BUTTONS
};
static ui_button_t BUTTON[BUTTONS];

static void place_buttons(void) {
        static const char *const label[BUTTONS] = {"LOBBY", "NEW GAME", "OPTIONS", "HELP", "UNDO"};
        static const uint32_t    key[BUTTONS] = {0, K_N, K_O, K_F1, K_U};
        for (int i = 0; i < BUTTONS; ++i) {
                BUTTON[i].r = i < B_UNDO ? app_system_button(i) : rect(WIDTH - 136, STATUS_Y + 6, 120, 34);
                BUTTON[i].label = label[i];
                BUTTON[i].key = key[i];
        }
        return;
}

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
        BOX_OPTIONS,
        BOX_WON
};

#define MAX_DIRTY 24

typedef struct {
                solitaire_t game;
                uint32_t    draw, scoring, played, won;
                int32_t     vegas_synced;       /* the Vegas score the bank has been paid to */
                int         hover, shown_hover; /* the status band's button under the pointer */
                /* What the last frame showed, and what to redraw. */
                view_t      shown;
                int         redraw_all;
                bmds_rect_t dirty[MAX_DIRTY];
                uint32_t    dirty_count;
                bmds_rect_t shown_cursor, shown_held; /* width 0: none */
                int         shown_box;
                uint32_t    box_version, shown_box_version;
                char        shown_status[160];
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
} state_t;

static state_t s_state;

/* ---- Settings, in the casino's */

static void load_settings(state_t *s) {
        s->draw = setting("solitaire.draw", 1) == 3 ? 3 : 1;
        s->scoring = (uint32_t)setting("solitaire.scoring", SOL_SCORE_STANDARD);
        if (s->scoring >= SOL_SCORINGS) s->scoring = SOL_SCORE_STANDARD;
        s->played = (uint32_t)setting("solitaire.played", 0);
        s->won = (uint32_t)setting("solitaire.won", 0);
        return;
}

static void save_settings(const state_t *s) {
        set_setting("solitaire.draw", s->draw);
        set_setting("solitaire.scoring", s->scoring);
        set_setting("solitaire.played", s->played);
        set_setting("solitaire.won", s->won);
        return;
}

/* ---- Sound */

static void play(state_t *s, int sound, float volume) {
        (void)s;
        sfx(sound, volume);
        return;
}

/* ---- Vegas: the score is the bank's (devplan/luckyduck.md §7) */

static void vegas_sync(state_t *s) {
        /* A demo (-W) isn't a game: it neither costs nor pays. */
        if (s->game.scoring != SOL_SCORE_VEGAS || !s->counted || s->game.score == s->vegas_synced) return;
        bank_add(s->game.score - s->vegas_synced);
        s->vegas_synced = s->game.score;
        return;
}

/* ---- Rectangles */

/* Adds @p r to what the next frame redraws. */
static void dirty(state_t *s, bmds_rect_t r) {
        dirty_add(s->dirty, &s->dirty_count, MAX_DIRTY, r);
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

/* Whether flight @p f is waiting (a face-down card yet to leave) under another waiting at the
   same place: not drawn, as it can't be seen (a deal's cards wait on the stock: each drawn there
   would cost every redraw near it a whole card). */
static int hidden_waiting(const state_t *s, uint32_t f) {
        const flight_t *fl = &s->flight[f];
        if (fl->delay == 0 || fl->ticks != 0) return 0;
        for (uint32_t g = f + 1; g < s->flights; ++g) {
                const flight_t *o = &s->flight[g];
                if (o->delay > 0 && o->ticks == 0 && o->fx == fl->fx && o->fy == fl->fy) return 1;
        }
        return 0;
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
                if (hidden_waiting(s, f)) continue;
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
        land_all(s);
        s->auto_play = 0;
        s->counted = !s->demo;
        if (s->demo) {
                sol_deal_finished(&s->game, s->draw, s->scoring);
                s->game.started = 1;
                s->auto_play = 1;
                s->demo = 0;
        } else {
                if (s->scoring == SOL_SCORE_VEGAS && bank_ensure(52)) play(s, SFX_SAD, 0.5f); /* Lucky tops it up */
                sol_deal(&s->game, s->draw, s->scoring, app_key());
        }
        s->vegas_synced = 0;
        vegas_sync(s);
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
        /* Before the first move, dealt again at once; else from the next deal (the game under way
           isn't ended for it). */
        if (!s->game.started) deal(s);
        s->box = BOX_OPTIONS;
        ++s->box_version;
        return;
}

static const char *const HELP[] = {
        "Klondike: build the four suits up from the Aces.",
        "The columns build down in alternating colours; only a",
        "King goes to an empty column. Draw from the stock, one",
        "card or three (Options), and play the waste's top card.",
        "",
        "Mouse: drag cards; a double click sends a card up;",
        "a right click sends up everything that can go.",
        "",
        "Arrows       move the cursor",
        "Space/Enter  draw, pick up, put down",
        "D  draw      F  this card up     A  all up",
        "Esc          let go (or back to the lobby)",
        "Ctrl+Z/U     undo",
        "N/F2  new game   O  options   M  sound   F1  help",
        "",
        "Scoring: standard (Windows'), Vegas (a deal costs $52 from",
        "the bank, each card sent up pays $5), or none.",
};

static void show_help(void) {
        app_info("SOLITAIRE", HELP, (int)(sizeof(HELP) / sizeof(HELP[0])));
        return;
}

static void deal_now(void) {
        deal(&s_state);
        return;
}

/* A new game: asked first when one is under way (it can't be come back to). */
static void new_game(state_t *s) {
        if (s->game.started && !s->game.won && !s->winning)
                app_confirm("START A NEW GAME?", "The game under way ends if you deal again.", "NEW GAME", deal_now);
        else
                deal(s);
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
                sound_toggle();
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
        if (usage == K_ESCAPE && s->held_pile < 0) {
                app_leave();
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
                case K_F2: new_game(s); return;
                case K_O: s->box = BOX_OPTIONS; break;
                case K_H:
                case K_F1: show_help(); break;
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
        s->hover = ui_button_at(BUTTON, BUTTONS, x, y);
        if (s->winning) {
                if (pressed != 0) deal(s);
                return;
        }
        if (s->box != BOX_NONE) {
                if (pressed != 0) s->box = BOX_NONE;
                return;
        }
        if ((pressed & 1u) != 0 && s->hover >= 0 && !s->dragging) {
                play(s, SFX_CLICK, 0.6f);
                land_all(s);
                s->auto_play = 0;
                switch (s->hover) {
                        case B_LOBBY: app_leave(); return;
                        case B_NEW: new_game(s); return;
                        case B_UNDO:
                                s->held_pile = -1;
                                if (sol_undo(&s->game) == 0) play(s, S_FLIP, 0.5f);
                                return;
                        case B_OPTIONS: s->box = BOX_OPTIONS; return;
                        default: show_help(); return;
                }
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

static void scene_event(const bmds_surface_event_t *e) {
        state_t *s = &s_state;
        switch (e->type) {
                case BMDS_EVENT_POINTER: on_pointer(s, e); break;
                case BMDS_EVENT_KEY:
                        if ((e->flags & BMDS_KEY_DOWN) != 0) on_key(s, e);
                        break;
                default: break;
        }
        return;
}

static void scene_tick(void) {
        state_t *s = &s_state;
        vegas_sync(s);
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
        (void)s;
        ui_card(c, x, y, tile);
        return;
}

#define FIELDS 4
#define FIELD_W 220 /* each field its own column: a number changing moves nothing after it */

/* The status band's fields: the score (and the bank, Vegas), the moves, the time last. @return How many. */
static int status_fields(const state_t *s, char out[FIELDS][32]) {
        const solitaire_t *g = &s->game;
        int                n = 0;
        if (g->scoring == SOL_SCORE_STANDARD) {
                snprintf(out[n++], 32, "SCORE: %d", (int)g->score);
        } else if (g->scoring == SOL_SCORE_VEGAS) {
                char amount[24];
                money(amount, sizeof(amount), g->score);
                snprintf(out[n++], 32, "SCORE: %s", amount);
                money(amount, sizeof(amount), bank_balance());
                snprintf(out[n++], 32, "BANK: %s", amount);
        }
        snprintf(out[n++], 32, "MOVES: %u", (unsigned)g->moves);
        snprintf(out[n++], 32, "TIME: %u:%02u", (unsigned)(g->seconds / 60), (unsigned)(g->seconds % 60));
        return n;
}

/* The fields as one text: what changed since the last frame. */
static void status_text(const state_t *s, char *out, size_t size) {
        char      f[FIELDS][32];
        const int n = status_fields(s, f);
        out[0] = '\0';
        for (int i = 0; i < n; ++i) {
                const size_t used = strlen(out);
                snprintf(out + used, size - used, "%s|", f[i]);
        }
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
        return rect(WIDTH / 2 - 300, HEIGHT / 2 - 190, 600, 380);
}

#define BOX_X (WIDTH / 2 - 250)

/* A line @p y down the box as laid out at y 150 (the box at the picture's middle). */
static void box_line(bmds_canvas_t *c, int32_t y, const char *text, uint32_t rgb) {
        ui_line(c, BOX_X, y - 150 + box_rect().y, text, rgb);
        return;
}

static void draw_box(bmds_canvas_t *c, const state_t *s) {
        const bmds_rect_t r = box_rect();
        char              line[80];
        ui_panel(c, r);
        if (s->box == BOX_OPTIONS) {
                static const char *const scoring[] = {"standard", "Vegas (from the bank)", "none"};
                ui_text_centred(c, ART_FONT_MED, WIDTH / 2, r.y + 24, "OPTIONS");
                snprintf(line, sizeof(line), "1 / 3    draw %s", s->draw == 3 ? "three" : "one");
                box_line(c, 226, line, TEXT);
                snprintf(line, sizeof(line), "S        scoring: %s", scoring[s->scoring]);
                box_line(c, 252, line, TEXT);
                snprintf(line, sizeof(line), "M        sound %s", sound_on() ? "on" : "off");
                box_line(c, 278, line, TEXT);
                box_line(c, 314, "A change of draw or scoring starts with the next deal.", DIM);
                box_line(c, 336, "Vegas: a deal costs $52 from the bank, each card up pays $5.", DIM);
                snprintf(line, sizeof(line), "Games won: %u of %u", (unsigned)s->won, (unsigned)s->played);
                box_line(c, 384, line, TEXT);
                box_line(c, 480, "Esc or a click: close", DIM);
        } else {
                ui_text_centred(c, ART_FONT_BIG, WIDTH / 2, r.y + 44, "YOU WON!");
                if (s->game.scoring != SOL_SCORE_NONE) {
                        char amount[24];
                        if (s->game.scoring == SOL_SCORE_VEGAS)
                                money(amount, sizeof(amount), s->game.score);
                        else
                                snprintf(amount, sizeof(amount), "%d", (int)s->game.score);
                        snprintf(line, sizeof(line), "Score %s in %u:%02u", amount, (unsigned)(s->game.seconds / 60),
                                 (unsigned)(s->game.seconds % 60));
                        box_line(c, 300, line, TEXT);
                }
                snprintf(line, sizeof(line), "Games won: %u of %u", (unsigned)s->won, (unsigned)s->played);
                box_line(c, 326, line, TEXT);
                box_line(c, 480, "Click or press a key to deal again", DIM);
        }
        return;
}

/* The status band: the score, the time, the moves, and undo. */
static void draw_band(bmds_canvas_t *c, const state_t *s) {
        char text[160];
        status_text(s, text, sizeof(text));
        (void)text;
        char      f[FIELDS][32];
        const int n = status_fields(s, f);
        for (int i = 0; i < n; ++i) (void)ui_text(c, ART_FONT_SMALL, 20 + i * FIELD_W, STATUS_Y + 18, f[i]);
        ui_button(c, &BUTTON[B_UNDO], s->hover == B_UNDO ? UI_HOVER : UI_NORMAL);
        return;
}

/* The system's buttons at the top right, those inside @p r. */
static void draw_system(bmds_canvas_t *c, const state_t *s, bmds_rect_t r) {
        for (int i = 0; i < B_UNDO; ++i)
                if (meets(&BUTTON[i].r, &r)) ui_button(c, &BUTTON[i], s->hover == i ? UI_HOVER : UI_NORMAL);
        return;
}

/* Everything inside @p r, from the felt up. */
static void draw_scene(bmds_canvas_t *c, const state_t *s, const view_t *v, bmds_rect_t r) {
        bmds_canvas_set_clip(c, &r);
        r = c->clip;
        /* Each card drawn only where no card over it hides it, the felt likewise (columns are
           mostly cards under cards). */
        bmds_rect_t cards[KEYS];
        int         order[KEYS], n = 0;
        for (int k = 0; k < v->count; ++k) {
                const item_t     *it = &v->item[v->order[k]];
                const bmds_rect_t cr = rect(it->x, it->y, CW, CH);
                if (!meets(&cr, &r)) continue;
                order[n] = v->order[k];
                cards[n++] = it->tile < TILE_EMPTY ? cr : rect(0, 0, 0, 0); /* the places: see-through */
        }
        felt_restore(c, ui_uncovered(r, cards, n));
        const bmds_rect_t band = rect(0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y);
        if (meets(&r, &band)) draw_band(c, s);
        for (int k = 0; k < n; ++k) {
                const item_t     *it = &v->item[order[k]];
                const bmds_rect_t shows =
                        ui_uncovered(intersection(rect(it->x, it->y, CW, CH), r), cards + k + 1, n - k - 1);
                if (shows.width == 0) continue;
                bmds_canvas_set_clip(c, &shows);
                blit_tile(c, s, it->x, it->y, it->tile);
        }
        bmds_canvas_set_clip(c, &r);
        const bmds_rect_t held = held_rect(s), cursor = cursor_rect(s);
        if (held.width > 0 && meets(&held, &r)) outline(c, held, HELD);
        if (cursor.width > 0 && meets(&cursor, &r)) outline(c, cursor, CURSOR);
        draw_system(c, s, r);
        const bmds_rect_t box = box_rect();
        if (s->box != BOX_NONE && meets(&box, &r)) draw_box(c, s);
        bmds_canvas_set_clip(c, NULL);
        return;
}

static int same(const item_t *a, const item_t *b) {
        return a->on == b->on && (!a->on || (a->x == b->x && a->y == b->y && a->tile == b->tile &&
                                             a->layer == b->layer && a->pile == b->pile && a->index == b->index));
}

static void draw_win(bmds_canvas_t *c, state_t *s) {
        if (s->redraw_all) { /* the trails are lost: the felt and the foundations again */
                felt_restore(c, rect(0, 0, c->width, c->height));
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
                felt_restore(c, rect(x, y, CW, CH));
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

static void scene_draw(bmds_canvas_t *c) {
        state_t *s = &s_state;
        bmds_game_up_to_date(app.game); /* the win's trails are never erased: what is there counts */
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
        char text[160];
        status_text(s, text, sizeof(text));
        if (s->hover != s->shown_hover) { /* the buttons lit and unlit */
                if (s->shown_hover >= 0) dirty(s, BUTTON[s->shown_hover].r);
                if (s->hover >= 0) dirty(s, BUTTON[s->hover].r);
                s->shown_hover = s->hover;
        }
        if (strcmp(text, s->shown_status) != 0) {
                dirty(s, rect(0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y));
                snprintf(s->shown_status, sizeof(s->shown_status), "%s", text);
        }
        for (uint32_t i = 0; i < s->dirty_count; ++i) draw_scene(c, s, &now, s->dirty[i]);
        s->dirty_count = 0;
        s->shown = now;
        return;
}

static void scene_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        state_t *s = &s_state;
        bmds_game_up_to_date(app.game);
        if (s->winning) { /* the trails are lost */
                s->redraw_all = 1;
                draw_win(c, s);
                return;
        }
        static view_t now;
        layout(s, &now);
        draw_scene(c, s, &now, r);
        return;
}

/* ---- The scene */

static int scene_enter(int argc, char **argv) {
        state_t *s = &s_state;
        s->demo = 0;
        for (int i = 0; i < argc; ++i)
                if (strcmp(argv[i], "-W") == 0) s->demo = 1;
        load_settings(s);
        felt_baize(0x1c9450, 0x0a4a26);
        bmds_canvas_fill_blend(&app.felt_canvas, 0, STATUS_Y, WIDTH, HEIGHT - STATUS_Y, 0x000000, 120);
        bmds_canvas_fill(&app.felt_canvas, 0, STATUS_Y, WIDTH, 2, bmds_canvas_pixel(&app.felt_canvas, 0xb08028));
        bmds_canvas_damage_clear(&app.felt_canvas);
        place_buttons();
        s->last_press_card = -1;
        s->hover = s->shown_hover = -1;
        s->buttons = 0;
        s->cursor_on = 0;
        deal(s);
        /* What shows is drawn by the transition (or the first frame's redraw): this frame is the last's. */
        s->redraw_all = 0;
        layout(s, &s->shown);
        s->shown_box = s->box;
        status_text(s, s->shown_status, sizeof(s->shown_status));
        return 0;
}

static void scene_leave(void) {
        state_t *s = &s_state;
        vegas_sync(s);
        save_settings(s);
        return;
}

static int scene_busy(void) {
        const state_t *s = &s_state;
        return s->game.started && !s->game.won && !s->winning;
}

const scene_t solitaire_scene = {
        "solitaire", "Solitaire", scene_enter,  scene_leave, scene_event,
        scene_tick,  scene_draw,  scene_redraw, scene_busy,
};
