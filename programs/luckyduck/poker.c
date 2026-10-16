/**
 * @file      poker.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Poker (devplan/luckyduck.md §8e, §25): an oval table of six seats, the player at
 * the bottom, up to five bot ducks round it, Lucky dealing from the top left. Hold'em, Draw and
 * both studs, as a cash game or a sit-and-go. The rules are poker_rules.c's; this scene replays
 * their log one event at a time into its own model of the table (the picture lags the rules,
 * each event's animation holding the next), so what it draws comes only from what the player's
 * seat may see. The ducks decide from their own seats' views (poker_bot.c), their equity worked
 * out over frames while they think.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hand.h"
#include "poker_bot.h"
#include "stage.h"
#include "ui.h"

#define ME 0
#define BAND_H 64
#define BAND_Y (HEIGHT - BAND_H)
#define CARDS 80
#define NONE 0xffu
#define FLIGHT 11
#define FLIP 8
#define MOVE 10
#define PILE_FLIGHT 14
#define FLYING 24
#define BOT_THINK 34
#define BOT_SCALE 160 /* the ducks' cards, of 256 */
#define BOARD_SCALE 216
#define EQ_TRIALS 1000
#define EQ_SLICE_NS 5000000 /* the equity's share of a frame */
#define GLOWS 12
#define TOURNEY_FEE 100
#define TOURNEY_CHIPS 1500
#define LEVEL_HANDS 8

/* Stage keys. */
enum {
        K_CARD = 0,          /* + the card sprite */
        K_GLOW = 80,         /* + 12 */
        K_SHADE = 92,        /* + 5: the player's discards dimmed */
        K_DUCK = 97,         /* + seat */
        K_DUCK_SHADE = 103,  /* + seat */
        K_NAME = 109,        /* + seat */
        K_STACK = 115,       /* + seat */
        K_BUBBLE = 121,      /* + seat */
        K_BUBBLE_TEXT = 127, /* + seat */
        K_HEADLINE = 133,    /* + seat */
        K_BET = 139,         /* + seat */
        K_BET_TEXT = 145,    /* + seat */
        K_FLYING = 151,      /* + 24 */
        K_POT = 175,
        K_POT_TEXT,
        K_DEALER,
        K_LUCKY,
        K_DECK = 179, /* + 3 */
        K_ARROW = 182,
        K_BAND = 183,   /* + 6 */
        K_BUTTON = 189, /* + 14 */
        K_MESSAGE = 203,
        K_LUCKY_BUBBLE,
        K_LUCKY_TEXT,
        K_LUCKY_TEXT2,
        K_BOX = 210 /* + 16 */
};

/* A card on the table: where it is going, and how it moves there. */
typedef struct {
                uint8_t  used;
                uint8_t  card;  /* its face (PK_HIDDEN while unknown) */
                uint8_t  shown; /* the face shown (it turns at a flip's middle) */
                uint8_t  up;    /* to show its face once landed */
                uint8_t  z;
                int16_t  tx, ty; /* where it is going (top-left) */
                uint16_t tscale; /* its size there, of 256 */
                int16_t  fx, fy; /* where it moved from */
                uint16_t fscale;
                uint16_t t, dur, delay;
                uint16_t spin;
                uint8_t  flip;
                uint8_t  placed;
                uint8_t  discard; /* going to the deck, then gone */
} card_t;

/* A seat as the picture shows it. */
typedef struct {
                uint8_t  ids[PK_CARDS]; /* its cards' sprites (NONE: none there) */
                uint8_t  count;
                uint8_t  in, folded, all_in, out;
                uint8_t  down;          /* bit i: card i dealt face down */
                uint16_t glow;          /* bit i: card i makes the winning hand */
                int32_t  bet, bet_want; /* in front of it: landed, and once the chips flying land */
                int32_t  stack;
                uint8_t  pose;
                uint16_t pose_ticks;
                char     bubble[24];
                uint16_t bubble_ticks;
                char     headline[20];
                uint16_t pop;
} mseat_t;

/* A pile of chips sliding somewhere. */
typedef struct {
                uint8_t  used;
                uint8_t  to; /* what it lands on */
                int8_t   seat;
                int32_t  amount;
                int16_t  fx, fy, tx, ty;
                uint16_t t, dur, delay;
} flying_t;

enum {
        TO_NOTHING,
        TO_BET,
        TO_POT,
        TO_STACK
};

enum {
        POSE_IDLE,
        POSE_THINK,
        POSE_HAPPY,
        POSE_SAD
};

/* The buttons: the system's (top right), the game's (the band) and the bet's size (the felt). */
enum {
        B_LOBBY,
        B_RULES,
        B_OPTIONS,
        B_NEW,
        B_HINT,
        B_REBUY,
        B_DEAL,
        B_FOLD,
        B_CALL,
        B_RAISE,
        B_DRAW,
        B_LESS,
        B_MORE,
        B_HALF,
        B_POT,
        B_ALL,
        BUTTONS
};

static const char *const BUTTON_LABEL[BUTTONS] = {"LOBBY", "RULES",    "OPTIONS", "NEW TABLE", "HINT", "REBUY",
                                                  "DEAL",  "FOLD",     "CALL",    "RAISE",     "DRAW", "-",
                                                  "+",     "HALF POT", "POT",     "ALL IN"};
/* Their keys: F1 the rules, Enter deals (and draws), T a tip, A all in; Up and Down, the size. */
static const uint32_t BUTTON_KEY[BUTTONS] = {0,   K_F1, K_O,     K_N,    K_T,  K_B, K_ENTER, K_F,
                                             K_C, K_R,  K_ENTER, K_DOWN, K_UP, K_H, K_P,     K_A};

/* The ducks, a seat each (1 to 5 round the table from the player's left). */
static const char *const DUCK_NAME[PK_SEATS] = {"YOU", "MALLORY", "PEARL", "WOODY", "SQUEAKY", "INKY"};
static const pk_style_t  DUCK_STYLE[PK_SEATS] = {
        {50, 50, 20}, {30, 70, 30}, /* Mallory: tight, aggressive */
        {70, 30, 10},               /* Pearl: plays everything, calls */
        {50, 50, 40},               /* Woody: in between, a bluffer */
        {85, 85, 60},               /* Squeaky: a maniac */
        {15, 45, 5},                /* Inky: a rock */
};

static const char *const GAME_NAME[] = {"HOLD'EM NO LIMIT", "HOLD'EM LIMIT", "FIVE-CARD DRAW", "FIVE-CARD STUD",
                                        "SEVEN-CARD STUD"};
#define GAME_CHOICES 5
static const int32_t STAKES[] = {2, 4, 10}; /* the big blind */
#define STAKE_CHOICES 3
static const int32_t LEVELS[] = {20, 30, 50, 100, 150, 200, 300, 400, 600, 800, 1000, 1500, 2000, 3000, 4000, 6000};
#define LEVEL_COUNT (int)(sizeof(LEVELS) / sizeof(LEVELS[0]))
#define OPTION_LINES 4

static struct {
                pk_table_t table;
                stage_t    stage;
                deck_rng_t rng; /* the ducks' whims */
                /* The settings: the game (GAME_NAME's), a tournament, the ducks, the stakes. */
                int choice, tournament, ducks, stake;
                /* The picture's model. */
                card_t   card[CARDS];
                mseat_t  seat[PK_SEATS];
                uint8_t  board[PK_BOARD], board_count;
                uint8_t  board_glow;
                int32_t  pot, pot_want;
                int8_t   turn;
                int8_t   button;
                int16_t  dealer_fx, dealer_fy, dealer_tx, dealer_ty;
                uint16_t dealer_t;
                flying_t flying[FLYING];
                uint32_t cursor; /* the next event to show */
                uint16_t wait;
                int      glows;
                /* The ducks' thinking. */
                pk_equity_t eq;
                int8_t      eq_seat; /* whose equity is under way (or ME's, for the hint) */
                uint8_t     eq_done;
                uint16_t    think;
                /* The player. */
                int32_t  synced; /* cash: the stack the bank was last written from */
                int32_t  buy_in;
                int32_t  raise_to;
                uint8_t  discards; /* Draw: the cards marked to throw */
                uint8_t  hand_over;
                uint8_t  finished; /* a tournament over for the player */
                int      level;
                uint16_t hint_ticks;
                char     hint[32];
                /* Lucky. */
                uint16_t lucky_deal, lucky_mood;
                int      lucky_pose;
                /* Input. */
                int         buttons[BUTTONS];
                int         button_count;
                ui_button_t button_at[BUTTONS];
                int         hover;
                int         options;
                uint16_t    message_ticks;
                char        message[36];
                uint32_t    ticks;
} p;

/* ---- Geometry */

static int32_t table_cx(void) {
        return WIDTH / 2;
}
static int32_t table_cy(void) {
        return BAND_Y / 2 + 6;
}
static int32_t table_rx(void) {
        return WIDTH * 37 / 100;
}
#define TABLE_RY 200

/* Each seat's places, worked out on entering. */
static struct {
                int32_t px, py;         /* on the rail */
                int32_t duck_x, duck_y; /* the duck's top-left */
                int32_t hand_x, hand_y; /* its cards' middle */
                int32_t bet_x, bet_y;   /* its bet pile's (bottom chip's top-left) */
                int32_t button_x, button_y;
                int     mirror;
} seat_at[PK_SEATS];

static void place_seats(void) {
        static const float angle[PK_SEATS] = {90, 150, 210, 270, 330, 30};
        const float        cx = (float)table_cx(), cy = (float)table_cy();
        for (int s = 0; s < PK_SEATS; ++s) {
                const float a = angle[s] * 3.14159265f / 180.0f;
                const float x = cx + (float)table_rx() * cosf(a), y = cy + (float)TABLE_RY * sinf(a);
                float       ux = cx - x, uy = cy - y;
                const float len = sqrtf(ux * ux + uy * uy);
                ux /= len;
                uy /= len;
                seat_at[s].px = (int32_t)x;
                seat_at[s].py = (int32_t)y;
                const float hand = s == 3 ? 58.0f : 70.0f, bet = s == 3 ? 0.0f : 150.0f;
                seat_at[s].duck_x = (int32_t)(x - ux * 52.0f) - 42;
                seat_at[s].duck_y = (int32_t)(y - uy * 52.0f) - 42 + (s == 4 ? 28 : 0);
                seat_at[s].hand_x = (int32_t)(x + ux * hand);
                seat_at[s].hand_y = (int32_t)(y + uy * hand);
                seat_at[s].bet_x = (int32_t)(x + ux * bet) - 24;
                seat_at[s].bet_y = (int32_t)(y + uy * bet) - 15;
                seat_at[s].mirror = s >= 4;
                /* The dealer's button beside the bet, along the rail. */
                seat_at[s].button_x = (int32_t)(x + ux * 110.0f - uy * 60.0f) - 22;
                seat_at[s].button_y = (int32_t)(y + uy * 110.0f + ux * 60.0f) - 15;
        }
        /* The player: its cards at full size at the bottom, its bet in front of them. */
        seat_at[ME].hand_y = BAND_Y - 20 - ART_CARD_H / 2;
        if (p.table.rules.game == PK_STUD5 || p.table.rules.game == PK_STUD7)
                seat_at[ME].hand_y -= 16; /* its down cards sit lower */
        seat_at[ME].bet_x = table_cx() - 24;
        seat_at[ME].bet_y = seat_at[ME].hand_y - ART_CARD_H / 2 - 36;
        seat_at[ME].button_x = table_cx() + 150;
        seat_at[ME].button_y = seat_at[ME].bet_y;
        /* The side seats' bets above (the lower seats) or below (the upper) their cards, towards
           the middle: beyond the cards inwards is the board. */
        for (int s = 1; s < PK_SEATS; ++s) {
                if (s == 3) continue;
                const int left = s < 3, lower = s == 1 || s == 5;
                seat_at[s].bet_x = left ? seat_at[s].hand_x + 10 : seat_at[s].hand_x - 58;
                seat_at[s].bet_y = lower ? seat_at[s].hand_y - 82 : seat_at[s].hand_y + 50;
        }
        /* The top seat's bet beside its cards (the pot is under them), its button the other side. */
        seat_at[3].bet_x = seat_at[3].hand_x + 70;
        seat_at[3].bet_y = seat_at[3].hand_y + 10;
        seat_at[3].button_x = seat_at[3].hand_x - 70 - 44;
        seat_at[3].button_y = seat_at[3].hand_y + 10;
        return;
}

static int32_t board_y(void) {
        return table_cy() + 16 - ART_CARD_H * BOARD_SCALE / 512;
}

static void board_place(int i, int32_t *x, int32_t *y) {
        const int32_t w = ART_CARD_W * BOARD_SCALE / 256, gap = 8;
        *x = table_cx() - (5 * w + 4 * gap) / 2 + i * (w + gap);
        *y = board_y();
        return;
}

static void pot_place(int32_t *x, int32_t *y) {
        *x = table_cx() - 24;
        *y = board_y() - 34;
        return;
}

static void deck_place(int32_t *x, int32_t *y) {
        *x = 140;
        *y = 40;
        return;
}

/* Where the player's chips come from and go to: its chips' amount in the band. */
static void bank_point(int32_t *x, int32_t *y) {
        *x = 40;
        *y = BAND_Y + 6;
        return;
}

static void stack_point(int s, int32_t *x, int32_t *y) {
        if (s == ME) {
                bank_point(x, y);
                return;
        }
        *x = seat_at[s].duck_x + 18;
        *y = seat_at[s].duck_y + 50;
        return;
}

static uint16_t seat_scale(int s) {
        return s == ME ? 256 : BOT_SCALE;
}

/* The step between a seat's @p n cards: a duck's spread wider the fewer it has. */
static int32_t seat_step(int s, int n) {
        if (s != ME) return n <= 2 ? 40 : n <= 5 ? 28 : 20;
        const int game = p.table.rules.game;
        return game == PK_STUD7 || game == PK_STUD5 ? 44 : 108;
}

/* Card @p i of @p n of seat @p s: its top-left. */
static void seat_card_place(int s, int i, int n, int32_t *x, int32_t *y) {
        const uint16_t k = seat_scale(s);
        const int32_t  w = ART_CARD_W * k / 256, h = ART_CARD_H * k / 256, step = seat_step(s, n);
        const int slots = s == ME ? pk_final_cards(p.table.rules.game) - (p.table.rules.game == PK_HOLDEM ? 5 : 0) : n;
        const int32_t width = w + step * (slots - 1);
        *x = seat_at[s].hand_x - width / 2 + i * step;
        *y = seat_at[s].hand_y - h / 2;
        const mseat_t *m = &p.seat[s];
        if (s == ME && (m->down & (1u << i)) && p.table.rules.game != PK_HOLDEM && p.table.rules.game != PK_DRAW)
                *y += 16; /* stud: what only the player sees sits lower */
        if (s == ME && (p.discards & (1u << i))) *y += 10;
        if (m->glow & (1u << i)) *y -= 12;
        return;
}

/* ---- Easing */

static int32_t ease(int32_t from, int32_t to, uint32_t t, uint32_t n) {
        if (t >= n) return to;
        const int32_t k = (int32_t)n, u = (int32_t)t;
        return from + (to - from) * (2 * k * u - u * u) / (k * k);
}

/* ---- Cards */

static int new_card(uint8_t face) {
        for (int i = 0; i < CARDS; ++i) {
                if (p.card[i].used) continue;
                card_t *c = &p.card[i];
                memset(c, 0, sizeof(*c));
                c->used = 1;
                c->card = face;
                c->shown = PK_HIDDEN;
                c->up = face != PK_HIDDEN;
                int32_t x, y;
                deck_place(&x, &y);
                c->fx = c->tx = (int16_t)x;
                c->fy = c->ty = (int16_t)y;
                c->fscale = c->tscale = 128;
                return i;
        }
        return -1;
}

static void card_now(const card_t *c, int32_t *x, int32_t *y, uint16_t *scale) {
        const uint16_t t = c->t;
        *x = ease(c->fx, c->tx, t, c->dur);
        *y = ease(c->fy, c->ty, t, c->dur);
        *scale = (uint16_t)ease(c->fscale, c->tscale, t, c->dur);
        return;
}

static void card_to(int id, int32_t x, int32_t y, uint16_t scale, uint16_t dur) {
        card_t *c = &p.card[id];
        if (c->placed && c->tx == x && c->ty == y && c->tscale == scale) return;
        int32_t  nx, ny;
        uint16_t ns;
        card_now(c, &nx, &ny, &ns);
        c->fx = (int16_t)nx;
        c->fy = (int16_t)ny;
        c->fscale = ns;
        c->tx = (int16_t)x;
        c->ty = (int16_t)y;
        c->tscale = scale;
        c->t = 0;
        c->dur = c->placed ? MOVE : dur;
        c->placed = 1;
        return;
}

static void discard_card(int id) {
        if (id == NONE || !p.card[id].used) return;
        p.card[id].discard = 1;
        p.card[id].up = 0;
        p.card[id].flip = 0;
        p.card[id].shown = PK_HIDDEN;
        return;
}

static void place_cards(void) {
        for (int s = 0; s < PK_SEATS; ++s) {
                const mseat_t *m = &p.seat[s];
                for (int i = 0; i < m->count; ++i) {
                        if (m->ids[i] == NONE) continue;
                        int32_t x, y;
                        seat_card_place(s, i, m->count, &x, &y);
                        card_to(m->ids[i], x, y, seat_scale(s), FLIGHT);
                        p.card[m->ids[i]].z = (uint8_t)(30 + i);
                }
        }
        for (int i = 0; i < p.board_count; ++i) {
                int32_t x, y;
                board_place(i, &x, &y);
                if (p.board_glow & (1u << i)) y -= 12;
                card_to(p.board[i], x, y, BOARD_SCALE, FLIGHT);
                p.card[p.board[i]].z = 30;
        }
        int32_t dx, dy;
        deck_place(&dx, &dy);
        for (int i = 0; i < CARDS; ++i)
                if (p.card[i].used && p.card[i].discard) card_to(i, dx, dy, 128, 14);
        return;
}

static void cards_tick(void) {
        for (int i = 0; i < CARDS; ++i) {
                card_t *c = &p.card[i];
                if (!c->used) continue;
                if (c->delay > 0) {
                        --c->delay;
                        continue;
                }
                if (c->t < c->dur) {
                        ++c->t;
                        c->spin = c->t < c->dur && !c->discard ? (uint16_t)(c->spin + 2900) : 0;
                        if (c->t == c->dur && c->up && c->shown != c->card) c->flip = FLIP;
                } else if (c->discard) {
                        c->used = 0;
                }
                if (c->flip > 0) {
                        --c->flip;
                        if (c->flip == FLIP / 2) {
                                c->shown = c->card;
                                sfx(SFX_FLIP, 0.25f);
                        }
                }
        }
        return;
}

static int cards_still(void) {
        for (int i = 0; i < CARDS; ++i) {
                const card_t *c = &p.card[i];
                if (c->used && !c->discard && (c->t < c->dur || c->flip > 0 || c->delay > 0)) return 0;
        }
        return 1;
}

/* ---- Chips moving */

static void fly(int32_t amount, int32_t fx, int32_t fy, int32_t tx, int32_t ty, uint16_t delay, int to, int seat) {
        if (amount <= 0) return;
        for (int i = 0; i < FLYING; ++i) {
                flying_t *f = &p.flying[i];
                if (f->used) continue;
                f->used = 1;
                f->amount = amount;
                f->fx = (int16_t)fx;
                f->fy = (int16_t)fy;
                f->tx = (int16_t)tx;
                f->ty = (int16_t)ty;
                f->t = 0;
                f->dur = PILE_FLIGHT;
                f->delay = delay;
                f->to = (uint8_t)to;
                f->seat = (int8_t)seat;
                return;
        }
        /* No room: it lands at once. */
        if (to == TO_BET) p.seat[seat].bet += amount;
        if (to == TO_POT) p.pot += amount;
        if (to == TO_STACK) p.seat[seat].stack += amount;
        return;
}

static void flying_tick(void) {
        for (int i = 0; i < FLYING; ++i) {
                flying_t *f = &p.flying[i];
                if (!f->used) continue;
                if (f->delay > 0) {
                        --f->delay;
                        continue;
                }
                if (++f->t < f->dur) continue;
                if (f->to == TO_BET) p.seat[f->seat].bet += f->amount;
                if (f->to == TO_POT) p.pot += f->amount;
                if (f->to == TO_STACK) p.seat[f->seat].stack += f->amount;
                if (f->seat == ME || f->to == TO_POT) sfx(SFX_PLACE, 0.35f);
                f->used = 0;
        }
        return;
}

/* Chips from @p seat's stack to its bet. */
static void to_bet(int seat, int32_t amount) {
        if (amount <= 0) return;
        int32_t x, y;
        stack_point(seat, &x, &y);
        p.seat[seat].stack -= amount;
        p.seat[seat].bet_want += amount;
        fly(amount, x, y, seat_at[seat].bet_x, seat_at[seat].bet_y, 0, TO_BET, seat);
        return;
}

/* ---- The model, from the log */

static void say(int seat, const char *text) {
        mseat_t *m = &p.seat[seat];
        snprintf(m->bubble, sizeof(m->bubble), "%s", text);
        m->bubble_ticks = 100;
        return;
}

static void headline(int seat, const char *text) {
        mseat_t *m = &p.seat[seat];
        snprintf(m->headline, sizeof(m->headline), "%s", text);
        m->pop = 1;
        return;
}

static void set_pose(int s, int pose, uint16_t ticks) {
        p.seat[s].pose = (uint8_t)pose;
        p.seat[s].pose_ticks = ticks;
        return;
}

static void dealer_to(int seat) {
        p.dealer_fx = (int16_t)ease(p.dealer_fx, p.dealer_tx, p.dealer_t, 24);
        p.dealer_fy = (int16_t)ease(p.dealer_fy, p.dealer_ty, p.dealer_t, 24);
        p.dealer_tx = (int16_t)seat_at[seat].button_x;
        p.dealer_ty = (int16_t)seat_at[seat].button_y;
        p.dealer_t = 0;
        p.button = (int8_t)seat;
        return;
}

/* The table cleared for a hand: the cards to the deck, every seat's picture empty. */
static void sweep(void) {
        for (int i = 0; i < CARDS; ++i)
                if (p.card[i].used) discard_card(i);
        for (int s = 0; s < PK_SEATS; ++s) {
                mseat_t *m = &p.seat[s];
                memset(m->ids, NONE, sizeof(m->ids));
                m->count = 0;
                m->in = m->folded = m->all_in = 0;
                m->down = 0;
                m->glow = 0;
                m->bet = m->bet_want = 0;
                m->headline[0] = 0;
                m->pop = 0;
        }
        p.board_count = 0;
        p.board_glow = 0;
        p.pot = p.pot_want = 0;
        p.turn = PK_NOBODY;
        p.discards = 0;
        p.glows = 0;
        return;
}

static void money_text(char *out, size_t size, int32_t amount) {
        if (p.tournament) {
                /* Tournament chips aren't money: no dollar sign. */
                char m[16];
                money(m, sizeof(m), amount);
                snprintf(out, size, "%s", m[0] == '$' ? m + 1 : m);
        } else {
                money(out, size, amount);
        }
        return;
}

/* Shows event @p e: changes the model, starts its animation. @return The ticks it holds the next. */
static uint16_t show(const pk_event_t *e) {
        char    text[24], amount[16];
        int32_t x, y;
        switch (e->type) {
                case PK_EV_START:
                        sweep();
                        for (int s = 0; s < PK_SEATS; ++s) p.seat[s].stack = p.table.seat[s].started;
                        dealer_to(e->seat);
                        p.hand_over = 0;
                        sfx(SFX_DEAL, 0.6f);
                        return 24;
                case PK_EV_POST: {
                        mseat_t *m = &p.seat[e->seat];
                        if (e->action == PK_POST_ANTE) {
                                int32_t px, py;
                                stack_point(e->seat, &x, &y);
                                pot_place(&px, &py);
                                m->stack -= e->amount;
                                fly(e->amount, x, y, px, py, 0, TO_POT, e->seat);
                                p.pot_want += e->amount;
                                return 3;
                        }
                        to_bet(e->seat, e->amount);
                        if (e->action == PK_POST_BRING_IN) {
                                money_text(amount, sizeof(amount), e->amount);
                                snprintf(text, sizeof(text), "BRING-IN %.12s", amount);
                                say(e->seat, text);
                        }
                        if (e->up) m->all_in = 1;
                        return 8;
                }
                case PK_EV_DEAL: {
                        mseat_t *m = &p.seat[e->seat];
                        m->in = 1;
                        if (e->index >= PK_CARDS) return 0;
                        if (e->index < m->count && m->ids[e->index] != NONE) discard_card(m->ids[e->index]);
                        const int id = new_card(e->card);
                        if (id < 0) return 0;
                        m->ids[e->index] = (uint8_t)id;
                        if (e->index >= m->count) m->count = (uint8_t)(e->index + 1);
                        if (e->up)
                                m->down &= (uint8_t) ~(1u << e->index);
                        else
                                m->down |= (uint8_t)(1u << e->index);
                        p.card[id].placed = 0;
                        p.lucky_deal = 8;
                        sfx(SFX_PLACE, 0.4f);
                        return 5;
                }
                case PK_EV_BOARD: {
                        const int id = new_card(e->card);
                        if (id < 0 || p.board_count >= PK_BOARD) return 0;
                        p.board[p.board_count++] = (uint8_t)id;
                        p.card[id].placed = 0;
                        p.lucky_deal = 8;
                        sfx(SFX_PLACE, 0.4f);
                        /* The flop goes down in a row and flips in a ripple; the turn and river each
                           after a pause. */
                        if (e->index < 3) {
                                p.card[id].delay = (uint16_t)(e->index * 4);
                                return e->index == 2 ? FLIGHT + FLIP + 12 : 0;
                        }
                        p.card[id].delay = 16;
                        return 16 + FLIGHT + FLIP + 10;
                }
                case PK_EV_TURN:
                        p.turn = e->seat;
                        if (e->seat == ME) p.raise_to = 0; /* the least again, each turn */
                        return 0;
                case PK_EV_ACT: {
                        mseat_t      *m = &p.seat[e->seat];
                        const int32_t add = e->amount - m->bet_want;
                        money_text(amount, sizeof(amount), e->amount);
                        switch (e->action) {
                                case PK_FOLD:
                                        m->folded = 1;
                                        for (int i = 0; i < m->count; ++i) {
                                                discard_card(m->ids[i]);
                                                m->ids[i] = NONE;
                                        }
                                        say(e->seat, "FOLD");
                                        if (e->seat != ME) set_pose(e->seat, POSE_IDLE, 0);
                                        break;
                                case PK_CHECK: say(e->seat, "CHECK"); break;
                                case PK_CALL:
                                        to_bet(e->seat, add);
                                        if (e->up) {
                                                say(e->seat, "ALL IN");
                                        } else {
                                                money_text(amount, sizeof(amount), add);
                                                snprintf(text, sizeof(text), "CALL %s", amount);
                                                say(e->seat, text);
                                        }
                                        break;
                                default: {
                                        int bet = 1; /* nothing was bet before: a bet, not a raise */
                                        for (int s = 0; s < PK_SEATS; ++s)
                                                if (s != e->seat && p.seat[s].bet_want > 0) bet = 0;
                                        to_bet(e->seat, add);
                                        if (e->up)
                                                snprintf(text, sizeof(text), "ALL IN %s", amount);
                                        else
                                                snprintf(text, sizeof(text), "%s %.12s", bet ? "BET" : "RAISE TO",
                                                         amount);
                                        say(e->seat, text);
                                        if (e->seat != ME) sfx(SFX_QUACK2, 0.35f);
                                        break;
                                }
                        }
                        if (e->up) m->all_in = 1;
                        if (p.turn == e->seat) p.turn = PK_NOBODY;
                        return e->seat == ME ? 6 : 14;
                }
                case PK_EV_COLLECT: {
                        int32_t px, py;
                        pot_place(&px, &py);
                        int any = 0;
                        for (int s = 0; s < PK_SEATS; ++s) {
                                mseat_t *m = &p.seat[s];
                                if (m->bet_want <= 0) continue;
                                /* What has landed; what is still flying goes on below. */
                                fly(m->bet, seat_at[s].bet_x, seat_at[s].bet_y, px, py, 4, TO_POT, s);
                                p.pot_want += m->bet_want;
                                m->bet = m->bet_want = 0;
                                any = 1;
                        }
                        /* Chips still on their way to a bet go on to the pot. */
                        for (int i = 0; i < FLYING; ++i) {
                                flying_t *f = &p.flying[i];
                                if (!f->used || f->to != TO_BET) continue;
                                f->fx = (int16_t)ease(f->fx, f->tx, f->t, f->dur);
                                f->fy = (int16_t)ease(f->fy, f->ty, f->t, f->dur);
                                f->tx = (int16_t)px;
                                f->ty = (int16_t)py;
                                f->t = 0;
                                f->to = TO_POT;
                                any = 1;
                        }
                        /* The pot as the rules have it (the antes' arrive in their own piles). */
                        p.pot_want = e->amount;
                        return any ? PILE_FLIGHT + 10 : 0;
                }
                case PK_EV_RETURN: {
                        mseat_t *m = &p.seat[e->seat];
                        stack_point(e->seat, &x, &y);
                        fly(e->amount, seat_at[e->seat].bet_x, seat_at[e->seat].bet_y, x, y, 0, TO_STACK, e->seat);
                        m->bet -= e->amount;
                        m->bet_want -= e->amount;
                        if (m->bet < 0) m->bet = 0;
                        m->all_in = 0;
                        return 12;
                }
                case PK_EV_DISCARD: {
                        mseat_t *m = &p.seat[e->seat];
                        for (int i = 0; i < m->count; ++i)
                                if (e->value & (1u << i)) {
                                        discard_card(m->ids[i]);
                                        m->ids[i] = NONE;
                                }
                        if (e->seat == ME) p.discards = 0;
                        if (e->amount == 0) {
                                say(e->seat, "STAND PAT");
                        } else {
                                snprintf(text, sizeof(text), "DRAW %d", (int)e->amount);
                                say(e->seat, text);
                        }
                        if (p.turn == e->seat) p.turn = PK_NOBODY;
                        return 12;
                }
                case PK_EV_SHOW: {
                        mseat_t *m = &p.seat[e->seat];
                        if (e->index < m->count && m->ids[e->index] != NONE) {
                                card_t *c = &p.card[m->ids[e->index]];
                                if (c->shown != e->card) {
                                        c->card = e->card;
                                        c->up = 1;
                                        c->flip = FLIP;
                                }
                        }
                        m->down &= (uint8_t) ~(1u << e->index);
                        return 4;
                }
                case PK_EV_HAND:
                        headline(e->seat, hand_name(hand_category(e->value)));
                        p.seat[e->seat].glow = 0;
                        return 34;
                case PK_EV_POT: p.turn = PK_NOBODY; return 6;
                case PK_EV_WIN: {
                        mseat_t *m = &p.seat[e->seat];
                        int32_t  px, py;
                        pot_place(&px, &py);
                        stack_point(e->seat, &x, &y);
                        /* The pot's chips to the winner; its winning five lift. */
                        p.pot -= e->amount;
                        p.pot_want -= e->amount;
                        if (p.pot < 0) p.pot = 0;
                        fly(e->amount, px, py, x, y, 0, TO_STACK, e->seat);
                        for (uint32_t k = p.cursor; k-- > 0;) {
                                const pk_event_t h = pk_event_for(&p.table, k, ME);
                                if (h.type != PK_EV_HAND || h.seat != e->seat) continue;
                                m->glow = (uint16_t)(h.mask & 0x7fu);
                                p.board_glow |= (uint8_t)(h.mask >> PK_CARDS);
                                break;
                        }
                        money_text(amount, sizeof(amount), e->amount);
                        snprintf(text, sizeof(text), "WINS %s", amount);
                        say(e->seat, text);
                        if (e->seat == ME) {
                                sfx(SFX_WIN, 0.6f);
                                p.lucky_pose = ART_LUCKY_CHEER;
                                p.lucky_mood = 90;
                        } else {
                                set_pose(e->seat, POSE_HAPPY, 120);
                                sfx(SFX_QUACK, 0.4f);
                        }
                        return 50;
                }
                case PK_EV_OUT: {
                        mseat_t *m = &p.seat[e->seat];
                        if (!p.tournament) {
                                if (e->seat != ME) say(e->seat, "BROKE!");
                                return 10;
                        }
                        if (e->index == 1) return 0; /* the winner: said at the end */
                        m->out = e->index;
                        if (e->seat != ME) {
                                set_pose(e->seat, POSE_SAD, 200);
                                snprintf(text, sizeof(text), "OUT IN %d%s", e->index,
                                         e->index == 2   ? "ND"
                                         : e->index == 3 ? "RD"
                                                         : "TH");
                                say(e->seat, text);
                        }
                        return 30;
                }
                case PK_EV_END:
                        if (!p.tournament) {
                                const int32_t stack = p.table.seat[ME].stack;
                                bank_add(stack - p.synced);
                                p.synced = stack;
                        }
                        p.hand_over = 1;
                        p.turn = PK_NOBODY;
                        for (int s = 1; s < PK_SEATS; ++s)
                                if (p.seat[s].pose == POSE_THINK) set_pose(s, POSE_IDLE, 0);
                        return 0;
                default: return 0;
        }
}

static void replay(void) {
        if (p.wait > 0) {
                --p.wait;
                return;
        }
        while (p.wait == 0 && p.cursor < p.table.event_count) {
                const pk_event_t e = pk_event_for(&p.table, p.cursor++, ME);
                p.wait = show(&e);
        }
        return;
}

static int caught_up(void) {
        if (p.wait > 0 || p.cursor < p.table.event_count) return 0;
        for (int i = 0; i < FLYING; ++i)
                if (p.flying[i].used) return 0;
        return cards_still();
}

/* ---- The tournament */

static int entrants(void) {
        int n = 0;
        for (int s = 0; s < PK_SEATS; ++s) n += p.table.seat[s].sitting || p.table.seat[s].place != 0;
        return n;
}

/* The prize for @p place among the entrants. */
static int32_t prize(int place) {
        const int            n = entrants();
        const int32_t        pool = TOURNEY_FEE * n;
        static const int32_t three[3] = {50, 30, 20}, two[2] = {70, 30};
        if (n <= 2) return place == 1 ? pool : 0;
        if (n <= 4) return place <= 2 ? pool * two[place - 1] / 100 : 0;
        return place <= 3 ? pool * three[place - 1] / 100 : 0;
}

static const char *ordinal(int n) {
        static const char *const names[7] = {"", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH"};
        return n >= 1 && n <= 6 ? names[n] : "";
}

static char              result_line[2][64];
static const char *const result_lines[] = {result_line[0], "", result_line[1]};

/* The player's tournament over: its place and prize, paid. */
static void tournament_over(int place) {
        if (p.finished) return;
        p.finished = 1;
        const int32_t won = prize(place);
        char          amount[16];
        money(amount, sizeof(amount), won);
        snprintf(result_line[0], sizeof(result_line[0]), "You finished %s of %d.", ordinal(place), entrants());
        if (won > 0) {
                bank_add(won);
                snprintf(result_line[1], sizeof(result_line[1]), "Your prize: %s.", amount);
                sfx(SFX_WIN, 0.7f);
                p.lucky_pose = ART_LUCKY_CHEER;
                p.lucky_mood = 150;
        } else {
                snprintf(result_line[1], sizeof(result_line[1]), "No prize this time. Lucky says: again!");
                sfx(SFX_SAD, 0.5f);
                p.lucky_pose = ART_LUCKY_SAD;
                p.lucky_mood = 150;
        }
        app_info(place == 1 ? "YOU WON THE TOURNAMENT!" : "TOURNAMENT OVER", result_lines, 3);
        return;
}

/* ---- The table */

static pk_rules_t rules_now(void) {
        const int     game = p.choice == 0 ? PK_HOLDEM : p.choice - 1;
        const int32_t bb = p.tournament ? LEVELS[p.level] : STAKES[p.stake];
        return pk_stakes(game, p.choice == 0, bb);
}

static const int DUCK_ORDER[5][5] = {{3}, {2, 4}, {1, 3, 5}, {1, 2, 4, 5}, {1, 2, 3, 4, 5}};

static void paint_felt(void);

static void new_table(void) {
        memset(&p.table, 0, sizeof(p.table));
        memset(p.card, 0, sizeof(p.card));
        memset(p.seat, 0, sizeof(p.seat));
        memset(p.flying, 0, sizeof(p.flying));
        sweep();
        p.cursor = 0;
        p.wait = 0;
        p.level = 0;
        p.finished = 0;
        p.hand_over = 1;
        p.eq_seat = PK_NOBODY;
        p.think = 0;
        p.hint_ticks = 0;
        const pk_rules_t r = rules_now();
        pk_init(&p.table, &r, app_key(), p.tournament);
        place_seats(); /* the player's cards sit by the game */
        /* Cash: 100 big blinds (or the bank, if less); a tournament: its fee for its chips. */
        int32_t stack;
        if (p.tournament) {
                if (bank_ensure(TOURNEY_FEE)) sfx(SFX_SAD, 0.5f);
                bank_add(-TOURNEY_FEE);
                stack = TOURNEY_CHIPS;
        } else {
                p.buy_in = r.big_blind * 100;
                if (bank_ensure(r.big_blind * 20)) sfx(SFX_SAD, 0.5f);
                stack = (int32_t)bank_balance() < p.buy_in ? (int32_t)bank_balance() : p.buy_in;
                p.synced = stack;
        }
        pk_sit(&p.table, ME, 1, stack);
        const int n = p.ducks >= 1 && p.ducks <= 5 ? p.ducks : 3;
        for (int i = 0; i < n; ++i)
                pk_sit(&p.table, DUCK_ORDER[n - 1][i], 1, p.tournament ? TOURNEY_CHIPS : r.big_blind * 100);
        for (int s = 0; s < PK_SEATS; ++s) p.seat[s].stack = p.table.seat[s].stack;
        p.button = PK_NOBODY;
        p.dealer_fx = p.dealer_tx = (int16_t)(table_cx() - 22);
        p.dealer_fy = p.dealer_ty = (int16_t)(table_cy() - 15);
        p.dealer_t = 24;
        paint_felt();
        stage_dirty(&p.stage, rect(0, 0, WIDTH, HEIGHT));
        return;
}

/* Between hands: broke ducks rebuy (cash), the blinds rise (a tournament), the next hand dealt. */
static void deal(void) {
        pk_table_t *t = &p.table;
        if (t->phase != PK_WAITING || !caught_up() || p.finished) return;
        if (t->seat[ME].stack == 0) return; /* the player rebuys first */
        if (p.tournament) {
                const int level = (int)(t->hands / LEVEL_HANDS);
                if (level != p.level && level < LEVEL_COUNT) {
                        p.level = level;
                        const pk_rules_t r = rules_now();
                        (void)pk_set_rules(t, &r);
                        char line[36];
                        snprintf(line, sizeof(line), "BLINDS UP: LEVEL %d", p.level + 1);
                        snprintf(p.message, sizeof(p.message), "%s", line);
                        p.message_ticks = 150;
                }
        } else {
                for (int s = 1; s < PK_SEATS; ++s)
                        if (t->seat[s].sitting && t->seat[s].stack == 0) {
                                pk_add_chips(t, s, p.buy_in);
                                p.seat[s].stack = t->seat[s].stack;
                                say(s, "REBUY");
                        }
        }
        if (pk_start_hand(t) != 0) return;
        p.cursor = 0;
        p.wait = 0;
        p.hint_ticks = 0;
        return;
}

static void rebuy(void) {
        pk_table_t *t = &p.table;
        if (p.tournament || t->phase != PK_WAITING) return;
        const int32_t need = p.buy_in - t->seat[ME].stack;
        if (need <= 0) return;
        if (bank_ensure(t->rules.big_blind * 20) && (int32_t)bank_balance() >= t->rules.big_blind * 20) {
                p.lucky_pose = ART_LUCKY_SAD;
                p.lucky_mood = 120;
                sfx(SFX_SAD, 0.5f);
        }
        /* What the bank has beyond the chips already at the table. */
        const int32_t free = (int32_t)bank_balance() - t->seat[ME].stack;
        const int32_t add = need < free ? need : free;
        if (add <= 0) return;
        pk_add_chips(t, ME, add);
        p.synced += add;
        p.seat[ME].stack = t->seat[ME].stack;
        sfx(SFX_PLACE, 0.5f);
        return;
}

/* ---- The ducks' turns */

static void start_think(int seat) {
        pk_view_t v;
        pk_view(&p.table, seat, &v);
        deck_key_t key;
        for (int i = 0; i < 4; ++i) key.w[i] = deck_next(&p.rng);
        pk_equity_start(&p.eq, &v, key, EQ_TRIALS);
        p.eq_seat = (int8_t)seat;
        p.eq_done = 0;
        p.think = 0;
        return;
}

static void equity_tick(void) {
        if (p.eq_seat == PK_NOBODY || p.eq_done) return;
        int64_t start = 0, now = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &start);
        do {
                if (pk_equity_step(&p.eq, 50)) {
                        p.eq_done = 1;
                        return;
                }
                bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
        } while (now - start < EQ_SLICE_NS);
        return;
}

static void make_hint(void) {
        const int pct = (int)(pk_equity_value(&p.eq) * 100.0f + 0.5f);
        snprintf(p.hint, sizeof(p.hint), "YOU WIN %d%% OF THE TIME", pct);
        p.hint_ticks = 240;
        sfx(SFX_QUACK2, 0.3f);
        p.eq_seat = PK_NOBODY;
        return;
}

static void table_tick(void) {
        pk_table_t *t = &p.table;
        if (p.eq_seat == ME && p.eq_done) make_hint();
        if (!caught_up() || t->phase == PK_WAITING) return;
        const int seat = t->turn;
        if (seat == ME || seat == PK_NOBODY) return;
        /* A duck's turn: think (its equity worked out meanwhile), then act. */
        if (p.eq_seat != seat) {
                if (t->phase == PK_DRAWING) {
                        p.eq_seat = (int8_t)seat;
                        p.eq_done = 1;
                        p.think = 0;
                } else {
                        start_think(seat);
                }
                set_pose(seat, POSE_THINK, 0);
        }
        /* Quicker when the player is out of the hand. */
        const uint16_t think = t->seat[ME].in_hand ? BOT_THINK : BOT_THINK / 2;
        if (++p.think < think || !p.eq_done) return;
        p.eq_seat = PK_NOBODY;
        set_pose(seat, POSE_IDLE, 0);
        if (t->phase == PK_DRAWING) {
                (void)pk_discard(t, seat, pk_bot_discard(t->seat[seat].cards));
                return;
        }
        pk_view_t v;
        pk_view(t, seat, &v);
        const pk_choice_t c = pk_bot_decide(&v, &DUCK_STYLE[seat], pk_equity_value(&p.eq), deck_next(&p.rng));
        if (pk_act(t, seat, c.action, c.amount) != 0) /* never expected: checked legal */
                (void)pk_act(t, seat, pk_can(t, seat, PK_CHECK) ? PK_CHECK : PK_FOLD, 0);
        return;
}

/* ---- The player's actions */

static int my_turn(void) {
        return caught_up() && p.table.turn == ME && p.table.phase == PK_BETTING;
}

static int my_draw(void) {
        return caught_up() && p.table.turn == ME && p.table.phase == PK_DRAWING;
}

static void clamp_raise(void) {
        const int32_t lo = pk_min_raise(&p.table, ME), hi = pk_max_raise(&p.table, ME);
        if (p.raise_to < lo) p.raise_to = lo;
        if (p.raise_to > hi) p.raise_to = hi;
        return;
}

static void size_to(int32_t to) {
        p.raise_to = to;
        clamp_raise();
        sfx(SFX_CLICK, 0.3f);
        return;
}

static void act(int action) {
        if (pk_act(&p.table, ME, action, p.raise_to) == 0) {
                p.hint_ticks = 0;
                if (p.eq_seat == ME) p.eq_seat = PK_NOBODY;
                sfx(SFX_CLICK, 0.4f);
        }
        return;
}

static void hint(void) {
        if (!my_turn() || p.eq_seat != PK_NOBODY) return;
        start_think(ME);
        snprintf(p.hint, sizeof(p.hint), "HMM...");
        p.hint_ticks = 600;
        return;
}

/* ---- The felt */

static uint16_t rgb565(uint32_t rgb) {
        return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

/* The oval's padded rail, a line round the middle, the floor beyond, the band. */
static void paint_felt(void) {
        felt_baize(0x1a8c4a, 0x0a4422);
        uint16_t       *px = (uint16_t *)(void *)app.felt.pixels;
        const int64_t   cx = table_cx(), cy = table_cy(), rx = table_rx(), ry = TABLE_RY;
        const int64_t   orx = rx + 28, ory = ry + 28;
        static uint16_t rail[29];
        for (int i = 0; i <= 28; ++i) { /* leather, rounded: lighter in the middle */
                const int32_t d = i - 14, k = 255 - d * d * 255 / 260;
                rail[i] = rgb565(((uint32_t)(0x5a * k / 255) << 16) | ((uint32_t)(0x30 * k / 255) << 8) |
                                 (uint32_t)(0x1a * k / 255));
        }
        const uint16_t floor = rgb565(0x1c120c), band = rgb565(0x2a1a10), gold = rgb565(0xb08028);
        const uint16_t line = rgb565(0x3aa862);
        const int64_t  lrx = rx - 70, lry = ry - 52; /* the line inside */
        for (int32_t y = 0; y < HEIGHT; ++y) {
                for (int32_t x = 0; x < WIDTH; ++x) {
                        uint16_t *q = &px[(size_t)y * WIDTH + x];
                        if (y >= BAND_Y) {
                                *q = y < BAND_Y + 2 ? gold : band;
                                continue;
                        }
                        const int64_t dx = x - cx, dy = y - cy;
                        const int64_t qi = dx * dx * ry * ry + dy * dy * rx * rx;
                        if (qi <= rx * rx * ry * ry) {
                                const int64_t ql = dx * dx * lry * lry + dy * dy * lrx * lrx,
                                              edge = lrx * lrx * lry * lry;
                                /* A line two pixels wide: within a band of the inner ellipse's value. */
                                if (ql > edge - edge / 90 && ql < edge + edge / 90) *q = line;
                                continue;
                        }
                        const int64_t qo = dx * dx * ory * ory + dy * dy * orx * orx;
                        if (qo > orx * orx * ory * ory) {
                                *q = floor;
                                continue;
                        }
                        const int64_t a = qi / (rx * rx) - ry * ry, z = qo / (orx * orx) - ory * ory;
                        int32_t       i = (int32_t)(28 * a / (a - z > 0 ? a - z : 1));
                        if (i < 0) i = 0;
                        if (i > 28) i = 28;
                        *q = rail[i];
                }
        }
        bmds_canvas_damage_clear(&app.felt_canvas);
        return;
}

/* ---- The frame */

static void add_button(int *n, int id, bmds_rect_t r, const char *label) {
        p.buttons[*n] = id;
        p.button_at[*n].r = r;
        p.button_at[*n].label = label;
        p.button_at[*n].key = BUTTON_KEY[id];
        ++*n;
        return;
}

static char call_label[24], raise_label[28], draw_label[16];

static void show_buttons(void) {
        const pk_table_t *t = &p.table;
        int               n = 0;
        for (int i = B_LOBBY; i <= B_NEW; ++i) add_button(&n, i, app_system_button(i), BUTTON_LABEL[i]);
        /* The band's, right-aligned: the widest first on the left. */
        int ids[8], count = 0;
        if (p.finished && caught_up()) {
                ids[count++] = B_NEW; /* the band's: another tournament */
        } else if (t->phase == PK_WAITING && caught_up() && !p.finished) {
                if (!p.tournament && t->seat[ME].stack < p.buy_in) ids[count++] = B_REBUY;
                if (t->seat[ME].stack > 0) ids[count++] = B_DEAL;
        } else if (my_turn()) {
                char amount[16];
                ids[count++] = B_HINT;
                if (pk_can(t, ME, PK_CALL)) {
                        ids[count++] = B_FOLD;
                        money_text(amount, sizeof(amount), pk_to_call(t, ME));
                        snprintf(call_label, sizeof(call_label), "CALL %s", amount);
                } else {
                        snprintf(call_label, sizeof(call_label), "CHECK");
                }
                ids[count++] = B_CALL;
                if (pk_can(t, ME, PK_RAISE)) {
                        clamp_raise();
                        money_text(amount, sizeof(amount), p.raise_to);
                        const int all = p.raise_to == pk_max_raise(t, ME) && t->rules.no_limit;
                        snprintf(raise_label, sizeof(raise_label), "%s %s",
                                 all               ? "ALL IN"
                                 : t->cur_bet == 0 ? "BET"
                                                   : "RAISE TO",
                                 amount);
                        ids[count++] = B_RAISE;
                }
        } else if (my_draw()) {
                const int k = __builtin_popcount(p.discards);
                if (k == 0)
                        snprintf(draw_label, sizeof(draw_label), "STAND PAT");
                else
                        snprintf(draw_label, sizeof(draw_label), "DRAW %d", k);
                ids[count++] = B_DRAW;
        }
        int32_t x = WIDTH - 16;
        for (int i = count - 1; i >= 0; --i) {
                const int     id = ids[i];
                const char   *label = id == B_CALL    ? call_label
                                      : id == B_RAISE ? raise_label
                                      : id == B_DRAW  ? draw_label
                                      : id == B_NEW   ? "NEW TOURNAMENT"
                                                      : BUTTON_LABEL[id];
                const int32_t w = ui_text_width(ART_FONT_SMALL, label) + 26;
                x -= w;
                add_button(&n, id, rect(x, BAND_Y + 13, w, 40), label);
                x -= 8;
        }
        /* No-limit's sizes on the felt above the band. */
        if (my_turn() && t->rules.no_limit && pk_can(t, ME, PK_RAISE) && pk_min_raise(t, ME) < pk_max_raise(t, ME)) {
                static const int sizes[5] = {B_LESS, B_MORE, B_HALF, B_POT, B_ALL};
                int32_t          sx = WIDTH - 16;
                for (int i = 4; i >= 0; --i) {
                        const char   *label = BUTTON_LABEL[sizes[i]];
                        const int32_t w = sizes[i] <= B_MORE ? 40 : ui_text_width(ART_FONT_SMALL, label) + 22;
                        sx -= w;
                        add_button(&n, sizes[i], rect(sx, BAND_Y - 46, w, 36), label);
                        sx -= 6;
                }
        }
        p.button_count = n;
        return;
}

static int duck_sprite(int s) {
        static const int base[PK_SEATS] = {0,
                                           ART_BOT_MALLARD_IDLE,
                                           ART_BOT_PEKIN_IDLE,
                                           ART_BOT_WOOD_IDLE,
                                           ART_BOT_RUBBER_IDLE,
                                           ART_BOT_BLACK_IDLE};
        return base[s] + p.seat[s].pose;
}

static void layout_seat(stage_t *st, int s) {
        const mseat_t *m = &p.seat[s];
        stage_item_t  *it;
        char           amount[16];
        if (s != ME && p.table.seat[s].sitting + (m->out != 0) > 0) {
                const int32_t dx = seat_at[s].duck_x, dy = seat_at[s].duck_y;
                it = stage_put(st, (uint32_t)(K_DUCK + s), STAGE_SPRITE, 20, dx, dy);
                it->tile = (uint16_t)duck_sprite(s);
                if (seat_at[s].mirror) it->flags = STAGE_MIRROR;
                if (m->folded || m->out) { /* out of the hand: greyed */
                        it = stage_put(st, (uint32_t)(K_DUCK_SHADE + s), STAGE_SHADE, 21, dx, dy);
                        it->tile = (uint16_t)duck_sprite(s);
                        it->flags = seat_at[s].mirror ? STAGE_MIRROR : 0;
                        it->alpha = 60;
                }
                /* Its name and stack: under the duck, or beside it at the top. */
                const int     top = s == 3;
                const int32_t lx = top ? dx + 96 : dx + 42, ly = top ? dy + 18 : dy + 84;
                it = stage_put(st, (uint32_t)(K_NAME + s), STAGE_TEXT, 22, lx, ly);
                it->font = ART_FONT_SMALL;
                it->flags = top ? 0 : STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", DUCK_NAME[s]);
                it = stage_put(st, (uint32_t)(K_STACK + s), STAGE_TEXT, 22, lx, ly + 20);
                it->font = ART_FONT_HINT;
                it->flags = top ? 0 : STAGE_CENTRE;
                if (m->out)
                        snprintf(it->text, sizeof(it->text), "OUT: %s", ordinal(m->out));
                else if (m->all_in && !m->folded && m->in && !p.hand_over)
                        snprintf(it->text, sizeof(it->text), "ALL IN");
                else {
                        money_text(amount, sizeof(amount), m->stack);
                        snprintf(it->text, sizeof(it->text), "%s", amount);
                }
                if (m->bubble_ticks > 0) {
                        const int32_t w = ui_text_width(ART_FONT_SMALL, m->bubble) + 28;
                        /* Over the duck; the top one's and the upper right one's (under the
                           system buttons) beside it. */
                        const int     side = top || s == 4;
                        const int32_t bx = side ? dx - 8 - w : dx + 42 - w / 2, by = side ? dy + 20 : dy - 46;
                        it = stage_put(st, (uint32_t)(K_BUBBLE + s), STAGE_PANEL, 70, bx, by);
                        it->w = (int16_t)w;
                        it->h = 38;
                        it = stage_put(st, (uint32_t)(K_BUBBLE_TEXT + s), STAGE_TEXT, 71, bx + 14, by + 11);
                        it->font = ART_FONT_SMALL;
                        snprintf(it->text, sizeof(it->text), "%.23s", m->bubble);
                }
        } else if (s == ME && m->bubble_ticks > 0 && !my_turn()) {
                /* The player's words over its cards. */
                const int32_t w = ui_text_width(ART_FONT_SMALL, m->bubble) + 28;
                const int32_t bx = seat_at[ME].hand_x + 130, by = seat_at[ME].hand_y - 20;
                it = stage_put(st, (uint32_t)(K_BUBBLE + s), STAGE_PANEL, 70, bx, by);
                it->w = (int16_t)w;
                it->h = 38;
                it = stage_put(st, (uint32_t)(K_BUBBLE_TEXT + s), STAGE_TEXT, 71, bx + 14, by + 11);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%.23s", m->bubble);
        }
        /* Its bet in front. */
        if (m->bet > 0) {
                it = stage_put(st, (uint32_t)(K_BET + s), STAGE_CHIPS, 25, seat_at[s].bet_x, seat_at[s].bet_y);
                it->rgb = (uint32_t)m->bet;
                money_text(amount, sizeof(amount), m->bet);
                /* The amount beside the pile, on the side away from the seat's cards. */
                const int32_t tx = s >= 4 ? seat_at[s].bet_x - 6 - ui_text_ink_width(ART_FONT_HINT, amount)
                                          : seat_at[s].bet_x + 52;
                it = stage_put(st, (uint32_t)(K_BET_TEXT + s), STAGE_TEXT, 26, tx, seat_at[s].bet_y + 6);
                it->font = ART_FONT_HINT;
                snprintf(it->text, sizeof(it->text), "%s", amount);
        }
        /* The hand's name, popping up: the player's over its cards, a duck's under its stack (the
           top one's beside it), clear of the cards. */
        if (m->headline[0] && m->count > 0) {
                const int32_t t = m->pop > 20 ? 20 : m->pop;
                const int32_t lift = (t < 12 ? t * 4 : 48 - (t - 12) * 2) / 2;
                int32_t       x = seat_at[s].hand_x, y = seat_at[s].hand_y - ART_CARD_H / 2 - 10 - lift;
                if (s == 3) {
                        x = seat_at[s].duck_x + 96;
                        y = seat_at[s].duck_y + 60;
                } else if (s != ME) {
                        x = seat_at[s].duck_x + 42;
                        y = seat_at[s].duck_y + 128;
                }
                it = stage_put(st, (uint32_t)(K_HEADLINE + s), STAGE_TEXT, 60, x, y);
                it->font = s == ME ? ART_FONT_MED : ART_FONT_HINT;
                it->flags = s == 3 ? 0 : STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", m->headline);
        }
        return;
}

/* A gold edge under a card that makes a winning hand. */
static void glow(stage_t *st, int id) {
        if (p.glows >= GLOWS || id == NONE) return;
        const card_t *c = &p.card[id];
        int32_t       x, y;
        uint16_t      k;
        card_now(c, &x, &y, &k);
        const int32_t w = ART_CARD_W * k / 256, h = ART_CARD_H * k / 256;
        stage_item_t *it = stage_put(st, (uint32_t)(K_GLOW + p.glows++), STAGE_BLEND, 29, x - 4, y - 4);
        it->w = (int16_t)(w + 8);
        it->h = (int16_t)(h + 8);
        it->rgb = 0xffd040;
        it->alpha = 230;
        return;
}

static void layout_band(stage_t *st) {
        stage_item_t *it;
        char          amount[24];
        /* The stakes: no-limit's blinds, fixed limit's small and big bets. */
        const int   nl = p.table.rules.no_limit;
        const char *labels[3] = {"CHIPS", "BANK", p.tournament ? "LEVEL" : nl ? "BLINDS" : "BETS"};
        for (int i = 0; i < 3; ++i) {
                const int32_t x = 20 + i * 170;
                it = stage_put(st, (uint32_t)(K_BAND + i), STAGE_TEXT, 80, x, BAND_Y + 8);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", labels[i]);
                it = stage_put(st, (uint32_t)(K_BAND + 3 + i), STAGE_TEXT, 80, x, BAND_Y + 31);
                it->font = ART_FONT_MED;
                if (i == 0) {
                        money_text(amount, sizeof(amount), p.seat[ME].stack);
                        snprintf(it->text, sizeof(it->text), "%s", amount);
                } else if (i == 1) {
                        /* The bank beyond the chips at the table. */
                        money(amount, sizeof(amount), bank_balance() - (p.tournament ? 0 : p.synced));
                        snprintf(it->text, sizeof(it->text), "%s", amount);
                } else {
                        const pk_rules_t *r = &p.table.rules;
                        char              a[16], b2[16];
                        money_text(a, sizeof(a), nl ? r->small_blind : pk_bet_size(&p.table, 0));
                        money_text(b2, sizeof(b2), nl ? r->big_blind : pk_bet_size(&p.table, 9));
                        if (p.tournament)
                                snprintf(it->text, sizeof(it->text), "%d: %.9s/%.9s", (p.level + 1) % 100, a, b2);
                        else
                                snprintf(it->text, sizeof(it->text), "%s/%s", a, b2);
                }
        }
        return;
}

static bmds_rect_t option_line(int i) {
        return rect(WIDTH / 2 - 250, HEIGHT / 2 - 92 + i * 36, 500, 32);
}

static void layout_options(stage_t *st) {
        const int32_t bx = WIDTH / 2 - 280, by = HEIGHT / 2 - 170;
        stage_item_t *it = stage_put(st, K_BOX, STAGE_PANEL, 90, bx, by);
        it->w = 560;
        it->h = 330;
        it = stage_put(st, K_BOX + 1, STAGE_TEXT, 91, WIDTH / 2, by + 22);
        it->font = ART_FONT_MED;
        it->flags = STAGE_CENTRE;
        snprintf(it->text, sizeof(it->text), "OPTIONS");
        for (int i = 0; i < OPTION_LINES; ++i) {
                const bmds_rect_t r = option_line(i);
                if (contains(&r, app.px, app.py)) {
                        it = stage_put(st, (uint32_t)(K_BOX + 2 + i), STAGE_BLEND, 91, r.x, r.y);
                        it->w = (int16_t)r.width;
                        it->h = (int16_t)r.height;
                        it->rgb = 0xffd040;
                        it->alpha = 50;
                }
                it = stage_put(st, (uint32_t)(K_BOX + 6 + i), STAGE_TEXT, 92, r.x + 20, r.y + 9);
                it->font = ART_FONT_SMALL;
                char a[16], b2[16];
                switch (i) {
                        case 0: snprintf(it->text, sizeof(it->text), "G   %s", GAME_NAME[p.choice]); break;
                        case 1:
                                snprintf(it->text, sizeof(it->text), "T   %s",
                                         p.tournament ? "TOURNAMENT, $100 TO ENTER" : "CASH GAME");
                                break;
                        case 2: snprintf(it->text, sizeof(it->text), "D   DUCKS AT THE TABLE: %d", p.ducks); break;
                        default:
                                money(a, sizeof(a), STAKES[p.stake] / 2);
                                money(b2, sizeof(b2), STAKES[p.stake]);
                                snprintf(it->text, sizeof(it->text), "S   STAKES: %.8s/%.8s", a, b2);
                                break;
                }
        }
        it = stage_put(st, K_BOX + 12, STAGE_LABEL, 91, bx + 50, by + 270);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "Click or press a key to change.");
        it = stage_put(st, K_BOX + 13, STAGE_LABEL, 91, bx + 50, by + 290);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "Changes: a new table. Esc: close");
        return;
}

static void layout(void) {
        stage_t *st = &p.stage;
        stage_begin(st);
        p.glows = 0;
        stage_item_t *it;
        int32_t       x, y;
        /* Lucky and the deck. */
        it = stage_put(st, K_LUCKY, STAGE_SPRITE, 8, 6, 4);
        it->tile = (uint16_t)(p.lucky_deal > 0      ? ART_LUCKY_DEAL
                              : p.lucky_mood > 0    ? p.lucky_pose
                              : (p.ticks % 240) < 8 ? ART_LUCKY_BLINK
                                                    : ART_LUCKY_IDLE);
        deck_place(&x, &y);
        for (int k = 0; k < 3; ++k) {
                it = stage_put(st, (uint32_t)(K_DECK + k), STAGE_CARD, 9, x, y - k * 2);
                it->tile = ART_CARD_BACK;
                it->w = ART_CARD_W / 2;
                it->h = ART_CARD_H / 2;
        }
        if (p.hint_ticks > 0) { /* Lucky's whisper: the player's chance */
                const int     thinking = p.eq_seat == ME;
                const int32_t w = ui_text_width(ART_FONT_SMALL, p.hint) + 40;
                it = stage_put(st, K_LUCKY_BUBBLE, STAGE_PANEL, 70, 200, 6);
                it->w = (int16_t)(w > 160 ? w : 160);
                it->h = (int16_t)(thinking ? 44 : 68);
                it = stage_put(st, K_LUCKY_TEXT, STAGE_TEXT, 71, 220, 20);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", thinking ? p.hint : "PSST...");
                if (!thinking) {
                        it = stage_put(st, K_LUCKY_TEXT2, STAGE_TEXT, 71, 220, 44);
                        it->font = ART_FONT_SMALL;
                        snprintf(it->text, sizeof(it->text), "%s", p.hint);
                }
        }
        if (p.message_ticks > 0) {
                it = stage_put(st, K_MESSAGE, STAGE_TEXT, 72, table_cx(), table_cy() - 150);
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", p.message);
        }
        /* The seats. */
        for (int s = 0; s < PK_SEATS; ++s) layout_seat(st, s);
        /* The dealer's button. */
        if (p.button != PK_NOBODY) {
                it = stage_put(st, K_DEALER, STAGE_SPRITE, 24, ease(p.dealer_fx, p.dealer_tx, p.dealer_t, 24),
                               ease(p.dealer_fy, p.dealer_ty, p.dealer_t, 24));
                it->tile = ART_DEALER_BUTTON;
        }
        /* The cards; the winning ones' gold edges under them. */
        for (int s = 0; s < PK_SEATS; ++s)
                for (int i = 0; i < p.seat[s].count; ++i)
                        if (p.seat[s].glow & (1u << i)) glow(st, p.seat[s].ids[i]);
        for (int i = 0; i < p.board_count; ++i)
                if (p.board_glow & (1u << i)) glow(st, p.board[i]);
        for (int i = 0; i < CARDS; ++i) {
                const card_t *c = &p.card[i];
                if (!c->used) continue;
                int32_t  cx, cy;
                uint16_t k;
                card_now(c, &cx, &cy, &k);
                const int moving = c->t < c->dur;
                it = stage_put(st, (uint32_t)(K_CARD + i), STAGE_CARD, moving ? 50 : c->z, cx, cy);
                it->tile = c->shown == PK_HIDDEN ? ART_CARD_BACK : c->shown;
                if (moving && c->spin != 0) {
                        it->turn = c->spin;
                        it->scale = k;
                        it->x = (int16_t)(cx - (ART_CARD_W - ART_CARD_W * k / 256) / 2);
                        it->y = (int16_t)(cy - (ART_CARD_H - ART_CARD_H * k / 256) / 2);
                } else {
                        int32_t w = ART_CARD_W * k / 256;
                        if (c->flip > 0) {
                                const int32_t f = c->flip > FLIP / 2 ? c->flip - FLIP / 2 : FLIP / 2 - c->flip;
                                w = w * f / (FLIP / 2);
                                if (w < 2) w = 2;
                        }
                        it->x = (int16_t)(cx + (ART_CARD_W * k / 256 - w) / 2);
                        it->w = (int16_t)w;
                        it->h = (int16_t)(ART_CARD_H * k / 256);
                }
        }
        /* The player's discards dimmed. */
        for (int i = 0; i < 5 && p.discards; ++i) {
                if (!(p.discards & (1u << i)) || p.seat[ME].ids[i] == NONE) continue;
                const card_t *c = &p.card[p.seat[ME].ids[i]];
                int32_t       cx, cy;
                uint16_t      k;
                card_now(c, &cx, &cy, &k);
                it = stage_put(st, (uint32_t)(K_SHADE + i), STAGE_SHADE, 45, cx, cy);
                it->tile = (uint16_t)(c->shown == PK_HIDDEN ? ART_CARD_BACK : c->shown);
                it->flags = STAGE_ON_CARD;
                it->alpha = 110;
        }
        /* The pot. */
        if (p.pot > 0) {
                pot_place(&x, &y);
                it = stage_put(st, K_POT, STAGE_CHIPS, 25, x, y);
                it->rgb = (uint32_t)p.pot;
                char amount[16];
                money_text(amount, sizeof(amount), p.pot_want > p.pot ? p.pot_want : p.pot);
                it = stage_put(st, K_POT_TEXT, STAGE_TEXT, 26, x + 56, y + 4);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "POT %s", amount);
        }
        /* The duck to act: an arrow over it, bobbing. */
        if (p.turn != PK_NOBODY && p.turn != ME && caught_up()) {
                const int32_t bob = (int32_t)((p.ticks / 4) % 8);
                it = stage_put(st, K_ARROW, STAGE_SPRITE, 62,
                               seat_at[p.turn].duck_x + 42 - art_sprites[ART_ARROW].w / 2,
                               seat_at[p.turn].duck_y - 40 + (bob < 4 ? bob : 8 - bob));
                it->tile = ART_ARROW;
        }
        /* Chips on their way. */
        for (int i = 0; i < FLYING; ++i) {
                const flying_t *f = &p.flying[i];
                if (!f->used || f->delay > 0) continue;
                it = stage_put(st, (uint32_t)(K_FLYING + i), STAGE_CHIPS, 55, ease(f->fx, f->tx, f->t, f->dur),
                               ease(f->fy, f->ty, f->t, f->dur));
                it->rgb = (uint32_t)f->amount;
        }
        layout_band(st);
        show_buttons();
        for (int i = 0; i < p.button_count; ++i) {
                const ui_button_t *bt = &p.button_at[i];
                it = stage_put(st, (uint32_t)(K_BUTTON + i), STAGE_BUTTON, 80, bt->r.x, bt->r.y);
                it->w = (int16_t)bt->r.width;
                it->h = (int16_t)bt->r.height;
                it->font = p.hover == i ? UI_HOVER : UI_NORMAL;
                snprintf(it->text, sizeof(it->text), "%s", bt->label);
        }
        if (p.options) layout_options(st);
        return;
}

/* ---- Options */

static int busy(void);

static void restart(void) {
        new_table();
        return;
}

static void option_change(int line) {
        switch (line) {
                case 0:
                        p.choice = (p.choice + 1) % GAME_CHOICES;
                        set_setting("poker.game", p.choice);
                        break;
                case 1:
                        p.tournament = !p.tournament;
                        set_setting("poker.tournament", p.tournament);
                        break;
                case 2:
                        p.ducks = p.ducks % 5 + 1;
                        set_setting("poker.ducks", p.ducks);
                        break;
                default:
                        p.stake = (p.stake + 1) % STAKE_CHOICES;
                        set_setting("poker.stakes", p.stake);
                        break;
        }
        sfx(SFX_CLICK, 0.4f);
        return;
}

/* Closing the options: a change starts a new table (asking first if a game is under way). */
static void close_options(void) {
        p.options = 0;
        const pk_rules_t r = rules_now();
        int              ducks = 0;
        for (int s = 1; s < PK_SEATS; ++s) ducks += p.table.seat[s].sitting || p.seat[s].out;
        const int same = r.game == p.table.rules.game && r.no_limit == p.table.rules.no_limit &&
                         p.tournament == p.table.tournament && ducks == p.ducks &&
                         (p.tournament || r.big_blind == p.table.rules.big_blind);
        if (same) return;
        if (busy())
                app_confirm("START A NEW TABLE?", "The options changed. The game under way ends.", "NEW TABLE",
                            restart);
        else
                restart();
        return;
}

/* ---- Input */

static const char *const RULES_HOLDEM[] = {
        "Make the best five-card hand from your two cards and the five",
        "shared on the board. Lucky deals the flop (three), the turn and",
        "the river, with a round of betting before each and after.",
        "",
        "The blinds: the two seats left of the dealer's button bet first.",
        "No limit: bet or raise any amount from the big blind (or the last",
        "raise) up to all your chips. Limit: bets of the small bet before",
        "the turn, the big bet (twice it) after; four bets a round.",
        "",
        "FOLD (F)    give up the hand",
        "CHECK/CALL (C)  bet nothing more, or match the bet",
        "BET/RAISE (R)   the size: - and + (Up, Down, the wheel), HALF POT",
        "            (H), POT (P), ALL IN (A)",
        "HINT (T)    Lucky works out how often you win from here",
        "",
        "Hands, best first: straight flush, four of a kind, full house,",
        "flush, straight, three of a kind, two pair, pair, high card.",
        "Enter deals the next hand. Cash: REBUY tops your chips up.",
};

static const char *const RULES_DRAW[] = {
        "Five cards each, face down. A round of betting (the blinds as",
        "in Hold'em), then the draw: click the cards you want to throw",
        "(or press 1 to 5), then DRAW (Enter) for new ones, or STAND PAT.",
        "Another round of betting, then the best hand wins.",
        "",
        "Fixed limit: the small bet before the draw, twice it after;",
        "four bets a round at most.",
        "",
        "FOLD (F), CHECK/CALL (C), BET/RAISE (R), HINT (T).",
        "",
        "Hands, best first: straight flush, four of a kind, full house,",
        "flush, straight, three of a kind, two pair, pair, high card.",
};

static const char *const RULES_STUD[] = {
        "Stud: no draw, no board. Each seat's cards come one round at a",
        "time, some face up for all to see, the rest face down (yours sit",
        "lower). Five-Card Stud: one down, then four up. Seven-Card Stud:",
        "two down, four up, the last down; the best five of seven count.",
        "",
        "Everyone puts in an ante. The lowest card showing brings it in",
        "(a small forced bet); later rounds start with the best hand",
        "showing. Fixed limit: the small bet for the first two rounds,",
        "twice it after; four bets a round at most.",
        "",
        "FOLD (F), CHECK/CALL (C), BET/RAISE (R), HINT (T).",
        "",
        "Hands, best first: straight flush, four of a kind, full house,",
        "flush, straight, three of a kind, two pair, pair, high card.",
};

static void press(int id) {
        switch (id) {
                case B_LOBBY: app_leave(); break;
                case B_RULES: {
                        const int game = p.table.rules.game;
                        if (game == PK_HOLDEM)
                                app_info("HOLD'EM RULES", RULES_HOLDEM,
                                         (int)(sizeof(RULES_HOLDEM) / sizeof(RULES_HOLDEM[0])));
                        else if (game == PK_DRAW)
                                app_info("FIVE-CARD DRAW RULES", RULES_DRAW,
                                         (int)(sizeof(RULES_DRAW) / sizeof(RULES_DRAW[0])));
                        else
                                app_info("STUD RULES", RULES_STUD, (int)(sizeof(RULES_STUD) / sizeof(RULES_STUD[0])));
                        break;
                }
                case B_OPTIONS: p.options = 1; break;
                case B_NEW:
                        if (busy())
                                app_confirm("START A NEW TABLE?", "The game under way ends.", "NEW TABLE", restart);
                        else
                                restart();
                        break;
                case B_HINT: hint(); break;
                case B_REBUY: rebuy(); break;
                case B_DEAL: deal(); break;
                case B_FOLD: act(PK_FOLD); break;
                case B_CALL: act(pk_can(&p.table, ME, PK_CALL) ? PK_CALL : PK_CHECK); break;
                case B_RAISE: act(PK_RAISE); break;
                case B_DRAW:
                        if (pk_discard(&p.table, ME, p.discards) == 0) sfx(SFX_CLICK, 0.4f);
                        break;
                case B_LESS: size_to(p.raise_to - p.table.rules.big_blind); break;
                case B_MORE: size_to(p.raise_to + p.table.rules.big_blind); break;
                case B_HALF:
                case B_POT: {
                        const int32_t call = pk_to_call(&p.table, ME), pot = pk_pot_total(&p.table) + call;
                        size_to(p.table.seat[ME].bet + call + (id == B_HALF ? pot / 2 : pot));
                        break;
                }
                case B_ALL: size_to(pk_max_raise(&p.table, ME)); break;
                default: break;
        }
        return;
}

/* The player's card under (@p x, @p y) (Draw's marking), or -1. */
static int my_card_at(int32_t x, int32_t y) {
        const mseat_t *m = &p.seat[ME];
        for (int i = m->count - 1; i >= 0; --i) {
                if (m->ids[i] == NONE) continue;
                int32_t cx, cy;
                seat_card_place(ME, i, m->count, &cx, &cy);
                const bmds_rect_t r = rect(cx, cy, ART_CARD_W, ART_CARD_H);
                if (contains(&r, x, y)) return i;
        }
        return -1;
}

static void toggle_discard(int i) {
        if (!my_draw() || i < 0 || i >= 5) return;
        p.discards ^= (uint8_t)(1u << i);
        sfx(SFX_CLICK, 0.4f);
        return;
}

static void poker_scene_event(const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t  x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const uint32_t pressed = e->data[2] & ~app.buttons;
                p.hover = ui_button_at(p.button_at, p.button_count, x, y);
                if (p.options) {
                        if ((pressed & 1u) != 0) {
                                for (int i = 0; i < OPTION_LINES; ++i) {
                                        const bmds_rect_t r = option_line(i);
                                        if (contains(&r, x, y)) {
                                                option_change(i);
                                                return;
                                        }
                                }
                                close_options();
                        }
                        return;
                }
                if ((pressed & 1u) != 0) {
                        if (p.hover >= 0)
                                press(p.buttons[p.hover]);
                        else
                                toggle_discard(my_card_at(x, y));
                }
                return;
        }
        if (e->type == BMDS_EVENT_WHEEL) {
                if (my_turn() && p.table.rules.no_limit)
                        size_to(p.raise_to + (int32_t)e->data[0] * p.table.rules.big_blind);
                return;
        }
        if (e->type == BMDS_EVENT_ACTIVE && e->data[0] == 0) {
                p.hover = -1;
                return;
        }
        if (e->type != BMDS_EVENT_KEY || (e->flags & BMDS_KEY_DOWN) == 0) return;
        const uint32_t usage = e->data[0];
        if (usage == K_M) {
                sound_toggle();
                return;
        }
        if (p.options) {
                if (usage == K_G)
                        option_change(0);
                else if (usage == K_T)
                        option_change(1);
                else if (usage == K_D)
                        option_change(2);
                else if (usage == K_S)
                        option_change(3);
                else if (usage == K_ESCAPE || usage == K_O || usage == K_ENTER)
                        close_options();
                return;
        }
        if (usage == K_ESCAPE) {
                app_leave();
                return;
        }
        if (usage >= K_1 && usage <= K_5) {
                toggle_discard((int)(usage - K_1));
                return;
        }
        for (int i = 0; i < p.button_count; ++i)
                if (p.button_at[i].key == usage || (usage == K_SPACE && p.buttons[i] == B_DEAL) ||
                    (usage == K_KP_ENTER && (p.buttons[i] == B_DEAL || p.buttons[i] == B_DRAW))) {
                        press(p.buttons[i]);
                        return;
                }
        return;
}

/* ---- The scene */

static void poker_scene_tick(void) {
        ++p.ticks;
        const int8_t turn_was = p.table.turn;
        replay();
        cards_tick();
        flying_tick();
        equity_tick();
        table_tick();
        place_cards();
        if (p.dealer_t < 24) ++p.dealer_t;
        for (int s = 0; s < PK_SEATS; ++s) {
                mseat_t *m = &p.seat[s];
                if (m->pop > 0 && m->pop < 60) ++m->pop;
                if (m->bubble_ticks > 0) --m->bubble_ticks;
                if (m->pose_ticks > 0 && --m->pose_ticks == 0) m->pose = POSE_IDLE;
        }
        /* The player's turn come: the raise starts at the least. */
        if (my_turn() && (turn_was != ME || p.raise_to == 0)) {
                p.raise_to = pk_min_raise(&p.table, ME);
        }
        if (p.table.turn != ME) p.raise_to = 0;
        /* The player out of a tournament, or its winner. */
        if (p.tournament && caught_up() && !p.finished && p.table.phase == PK_WAITING) {
                if (p.table.seat[ME].place != 0) tournament_over(p.table.seat[ME].place);
        }
        if (p.lucky_deal > 0) --p.lucky_deal;
        if (p.lucky_mood > 0) --p.lucky_mood;
        if (p.hint_ticks > 0 && p.eq_seat != ME) --p.hint_ticks;
        if (p.message_ticks > 0) --p.message_ticks;
        return;
}

static void poker_scene_draw(bmds_canvas_t *c) {
        layout();
        stage_draw(&p.stage, c);
        return;
}

static void poker_scene_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        layout();
        stage_redraw(&p.stage, c, r);
        return;
}

static int poker_scene_enter(int argc, char **argv) {
        (void)argc;
        (void)argv;
        memset(&p, 0, sizeof(p));
        deck_seed_key(&p.rng, app_key());
        p.choice = (int)setting("poker.game", 0);
        if (p.choice < 0 || p.choice >= GAME_CHOICES) p.choice = 0;
        p.tournament = setting("poker.tournament", 0) != 0;
        p.ducks = (int)setting("poker.ducks", 3);
        if (p.ducks < 1 || p.ducks > 5) p.ducks = 3;
        p.stake = (int)setting("poker.stakes", 0);
        if (p.stake < 0 || p.stake >= STAKE_CHOICES) p.stake = 0;
        p.hover = -1;
        place_seats();
        new_table();
        layout();
        memcpy(p.stage.shown, p.stage.now, sizeof(p.stage.now));
        p.stage.dirty_count = 0;
        return 0;
}

static void poker_scene_leave(void) {
        /* Cash: the chips back in the bank (what is in a pot under way is lost: the leave box said
           so). A tournament's fee was paid at the start. */
        if (!p.tournament) {
                bank_add(p.table.seat[ME].stack - p.synced);
                p.synced = p.table.seat[ME].stack;
        }
        return;
}

/* Cash: a hand the player is in. A tournament: until the player is out (leaving forfeits it). */
static int busy(void) {
        const pk_table_t *t = &p.table;
        if (p.tournament) return !p.finished && t->hands > 0;
        return (t->phase != PK_WAITING || !caught_up()) && t->seat[ME].put > 0 && !p.hand_over;
}

static int poker_scene_busy(void) {
        return busy();
}

const scene_t poker_scene = {
        "poker",          "Poker",          poker_scene_enter,  poker_scene_leave, poker_scene_event,
        poker_scene_tick, poker_scene_draw, poker_scene_redraw, poker_scene_busy,
};
