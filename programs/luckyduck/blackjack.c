/**
 * @file      blackjack.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Blackjack (devplan/luckyduck.md §8b, §16): the player at the middle seat, bot
 * ducks either side, Lucky dealing. The rules are blackjack_rules.c's; this scene replays their
 * log one event at a time into its own model of the table (the picture lags the rules, each
 * event's animation holding the next), so what it draws comes only from what the player's seat
 * may see.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blackjack_rules.h"
#include "stage.h"
#include "ui.h"

#define PLAYER 2
#define BAND_H 64
#define BAND_Y (HEIGHT - BAND_H)
#define CARDS 64 /* card sprites a round */
#define FLIGHT 11
#define FLIP 8
#define MOVE 10
#define PILE_FLIGHT 16
#define FLYING 16
#define BOT_THINK 36

/* Stage keys. */
enum {
        K_CARD = 0,       /* + the card sprite */
        K_TOTAL = 64,     /* + seat × 4 + hand; the dealer's at K_TOTAL + 20 */
        K_RESULT = 90,    /* + seat × 4 + hand */
        K_PILE = 111,     /* + seat × 4 + hand */
        K_INSURE = 131,   /* + seat */
        K_FLYING = 136,   /* + the flying pile */
        K_BOT = 152,      /* + seat */
        K_BOT_NAME = 157, /* + seat */
        K_BOT_STACK = 162,
        K_RACK = 170, /* + the chip */
        K_BANK_LABEL = 177,
        K_BANK,
        K_BET_LABEL,
        K_BET,
        K_BUTTON = 182, /* + the button */
        K_LUCKY = 192,
        K_BUBBLE,
        K_BUBBLE_TEXT,
        K_SHOE,
        K_TRAY,
        K_SHADOW,
        K_ARROW,
        K_BOX = 200, /* + 12 */
        K_MESSAGE = 220
};

/* A card on the table: where it is going, and how it moves there. */
typedef struct {
                uint8_t  used;
                uint8_t  card;   /* its face (BJ_HIDDEN while unknown) */
                uint8_t  shown;  /* the face shown (it turns at a flip's middle) */
                uint8_t  up;     /* to show its face once landed */
                int16_t  tx, ty; /* where it is going (top-left) */
                uint16_t tscale; /* its size there, of 256 */
                int16_t  fx, fy; /* where it moved from */
                uint16_t fscale;
                uint16_t t, dur;  /* the move's ticks */
                uint16_t spin;    /* turns while flying (of BMDS_GAME_TURN) */
                uint8_t  flip;    /* a flip's ticks left */
                uint8_t  placed;  /* a target set */
                uint8_t  discard; /* going to the tray, then gone */
} card_t;

/* A hand as the picture shows it. */
typedef struct {
                uint8_t  ids[BJ_CARDS];
                uint8_t  n;
                int32_t  bet;
                uint8_t  result; /* BJ_NONE until settled */
                uint8_t  bust, blackjack;
                uint16_t pop; /* ticks since its headline came */
                char     headline[16];
} mhand_t;

typedef struct {
                mhand_t  hand[BJ_HANDS];
                uint8_t  hands;
                int32_t  insurance;
                uint8_t  pose; /* a bot's: BOT_POSE_... */
                uint16_t pose_ticks;
} mseat_t;

/* A pile of chips sliding somewhere, then gone (or added to a hand's bet). */
typedef struct {
                uint8_t  used;
                int32_t  amount;
                int16_t  fx, fy, tx, ty;
                uint16_t t, dur, delay;
                int8_t   seat, hand; /* lands on this hand's bet (seat -1: just goes) */
} flying_t;

enum {
        POSE_IDLE,
        POSE_THINK,
        POSE_HAPPY,
        POSE_SAD
};

/* The buttons: the system's (top right) and the game's (the band, by the phase they belong to). */
enum {
        B_LOBBY,
        B_RULES,
        B_OPTIONS,
        B_CLEAR,
        B_REBET,
        B_DEAL,
        B_INSURE,
        B_NO,
        B_HINT,
        B_SURRENDER,
        B_SPLIT,
        B_DOUBLE,
        B_STAND,
        B_HIT,
        BUTTONS
};

static const char *const BUTTON_LABEL[BUTTONS] = {"LOBBY", "RULES",     "OPTIONS", "CLEAR", "REBET",
                                                  "DEAL",  "INSURANCE", "NO",      "HINT",  "SURRENDER",
                                                  "SPLIT", "DOUBLE",    "STAND",   "HIT"};
/* Their keys: F1 the rules, Enter deals, B bets as last time, T a tip (the hint). */
static const uint32_t BUTTON_KEY[BUTTONS] = {0, K_F1, K_O, K_C, K_B, K_ENTER, K_Y, K_N, K_T, K_R, K_P, K_D, K_S, K_H};

static struct {
                bj_table_t table;
                stage_t    stage;
                /* The picture's model. */
                card_t   card[CARDS];
                mseat_t  seat[BJ_SEATS];
                mhand_t  dealer;
                int8_t   turn_seat;
                uint8_t  turn_hand;
                flying_t flying[FLYING];
                uint32_t cursor;     /* the next event to show */
                uint16_t wait;       /* ticks before the next */
                int32_t  bank_shown; /* the player's stack as the picture has got to */
                int32_t  synced;     /* the stack the bank was last written from */
                int32_t  last_bet;
                int      bots;
                uint16_t bot_wait;
                uint16_t lucky_deal, lucky_mood; /* ticks of a dealing flick; of a cheer or a droop */
                int      lucky_pose;
                uint16_t shuffle_ticks;
                /* Input. */
                int         buttons[BUTTONS]; /* shown now (in order of the list) */
                int         button_count;
                ui_button_t button_at[BUTTONS];
                int         hover, rack_hover;
                int         options;
                uint16_t    hint_ticks;
                char        hint[32];
                uint16_t    message_ticks; /* a word to the player over its circle */
                char        message[sizeof(((stage_item_t *)0)->text)];
                uint8_t     discards; /* cards in the tray since the shuffle */
                uint32_t    ticks;
} b;

/* ---- Geometry */

static const int32_t SPOT_DX[BJ_SEATS] = {480, 250, 0, -250, -480};

/* The table's edge: an ellipse whose bottom runs along the band, its sides rising. */
static int32_t edge_cx(void) {
        return WIDTH / 2;
}
static int32_t edge_cy(void) {
        return BAND_Y - 420;
}
static int32_t edge_rx(void) {
        return WIDTH * 62 / 100;
}
#define EDGE_RY 400

static int32_t edge_y(int32_t x) {
        const float dx = (float)(x - edge_cx()) / (float)edge_rx();
        return edge_cy() + (int32_t)(EDGE_RY * sqrtf(1.0f - dx * dx));
}

/* The betting circles' middles, worked out on entering (the picture's size known). */
static int32_t spot_x[BJ_SEATS], spot_y[BJ_SEATS];

static void place_spots(void) {
        for (int s = 0; s < BJ_SEATS; ++s) {
                spot_x[s] = WIDTH / 2 + SPOT_DX[s] * WIDTH / 1272;
                spot_y[s] = edge_y(spot_x[s]) - 74;
        }
        return;
}

/* Seat @p s's betting circle's middle. */
static void spot(int s, int32_t *x, int32_t *y) {
        *x = spot_x[s];
        *y = spot_y[s];
        return;
}

static uint16_t seat_scale(int s) {
        if (s != PLAYER) return 184; /* 72% */
        return b.seat[s].hands >= 3 ? 192 : 256;
}

/* Hand @p h of seat @p s: its first card's top-left, its cards' step and size. */
static void hand_place(int s, int h, int32_t *x, int32_t *y, int32_t *step, uint16_t *scale) {
        int32_t sx, sy;
        spot(s, &sx, &sy);
        const uint16_t k = seat_scale(s);
        const int32_t  w = ART_CARD_W * k / 256, hh = ART_CARD_H * k / 256;
        const int      n = b.seat[s].hands > 0 ? b.seat[s].hands : 1;
        *step = 24 * k / 256;
        const int32_t slot = w + *step * 3 + 14;
        *x = sx - n * slot / 2 + h * slot + (slot - w - *step) / 2 - 7;
        *y = sy - 30 - hh;
        *scale = k;
        return;
}

static void dealer_place(int i, int n, int32_t *x, int32_t *y) {
        const int32_t width = ART_CARD_W + 30 * (n - 1);
        *x = WIDTH / 2 - width / 2 + i * 30;
        *y = 142;
        return;
}

/* The shoe stands left of the system buttons. */
#define SHOE_X (WIDTH - 300)

static void shoe_mouth(int32_t *x, int32_t *y) {
        *x = SHOE_X + 110;
        *y = 40;
        return;
}

static void tray_place(int32_t *x, int32_t *y) {
        *x = 52;
        *y = 30;
        return;
}

/* Where a hand's bet pile sits (its bottom chip's top-left). */
static void pile_place(int s, int h, int32_t *x, int32_t *y) {
        int32_t  hx, hy, step;
        uint16_t k;
        hand_place(s, h, &hx, &hy, &step, &k);
        int32_t sx, sy;
        spot(s, &sx, &sy);
        *x = (b.seat[s].hands <= 1 ? sx : hx + ART_CARD_W * k / 512) - 24;
        *y = sy - 12;
        return;
}

static void bot_place(int s, int32_t *x, int32_t *y) {
        int32_t sx, sy;
        spot(s, &sx, &sy);
        *x = s < PLAYER ? sx + 46 : sx - 46 - 84;
        *y = sy - 70;
        return;
}

/* ---- Easing */

static int32_t ease(int32_t from, int32_t to, uint32_t t, uint32_t n) {
        if (t >= n) return to;
        const int32_t k = (int32_t)n;
        const int32_t u = (int32_t)t;
        return from + (to - from) * (2 * k * u - u * u) / (k * k); /* out */
}

/* ---- Cards */

static int new_card(uint8_t face, int up) {
        for (int i = 0; i < CARDS; ++i) {
                if (b.card[i].used) continue;
                card_t *c = &b.card[i];
                memset(c, 0, sizeof(*c));
                c->used = 1;
                c->card = face;
                c->shown = BJ_HIDDEN;
                c->up = (uint8_t)up;
                int32_t x, y;
                shoe_mouth(&x, &y);
                c->fx = c->tx = (int16_t)(x - ART_CARD_W / 2);
                c->fy = c->ty = (int16_t)y;
                c->fscale = c->tscale = 150;
                return i;
        }
        return -1;
}

/* Where card @p c shows now. */
static void card_now(const card_t *c, int32_t *x, int32_t *y, uint16_t *scale) {
        *x = ease(c->fx, c->tx, c->t, c->dur);
        *y = ease(c->fy, c->ty, c->t, c->dur);
        *scale = (uint16_t)ease(c->fscale, c->tscale, c->t, c->dur);
        return;
}

/* Sends card @p id towards (@p x, @p y) at @p scale: from where it shows, if that changed. */
static void card_to(int id, int32_t x, int32_t y, uint16_t scale, uint16_t dur) {
        card_t *c = &b.card[id];
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

/* Every card's place from the model: hands, the dealer's, the discards. */
static void place_cards(void) {
        for (int s = 0; s < BJ_SEATS; ++s) {
                for (int h = 0; h < b.seat[s].hands; ++h) {
                        const mhand_t *mh = &b.seat[s].hand[h];
                        int32_t        x, y, step;
                        uint16_t       k;
                        hand_place(s, h, &x, &y, &step, &k);
                        for (int i = 0; i < mh->n; ++i) card_to(mh->ids[i], x + i * step, y - i * 2, k, FLIGHT);
                }
        }
        for (int i = 0; i < b.dealer.n; ++i) {
                int32_t x, y;
                dealer_place(i, b.dealer.n, &x, &y);
                card_to(b.dealer.ids[i], x, y, 256, FLIGHT);
        }
        int32_t tx, ty;
        tray_place(&tx, &ty);
        for (int i = 0; i < CARDS; ++i)
                if (b.card[i].used && b.card[i].discard) card_to(i, tx + 8, ty - 30, 150, 14);
        return;
}

static void cards_tick(void) {
        for (int i = 0; i < CARDS; ++i) {
                card_t *c = &b.card[i];
                if (!c->used) continue;
                if (c->t < c->dur) {
                        ++c->t;
                        c->spin = c->t < c->dur ? (uint16_t)(c->spin + 2900) : 0;
                        if (c->t == c->dur && c->up && c->shown != c->card) c->flip = FLIP;
                } else if (c->discard) {
                        c->used = 0;
                        if (b.discards < 255) ++b.discards;
                }
                if (c->flip > 0) {
                        --c->flip;
                        if (c->flip == FLIP / 2) c->shown = c->card;
                }
        }
        return;
}

/* ---- Chips moving */

static void fly(int32_t amount, int32_t fx, int32_t fy, int32_t tx, int32_t ty, uint16_t delay, int seat, int hand) {
        if (amount <= 0) return;
        for (int i = 0; i < FLYING; ++i) {
                if (b.flying[i].used) continue;
                flying_t *f = &b.flying[i];
                f->used = 1;
                f->amount = amount;
                f->fx = (int16_t)fx;
                f->fy = (int16_t)fy;
                f->tx = (int16_t)tx;
                f->ty = (int16_t)ty;
                f->t = 0;
                f->dur = PILE_FLIGHT;
                f->delay = delay;
                f->seat = (int8_t)seat;
                f->hand = (int8_t)hand;
                return;
        }
        return;
}

static void flying_tick(void) {
        for (int i = 0; i < FLYING; ++i) {
                flying_t *f = &b.flying[i];
                if (!f->used) continue;
                if (f->delay > 0) {
                        --f->delay;
                        continue;
                }
                if (++f->t >= f->dur) {
                        if (f->seat >= 0) b.seat[f->seat].hand[f->hand].bet += f->amount;
                        if (f->seat == PLAYER || f->seat == -2) sfx(SFX_PLACE, 0.5f);
                        f->used = 0;
                }
        }
        return;
}

/* Where the player's chips go to and come from: the bank, in the band. */
static void bank_point(int32_t *x, int32_t *y) {
        *x = 440;
        *y = BAND_Y + 14;
        return;
}

static void dealer_point(int32_t *x, int32_t *y) {
        *x = WIDTH / 2 - 24;
        *y = 104;
        return;
}

static void owner_point(int s, int32_t *x, int32_t *y) {
        if (s == PLAYER) {
                bank_point(x, y);
        } else {
                bot_place(s, x, y);
                *x += 18;
                *y += 40;
        }
        return;
}

/* ---- The model, from the log */

static void headline(mhand_t *h, const char *text) {
        snprintf(h->headline, sizeof(h->headline), "%s", text);
        h->pop = 1;
        return;
}

static void set_pose(int s, int pose, uint16_t ticks) {
        b.seat[s].pose = (uint8_t)pose;
        b.seat[s].pose_ticks = ticks;
        return;
}

static mhand_t *hand_of(int seat, int hand) {
        return seat == BJ_DEALER ? &b.dealer : &b.seat[seat].hand[hand];
}

/* Shows event @p e: changes the model, starts its animation. @return The ticks it holds the next. */
static uint16_t show(const bj_event_t *e) {
        int32_t x, y, px, py;
        switch (e->type) {
                case BJ_EV_SHUFFLE:
                        b.shuffle_ticks = 60;
                        b.discards = 0;
                        sfx(SFX_DEAL, 0.8f);
                        return b.cursor > 1 ? 50 : 0;
                case BJ_EV_BET: {
                        mseat_t      *s = &b.seat[e->seat];
                        const int32_t had = s->hand[0].bet;
                        if (e->seat == PLAYER) b.bank_shown += had - e->amount;
                        if (e->amount > had) {
                                owner_point(e->seat, &x, &y);
                                pile_place(e->seat, 0, &px, &py);
                                fly(e->amount - had, x, y, px, py, 0, e->seat, 0);
                        } else {
                                s->hand[0].bet = e->amount;
                        }
                        if (s->hands == 0) s->hands = 1;
                        return e->seat == PLAYER ? 0 : 10;
                }
                case BJ_EV_DEAL: {
                        mhand_t  *h = hand_of(e->seat, e->hand);
                        const int id = new_card(e->card, e->up && e->card != BJ_HIDDEN);
                        if (id < 0 || h->n >= BJ_CARDS) return 0;
                        h->ids[h->n++] = (uint8_t)id;
                        b.card[id].placed = 0;
                        b.lucky_deal = 8;
                        sfx(SFX_PLACE, 0.6f);
                        /* The deal goes round briskly; the dealer's own draws land and turn before
                           what follows from them shows. */
                        return e->seat == BJ_DEALER && b.dealer.n > 2 ? FLIGHT + FLIP + 6 : 9;
                }
                case BJ_EV_HOLE:
                        if (b.dealer.n > 1) {
                                card_t *c = &b.card[b.dealer.ids[1]];
                                c->card = e->card;
                                c->up = 1;
                                c->flip = FLIP;
                        }
                        sfx(SFX_FLIP, 0.6f);
                        return 18;
                case BJ_EV_INSURANCE:
                        if (e->amount > 0) {
                                b.seat[e->seat].insurance = e->amount;
                                if (e->seat == PLAYER) b.bank_shown -= e->amount;
                        }
                        return 6;
                case BJ_EV_INSURED: {
                        mseat_t *s = &b.seat[e->seat];
                        spot(e->seat, &px, &py);
                        if (e->amount > 0) {
                                owner_point(e->seat, &x, &y);
                                fly(s->insurance + e->amount, px - 100, py - 12, x, y, 0, -1, 0);
                                if (e->seat == PLAYER) b.bank_shown += s->insurance + e->amount;
                        } else {
                                dealer_point(&x, &y);
                                fly(s->insurance, px - 100, py - 12, x, y, 0, -1, 0);
                        }
                        s->insurance = 0;
                        return 20;
                }
                case BJ_EV_TURN:
                        b.turn_seat = e->seat;
                        b.turn_hand = e->hand;
                        if (e->seat != PLAYER) set_pose(e->seat, POSE_THINK, 0);
                        return 0;
                case BJ_EV_SPLIT: {
                        mseat_t *s = &b.seat[e->seat];
                        mhand_t *from = &s->hand[e->hand], *to = &s->hand[e->amount];
                        memset(to, 0, sizeof(*to));
                        to->ids[0] = from->ids[1];
                        to->n = 1;
                        from->n = 1;
                        s->hands = (uint8_t)(e->amount + 1);
                        owner_point(e->seat, &x, &y);
                        pile_place(e->seat, e->amount, &px, &py);
                        fly(from->bet, x, y, px, py, 4, e->seat, e->amount);
                        if (e->seat == PLAYER) b.bank_shown -= from->bet;
                        return 16;
                }
                case BJ_EV_DOUBLE:
                        owner_point(e->seat, &x, &y);
                        pile_place(e->seat, e->hand, &px, &py);
                        fly(e->amount, x, y, px, py, 0, e->seat, e->hand);
                        if (e->seat == PLAYER) b.bank_shown -= e->amount;
                        return 12;
                case BJ_EV_SURRENDER: headline(&b.seat[e->seat].hand[e->hand], "SURRENDER"); return 18;
                case BJ_EV_BUST: {
                        mhand_t *h = hand_of(e->seat, e->hand);
                        h->bust = 1;
                        headline(h, "BUST");
                        if (e->seat == PLAYER) sfx(SFX_SAD, 0.4f);
                        if (e->seat == BJ_DEALER) {
                                b.lucky_pose = ART_LUCKY_SAD;
                                b.lucky_mood = 90;
                        } else if (e->seat != PLAYER) {
                                set_pose(e->seat, POSE_SAD, 80);
                        }
                        return 26;
                }
                case BJ_EV_BLACKJACK: {
                        mhand_t *h = hand_of(e->seat, e->hand);
                        h->blackjack = 1;
                        headline(h, "BLACKJACK!");
                        if (e->seat == PLAYER) {
                                sfx(SFX_WIN, 0.7f);
                                b.lucky_pose = ART_LUCKY_CHEER;
                                b.lucky_mood = 90;
                        } else if (e->seat != BJ_DEALER) {
                                set_pose(e->seat, POSE_HAPPY, 90);
                                sfx(SFX_QUACK2, 0.4f);
                        }
                        return 30;
                }
                case BJ_EV_RESULT: {
                        mseat_t *s = &b.seat[e->seat];
                        mhand_t *h = &s->hand[e->hand];
                        h->result = e->result;
                        pile_place(e->seat, e->hand, &px, &py);
                        owner_point(e->seat, &x, &y);
                        int32_t dx, dy;
                        dealer_point(&dx, &dy);
                        const int32_t bet = h->bet;
                        h->bet = 0;
                        switch (e->result) {
                                case BJ_WIN:
                                case BJ_BLACKJACK_WIN:
                                        /* Paid from the dealer's tray beside the bet, then both home. */
                                        fly(e->amount - bet, dx, dy, px + 30, py, 0, -1, 0);
                                        fly(bet, px, py, x, y, PILE_FLIGHT + 6, e->seat == PLAYER ? -2 : -1, 0);
                                        fly(e->amount - bet, px + 30, py, x, y, PILE_FLIGHT + 6, -1, 0);
                                        if (!h->headline[0] || h->blackjack) {
                                                if (!h->blackjack) headline(h, "WIN");
                                        }
                                        if (e->seat != PLAYER) set_pose(e->seat, POSE_HAPPY, 70);
                                        break;
                                case BJ_PUSH:
                                        fly(bet, px, py, x, y, 4, -1, 0);
                                        if (!h->blackjack) headline(h, "PUSH");
                                        break;
                                case BJ_SURRENDERED:
                                        fly(e->amount, px, py, x, y, 4, -1, 0);
                                        fly(bet - e->amount, px, py, dx, dy, 4, -1, 0);
                                        break;
                                default: /* lost: to the dealer */
                                        fly(bet, px, py, dx, dy, 4, -1, 0);
                                        if (!h->bust) headline(h, "LOSE");
                                        if (e->seat != PLAYER && !h->bust) set_pose(e->seat, POSE_SAD, 70);
                                        break;
                        }
                        if (e->seat == PLAYER) {
                                b.bank_shown += e->amount;
                                if (e->result == BJ_WIN || e->result == BJ_BLACKJACK_WIN) sfx(SFX_WIN, 0.4f);
                        }
                        return 22;
                }
                case BJ_EV_ROUND_END: {
                        /* The bank written: the stack, as the rules have it now. */
                        const int32_t stack = b.table.seat[PLAYER].stack;
                        bank_add(stack - b.synced);
                        b.synced = stack;
                        b.turn_seat = -1;
                        return 0;
                }
                default: return 0;
        }
}

/* Takes the table's events the player's seat may see, one at a time as their animations end. */
static void replay(void) {
        if (b.wait > 0) {
                --b.wait;
                return;
        }
        while (b.wait == 0 && b.cursor < b.table.event_count) {
                const bj_event_t e = bj_event_for(&b.table, b.cursor++, PLAYER);
                b.wait = show(&e);
        }
        return;
}

static int caught_up(void) {
        if (b.wait > 0 || b.cursor < b.table.event_count) return 0;
        for (int i = 0; i < FLYING; ++i)
                if (b.flying[i].used) return 0;
        return 1;
}

/* ---- The round */

/* Clears the table for a new round: the cards to the tray, the model empty. */
static void sweep(void) {
        for (int i = 0; i < CARDS; ++i)
                if (b.card[i].used) b.card[i].discard = 1;
        for (int s = 0; s < BJ_SEATS; ++s) {
                memset(b.seat[s].hand, 0, sizeof(b.seat[s].hand));
                b.seat[s].hands = 0;
                b.seat[s].insurance = 0;
                if (b.seat[s].pose != POSE_IDLE && b.seat[s].pose_ticks == 0) b.seat[s].pose = POSE_IDLE;
        }
        memset(&b.dealer, 0, sizeof(b.dealer));
        b.cursor = 0;
        b.turn_seat = -1;
        return;
}

/* Betting again after a round: the cards swept, Lucky's top-up if the bank ran dry. */
static void next_round(void) {
        if (b.table.phase != BJ_SETTLED) return;
        bj_next_round(&b.table);
        sweep();
        bj_seat_t *me = &b.table.seat[PLAYER];
        if (me->stack < b.table.rules.min_bet && bank_ensure(b.table.rules.min_bet)) {
                me->stack = (int32_t)bank_balance();
                b.synced = me->stack;
                b.bank_shown = me->stack;
                b.lucky_pose = ART_LUCKY_SAD;
                b.lucky_mood = 120;
                sfx(SFX_SAD, 0.5f);
        }
        return;
}

static void bot_bets(void) {
        static const int32_t amounts[] = {5, 10, 10, 25, 25, 50};
        for (int s = 0; s < BJ_SEATS; ++s) {
                if (s == PLAYER || !b.table.seat[s].playing) continue;
                if (b.table.seat[s].stack < 50) b.table.seat[s].stack += 1000; /* a duck's own top-up */
                (void)bj_bet(&b.table, s, amounts[(unsigned)rand() % 6]);
        }
        return;
}

static void deal(void) {
        if (b.table.phase == BJ_SETTLED) next_round();
        if (b.table.phase != BJ_BETTING) return;
        if (b.table.seat[PLAYER].hand[0].bet == 0) {
                if (b.last_bet == 0 || bj_bet(&b.table, PLAYER, b.last_bet) != 0) return;
        }
        const int32_t bet = b.table.seat[PLAYER].hand[0].bet;
        if (!bj_bet_ok(&b.table, PLAYER)) { /* said, not dealt */
                const int under = bet < b.table.rules.min_bet;
                char      limit[12];
                money(limit, sizeof(limit), under ? b.table.rules.min_bet : b.table.rules.max_bet);
                snprintf(b.message, sizeof(b.message), "TABLE %s IS %s", under ? "MINIMUM" : "MAXIMUM", limit);
                b.message_ticks = 150;
                sfx(SFX_QUACK2, 0.4f);
                return;
        }
        b.last_bet = bet;
        bot_bets();
        (void)bj_deal(&b.table);
        return;
}

static void add_chip(int kind) {
        if (b.table.phase == BJ_SETTLED) next_round();
        if (b.table.phase != BJ_BETTING) return;
        /* The chip as it is: the limits are the deal's to check. */
        const int32_t want = b.table.seat[PLAYER].hand[0].bet + ui_chip_value[kind];
        if (bj_bet(&b.table, PLAYER, want) != 0) {
                sfx(SFX_CLICK, 0.4f);
                return;
        }
        sfx(SFX_PLACE, 0.5f);
        return;
}

static void take_chip(void) {
        if (b.table.phase != BJ_BETTING) return;
        const int32_t now = b.table.seat[PLAYER].hand[0].bet;
        if (now == 0) return;
        int32_t less = 0;
        for (int k = UI_CHIP_KINDS - 1; k >= 0; --k)
                if (ui_chip_value[k] <= now) {
                        less = now - ui_chip_value[k];
                        break;
                }
        (void)bj_bet(&b.table, PLAYER, less);
        sfx(SFX_CLICK, 0.5f);
        return;
}

static void act(int action) {
        if (bj_act(&b.table, PLAYER, action) == 0) {
                b.hint_ticks = 0;
                sfx(SFX_CLICK, 0.4f);
        }
        return;
}

/* The bots and the dealer's part, once the picture has caught up. */
static void table_tick(void) {
        if (!caught_up()) return;
        bj_table_t *t = &b.table;
        int         seat = -1;
        if (t->phase == BJ_INSURANCE) seat = bj_insurance_seat(t);
        if (t->phase == BJ_PLAYING) seat = t->turn_seat;
        if (seat < 0 || seat == PLAYER) {
                b.bot_wait = 0;
                return;
        }
        if (++b.bot_wait < BOT_THINK) return;
        b.bot_wait = 0;
        if (t->phase == BJ_INSURANCE) {
                (void)bj_insure(t, seat, 0); /* basic strategy never insures */
                return;
        }
        const bj_hand_t *h = &t->seat[seat].hand[t->turn_hand];
        int              action = bj_basic(h->cards, h->count, t->dealer.cards[0], bj_can(t, seat, BJ_DOUBLE),
                                           bj_can(t, seat, BJ_SPLIT), bj_can(t, seat, BJ_SURRENDER), t->rules.hit_soft_17);
        if (!bj_can(t, seat, action)) action = BJ_STAND;
        (void)bj_act(t, seat, action);
        set_pose(seat, POSE_IDLE, 0);
        return;
}

/* ---- The felt */

static uint16_t rgb565(uint32_t rgb) {
        return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

/* The table's padded rail along its edge, the floor beyond, the band. */
static void paint_rail(void) {
        uint16_t       *px = (uint16_t *)(void *)app.felt.pixels;
        const int64_t   cx = edge_cx(), cy = edge_cy(), rx = edge_rx(), ry = EDGE_RY;
        const int64_t   orx = rx + 30, ory = ry + 30;
        static uint16_t rail[31];
        for (int i = 0; i <= 30; ++i) { /* leather, rounded: lighter in the middle */
                const int32_t d = i - 15, k = 255 - d * d * 255 / 300;
                rail[i] = rgb565(((uint32_t)(0x5a * k / 255) << 16) | ((uint32_t)(0x30 * k / 255) << 8) |
                                 (uint32_t)(0x1a * k / 255));
        }
        const uint16_t floor = rgb565(0x1c120c), band = rgb565(0x2a1a10), gold = rgb565(0xb08028);
        for (int32_t y = 0; y < HEIGHT; ++y) {
                for (int32_t x = 0; x < WIDTH; ++x) {
                        uint16_t *p = &px[(size_t)y * WIDTH + x];
                        if (y >= BAND_Y) {
                                *p = y < BAND_Y + 2 ? gold : band;
                                continue;
                        }
                        const int64_t dx = x - cx, dy = y - cy;
                        if (dy < 0) continue; /* above the edge's middle: felt */
                        const int64_t qi = dx * dx * ry * ry + dy * dy * rx * rx;
                        if (qi <= rx * rx * ry * ry) continue; /* the felt */
                        const int64_t qo = dx * dx * ory * ory + dy * dy * orx * orx;
                        if (qo > orx * orx * ory * ory) {
                                *p = floor;
                                continue;
                        }
                        /* How far across the rail: by where between the two the point falls. */
                        const int64_t a = qi / (rx * rx) - ry * ry, z = qo / (orx * orx) - ory * ory;
                        int32_t       i = (int32_t)(30 * a / (a - z > 0 ? a - z : 1));
                        if (i < 0) i = 0;
                        if (i > 30) i = 30;
                        *p = rail[i];
                }
        }
        return;
}

static void paint_felt(void) {
        felt_baize(0x1a8c4a, 0x0a4422);
        paint_rail();
        bmds_canvas_t *c = &app.felt_canvas;
        /* The betting circles: gold rings. */
        for (int s = 0; s < BJ_SEATS; ++s) {
                int32_t x, y;
                spot(s, &x, &y);
                for (int32_t r = 0; r < 3; ++r)
                        for (int32_t a = 0; a < 720; ++a) {
                                const float t = (float)a * 3.14159265f / 360.0f;
                                bmds_canvas_plot(c, x + (int32_t)((40 - r) * cosf(t)),
                                                 y + (int32_t)((26 - r) * sinf(t)),
                                                 bmds_canvas_pixel(c, r == 1 ? 0xf0c850 : 0xb08028));
                        }
        }
        bmds_canvas_damage_clear(c);
        return;
}

/* ---- The frame */

static void show_buttons(void) {
        const bj_table_t *t = &b.table;
        int               n = 0;
        const int         ready = caught_up();
        /* The system's buttons (always there), then the game's for the phase. */
        b.buttons[n++] = B_LOBBY;
        b.buttons[n++] = B_RULES;
        b.buttons[n++] = B_OPTIONS;
        if (ready && (t->phase == BJ_BETTING || t->phase == BJ_SETTLED)) {
                if (t->phase == BJ_BETTING && t->seat[PLAYER].hand[0].bet > 0) b.buttons[n++] = B_CLEAR;
                if ((t->phase == BJ_SETTLED || t->seat[PLAYER].hand[0].bet == 0) && b.last_bet > 0)
                        b.buttons[n++] = B_REBET;
                if (t->phase == BJ_BETTING && t->seat[PLAYER].hand[0].bet > 0) b.buttons[n++] = B_DEAL;
        } else if (ready && t->phase == BJ_INSURANCE && bj_insurance_seat(t) == PLAYER) {
                b.buttons[n++] = B_INSURE;
                b.buttons[n++] = B_NO;
        } else if (ready && t->phase == BJ_PLAYING && t->turn_seat == PLAYER) {
                b.buttons[n++] = B_HINT;
                if (bj_can(t, PLAYER, BJ_SURRENDER)) b.buttons[n++] = B_SURRENDER;
                if (bj_can(t, PLAYER, BJ_SPLIT)) b.buttons[n++] = B_SPLIT;
                if (bj_can(t, PLAYER, BJ_DOUBLE)) b.buttons[n++] = B_DOUBLE;
                b.buttons[n++] = B_STAND;
                b.buttons[n++] = B_HIT;
        }
        b.button_count = n;
        /* Right-aligned in the band, the widest first on the left; the hint on the felt beside the
           player's circle (the band is full on the player's turn). */
        int32_t x = WIDTH - 16;
        for (int i = n - 1; i >= 0; --i) {
                if (b.buttons[i] <= B_OPTIONS) { /* the system's: stacked at the top right */
                        b.button_at[i].r = app_system_button(b.buttons[i]);
                        b.button_at[i].label = BUTTON_LABEL[b.buttons[i]];
                        b.button_at[i].key = BUTTON_KEY[b.buttons[i]];
                        continue;
                }
                if (b.buttons[i] == B_HINT) {
                        int32_t sx, sy;
                        spot(PLAYER, &sx, &sy);
                        b.button_at[i].r = rect(sx - 170, sy - 18, 90, 36);
                        b.button_at[i].label = BUTTON_LABEL[B_HINT];
                        b.button_at[i].key = BUTTON_KEY[B_HINT];
                        continue;
                }
                const int   id = b.buttons[i];
                const char *label =
                        id == B_INSURE && bj_is_blackjack(&t->seat[PLAYER].hand[0]) ? "EVEN MONEY" : BUTTON_LABEL[id];
                const int32_t w = ui_text_width(ART_FONT_SMALL, label) + 26;
                x -= w;
                b.button_at[i].r = rect(x, BAND_Y + 13, w, 40);
                b.button_at[i].label = label;
                b.button_at[i].key = BUTTON_KEY[id];
                x -= 8;
        }
        return;
}

static int bot_sprite(int s) {
        static const int base[BJ_SEATS] = {ART_BOT_MALLARD_IDLE, ART_BOT_PEKIN_IDLE, 0, ART_BOT_WOOD_IDLE,
                                           ART_BOT_RUBBER_IDLE};
        return base[s] + b.seat[s].pose;
}

static const char *const BOT_NAME[BJ_SEATS] = {"MALLORY", "PEARL", "", "WOODY", "SQUEAKY"};

static void layout_hand(stage_t *st, int s, int h, const mhand_t *mh, int key) {
        int32_t  x, y, step;
        uint16_t k;
        if (s == BJ_DEALER) {
                dealer_place(0, mh->n, &x, &y);
                step = 30;
                k = 256;
        } else {
                hand_place(s, h, &x, &y, &step, &k);
        }
        const int32_t w = ART_CARD_W * k / 256;
        if (mh->n > 0) {
                /* The total of the cards shown face up. */
                uint8_t vis[BJ_CARDS];
                int     n = 0;
                for (int i = 0; i < mh->n; ++i) {
                        const card_t *c = &b.card[mh->ids[i]];
                        if (c->shown != BJ_HIDDEN && c->t >= c->dur) vis[n++] = c->shown;
                }
                if (n > 0) {
                        int           soft;
                        const int     total = bj_total(vis, n, &soft);
                        stage_item_t *it = stage_put(st, (uint32_t)(K_TOTAL + key), STAGE_LABEL, 40, x, y - 20);
                        it->rgb = 0xfff4dc;
                        if (soft && total < 21 && n == mh->n)
                                snprintf(it->text, sizeof(it->text), "%d/%d", total - 10, total);
                        else
                                snprintf(it->text, sizeof(it->text), "%d", total);
                }
        }
        if (mh->headline[0]) {
                /* Popping up: it rises fast, overshoots and settles. */
                const int32_t t = mh->pop > 20 ? 20 : mh->pop;
                const int32_t lift = t < 12 ? t * 4 : 48 - (t - 12) * 2;
                stage_item_t *it = stage_put(st, (uint32_t)(K_RESULT + key), STAGE_TEXT, 60,
                                             x + (w + step * (mh->n - 1)) / 2, y + 30 - lift);
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", mh->headline);
        }
        return;
}

static void layout(void) {
        stage_t *st = &b.stage;
        stage_begin(st);
        int32_t       x, y;
        stage_item_t *it;
        /* The shoe, the tray, Lucky. */
        it = stage_put(st, K_SHOE, STAGE_SPRITE, 5, SHOE_X, 6);
        it->tile = ART_SHOE;
        tray_place(&x, &y);
        it = stage_put(st, K_TRAY, STAGE_SPRITE, 5, x - 10, y - 20);
        it->tile = ART_TRAY;
        if (b.discards > 0) { /* the discards: backs lying in the tray, the pile higher as it grows */
                const int32_t cx = x + 48, cy = y + 15, height = 1 + b.discards / 16;
                for (int32_t k = 0; k < height && k < 6; ++k) {
                        it = stage_put(st, (uint32_t)(K_BOX + 12 + k), STAGE_CARD, 6, cx - ART_CARD_W / 2,
                                       cy - ART_CARD_H / 2 - k * 2);
                        it->tile = ART_CARD_BACK;
                        it->turn = BMDS_GAME_TURN / 4;
                        it->scale = 98;
                }
        }
        it = stage_put(st, K_LUCKY, STAGE_SPRITE, 8, WIDTH / 2 - 64, 4);
        it->tile = (uint16_t)(b.lucky_deal > 0      ? ART_LUCKY_DEAL
                              : b.lucky_mood > 0    ? b.lucky_pose
                              : (b.ticks % 240) < 8 ? ART_LUCKY_BLINK
                                                    : ART_LUCKY_IDLE);
        if (b.hint_ticks > 0) {
                it = stage_put(st, K_BUBBLE, STAGE_PANEL, 70, WIDTH / 2 + 60, 20);
                it->w = (int16_t)(ui_text_width(ART_FONT_SMALL, b.hint) + 40);
                it->h = 44;
                it = stage_put(st, K_BUBBLE_TEXT, STAGE_TEXT, 71, WIDTH / 2 + 80, 34);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", b.hint);
        }
        if (b.message_ticks > 0) {
                int32_t sx, sy;
                spot(PLAYER, &sx, &sy);
                it = stage_put(st, K_MESSAGE + 1, STAGE_TEXT, 72, sx, sy - 220);
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", b.message);
        }
        if (b.shuffle_ticks > 0) {
                it = stage_put(st, K_MESSAGE, STAGE_TEXT, 70, SHOE_X + 65, 110);
                it->font = ART_FONT_SMALL;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "SHUFFLING");
        }
        /* Bots. */
        for (int s = 0; s < BJ_SEATS; ++s) {
                if (s == PLAYER || !b.table.seat[s].playing) continue;
                bot_place(s, &x, &y);
                it = stage_put(st, (uint32_t)(K_BOT + s), STAGE_SPRITE, 20, x, y);
                it->tile = (uint16_t)bot_sprite(s);
                if (s < PLAYER) it->flags = STAGE_MIRROR;
                it = stage_put(st, (uint32_t)(K_BOT_NAME + s), STAGE_TEXT, 20, x + 42, y + 86);
                it->font = ART_FONT_SMALL;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", BOT_NAME[s]);
        }
        /* Cards (in the order dealt: later over earlier), flying ones on top. */
        for (int i = 0; i < CARDS; ++i) {
                const card_t *c = &b.card[i];
                if (!c->used) continue;
                int32_t  cx, cy;
                uint16_t k;
                card_now(c, &cx, &cy, &k);
                const int moving = c->t < c->dur;
                it = stage_put(st, (uint32_t)(K_CARD + i), STAGE_CARD, moving ? 50 : 30, cx, cy);
                it->tile = c->shown == BJ_HIDDEN ? ART_CARD_BACK : c->shown;
                if (moving && c->spin != 0 && !c->discard) {
                        it->turn = c->spin;
                        it->scale = k;
                        /* Turned about its middle at its size: the stage's middle is its own size's. */
                        it->x = (int16_t)(cx - (ART_CARD_W - ART_CARD_W * k / 256) / 2);
                        it->y = (int16_t)(cy - (ART_CARD_H - ART_CARD_H * k / 256) / 2);
                } else {
                        int32_t w = ART_CARD_W * k / 256;
                        if (c->flip > 0) { /* narrowing to its edge, then widening on the other face */
                                const int32_t f = c->flip > FLIP / 2 ? c->flip - FLIP / 2 : FLIP / 2 - c->flip;
                                w = w * f / (FLIP / 2);
                                if (w < 2) w = 2;
                        }
                        it->x = (int16_t)(cx + (ART_CARD_W * k / 256 - w) / 2);
                        it->w = (int16_t)w;
                        it->h = (int16_t)(ART_CARD_H * k / 256);
                }
        }
        /* Hands' totals and headlines. */
        for (int s = 0; s < BJ_SEATS; ++s)
                for (int h = 0; h < b.seat[s].hands; ++h) layout_hand(st, s, h, &b.seat[s].hand[h], s * 4 + h);
        layout_hand(st, BJ_DEALER, 0, &b.dealer, 20);
        /* The hand to act: an arrow over it, bobbing. */
        if (b.turn_seat >= 0 && b.table.phase == BJ_PLAYING) {
                int32_t  hx, hy, step;
                uint16_t k;
                hand_place(b.turn_seat, b.turn_hand, &hx, &hy, &step, &k);
                const mhand_t *mh = &b.seat[b.turn_seat].hand[b.turn_hand];
                const int32_t  w = ART_CARD_W * k / 256 + step * (mh->n > 0 ? mh->n - 1 : 0);
                const int32_t  bob = (int32_t)((b.ticks / 4) % 8);
                it = stage_put(st, K_ARROW, STAGE_SPRITE, 62, hx + w / 2 - art_sprites[ART_ARROW].w / 2,
                               hy - 52 + (bob < 4 ? bob : 8 - bob));
                it->tile = ART_ARROW;
        }
        /* Chips: bets, insurance, the flying ones. */
        for (int s = 0; s < BJ_SEATS; ++s) {
                for (int h = 0; h < BJ_HANDS; ++h) {
                        if (b.seat[s].hand[h].bet <= 0) continue;
                        pile_place(s, h, &x, &y);
                        it = stage_put(st, (uint32_t)(K_PILE + s * 4 + h), STAGE_CHIPS, 25, x, y);
                        it->rgb = (uint32_t)b.seat[s].hand[h].bet;
                }
                if (b.seat[s].insurance > 0) {
                        spot(s, &x, &y);
                        it = stage_put(st, (uint32_t)(K_INSURE + s), STAGE_CHIPS, 25, x - 124, y - 24);
                        it->rgb = (uint32_t)b.seat[s].insurance;
                }
        }
        for (int i = 0; i < FLYING; ++i) {
                const flying_t *f = &b.flying[i];
                if (!f->used || f->delay > 0) continue;
                it = stage_put(st, (uint32_t)(K_FLYING + i), STAGE_CHIPS, 55, ease(f->fx, f->tx, f->t, f->dur),
                               ease(f->fy, f->ty, f->t, f->dur));
                it->rgb = (uint32_t)f->amount;
        }
        /* The band: the rack, the bank and the bet, the buttons. */
        const int betting = b.table.phase == BJ_BETTING || b.table.phase == BJ_SETTLED;
        for (int k = 0; k < UI_CHIP_KINDS; ++k) {
                const int32_t cx = 14 + k * 62, cy = BAND_Y + 4 - (b.rack_hover == k && betting ? 3 : 0);
                it = stage_put(st, (uint32_t)(K_RACK + k), STAGE_SPRITE, 80, cx, cy);
                it->tile = (uint16_t)(ART_CHIP_BIG_0 + k);
                if (!betting || ui_chip_value[k] > b.bank_shown + b.table.seat[PLAYER].hand[0].bet) {
                        it = stage_put(st, (uint32_t)(K_BOX + 2 + k), STAGE_SHADE, 81, cx, cy);
                        it->tile = (uint16_t)(ART_CHIP_BIG_0 + k);
                        it->alpha = 110;
                }
        }
        /* BANK and BET side by side beside the rack, each a small label over its amount. */
        char amount[24];
        it = stage_put(st, K_BANK_LABEL, STAGE_TEXT, 80, 392, BAND_Y + 8);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "BANK");
        money(amount, sizeof(amount), b.bank_shown);
        it = stage_put(st, K_BANK, STAGE_TEXT, 80, 392, BAND_Y + 31);
        it->font = ART_FONT_MED;
        snprintf(it->text, sizeof(it->text), "%s", amount);
        int32_t bet = 0;
        for (int h = 0; h < BJ_HANDS; ++h) bet += b.seat[PLAYER].hand[h].bet;
        if (b.table.phase == BJ_BETTING) bet = b.table.seat[PLAYER].hand[0].bet;
        it = stage_put(st, K_BET_LABEL, STAGE_TEXT, 80, 566, BAND_Y + 8);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "BET");
        money(amount, sizeof(amount), bet);
        it = stage_put(st, K_BET, STAGE_TEXT, 80, 566, BAND_Y + 31);
        it->font = ART_FONT_MED;
        snprintf(it->text, sizeof(it->text), "%s", amount);
        show_buttons();
        for (int i = 0; i < b.button_count; ++i) {
                const ui_button_t *bt = &b.button_at[i];
                it = stage_put(st, (uint32_t)(K_BUTTON + i), STAGE_BUTTON, 80, bt->r.x, bt->r.y);
                it->w = (int16_t)bt->r.width;
                it->h = (int16_t)bt->r.height;
                it->font = b.hover == i ? UI_HOVER : UI_NORMAL;
                snprintf(it->text, sizeof(it->text), "%s", bt->label);
        }
        /* The options box. */
        if (b.options) {
                const int32_t bx = WIDTH / 2 - 260, by = HEIGHT / 2 - 150;
                it = stage_put(st, K_BOX, STAGE_PANEL, 90, bx, by);
                it->w = 520;
                it->h = 260;
                it = stage_put(st, K_BOX + 1, STAGE_TEXT, 91, WIDTH / 2, by + 22);
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "OPTIONS");
                char line[3][sizeof(((stage_item_t *)0)->text)];
                snprintf(line[0], sizeof(line[0]), "B   bot ducks: %d", b.bots);
                snprintf(line[1], sizeof(line[1]), "H   soft 17: dealer %s",
                         b.table.rules.hit_soft_17 ? "hits" : "stands");
                snprintf(line[2], sizeof(line[2]), "R   surrender: %s",
                         b.table.rules.surrender ? "allowed" : "not allowed");
                for (int i = 0; i < 3; ++i) {
                        it = stage_put(st, (uint32_t)(K_BOX + 8 + i), STAGE_LABEL, 91, bx + 50, by + 90 + i * 30);
                        it->rgb = C_TEXT;
                        memcpy(it->text, line[i], sizeof(it->text));
                }
                it = stage_put(st, K_BOX + 11, STAGE_LABEL, 91, bx + 50, by + 216);
                it->rgb = C_DIM;
                snprintf(it->text, sizeof(it->text), "Esc or a click: close");
        }
        return;
}

/* ---- Options */

static void seat_bots(void) {
        static const int order[4] = {1, 3, 0, 4}; /* nearest the player first */
        for (int i = 0; i < 4; ++i) {
                const int s = order[i];
                if (i < b.bots && !b.table.seat[s].playing) bj_sit(&b.table, s, 1, 1000);
                if (i >= b.bots && b.table.seat[s].playing) bj_sit(&b.table, s, 0, 0);
        }
        return;
}

static void option_key(uint32_t usage) {
        bj_table_t *t = &b.table;
        if (usage == K_B) {
                b.bots = (b.bots + 1) % 5;
                set_setting("blackjack.bots", b.bots);
                if (t->phase == BJ_BETTING || t->phase == BJ_SETTLED) seat_bots();
        } else if (usage == K_H) {
                t->rules.hit_soft_17 = !t->rules.hit_soft_17;
                set_setting("blackjack.h17", t->rules.hit_soft_17);
        } else if (usage == K_R) {
                t->rules.surrender = !t->rules.surrender;
                set_setting("blackjack.surrender", t->rules.surrender);
        } else if (usage == K_ESCAPE || usage == K_O || usage == K_ENTER) {
                b.options = 0;
        }
        return;
}

/* ---- Input */

static const char *const RULES[] = {
        "Beat the dealer: get closer to 21 without going over.",
        "Cards count their number; J, Q and K count 10; an Ace 1 or 11.",
        "Bets from $5 to $500. Six decks, shuffled at the cut card.",
        "Blackjack (an Ace and a ten-card, first two) pays 3 to 2.",
        "A win pays 1 to 1; a tie (a push) gives the bet back.",
        "The dealer stands on 17 (Options: or hits a soft 17).",
        "",
        "HIT (H)        take a card",
        "STAND (S)      take no more",
        "DOUBLE (D)     double the bet on your first two cards, one card",
        "SPLIT (P)      two of a value: play them as two hands",
        "               (up to four; split Aces take one card each)",
        "SURRENDER (R)  give up the first two cards, half the bet back",
        "INSURANCE (Y)  the dealer shows an Ace: half your bet that it",
        "               has blackjack; pays 2 to 1 (with a blackjack:",
        "               even money)",
        "",
        "Chips: click to bet, right click your circle to take one back.",
        "Enter deals, B bets as last time, C clears, T: Lucky's tip.",
};

static void press(int id) {
        switch (id) {
                case B_LOBBY: app_leave(); break;
                case B_RULES: app_info("BLACKJACK RULES", RULES, (int)(sizeof(RULES) / sizeof(RULES[0]))); break;
                case B_OPTIONS: b.options = 1; break;
                case B_CLEAR: (void)bj_bet(&b.table, PLAYER, 0); break;
                case B_REBET:
                        if (b.table.phase == BJ_SETTLED) next_round();
                        if (b.last_bet > 0) (void)bj_bet(&b.table, PLAYER, b.last_bet);
                        break;
                case B_DEAL: deal(); break;
                case B_INSURE: (void)bj_insure(&b.table, PLAYER, 1); break;
                case B_NO: (void)bj_insure(&b.table, PLAYER, 0); break;
                case B_HINT: {
                        static const char *const names[BJ_ACTIONS] = {"HIT", "STAND", "DOUBLE", "SPLIT", "SURRENDER"};
                        const bj_table_t        *t = &b.table;
                        const bj_hand_t         *h = &t->seat[PLAYER].hand[t->turn_hand];
                        const int a = bj_basic(h->cards, h->count, t->dealer.cards[0], bj_can(t, PLAYER, BJ_DOUBLE),
                                               bj_can(t, PLAYER, BJ_SPLIT), bj_can(t, PLAYER, BJ_SURRENDER),
                                               t->rules.hit_soft_17);
                        snprintf(b.hint, sizeof(b.hint), "PSST... %s!", names[a]);
                        b.hint_ticks = 150;
                        sfx(SFX_QUACK2, 0.3f);
                        break;
                }
                case B_SURRENDER: act(BJ_SURRENDER); break;
                case B_SPLIT: act(BJ_SPLIT); break;
                case B_DOUBLE: act(BJ_DOUBLE); break;
                case B_STAND: act(BJ_STAND); break;
                case B_HIT: act(BJ_HIT); break;
                default: break;
        }
        return;
}

static int rack_at(int32_t x, int32_t y) {
        for (int k = 0; k < UI_CHIP_KINDS; ++k) {
                const int32_t cx = 14 + k * 62 + 28, cy = BAND_Y + 4 + 28;
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 28 * 28) return k;
        }
        return -1;
}

static int on_my_circle(int32_t x, int32_t y) {
        int32_t sx, sy;
        spot(PLAYER, &sx, &sy);
        return (x - sx) * (x - sx) * 26 * 26 + (y - sy) * (y - sy) * 40 * 40 <= 40 * 40 * 26 * 26 * 2;
}

static void bj_scene_event(const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t  x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const uint32_t pressed = e->data[2] & ~app.buttons;
                b.hover = ui_button_at(b.button_at, b.button_count, x, y);
                b.rack_hover = rack_at(x, y);
                if (b.options) {
                        if (pressed != 0) b.options = 0;
                        return;
                }
                if ((pressed & 1u) != 0) {
                        if (b.hover >= 0)
                                press(b.buttons[b.hover]);
                        else if (b.rack_hover >= 0 && caught_up())
                                add_chip(b.rack_hover);
                }
                if ((pressed & 2u) != 0 && on_my_circle(x, y)) take_chip();
                return;
        }
        if (e->type == BMDS_EVENT_ACTIVE && e->data[0] == 0) {
                b.hover = b.rack_hover = -1;
                return;
        }
        if (e->type != BMDS_EVENT_KEY || (e->flags & BMDS_KEY_DOWN) == 0) return;
        const uint32_t usage = e->data[0];
        if (usage == K_M) {
                sound_toggle();
                return;
        }
        if (b.options) {
                option_key(usage);
                return;
        }
        if (usage == K_ESCAPE) {
                app_leave();
                return;
        }
        for (int i = 0; i < b.button_count; ++i)
                if (b.button_at[i].key == usage || (usage == K_SPACE && b.buttons[i] == B_DEAL) ||
                    (usage == K_KP_ENTER && b.buttons[i] == B_DEAL)) {
                        press(b.buttons[i]);
                        return;
                }
        return;
}

/* ---- The scene */

static void bj_scene_tick(void) {
        ++b.ticks;
        replay();
        cards_tick();
        flying_tick();
        table_tick();
        place_cards();
        for (int s = 0; s < BJ_SEATS; ++s) {
                for (int h = 0; h < BJ_HANDS; ++h)
                        if (b.seat[s].hand[h].pop > 0 && b.seat[s].hand[h].pop < 60) ++b.seat[s].hand[h].pop;
                if (b.seat[s].pose_ticks > 0 && --b.seat[s].pose_ticks == 0) b.seat[s].pose = POSE_IDLE;
        }
        if (b.dealer.pop > 0 && b.dealer.pop < 60) ++b.dealer.pop;
        if (b.lucky_deal > 0) --b.lucky_deal;
        if (b.lucky_mood > 0) --b.lucky_mood;
        if (b.hint_ticks > 0) --b.hint_ticks;
        if (b.shuffle_ticks > 0) --b.shuffle_ticks;
        if (b.message_ticks > 0) --b.message_ticks;
        return;
}

static void bj_scene_draw(bmds_canvas_t *c) {
        layout();
        stage_draw(&b.stage, c);
        return;
}

static void bj_scene_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        layout();
        stage_redraw(&b.stage, c, r);
        return;
}

static int bj_scene_enter(int argc, char **argv) {
        (void)argc;
        (void)argv;
        bj_rules_t rules = bj_default_rules();
        rules.hit_soft_17 = setting("blackjack.h17", 0) != 0;
        rules.surrender = setting("blackjack.surrender", 1) != 0;
        memset(&b, 0, sizeof(b));
        bj_init(&b.table, &rules, app_key());
        if (bank_ensure(rules.min_bet)) sfx(SFX_SAD, 0.5f);
        const int32_t bank = (int32_t)bank_balance();
        bj_sit(&b.table, PLAYER, 1, bank);
        b.synced = b.bank_shown = bank;
        b.bots = (int)setting("blackjack.bots", 2);
        if (b.bots < 0 || b.bots > 4) b.bots = 2;
        seat_bots();
        b.turn_seat = -1;
        b.hover = b.rack_hover = -1;
        b.cursor = b.table.event_count; /* the first shuffle needn't show */
        place_spots();
        paint_felt();
        layout();
        memcpy(b.stage.shown, b.stage.now, sizeof(b.stage.now));
        b.stage.dirty_count = 0;
        return 0;
}

static void bj_scene_leave(void) {
        bj_table_t *t = &b.table;
        /* A bet not yet dealt comes back; one in play is lost (the leave box said so). */
        if (t->phase == BJ_BETTING && t->seat[PLAYER].hand[0].bet > 0) (void)bj_bet(t, PLAYER, 0);
        bank_add(t->seat[PLAYER].stack - b.synced);
        b.synced = t->seat[PLAYER].stack;
        return;
}

/* A round in play is lost by leaving; between rounds nothing is (a bet not yet dealt comes back). */
static int bj_scene_busy(void) {
        const int phase = b.table.phase;
        return phase == BJ_INSURANCE || phase == BJ_PLAYING || (phase == BJ_SETTLED && !caught_up());
}

const scene_t blackjack_scene = {
        "blackjack",   "Blackjack",   bj_scene_enter,  bj_scene_leave, bj_scene_event,
        bj_scene_tick, bj_scene_draw, bj_scene_redraw, bj_scene_busy,
};
