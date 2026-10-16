/**
 * @file      video.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Video Poker (devplan/luckyduck.md §8d, §21): a cabinet with a blue screen, its pay
 * table at the top, Lucky beside it dealing from a deck. The rules are video_rules.c's: the scene
 * moves its five cards after them (in from the deck, flipped in a ripple, back to the deck at the
 * draw), lifts the held ones, and counts a win into the bank coin by coin.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/sys.h>
#include "hand.h"
#include "stage.h"
#include "ui.h"
#include "video_rules.h"

#define BAND_H 64
#define BAND_Y (HEIGHT - BAND_H)
#define FLIGHT 12
#define FLIP 8
#define STAGGER 5 /* ticks between cards sent */
#define RIPPLE 6  /* between flips */
#define LIFT 14
#define CARD_GAP 30
#define DECK_SCALE 128
#define TABLE_H 273 /* 13 rows of 21 */
#define COINS 16
#define FEATHERS 24
#define DUCKLINGS 5
#define PARTY 320
#define FLASH_TICKS 600
#define EV_SLICE_NS 8000000 /* perfect play's share of a frame */

/* Stage keys. */
enum {
        K_CARD = 0,  /* + the slot */
        K_GLOW = 5,  /* + the slot */
        K_HELD = 10, /* + the slot */
        K_DECK = 16, /* + 3 */
        K_LUCKY = 20,
        K_BUBBLE,
        K_BUBBLE_TEXT, /* + 2 */
        K_STATUS = K_BUBBLE_TEXT + 2,
        K_HEADLINE,
        K_COLUMN,
        K_ROW,
        K_FLASH,
        K_COIN = 30,     /* + the coin */
        K_FEATHER = 50,  /* + the feather */
        K_DUCKLING = 80, /* + the duckling */
        K_BAND = 90,     /* + 6 */
        K_BUTTON = 100,  /* + the button */
        K_BOX = 200      /* + 16 */
};

/* A slot's card: in the deck (gone), on its way, or on the screen. A card sent to a slot that
   shows one first turns that one down and takes it back to the deck. */
enum {
        S_GONE,
        S_OUT_FLIP, /* turning face down to go */
        S_OUT,      /* flying back to the deck */
        S_WAIT,     /* in the deck, its turn to come */
        S_IN,       /* flying from the deck */
        S_LANDED,   /* face down, its turn to flip */
        S_UP_FLIP,  /* turning face up */
        S_UP
};

typedef struct {
                uint8_t  state;
                uint8_t  face;   /* the face shown when up */
                uint8_t  next;   /* the card it comes back as */
                uint8_t  coming; /* a card is to come back */
                uint8_t  up;     /* the face shows (it turns at a flip's middle) */
                uint8_t  lift;   /* pixels lifted, easing to the hold's */
                uint16_t t;
                uint16_t wait_in, wait_flip; /* S_WAIT's and S_LANDED's lengths */
} slot_t;

typedef struct {
                uint8_t  used, tile;
                int16_t  fx, fy, tx, ty;
                uint16_t t, dur, delay;
} coin_t;

typedef struct {
                float    x, y, vy, phase;
                uint16_t turn;
                uint8_t  kind;
} feather_t;

enum {
        B_LOBBY,
        B_RULES,
        B_OPTIONS,
        B_HINT,
        B_BET_ONE,
        B_BET_MAX,
        B_DEAL,
        BUTTONS
};

static const char *const BUTTON_LABEL[BUTTONS] = {"LOBBY", "RULES", "OPTIONS", "HINT", "BET ONE", "BET MAX", "DEAL"};
static const uint32_t    BUTTON_KEY[BUTTONS] = {0, K_F1, K_O, K_T, K_B, K_X, K_ENTER};

static const int32_t COIN_VALUES[] = {1, 5, 25};
#define COIN_KINDS 3
#define OPTION_LINES 2

enum {
        EV_NONE,
        EV_WORKING,
        EV_DONE
};

static struct {
                vp_t      vp;
                vp_ev_t   ev;
                stage_t   stage;
                slot_t    slot[VP_CARDS];
                coin_t    coin[COINS];
                feather_t feather[FEATHERS];
                int       machine; /* the options' (the machine's own changes at the next deal) */
                int32_t   coin_value;
                int32_t   bank_shown; /* the bank as the picture has got to */
                int32_t   synced;     /* the stack the bank was last written from */
                int32_t   win_shown;  /* the win counted so far */
                uint8_t   result_due; /* the draw's result shows once its cards are up */
                uint8_t   result;     /* shown: the headline, the glow, the flashing row */
                uint8_t   counting;
                uint8_t   scoring; /* the cards that make the hand */
                uint8_t   ev_state;
                uint16_t  party;
                uint16_t  pop;   /* ticks since the headline came */
                uint16_t  flash; /* ticks since the result showed */
                uint16_t  lucky_deal, lucky_mood;
                int       lucky_pose;
                uint16_t  hint_ticks, message_ticks;
                char      hint[32];
                char      message[sizeof(((stage_item_t *)0)->text)];
                char      headline[sizeof(((stage_item_t *)0)->text)];
                /* Input. */
                int         buttons[BUTTONS];
                int         button_count;
                ui_button_t button_at[BUTTONS];
                int         hover, hover_card;
                int         options;
                uint32_t    ticks;
} v;

/* ---- Geometry: the screen between Lucky and the system buttons, the band under it */

static bmds_rect_t screen(void) {
        const int32_t x = 196, y = 24;
        return rect(x, y, WIDTH - x - 176, BAND_Y - y - 24);
}

static bmds_rect_t pay_table(void) {
        const bmds_rect_t s = screen();
        return rect(s.x + 16, s.y + 12, s.width - 32, TABLE_H);
}

static int32_t row_h(void) {
        const int32_t h = TABLE_H / vp_rows(v.vp.machine);
        return h > 28 ? 28 : h;
}

static int32_t name_w(void) {
        return pay_table().width * 36 / 100;
}

static int32_t column_w(void) {
        return (pay_table().width - name_w()) / VP_MAX_COINS;
}

/* Column @p c's left (0: one coin). */
static int32_t column_x(int c) {
        return pay_table().x + name_w() + c * column_w();
}

static int32_t cards_y(void) {
        return screen().y + 12 + TABLE_H + 14;
}

static int32_t card_x(int i) {
        const bmds_rect_t s = screen();
        const int32_t     w = VP_CARDS * ART_CARD_W + (VP_CARDS - 1) * CARD_GAP;
        return s.x + s.width / 2 - w / 2 + i * (ART_CARD_W + CARD_GAP);
}

/* The line under the cards (and the room for HELD, or a royal flush's ducklings): the headline's
   top once it has popped up, the status line's. */
static int32_t status_y(void) {
        return cards_y() + ART_CARD_H + 62;
}

static void deck_place(int32_t *x, int32_t *y) {
        *x = 124;
        *y = BAND_Y - 124;
        return;
}

static void lucky_place(int32_t *x, int32_t *y) {
        *x = 6;
        *y = BAND_Y - 136;
        return;
}

/* The coin slot on the cabinet's frame, and where a win's coins leave the screen. */
static void slot_point(int32_t *x, int32_t *y) {
        const bmds_rect_t s = screen();
        *x = s.x + s.width - 70;
        *y = s.y + s.height + 2;
        return;
}

static void bank_point(int32_t *x, int32_t *y) {
        *x = 40;
        *y = BAND_Y + 18;
        return;
}

/* ---- Easing */

static int32_t ease(int32_t from, int32_t to, uint32_t t, uint32_t n) {
        if (t >= n) return to;
        const int32_t k = (int32_t)n, u = (int32_t)t;
        return from + (to - from) * (2 * k * u - u * u) / (k * k); /* out */
}

static float frand(float lo, float hi) {
        return lo + (hi - lo) * (float)(rand() & 0xffff) / 65535.0f;
}

/* ---- The cards */

/* Sends @p face to slot @p i: @p order of @p count sent together (they come in that order and
   flip in a ripple once all have landed). */
static void send(int i, uint8_t face, int order, int count) {
        slot_t *s = &v.slot[i];
        s->next = face;
        s->coming = 1;
        s->t = 0;
        if (s->state == S_GONE)
                s->state = S_WAIT;
        else
                s->state = s->up ? S_OUT_FLIP : S_OUT;
        s->wait_in = (uint16_t)(order * STAGGER);
        s->wait_flip = (uint16_t)((count - 1 - order) * STAGGER + 4 + order * RIPPLE);
        return;
}

static int cards_still(void) {
        for (int i = 0; i < VP_CARDS; ++i)
                if (v.slot[i].state != S_UP && v.slot[i].state != S_GONE) return 0;
        return 1;
}

static void slots_tick(void) {
        for (int i = 0; i < VP_CARDS; ++i) {
                slot_t *s = &v.slot[i];
                ++s->t;
                switch (s->state) {
                        case S_OUT_FLIP:
                        case S_UP_FLIP:
                                if (s->t == FLIP / 2) {
                                        s->up = s->state == S_UP_FLIP;
                                        if (s->up) s->face = s->next;
                                }
                                if (s->t >= FLIP) {
                                        s->state = s->state == S_UP_FLIP ? S_UP : S_OUT;
                                        s->t = 0;
                                }
                                break;
                        case S_OUT:
                                if (s->t >= FLIGHT) {
                                        s->state = s->coming ? S_WAIT : S_GONE;
                                        s->t = 0;
                                }
                                break;
                        case S_WAIT:
                                if (s->t >= s->wait_in) {
                                        s->state = S_IN;
                                        s->t = 0;
                                        s->coming = 0;
                                        v.lucky_deal = 8;
                                        sfx(SFX_PLACE, 0.5f);
                                }
                                break;
                        case S_IN:
                                if (s->t >= FLIGHT) {
                                        s->state = S_LANDED;
                                        s->t = 0;
                                }
                                break;
                        case S_LANDED:
                                if (s->t >= s->wait_flip) {
                                        s->state = S_UP_FLIP;
                                        s->t = 0;
                                        sfx(SFX_FLIP, 0.5f);
                                }
                                break;
                        default: break;
                }
                /* Held cards lift while the hand is dealt; afterwards the ones that won. */
                const int want = (v.vp.phase == VP_DEALT && s->state == S_UP && (v.vp.held & (1u << i))) ||
                                                 (v.result && (v.scoring & (1u << i)))
                                         ? LIFT
                                         : 0;
                if (s->lift < want) s->lift = (uint8_t)(s->lift + 2 > want ? want : s->lift + 2);
                if (s->lift > want) s->lift = (uint8_t)(s->lift < 2 ? 0 : s->lift - 2);
        }
        return;
}

/* ---- Coins */

static int coin_tile(void) {
        for (int k = 0; k < COIN_KINDS; ++k)
                if (COIN_VALUES[k] == v.vp.coin) return ART_CHIP_0 + k;
        return ART_CHIP_0;
}

static void fly_coin(int32_t fx, int32_t fy, int32_t tx, int32_t ty, uint16_t delay) {
        for (int i = 0; i < COINS; ++i) {
                coin_t *c = &v.coin[i];
                if (c->used) continue;
                c->used = 1;
                c->tile = (uint8_t)coin_tile();
                c->fx = (int16_t)fx;
                c->fy = (int16_t)fy;
                c->tx = (int16_t)tx;
                c->ty = (int16_t)ty;
                c->t = 0;
                c->dur = 16;
                c->delay = delay;
                return;
        }
        return;
}

static void coins_tick(void) {
        for (int i = 0; i < COINS; ++i) {
                coin_t *c = &v.coin[i];
                if (!c->used) continue;
                if (c->delay > 0) {
                        --c->delay;
                        continue;
                }
                if (++c->t >= c->dur) {
                        c->used = 0;
                        sfx(SFX_PLACE, 0.25f);
                }
        }
        return;
}

/* ---- The bank */

static void sync_bank(void) {
        bank_add(v.vp.stack - v.synced);
        v.synced = v.vp.stack;
        return;
}

/* A win still counting is counted at once (a click, or the next deal, hurries it). */
static void finish_count(void) {
        if (!v.counting) return;
        v.counting = 0;
        v.win_shown = v.vp.won;
        v.bank_shown = v.vp.stack;
        sync_bank();
        return;
}

static void count_tick(void) {
        if (!v.counting) return;
        const int32_t coin = v.vp.coin;
        int32_t       step = v.vp.won / 80 / coin * coin;
        if (step < coin) step = coin;
        if (v.win_shown + step > v.vp.won) step = v.vp.won - v.win_shown;
        v.win_shown += step;
        v.bank_shown += step;
        if (v.ticks % 3 == 0) {
                int32_t bx, by;
                bank_point(&bx, &by);
                const bmds_rect_t s = screen();
                fly_coin(s.x + s.width / 2 - 24 + (rand() % 41) - 20, status_y(), bx, by, 0);
        }
        if (v.win_shown >= v.vp.won) {
                v.counting = 0;
                v.bank_shown = v.vp.stack;
                sync_bank();
        }
        return;
}

/* ---- A royal flush: gold flashes, feathers, ducklings, the fanfare */

static void start_party(void) {
        const bmds_rect_t s = screen();
        for (int i = 0; i < FEATHERS; ++i) {
                feather_t *p = &v.feather[i];
                p->x = frand((float)s.x, (float)(s.x + s.width - 28));
                p->y = -20.0f - frand(0, 300);
                p->vy = frand(1.2f, 2.4f);
                p->phase = frand(0, 6.2831853f);
                p->turn = (uint16_t)(rand() & 0xffff);
                p->kind = (uint8_t)(i % 2);
        }
        v.party = PARTY;
        sfx(SFX_WIN, 0.8f);
        sfx(SFX_QUACKS, 0.5f);
        return;
}

static void party_tick(void) {
        if (v.party == 0) return;
        --v.party;
        for (int i = 0; i < FEATHERS; ++i) {
                feather_t *p = &v.feather[i];
                p->y += p->vy;
                p->phase += 0.06f;
                p->x += sinf(p->phase) * 1.2f;
                p->turn = (uint16_t)(p->turn + (int32_t)(sinf(p->phase) * 500.0f));
        }
        if (v.party == PARTY - 80) sfx(SFX_QUACK2, 0.4f);
        return;
}

/* ---- The hand */

static void say(const char *text) {
        snprintf(v.message, sizeof(v.message), "%s", text);
        v.message_ticks = 150;
        return;
}

/* The draw's cards are up: what they made. */
static void show_result(void) {
        v.result_due = 0;
        v.result = 1;
        v.flash = 0;
        const vp_t *m = &v.vp;
        set_setting("video.played", setting("video.played", 0) + 1);
        if (m->row < 0) {
                sync_bank();
                return;
        }
        const int royal = vp_row_royal(m->machine, m->row);
        snprintf(v.headline, sizeof(v.headline), "%s", royal ? "ROYAL FLUSH!" : vp_row_name(m->machine, m->row));
        v.pop = 1;
        v.scoring = vp_scoring(m->machine, m->cards, m->row);
        v.win_shown = 0;
        v.counting = 1;
        const int32_t per_coin = vp_row_pay(m->machine, m->row, 1);
        if (royal) {
                set_setting("video.royals", setting("video.royals", 0) + 1);
                start_party();
        } else {
                sfx(SFX_WIN, per_coin >= 3 ? 0.7f : 0.4f);
        }
        v.lucky_pose = per_coin >= 3 ? ART_LUCKY_CHEER : ART_LUCKY_QUACK;
        v.lucky_mood = royal ? PARTY : per_coin >= 3 ? 90 : 40;
        if (per_coin >= 25 && !royal) sfx(SFX_QUACKS, 0.4f);
        return;
}

static void deal(void) {
        vp_t *m = &v.vp;
        if (m->phase == VP_DEALT || !cards_still() || v.result_due) return;
        finish_count();
        v.party = 0;
        (void)vp_configure(m, v.machine, v.coin_value);
        /* What the bank covers: fewer coins, or Lucky's top-up below one. */
        if (m->stack < m->coin) {
                if (bank_ensure(m->coin)) {
                        m->stack = (int32_t)bank_balance();
                        v.synced = v.bank_shown = m->stack;
                        v.lucky_pose = ART_LUCKY_SAD;
                        v.lucky_mood = 120;
                        say("LUCKY TOPS YOU UP");
                        sfx(SFX_SAD, 0.5f);
                }
        }
        if (m->coins * m->coin > m->stack) (void)vp_bet(m, (int)(m->stack / m->coin));
        const int coins = m->coins;
        if (vp_deal(m, app_key()) != 0) return;
        set_setting("video.coins", coins);
        v.bank_shown -= m->coin * coins;
        int32_t bx, by, sx, sy;
        bank_point(&bx, &by);
        slot_point(&sx, &sy);
        for (int i = 0; i < coins; ++i) fly_coin(bx, by, sx, sy, (uint16_t)(i * 3));
        for (int i = 0; i < VP_CARDS; ++i) send(i, m->cards[i], i, VP_CARDS);
        v.result = 0;
        v.scoring = 0;
        v.win_shown = 0;
        v.ev_state = EV_NONE;
        v.hint_ticks = 0;
        sfx(SFX_DEAL, 0.5f);
        return;
}

static void draw_cards(void) {
        vp_t *m = &v.vp;
        if (m->phase != VP_DEALT || !cards_still()) return;
        (void)vp_draw(m);
        const int count = __builtin_popcount(m->drawn);
        int       order = 0;
        for (int i = 0; i < VP_CARDS; ++i)
                if (m->drawn & (1u << i)) send(i, m->cards[i], order++, count);
        v.result_due = 1;
        v.hint_ticks = 0;
        return;
}

static void toggle(int i) {
        vp_t *m = &v.vp;
        if (m->phase != VP_DEALT || !cards_still()) return;
        if (vp_hold(m, i, !(m->held & (1u << i))) == 0) sfx(SFX_CLICK, 0.4f);
        return;
}

static void bet_one(void) {
        vp_t *m = &v.vp;
        if (m->phase == VP_DEALT) return;
        finish_count();
        (void)vp_bet(m, m->coins % VP_MAX_COINS + 1);
        set_setting("video.coins", m->coins);
        sfx(SFX_PLACE, 0.5f);
        return;
}

static void bet_max(void) {
        vp_t *m = &v.vp;
        if (m->phase == VP_DEALT) return;
        (void)vp_bet(m, VP_MAX_COINS);
        deal();
        return;
}

/* ---- The hint: perfect play, worked out over frames while Lucky thinks */

static void make_hint(void) {
        const int hold = vp_ev_best(&v.ev);
        if (hold == 0) {
                snprintf(v.hint, sizeof(v.hint), "DRAW FIVE NEW!");
        } else if (hold == (1 << VP_CARDS) - 1) {
                snprintf(v.hint, sizeof(v.hint), "KEEP THEM ALL!");
        } else {
                static const char *const names[DECK_RANKS] = {"A", "2", "3",  "4", "5", "6", "7",
                                                              "8", "9", "10", "J", "Q", "K"};
                int                      n = snprintf(v.hint, sizeof(v.hint), "HOLD");
                for (int i = 0; i < VP_CARDS && n < (int)sizeof(v.hint); ++i)
                        if (hold & (1 << i))
                                n += snprintf(v.hint + n, sizeof(v.hint) - (size_t)n, " %s",
                                              names[deck_rank(v.vp.cards[i])]);
        }
        v.hint_ticks = 240;
        sfx(SFX_QUACK2, 0.3f);
        return;
}

static void hint(void) {
        if (v.vp.phase != VP_DEALT) return;
        if (v.ev_state == EV_DONE) {
                make_hint();
                return;
        }
        if (v.ev_state == EV_NONE) {
                vp_ev_start(&v.ev, v.vp.machine, v.vp.coins, v.vp.cards);
                v.ev_state = EV_WORKING;
        }
        return;
}

static void ev_tick(void) {
        if (v.ev_state != EV_WORKING) return;
        if (v.vp.phase != VP_DEALT) {
                v.ev_state = EV_NONE;
                return;
        }
        int64_t start = 0, now = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &start);
        do {
                if (vp_ev_step(&v.ev, 4096)) {
                        v.ev_state = EV_DONE;
                        make_hint();
                        return;
                }
                bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
        } while (now - start < EV_SLICE_NS);
        return;
}

/* ---- The felt: the floor's carpet, the cabinet, its blue screen and the pay table */

static void paint_table(bmds_canvas_t *c) {
        const bmds_rect_t t = pay_table();
        const int         rows = vp_rows(v.vp.machine);
        const int32_t     rh = row_h(), cap = art_fonts[ART_FONT_HINT].cap;
        const uint32_t    line = bmds_canvas_pixel(c, 0x2a48b8);
        bmds_canvas_fill(c, t.x, t.y, t.width, 1, line);
        for (int r = 0; r < rows; ++r) {
                const int32_t y = t.y + r * rh, ty = y + (rh - cap) / 2;
                ui_text(c, ART_FONT_HINT, t.x + 10, ty, vp_row_name(v.vp.machine, r));
                for (int k = 0; k < VP_MAX_COINS; ++k) {
                        char pays[12];
                        snprintf(pays, sizeof(pays), "%d", (int)vp_row_pay(v.vp.machine, r, k + 1));
                        const int32_t w = ui_text_ink_width(ART_FONT_HINT, pays);
                        ui_text(c, ART_FONT_HINT, column_x(k) + column_w() - 14 - w, ty, pays);
                }
                bmds_canvas_fill(c, t.x, y + rh, t.width, 1, line);
        }
        for (int k = 0; k <= VP_MAX_COINS; ++k) bmds_canvas_fill(c, column_x(k), t.y, 1, rh * rows, line);
        return;
}

static void paint_felt(void) {
        felt_carpet();
        bmds_canvas_t    *c = &app.felt_canvas;
        const bmds_rect_t s = screen();
        /* The cabinet: a dark edge, a gold frame, a black bezel; the coin slot on its foot. */
        ui_round_fill(c, rect(s.x - 16, s.y - 16, s.width + 32, s.height + 32), 20, 0x1a0e06);
        ui_round_fill(c, rect(s.x - 13, s.y - 13, s.width + 26, s.height + 26), 18, 0xb08028);
        ui_round_fill(c, rect(s.x - 10, s.y - 10, s.width + 20, s.height + 20), 16, 0xe0b848);
        ui_round_fill(c, rect(s.x - 7, s.y - 7, s.width + 14, s.height + 14), 14, 0x101014);
        int32_t px, py;
        slot_point(&px, &py);
        ui_round_fill(c, rect(px - 4, py - 1, 56, 14), 6, 0xb08028);
        ui_round_fill(c, rect(px, py + 3, 48, 6), 3, 0x000000);
        /* The screen: deep blue, lighter at the top. */
        for (int32_t y = 0; y < (int32_t)s.height; ++y) {
                const uint32_t k = (uint32_t)(255 - y * 120 / s.height);
                const uint32_t rgb = ((0x10u * k / 255) << 16) | ((0x24u * k / 255) << 8) | (0xa0u * k / 255);
                bmds_canvas_fill(c, s.x, s.y + y, s.width, 1, bmds_canvas_pixel(c, rgb));
        }
        paint_table(c);
        /* The band. */
        bmds_canvas_fill(c, 0, BAND_Y, WIDTH, BAND_H, bmds_canvas_pixel(c, 0x2a1a10));
        bmds_canvas_fill(c, 0, BAND_Y, WIDTH, 2, bmds_canvas_pixel(c, 0xb08028));
        bmds_canvas_damage_clear(c);
        return;
}

/* ---- The frame */

static void show_buttons(void) {
        const vp_t *m = &v.vp;
        const int   still = cards_still() && !v.result_due;
        int         n = 0;
        v.buttons[n++] = B_LOBBY;
        v.buttons[n++] = B_RULES;
        v.buttons[n++] = B_OPTIONS;
        if (still && m->phase == VP_DEALT) {
                v.buttons[n++] = B_HINT;
                v.buttons[n++] = B_DEAL;
        } else if (still) {
                v.buttons[n++] = B_BET_ONE;
                v.buttons[n++] = B_BET_MAX;
                v.buttons[n++] = B_DEAL;
        }
        v.button_count = n;
        int32_t x = WIDTH - 16;
        for (int i = n - 1; i >= 0; --i) {
                const int id = v.buttons[i];
                if (id <= B_OPTIONS) { /* the system's: stacked at the top right */
                        v.button_at[i].r = app_system_button(id);
                        v.button_at[i].label = BUTTON_LABEL[id];
                        v.button_at[i].key = BUTTON_KEY[id];
                        continue;
                }
                const char   *label = id == B_DEAL && m->phase == VP_DEALT ? "DRAW" : BUTTON_LABEL[id];
                const int32_t w = (id == B_DEAL ? 120 : ui_text_width(ART_FONT_SMALL, label) + 26);
                x -= w;
                v.button_at[i].r = rect(x, BAND_Y + 13, w, 40);
                v.button_at[i].label = label;
                v.button_at[i].key = BUTTON_KEY[id];
                x -= 8;
        }
        return;
}

static bmds_rect_t card_rect(int i) {
        return rect(card_x(i), cards_y() - v.slot[i].lift, ART_CARD_W, ART_CARD_H);
}

static void layout_card(stage_t *st, int i) {
        const slot_t *s = &v.slot[i];
        if (s->state == S_GONE || s->state == S_WAIT) return;
        int32_t dx, dy;
        deck_place(&dx, &dy);
        const bmds_rect_t home = card_rect(i);
        int32_t           x = home.x, y = home.y, k = 256;
        uint16_t          spin = 0;
        if (s->state == S_IN || s->state == S_OUT) {
                const int in = s->state == S_IN;
                x = in ? ease(dx, home.x, s->t, FLIGHT) : ease(home.x, dx, s->t, FLIGHT);
                y = in ? ease(dy, home.y, s->t, FLIGHT) : ease(home.y, dy, s->t, FLIGHT);
                k = in ? ease(DECK_SCALE, 256, s->t, FLIGHT) : ease(256, DECK_SCALE, s->t, FLIGHT);
                spin = (uint16_t)ease(0, BMDS_GAME_TURN - 1, s->t, FLIGHT); /* a turn, slowing to land flat */
        }
        stage_item_t *it = stage_put(st, (uint32_t)(K_CARD + i), STAGE_CARD, spin ? 50 : 30, x, y);
        it->tile = (uint16_t)(s->up ? s->face : ART_CARD_BACK);
        if (spin != 0) {
                it->turn = spin;
                it->scale = (uint16_t)k;
                it->x = (int16_t)(x - (ART_CARD_W - ART_CARD_W * k / 256) / 2);
                it->y = (int16_t)(y - (ART_CARD_H - ART_CARD_H * k / 256) / 2);
                return;
        }
        int32_t w = ART_CARD_W * k / 256;
        if (s->state == S_OUT_FLIP || s->state == S_UP_FLIP) { /* narrowing to its edge, widening on the other face */
                const int32_t f = s->t < FLIP / 2 ? FLIP / 2 - s->t : s->t - FLIP / 2;
                w = w * f / (FLIP / 2);
                if (w < 2) w = 2;
        }
        it->x = (int16_t)(x + (ART_CARD_W * k / 256 - w) / 2);
        it->w = (int16_t)w;
        it->h = (int16_t)(ART_CARD_H * k / 256);
        if (s->state != S_UP) return;
        if (v.vp.phase == VP_DEALT && (v.vp.held & (1u << i))) {
                it = stage_put(st, (uint32_t)(K_HELD + i), STAGE_TEXT, 40, x + ART_CARD_W / 2,
                               cards_y() + ART_CARD_H + 8);
                it->font = ART_FONT_HINT;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "HELD");
        }
        if (v.result && (v.scoring & (1u << i))) { /* a gold rim behind, pulsing lighter and back */
                static const uint32_t pulse[4] = {0xe8a820, 0xffc838, 0xffe070, 0xfff0b0};
                const int32_t         p = (int32_t)((v.ticks / 5) % 6);
                it = stage_put(st, (uint32_t)(K_GLOW + i), STAGE_BLEND, 20, x - 5, y - 5);
                it->w = ART_CARD_W + 10;
                it->h = ART_CARD_H + 10;
                it->rgb = pulse[p < 4 ? p : 6 - p];
                it->alpha = 255;
        }
        return;
}

static void layout_band(stage_t *st) {
        const vp_t              *m = &v.vp;
        stage_item_t            *it;
        char                     amount[24];
        static const char *const labels[3] = {"BANK", "BET", "WIN"};
        for (int i = 0; i < 3; ++i) {
                const int32_t x = 20 + i * 190;
                it = stage_put(st, (uint32_t)(K_BAND + i), STAGE_TEXT, 80, x, BAND_Y + 8);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", labels[i]);
                it = stage_put(st, (uint32_t)(K_BAND + 3 + i), STAGE_TEXT, 80, x, BAND_Y + 31);
                it->font = ART_FONT_MED;
                if (i == 0) {
                        money(amount, sizeof(amount), v.bank_shown);
                        snprintf(it->text, sizeof(it->text), "%s", amount);
                } else if (i == 1) {
                        money(amount, sizeof(amount), m->coin);
                        snprintf(it->text, sizeof(it->text), "%d x %s", m->coins, amount);
                } else {
                        money(amount, sizeof(amount), v.win_shown);
                        snprintf(it->text, sizeof(it->text), "%s", v.result && m->won > 0 ? amount : "");
                }
        }
        return;
}

static bmds_rect_t option_line(int i) {
        return rect(WIDTH / 2 - 230, HEIGHT / 2 - 64 + i * 36, 460, 32);
}

static void layout_options(stage_t *st) {
        const int32_t bx = WIDTH / 2 - 260, by = HEIGHT / 2 - 150;
        stage_item_t *it = stage_put(st, K_BOX, STAGE_PANEL, 90, bx, by);
        it->w = 520;
        it->h = 270;
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
                it = stage_put(st, (uint32_t)(K_BOX + 8 + i), STAGE_TEXT, 92, r.x + 20, r.y + 9);
                it->font = ART_FONT_SMALL;
                if (i == 0) {
                        snprintf(it->text, sizeof(it->text), "G   %s", vp_machine_name(v.machine));
                } else {
                        char amount[12];
                        money(amount, sizeof(amount), v.coin_value);
                        snprintf(it->text, sizeof(it->text), "C   COINS OF %s", amount);
                }
        }
        it = stage_put(st, K_BOX + 12, STAGE_LABEL, 91, bx + 50, by + 196);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "Click or press a key to change.");
        it = stage_put(st, K_BOX + 13, STAGE_LABEL, 91, bx + 50, by + 216);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "%s",
                 v.vp.phase == VP_DEALT ? "Changes come at the next deal." : "Esc: close");
        return;
}

static void layout(void) {
        stage_t *st = &v.stage;
        stage_begin(st);
        const vp_t       *m = &v.vp;
        const bmds_rect_t s = screen(), t = pay_table();
        stage_item_t     *it;
        int32_t           x, y;
        /* Lucky and the deck. */
        lucky_place(&x, &y);
        it = stage_put(st, K_LUCKY, STAGE_SPRITE, 8, x, y);
        it->tile = (uint16_t)(v.ev_state == EV_WORKING ? ART_LUCKY_THINK
                              : v.lucky_deal > 0       ? ART_LUCKY_DEAL
                              : v.lucky_mood > 0       ? v.lucky_pose
                              : (v.ticks % 240) < 8    ? ART_LUCKY_BLINK
                                                       : ART_LUCKY_IDLE);
        deck_place(&x, &y);
        for (int k = 0; k < 3; ++k) {
                it = stage_put(st, (uint32_t)(K_DECK + k), STAGE_CARD, 9, x, y - k * 2);
                it->tile = ART_CARD_BACK;
                it->w = ART_CARD_W * DECK_SCALE / 256;
                it->h = ART_CARD_H * DECK_SCALE / 256;
        }
        if (v.hint_ticks > 0 || v.ev_state == EV_WORKING) { /* "PSST..." over the advice: it fits left of the cards */
                const int   thinking = v.ev_state == EV_WORKING;
                const char *text = thinking ? "HMM..." : v.hint;
                int32_t     w = ui_text_width(ART_FONT_SMALL, text);
                if (!thinking && ui_text_width(ART_FONT_SMALL, "PSST...") > w)
                        w = ui_text_width(ART_FONT_SMALL, "PSST...");
                lucky_place(&x, &y);
                const int32_t top = thinking ? y - 56 : y - 80;
                it = stage_put(st, K_BUBBLE, STAGE_PANEL, 70, 8, top);
                it->w = (int16_t)(w + 40);
                it->h = (int16_t)(thinking ? 44 : 68);
                if (!thinking) {
                        it = stage_put(st, K_BUBBLE_TEXT + 1, STAGE_TEXT, 71, 28, top + 14);
                        it->font = ART_FONT_SMALL;
                        snprintf(it->text, sizeof(it->text), "PSST...");
                }
                it = stage_put(st, K_BUBBLE_TEXT, STAGE_TEXT, 71, 28, thinking ? top + 14 : top + 38);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", text);
        }
        /* The pay table's lit column (the coins bet) and row (the hand made; flashing once won). */
        it = stage_put(st, K_COLUMN, STAGE_BLEND, 2, column_x(m->coins - 1) + 1, t.y + 1);
        it->w = (int16_t)(column_w() - 1);
        it->h = (int16_t)(row_h() * vp_rows(m->machine) - 1);
        it->rgb = 0xd02018;
        it->alpha = 120;
        int row = -1;
        if (m->phase == VP_DEALT && cards_still()) row = vp_row(m->machine, m->cards);
        if (v.result && m->row >= 0) row = m->row;
        if (row >= 0) {
                const int lit = !v.result || v.flash >= FLASH_TICKS || (v.flash / 10) % 2 == 0;
                if (lit) {
                        it = stage_put(st, K_ROW, STAGE_BLEND, 3, t.x + 1, t.y + row * row_h() + 1);
                        it->w = (int16_t)(t.width - 2);
                        it->h = (int16_t)(row_h() - 1);
                        it->rgb = 0xffd040;
                        it->alpha = v.result ? 110 : 60;
                }
        }
        /* The cards. */
        for (int i = 0; i < VP_CARDS; ++i) layout_card(st, i);
        /* What the screen says under the cards: the headline popping, or a line. */
        if (v.result && v.headline[0] && m->row >= 0) {
                /* Rising from below, past its place, and back. */
                const int32_t p = v.pop > 20 ? 20 : v.pop, off = p < 12 ? 24 - p * 3 : -12 + (p - 12) * 3 / 2;
                it = stage_put(st, K_HEADLINE, STAGE_TEXT, 60, s.x + s.width / 2, status_y() + off);
                it->font = vp_row_royal(m->machine, m->row) ? ART_FONT_BIG : ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", v.headline);
        } else {
                it = stage_put(st, K_STATUS, STAGE_TEXT, 60, s.x + s.width / 2, status_y() + 10);
                it->font = ART_FONT_SMALL;
                it->flags = STAGE_CENTRE;
                char amount[12];
                money(amount, sizeof(amount), m->coin);
                if (v.message_ticks > 0)
                        snprintf(it->text, sizeof(it->text), "%s", v.message);
                else if (m->phase == VP_DEALT && cards_still())
                        snprintf(it->text, sizeof(it->text), "CLICK CARDS TO HOLD, THEN DRAW");
                else if (v.result)
                        snprintf(it->text, sizeof(it->text), "NO WIN. DEAL AGAIN?");
                else if (m->phase != VP_DEALT)
                        snprintf(it->text, sizeof(it->text), "%s  -  %s COINS", vp_machine_name(m->machine), amount);
                else
                        it->text[0] = 0;
        }
        /* Coins on their way. */
        for (int i = 0; i < COINS; ++i) {
                const coin_t *c = &v.coin[i];
                if (!c->used || c->delay > 0) continue;
                it = stage_put(st, (uint32_t)(K_COIN + i), STAGE_SPRITE, 55, ease(c->fx, c->tx, c->t, c->dur),
                               ease(c->fy, c->ty, c->t, c->dur));
                it->tile = c->tile;
        }
        /* A royal flush's party. */
        if (v.party > 0) {
                const int32_t p = PARTY - v.party;
                if (p < 120 && (p / 6) % 2 == 0) {
                        it = stage_put(st, K_FLASH, STAGE_BLEND, 4, s.x, s.y);
                        it->w = (int16_t)s.width;
                        it->h = (int16_t)s.height;
                        it->rgb = 0xffd040;
                        it->alpha = 90;
                }
                for (int i = 0; i < FEATHERS; ++i) {
                        const feather_t *f = &v.feather[i];
                        if (f->y < -20.0f || f->y > (float)BAND_Y) continue;
                        it = stage_put(st, (uint32_t)(K_FEATHER + i), STAGE_SPRITE, 86, (int32_t)f->x, (int32_t)f->y);
                        it->tile = (uint16_t)(ART_FEATHER_0 + f->kind);
                        it->turn = f->turn;
                }
                for (int i = 0; i < DUCKLINGS; ++i) { /* marching under the cards */
                        const int32_t dx = s.x - 60 - i * 58 + p * 4;
                        if (dx < s.x || dx > s.x + (int32_t)s.width - 56) continue;
                        it = stage_put(st, (uint32_t)(K_DUCKLING + i), STAGE_SPRITE, 84, dx,
                                       cards_y() + ART_CARD_H + 2);
                        it->tile = (uint16_t)(ART_DUCKLING_WALK0 + (p / 6 + i) % 4);
                }
        }
        layout_band(st);
        show_buttons();
        for (int i = 0; i < v.button_count; ++i) {
                const ui_button_t *bt = &v.button_at[i];
                it = stage_put(st, (uint32_t)(K_BUTTON + i), STAGE_BUTTON, 80, bt->r.x, bt->r.y);
                it->w = (int16_t)bt->r.width;
                it->h = (int16_t)bt->r.height;
                it->font = v.hover == i ? UI_HOVER : UI_NORMAL;
                snprintf(it->text, sizeof(it->text), "%s", bt->label);
        }
        if (v.options) layout_options(st);
        return;
}

/* ---- Options */

static void option_change(int line) {
        if (line == 0) {
                v.machine = (v.machine + 1) % VP_MACHINES;
                set_setting("video.machine", v.machine);
        } else {
                int k = 0;
                while (k < COIN_KINDS && COIN_VALUES[k] != v.coin_value) ++k;
                v.coin_value = COIN_VALUES[(k + 1) % COIN_KINDS];
                set_setting("video.coin", v.coin_value);
        }
        /* Between hands the machine changes now (its pay table repainted); during one, at the deal. */
        if (vp_configure(&v.vp, v.machine, v.coin_value) == 0) {
                paint_felt();
                stage_dirty(&v.stage, rect(0, 0, WIDTH, HEIGHT));
        }
        sfx(SFX_CLICK, 0.4f);
        return;
}

/* ---- Input */

static const char *const RULES[] = {
        "Make the best poker hand you can from five cards.",
        "",
        "Bet one to five coins (BET ONE, B), then DEAL (Enter). Click the",
        "cards you want to keep (or press 1 to 5): they lift and show HELD.",
        "DRAW (Enter) replaces the others. The pay table shows what each",
        "hand pays for the coins bet; five coins pay 4,000 for a royal flush.",
        "BET MAX (X) bets five coins and deals at once.",
        "",
        "JACKS OR BETTER     a pair of Jacks, Queens, Kings or Aces wins",
        "BONUS POKER         four of a kind pays more by rank",
        "DOUBLE DOUBLE BONUS more for four Aces or 2s-4s with a low kicker",
        "",
        "Options (O): the machine, and coins of $1, $5 or $25.",
        "HINT (T): Lucky works out the hold that pays best on average.",
};

static void press(int id) {
        switch (id) {
                case B_LOBBY: app_leave(); break;
                case B_RULES: app_info("VIDEO POKER RULES", RULES, (int)(sizeof(RULES) / sizeof(RULES[0]))); break;
                case B_OPTIONS: v.options = 1; break;
                case B_HINT: hint(); break;
                case B_BET_ONE: bet_one(); break;
                case B_BET_MAX: bet_max(); break;
                case B_DEAL:
                        if (v.vp.phase == VP_DEALT)
                                draw_cards();
                        else
                                deal();
                        break;
                default: break;
        }
        return;
}

static int card_at(int32_t x, int32_t y) {
        for (int i = 0; i < VP_CARDS; ++i) {
                const bmds_rect_t r = card_rect(i);
                if (v.slot[i].state == S_UP && contains(&r, x, y)) return i;
        }
        return -1;
}

static void video_scene_event(const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t  x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const uint32_t pressed = e->data[2] & ~app.buttons;
                v.hover = ui_button_at(v.button_at, v.button_count, x, y);
                v.hover_card = card_at(x, y);
                if (v.options) {
                        if ((pressed & 1u) != 0) {
                                int line = -1;
                                for (int i = 0; i < OPTION_LINES; ++i) {
                                        const bmds_rect_t r = option_line(i);
                                        if (contains(&r, x, y)) line = i;
                                }
                                if (line >= 0)
                                        option_change(line);
                                else
                                        v.options = 0;
                        }
                        return;
                }
                if ((pressed & 1u) == 0) return;
                if (v.hover >= 0)
                        press(v.buttons[v.hover]);
                else if (v.hover_card >= 0 && v.vp.phase == VP_DEALT)
                        toggle(v.hover_card);
                else
                        finish_count(); /* a click hurries a win's count */
                return;
        }
        if (e->type == BMDS_EVENT_ACTIVE && e->data[0] == 0) {
                v.hover = v.hover_card = -1;
                return;
        }
        if (e->type != BMDS_EVENT_KEY || (e->flags & BMDS_KEY_DOWN) == 0) return;
        const uint32_t usage = e->data[0];
        if (usage == K_M) {
                sound_toggle();
                return;
        }
        if (v.options) {
                if (usage == K_G) option_change(0);
                if (usage == K_C) option_change(1);
                if (usage == K_ESCAPE || usage == K_O || usage == K_ENTER) v.options = 0;
                return;
        }
        if (usage == K_ESCAPE) {
                app_leave();
                return;
        }
        if (usage >= K_1 && usage <= K_5) {
                toggle((int)(usage - K_1));
                return;
        }
        for (int i = 0; i < v.button_count; ++i) {
                const int id = v.buttons[i];
                const int alias = (usage == K_SPACE || usage == K_KP_ENTER) && id == B_DEAL;
                if (v.button_at[i].key == usage || alias) {
                        press(id);
                        return;
                }
        }
        return;
}

/* ---- The scene */

static void video_scene_tick(void) {
        ++v.ticks;
        slots_tick();
        coins_tick();
        if (v.result_due && cards_still()) show_result();
        count_tick();
        party_tick();
        ev_tick();
        if (v.result && v.flash < FLASH_TICKS) ++v.flash;
        if (v.pop > 0 && v.pop < 60) ++v.pop;
        if (v.lucky_deal > 0) --v.lucky_deal;
        if (v.lucky_mood > 0) --v.lucky_mood;
        if (v.hint_ticks > 0) --v.hint_ticks;
        if (v.message_ticks > 0) --v.message_ticks;
        return;
}

static void video_scene_draw(bmds_canvas_t *c) {
        layout();
        stage_draw(&v.stage, c);
        return;
}

static void video_scene_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        layout();
        stage_redraw(&v.stage, c, r);
        return;
}

static int video_scene_enter(int argc, char **argv) {
        (void)argc;
        (void)argv;
        memset(&v, 0, sizeof(v));
        v.machine = (int)setting("video.machine", VP_JACKS);
        if (v.machine < 0 || v.machine >= VP_MACHINES) v.machine = VP_JACKS;
        v.coin_value = (int32_t)setting("video.coin", 1);
        int known = 0;
        for (int k = 0; k < COIN_KINDS; ++k) known |= COIN_VALUES[k] == v.coin_value;
        if (!known) v.coin_value = 1;
        if (bank_ensure(v.coin_value)) sfx(SFX_SAD, 0.5f);
        const int32_t bank = (int32_t)bank_balance();
        vp_init(&v.vp, v.machine, v.coin_value, bank);
        (void)vp_bet(&v.vp, (int)setting("video.coins", VP_MAX_COINS));
        v.synced = v.bank_shown = bank;
        v.hover = v.hover_card = -1;
        paint_felt();
        layout();
        memcpy(v.stage.shown, v.stage.now, sizeof(v.stage.now));
        v.stage.dirty_count = 0;
        return 0;
}

static void video_scene_leave(void) {
        /* A hand dealt and not drawn loses its bet (the leave box said so); a win counting is kept. */
        sync_bank();
        return;
}

/* A dealt hand is lost by leaving. A win is already the stack's (leaving writes it), so its count
   showing doesn't hold anyone. */
static int video_scene_busy(void) {
        return v.vp.phase == VP_DEALT;
}

const scene_t video_scene = {
        "video",          "Video Poker",    video_scene_enter,  video_scene_leave, video_scene_event,
        video_scene_tick, video_scene_draw, video_scene_redraw, video_scene_busy,
};
