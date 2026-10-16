/**
 * @file      dice.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck Dice (devplan/luckyduck.md §8c, §18): the player against up to three bot ducks, Lucky
 * shaking the cup. The rules are dice_rules.c's; this scene replays their log into its own model
 * (the scorecard as shown, the dice where they lie), each event's animation holding the next: the
 * dice are scooped into the cup, shaken, thrown, and tumble across the tray under their own
 * physics (the picture's: the rules decide only the faces), a held die hops onto the shelf, points
 * fly to their box, and a Lucky Duck brings feathers, ducklings and a fanfare.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dice_rules.h"
#include "stage.h"
#include "ui.h"

#define PLAYER 0
#define BAND_H 64
#define BAND_Y (HEIGHT - BAND_H)
#define FLYERS 4
#define FEATHERS 24
#define DUCKLINGS 5
#define PARTY 200 /* a Lucky Duck's ticks */
#define DIE_R 30  /* a die's reach on the felt, for bumping (its corners, however it is turned) */
#define PI 3.14159265f

/* The scorecard. */
#define LABEL_W 190
#define COL_W 108
#define HEAD_H 40
#define ROW_H 30
#define TOTAL_H 40 /* the total's row: its bigger letters */
#define CARD_Y 10
#define CARD_ROWS 17 /* the 13 boxes, the upper sum and its bonus, the Lucky Duck bonus, the total */
enum {
        ROW_UPPER = 6,
        ROW_BONUS = 7,
        ROW_LUCKY = 15,
        ROW_TOTAL = 16
};

/* Stage keys. */
enum {
        K_DIE = 0,        /* + the die */
        K_DIE_SHADOW = 5, /* + the die */
        K_CELL = 10,      /* + seat × CARD_ROWS + row */
        K_COLUMN = 80,    /* + seat */
        K_HOVER_ROW = 84,
        K_BOT = 85,      /* + seat - 1 */
        K_BOT_NAME = 88, /* + seat - 1 */
        K_ARROW = 91,
        K_LUCKY,
        K_BUBBLE,
        K_BUBBLE_TEXT,
        K_BUTTON = 96, /* + the button */
        K_FLY = 102,   /* + the flyer */
        K_POP = 106,
        K_PARTY_TEXT,
        K_FEATHER = 110,  /* + the feather */
        K_DUCKLING = 134, /* + the duckling */
        K_BAND = 140,     /* + 16 */
        K_BOX = 160,      /* + 40 */
        K_MESSAGE = 200
};

/* A die's states. */
enum {
        D_CUP,   /* in the cup: not shown */
        D_SCOOP, /* on its way into the cup */
        D_ROLL,  /* thrown: tumbling under the physics */
        D_REST,  /* lying in the tray */
        D_HOP,   /* hopping (to the shelf, back, or into a line) */
        D_HELD   /* on the shelf */
};

typedef struct {
                uint8_t  state;
                uint8_t  value;   /* the face shown (0: none yet) */
                uint8_t  landed;  /* rolling: showing its face (settling) */
                uint8_t  after;   /* a hop's: the state it ends in */
                float    x, y, z; /* on the felt (its middle), and its height */
                float    vx, vy, vz;
                float    tumble; /* the tumbling frames' phase */
                float    spin;   /* turns a tick, landed */
                uint16_t turn;
                uint16_t delay; /* thrown after this many ticks */
                /* A move (scoop, hop): from, to, its ticks. */
                float    fx, fy, tx, ty;
                uint16_t fturn, tturn;
                uint16_t t, dur;
                float    rest_x, rest_y; /* where it lay in the tray (a hop back returns there) */
                uint16_t rest_turn;
} die_t;

/* Points on their way to a box. */
typedef struct {
                uint8_t  used;
                int8_t   seat;
                uint8_t  box;
                int16_t  value;
                float    fx, fy, tx, ty;
                uint16_t t, dur;
} flyer_t;

typedef struct {
                float    x, y, vy, phase;
                uint16_t turn;
                uint8_t  kind;
} feather_t;

/* The roll's steps: scooping the dice, shaking the cup, throwing (then they tumble). */
enum {
        CUP_IDLE,
        CUP_SCOOP,
        CUP_SHAKE,
        CUP_THROW
};

enum {
        POSE_IDLE,
        POSE_THINK,
        POSE_HAPPY,
        POSE_SAD
};

/* The buttons: the system's (top right, in app_system_button()'s order) and the game's (the band). */
enum {
        B_LOBBY,
        B_NEW,
        B_OPTIONS,
        B_RULES,
        B_HINT,
        B_ROLL,
        B_AGAIN,
        BUTTONS
};

static const char *const BUTTON_LABEL[BUTTONS] = {"LOBBY", "NEW GAME", "OPTIONS",   "RULES",
                                                  "HINT",  "ROLL",     "PLAY AGAIN"};
static const uint32_t    BUTTON_KEY[BUTTONS] = {0, K_N, K_O, K_F1, K_T, K_R, K_ENTER};

static const char *const BOX_LABEL[CARD_ROWS] = {
        "ONES",        "TWOS",       "THREES",      "FOURS",       "FIVES",      "SIXES",
        "UPPER",       "BONUS 35",   "3 OF A KIND", "4 OF A KIND", "FULL HOUSE", "SM STRAIGHT",
        "LG STRAIGHT", "LUCKY DUCK", "CHANCE",      "LUCKY BONUS", "TOTAL"};
static const char *const BOX_NAME[DICE_BOXES] = {
        "ONES",        "TWOS",       "THREES",      "FOURS",       "FIVES",      "SIXES", "3 OF A KIND",
        "4 OF A KIND", "FULL HOUSE", "SM STRAIGHT", "LG STRAIGHT", "LUCKY DUCK", "CHANCE"};
static const char *const SEAT_NAME[DICE_SEATS] = {"YOU", "PEARL", "WOODY", "MALLORY"};

static struct {
                dice_table_t table;
                stage_t      stage;
                int          players; /* the player and the bots */
                int          bots, fast;
                /* The picture's model. */
                die_t     die[DICE_DICE];
                int16_t   box[DICE_SEATS][DICE_BOXES];
                int16_t   lucky[DICE_SEATS];
                int8_t    turn_seat;
                uint8_t   round;
                uint8_t   rolls; /* this turn's, as shown */
                uint8_t   roll_values[DICE_DICE];
                uint8_t   cup; /* CUP_... */
                uint16_t  cup_ticks;
                flyer_t   flyer[FLYERS];
                feather_t feather[FEATHERS];
                uint16_t  party; /* a Lucky Duck's ticks left */
                uint8_t   pose[DICE_SEATS];
                uint16_t  pose_ticks[DICE_SEATS];
                uint32_t  cursor; /* the next event to show */
                uint16_t  wait;
                uint16_t  clack_wait;
                /* The bots. */
                uint16_t      bot_wait;
                dice_choice_t bot_choice;
                int32_t       bot_choice_for; /* the turn and roll it was worked out for (-1: none) */
                /* The game's end. */
                uint8_t over, recorded;
                int     high_new; /* the place the player's score took in the high scores, or -1 */
                int32_t high[5];
                /* Input. */
                int         buttons[BUTTONS];
                int         button_count;
                ui_button_t button_at[BUTTONS];
                int         hover, hover_die, hover_box;
                int         options;
                uint16_t    hint_ticks;
                char        hint[32];
                uint16_t    pop_ticks; /* a word over the tray */
                char        pop[sizeof(((stage_item_t *)0)->text)];
                uint16_t    lucky_mood;
                int         lucky_pose;
                uint32_t    ticks;
} d;

/* ---- Geometry */

static int32_t card_w(void) {
        return LABEL_W + d.players * COL_W + 12;
}

static int32_t card_x(void) {
        return WIDTH - 158 - 16 - card_w();
}

/* Row @p row's top and height: the header, the upper boxes and their sums, a gap, the lower
   boxes, a gap, the Lucky Duck bonus and the total. */
static int32_t row_top(int row) {
        return CARD_Y + HEAD_H + row * ROW_H + (row >= 8 ? 6 : 0) + (row >= ROW_LUCKY ? 6 : 0);
}

static int32_t row_h(int row) {
        return row == ROW_TOTAL ? TOTAL_H : ROW_H;
}

static int32_t card_h(void) {
        return row_top(ROW_TOTAL) + TOTAL_H + 8 - CARD_Y;
}

/* Where text in @p font starts (its capitals' top) to sit in the middle of row @p row. */
static int32_t row_text_y(int row, int font) {
        return row_top(row) + (row_h(row) - art_fonts[font].cap) / 2;
}

static int box_row(int box) {
        return box <= DICE_SIXES ? box : box + 2;
}

static int32_t col_x(int seat) {
        return card_x() + LABEL_W + seat * COL_W;
}

/* The tray: its outside (the rail in), and the felt inside, where the dice roll. */
#define TRAY_TOP 150
#define RAIL 16
static bmds_rect_t tray(void) {
        return rect(16, TRAY_TOP, card_x() - 32, BAND_Y - 108 - TRAY_TOP);
}

static bmds_rect_t tray_felt(void) {
        const bmds_rect_t r = tray();
        return rect(r.x + RAIL, r.y + RAIL, r.width - 2 * RAIL, r.height - 2 * RAIL);
}

/* The shelf's slot @p i: its middle. */
static void slot_middle(int i, float *x, float *y) {
        const bmds_rect_t t = tray();
        *x = (float)(t.x + t.width / 2 + (i - 2) * 76);
        *y = (float)(BAND_Y - 54);
        return;
}

#define LUCKY_X 6
#define LUCKY_Y 10

/* The cup's mouth, where the dice go in and come out. */
static void cup_mouth(float *x, float *y) {
        *x = LUCKY_X + 118;
        *y = LUCKY_Y + 92;
        return;
}

static void bot_place(int seat, int32_t *x, int32_t *y) {
        /* Beside Lucky, spread over the tray's width (at most 116 apart). */
        const bmds_rect_t t = tray();
        int32_t           step = d.players > 2 ? (t.x + (int32_t)t.width - 134 - 96) / (d.players - 2) : 116;
        if (step > 116) step = 116;
        *x = 134 + (seat - 1) * step;
        *y = 26;
        return;
}

/* ---- Easing */

static float ease_out(float from, float to, uint32_t t, uint32_t n) {
        if (t >= n) return to;
        const float u = (float)t / (float)n;
        return from + (to - from) * (2.0f * u - u * u);
}

static float frand(float a, float b) {
        return a + (b - a) * (float)(rand() % 10000) / 10000.0f;
}

/* ---- The dice */

/* Starts die @p i moving to (@p x, @p y) at @p turn over @p dur ticks, ending in @p after. */
static void die_move(int i, float x, float y, uint16_t turn, uint16_t dur, int state, int after) {
        die_t *e = &d.die[i];
        e->fx = e->x;
        e->fy = e->y;
        e->fturn = e->turn;
        e->tx = x;
        e->ty = y;
        e->tturn = turn;
        e->t = 0;
        e->dur = dur;
        e->state = (uint8_t)state;
        e->after = (uint8_t)after;
        e->z = 0.0f;
        return;
}

static int rolling(void) {
        if (d.cup != CUP_IDLE) return 1;
        for (int i = 0; i < DICE_DICE; ++i)
                if (d.die[i].state == D_SCOOP || d.die[i].state == D_ROLL) return 1;
        return 0;
}

static int hopping(void) {
        for (int i = 0; i < DICE_DICE; ++i)
                if (d.die[i].state == D_HOP) return 1;
        return 0;
}

static void clack(float level) {
        if (d.clack_wait > 0 || level < 0.08f) return;
        sfx(SFX_CLACK, level > 0.8f ? 0.8f : level);
        d.clack_wait = 3;
        return;
}

/* Where die @p i can lie in the tray clear of the others (lying there, landing there, rolling):
   where it lay before, or the nearest clear place around it. */
static void clear_spot(int i, float *x, float *y) {
        const bmds_rect_t f = tray_felt();
        const float       gap = 2.0f * DIE_R + 4.0f;
        const float       x0 = (float)f.x + DIE_R + 2, x1 = (float)(f.x + (int32_t)f.width) - DIE_R - 2;
        const float       y0 = (float)f.y + DIE_R + 2, y1 = (float)(f.y + (int32_t)f.height) - DIE_R - 2;
        const float       want_x = d.die[i].rest_x, want_y = d.die[i].rest_y;
        for (int ring = 0; ring < 8; ++ring) {
                const int steps = ring == 0 ? 1 : 6 * ring;
                for (int k = 0; k < steps; ++k) {
                        const float a = 2 * PI * (float)k / (float)steps;
                        float cx = want_x + cosf(a) * gap * (float)ring, cy = want_y + sinf(a) * gap * (float)ring;
                        cx = cx < x0 ? x0 : cx > x1 ? x1 : cx;
                        cy = cy < y0 ? y0 : cy > y1 ? y1 : cy;
                        int clear = 1;
                        for (int j = 0; j < DICE_DICE && clear; ++j) {
                                const die_t *o = &d.die[j];
                                float        ox, oy;
                                if (j == i) continue;
                                if (o->state == D_REST || o->state == D_ROLL) {
                                        ox = o->x;
                                        oy = o->y;
                                } else if (o->state == D_HOP && o->after == D_REST) {
                                        ox = o->tx;
                                        oy = o->ty;
                                } else {
                                        continue;
                                }
                                if ((ox - cx) * (ox - cx) + (oy - cy) * (oy - cy) < gap * gap) clear = 0;
                        }
                        if (clear) {
                                *x = cx;
                                *y = cy;
                                return;
                        }
                }
        }
        *x = want_x;
        *y = want_y;
        return;
}

/* Die @p i hops off the shelf back into the tray. */
static void hop_back(int i) {
        float x, y;
        clear_spot(i, &x, &y);
        die_move(i, x, y, d.die[i].rest_turn, 14, D_HOP, D_REST);
        return;
}

/* The roll begins: the dice not held go into the cup. */
static void start_roll(const dice_event_t *e) {
        memcpy(d.roll_values, e->dice, sizeof(d.roll_values));
        float mx, my;
        cup_mouth(&mx, &my);
        for (int i = 0; i < DICE_DICE; ++i) {
                if ((e->held >> i) & 1u) continue;
                die_t *x = &d.die[i];
                if (x->state != D_CUP) die_move(i, mx, my + 40.0f, x->turn, 12, D_SCOOP, D_CUP);
        }
        d.cup = CUP_SCOOP;
        d.cup_ticks = 0;
        d.hint_ticks = 0;
        return;
}

/* Out of the cup: each die not held thrown across the tray, a tick or two apart. */
static void throw_dice(void) {
        const bmds_rect_t f = tray_felt();
        int               k = 0;
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                if (x->state != D_CUP) continue;
                x->state = D_ROLL;
                x->landed = 0;
                x->x = (float)(f.x + 40) + frand(0, 30);
                x->y = (float)(f.y + 30) + frand(0, 30);
                x->z = 50.0f + frand(0, 16);
                x->vx = frand(0.65f, 1.05f) * (float)f.width / 75.0f; /* across the tray, however wide */
                x->vy = frand(1.5f, 5.0f);
                x->vz = frand(1.0f, 3.5f);
                x->tumble = frand(0, 8);
                x->spin = 0;
                x->delay = (uint16_t)(k++ * 3);
                x->value = d.roll_values[i];
        }
        sfx(SFX_DEAL, 0.25f);
        return;
}

static void physics(void) {
        const bmds_rect_t f = tray_felt();
        const float       left = (float)f.x + DIE_R, right = (float)(f.x + f.width) - DIE_R;
        const float       top = (float)f.y + DIE_R, bottom = (float)(f.y + f.height) - DIE_R;
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                if (x->state != D_ROLL) continue;
                if (x->delay > 0) {
                        --x->delay;
                        continue;
                }
                x->vz -= 0.6f;
                x->z += x->vz;
                if (x->z <= 0.0f) {
                        x->z = 0.0f;
                        if (x->vz < -1.6f) { /* a bounce */
                                clack(-x->vz * 0.12f);
                                x->vz = -x->vz * 0.42f;
                                x->vx *= 0.82f;
                                x->vy *= 0.82f;
                        } else {
                                x->vz = 0.0f;
                        }
                }
                if (x->z == 0.0f && x->vz == 0.0f) { /* on the felt: it slows */
                        x->vx *= 0.94f;
                        x->vy *= 0.94f;
                }
                x->x += x->vx;
                x->y += x->vy;
                if (x->x < left || x->x > right) {
                        x->x = x->x < left ? left : right;
                        clack(fabsf(x->vx) * 0.08f);
                        x->vx = -x->vx * 0.6f;
                }
                if (x->y < top || x->y > bottom) {
                        x->y = x->y < top ? top : bottom;
                        clack(fabsf(x->vy) * 0.08f);
                        x->vy = -x->vy * 0.6f;
                }
        }
        /* Dice bumping into each other: pushed apart, their speeds along the line between swapped. */
        for (int i = 0; i < DICE_DICE; ++i) {
                for (int j = i + 1; j < DICE_DICE; ++j) {
                        die_t    *a = &d.die[i], *b = &d.die[j];
                        const int ra = a->state == D_ROLL && a->delay == 0, rb = b->state == D_ROLL && b->delay == 0;
                        if (!(ra || rb) || (!ra && a->state != D_REST) || (!rb && b->state != D_REST)) continue;
                        if (fabsf(a->z - b->z) > 24.0f) continue;
                        const float dx = b->x - a->x, dy = b->y - a->y, dd = dx * dx + dy * dy;
                        if (dd >= 4.0f * DIE_R * DIE_R || dd < 0.01f) continue;
                        /* A die lying still is knocked: it slides on, face up as it lies. */
                        for (die_t *x = a; x != NULL; x = x == a ? b : NULL)
                                if (x->state == D_REST) {
                                        x->state = D_ROLL;
                                        x->landed = 1;
                                        x->spin = 0.0f;
                                }
                        const float dist = sqrtf(dd), nx = dx / dist, ny = dy / dist, push = (2.0f * DIE_R - dist) / 2;
                        a->x -= nx * push;
                        a->y -= ny * push;
                        b->x += nx * push;
                        b->y += ny * push;
                        const float va = a->vx * nx + a->vy * ny, vb = b->vx * nx + b->vy * ny;
                        if (va - vb <= 0.0f) continue;
                        const float swap = (va - vb) * 0.85f;
                        a->vx -= swap * nx;
                        a->vy -= swap * ny;
                        b->vx += swap * nx;
                        b->vy += swap * ny;
                        clack((va - vb) * 0.1f);
                }
        }
        /* Tumbling, landing on the face rolled, stopping. */
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                if (x->state != D_ROLL || x->delay > 0) continue;
                const float speed = sqrtf(x->vx * x->vx + x->vy * x->vy);
                const int   grounded = x->z == 0.0f && x->vz == 0.0f;
                if (!x->landed) {
                        x->tumble += speed * 0.11f + fabsf(x->vz) * 0.12f;
                        /* Turned the way it travels: its tumbling frames roll downwards. */
                        if (speed > 0.2f) x->turn = (uint16_t)(int32_t)(atan2f(-x->vx, x->vy) / (2 * PI) * 65536.0f);
                        if (grounded && speed < 2.6f) {
                                x->landed = 1;
                                x->spin = (rand() % 2 ? 1.0f : -1.0f) * speed * 260.0f;
                                clack(0.25f);
                        }
                } else {
                        x->turn = (uint16_t)(x->turn + (int32_t)x->spin);
                        x->spin *= 0.9f;
                }
                if (grounded && x->landed && speed < 0.25f) {
                        x->state = D_REST;
                        x->vx = x->vy = 0.0f;
                        x->rest_x = x->x;
                        x->rest_y = x->y;
                        x->rest_turn = x->turn;
                }
        }
        return;
}

static int lucky_duck_rolled(void) {
        for (int i = 1; i < DICE_DICE; ++i)
                if (d.roll_values[i] != d.roll_values[0] || d.roll_values[0] == 0) return 0;
        return 1;
}

static void start_party(void);

static void dice_tick_motion(void) {
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                if (x->state != D_SCOOP && x->state != D_HOP) continue;
                if (++x->t >= x->dur) {
                        x->x = x->tx;
                        x->y = x->ty;
                        x->turn = x->tturn;
                        x->z = 0.0f;
                        x->state = x->after;
                        if (x->state == D_REST) {
                                x->rest_x = x->x;
                                x->rest_y = x->y;
                                x->rest_turn = x->turn;
                        }
                        if (x->state == D_HELD || x->state == D_REST) clack(0.2f);
                        continue;
                }
                x->x = ease_out(x->fx, x->tx, x->t, x->dur);
                x->y = ease_out(x->fy, x->ty, x->t, x->dur);
                x->turn = (uint16_t)(x->fturn + (int16_t)(x->tturn - x->fturn) * (int32_t)x->t / (int32_t)x->dur);
                x->z = x->state == D_HOP ? 34.0f * sinf(PI * (float)x->t / (float)x->dur) : 0.0f;
        }
        physics();
        /* The cup's steps. */
        ++d.cup_ticks;
        switch (d.cup) {
                case CUP_SCOOP: {
                        int busy = 0;
                        for (int i = 0; i < DICE_DICE; ++i) busy |= d.die[i].state == D_SCOOP;
                        if (!busy) {
                                d.cup = CUP_SHAKE;
                                d.cup_ticks = 0;
                                sfx(SFX_RATTLE, 0.7f);
                        }
                        break;
                }
                case CUP_SHAKE:
                        if (d.cup_ticks >= 38) {
                                d.cup = CUP_THROW;
                                d.cup_ticks = 0;
                                throw_dice();
                        }
                        break;
                case CUP_THROW: {
                        int busy = 0;
                        for (int i = 0; i < DICE_DICE; ++i) busy |= d.die[i].state == D_ROLL;
                        if (!busy && d.cup_ticks >= 14) {
                                d.cup = CUP_IDLE;
                                if (lucky_duck_rolled()) start_party();
                        }
                        break;
                }
                default: break;
        }
        return;
}

/* ---- A Lucky Duck: the dice in a line, feathers, ducklings, the fanfare */

static void start_party(void) {
        const bmds_rect_t f = tray_felt();
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                if (x->state != D_REST) continue;
                die_move(i, (float)(f.x + f.width / 2 + (i - 2) * 64), (float)(f.y + f.height / 2 - 20), 0, 18, D_HOP,
                         D_REST);
        }
        for (int i = 0; i < FEATHERS; ++i) {
                feather_t *p = &d.feather[i];
                p->x = frand(0, (float)(card_x() + 80));
                p->y = -20.0f - frand(0, 260);
                p->vy = frand(1.2f, 2.4f);
                p->phase = frand(0, 2 * PI);
                p->turn = (uint16_t)(rand() & 0xffff);
                p->kind = (uint8_t)(i % 2);
        }
        d.party = PARTY;
        d.lucky_pose = ART_LUCKY_CHEER;
        d.lucky_mood = PARTY;
        sfx(SFX_WIN, 0.8f);
        sfx(SFX_QUACKS, 0.5f);
        return;
}

static void party_tick(void) {
        if (d.party == 0) return;
        --d.party;
        for (int i = 0; i < FEATHERS; ++i) {
                feather_t *p = &d.feather[i];
                p->y += p->vy;
                p->phase += 0.06f;
                p->x += sinf(p->phase) * 1.2f;
                p->turn = (uint16_t)(p->turn + (int32_t)(sinf(p->phase) * 500.0f));
        }
        if (d.party == PARTY - 70) sfx(SFX_QUACK2, 0.4f);
        return;
}

/* ---- Points flying to the card */

static void cell_middle(int seat, int row, float *x, float *y) {
        *x = (float)(col_x(seat) + COL_W / 2);
        *y = (float)(row_top(row) + row_h(row) / 2);
        return;
}

/* The dice's middle on the screen (where points fly from). */
static void dice_middle(float *x, float *y) {
        float sx = 0, sy = 0;
        int   n = 0;
        for (int i = 0; i < DICE_DICE; ++i) {
                if (d.die[i].state == D_CUP) continue;
                sx += d.die[i].x;
                sy += d.die[i].y;
                ++n;
        }
        if (n == 0) {
                const bmds_rect_t f = tray_felt();
                *x = (float)(f.x + f.width / 2);
                *y = (float)(f.y + f.height / 2);
                return;
        }
        *x = sx / (float)n;
        *y = sy / (float)n - 30.0f;
        return;
}

static void fly_points(int seat, int box, int value) {
        for (int i = 0; i < FLYERS; ++i) {
                flyer_t *f = &d.flyer[i];
                if (f->used) continue;
                f->used = 1;
                f->seat = (int8_t)seat;
                f->box = (uint8_t)box;
                f->value = (int16_t)value;
                dice_middle(&f->fx, &f->fy);
                cell_middle(seat, box_row(box), &f->tx, &f->ty);
                f->t = 0;
                f->dur = 26;
                return;
        }
        d.box[seat][box] = (int16_t)value; /* no room to fly: written at once */
        return;
}

static void flyers_tick(void) {
        for (int i = 0; i < FLYERS; ++i) {
                flyer_t *f = &d.flyer[i];
                if (!f->used) continue;
                if (++f->t >= f->dur) {
                        d.box[f->seat][f->box] = f->value;
                        f->used = 0;
                        sfx(SFX_PLACE, 0.5f);
                }
        }
        return;
}

/* ---- The model, from the log */

static void say(const char *text, uint16_t ticks) {
        snprintf(d.pop, sizeof(d.pop), "%s", text);
        d.pop_ticks = ticks;
        return;
}

static void set_pose(int seat, int pose, uint16_t ticks) {
        d.pose[seat] = (uint8_t)pose;
        d.pose_ticks[seat] = ticks;
        return;
}

static void record_game(void);

/* Shows event @p e: changes the model, starts its animation. @return The ticks it holds the next. */
static uint16_t show(const dice_event_t *e) {
        switch (e->type) {
                case DICE_EV_START: return 0;
                case DICE_EV_TURN:
                        d.turn_seat = e->seat;
                        d.round = e->roll;
                        d.rolls = 0;
                        for (int i = 0; i < DICE_DICE; ++i) /* the last turn's holds go back to the tray */
                                if (d.die[i].state == D_HELD) hop_back(i);
                        if (e->seat != PLAYER) set_pose(e->seat, POSE_THINK, 0);
                        if (e->seat == PLAYER && d.players > 1) say("YOUR TURN", 70);
                        return d.players > 1 ? 16 : 0;
                case DICE_EV_HOLD: {
                        die_t *x = &d.die[e->die];
                        if (e->held) {
                                float sx, sy;
                                slot_middle(e->die, &sx, &sy);
                                if (x->state == D_REST) {
                                        x->rest_x = x->x;
                                        x->rest_y = x->y;
                                        x->rest_turn = x->turn;
                                }
                                die_move(e->die, sx, sy, 0, 14, D_HOP, D_HELD);
                        } else {
                                hop_back(e->die);
                        }
                        sfx(SFX_CLICK, 0.3f);
                        return e->seat == PLAYER ? 0 : 12;
                }
                case DICE_EV_ROLL:
                        d.rolls = e->roll;
                        start_roll(e);
                        return 0;
                case DICE_EV_SCORE:
                        fly_points(e->seat, e->die, (int)e->amount);
                        if (e->seat != PLAYER) {
                                const int good =
                                        e->amount >= 25 || (e->die <= DICE_SIXES && e->amount >= 3 * (e->die + 1));
                                set_pose(e->seat, e->amount == 0 ? POSE_SAD : good ? POSE_HAPPY : POSE_IDLE, 70);
                                if (good) sfx(SFX_QUACK2, 0.3f);
                        } else if (e->amount == 0) {
                                sfx(SFX_SAD, 0.3f);
                        }
                        return 34;
                case DICE_EV_BONUS: {
                        char text[32];
                        if (e->die == DICE_LUCKY_DUCK) {
                                d.lucky[e->seat] = (int16_t)(d.lucky[e->seat] + e->amount);
                                snprintf(text, sizeof(text), "LUCKY BONUS +%d", (int)e->amount);
                        } else {
                                snprintf(text, sizeof(text), "UPPER BONUS +%d", (int)e->amount);
                        }
                        say(text, 80);
                        if (e->seat == PLAYER) sfx(SFX_WIN, 0.5f);
                        return 50;
                }
                case DICE_EV_END:
                        d.turn_seat = -1;
                        d.over = 1;
                        record_game();
                        if (d.players > 1) {
                                if (e->seat == PLAYER) {
                                        sfx(SFX_WIN, 0.7f);
                                        d.lucky_pose = ART_LUCKY_CHEER;
                                        d.lucky_mood = 150;
                                } else {
                                        sfx(SFX_SAD, 0.4f);
                                        set_pose(e->seat, POSE_HAPPY, 150);
                                }
                        } else if (d.high_new >= 0) {
                                sfx(SFX_WIN, 0.7f);
                        }
                        return 0;
                default: return 0;
        }
}

static int busy_showing(void) {
        if (d.wait > 0 || rolling() || d.party > 0) return 1;
        for (int i = 0; i < FLYERS; ++i)
                if (d.flyer[i].used) return 1;
        return 0;
}

/* Takes the table's events, one at a time as their animations end. */
static void replay(void) {
        if (d.wait > 0) {
                --d.wait;
                return;
        }
        while (!busy_showing() && d.cursor < d.table.event_count) {
                const dice_event_t e = *dice_event(&d.table, d.cursor++);
                d.wait = show(&e);
        }
        return;
}

static int caught_up(void) {
        return !busy_showing() && !hopping() && d.cursor == d.table.event_count;
}

/* ---- The game */

static void record_game(void) {
        if (d.recorded) return;
        d.recorded = 1;
        const int me = dice_total(&d.table.seat[PLAYER]);
        d.high_new = -1;
        for (int i = 0; i < 5; ++i) {
                char name[16];
                snprintf(name, sizeof(name), "dice.%d", i);
                d.high[i] = (int32_t)kept_score(name, 0);
        }
        for (int i = 0; i < 5; ++i) {
                if (me <= d.high[i]) continue;
                for (int k = 4; k > i; --k) d.high[k] = d.high[k - 1];
                d.high[i] = me;
                d.high_new = i;
                break;
        }
        for (int i = 0; i < 5; ++i) {
                char name[16];
                snprintf(name, sizeof(name), "dice.%d", i);
                keep_score(name, d.high[i]);
        }
        set_setting("dice.played", setting("dice.played", 0) + 1);
        if (d.players > 1) {
                int won = 1;
                for (int s = 1; s < d.players; ++s)
                        if (dice_total(&d.table.seat[s]) >= me) won = 0;
                if (won) set_setting("dice.won", setting("dice.won", 0) + 1);
        }
        return;
}

/* Under way from the first roll until the game's end is kept. */
static int dice_scene_busy(void) {
        const dice_table_t *t = &d.table;
        if (t->phase == DICE_OVER) return !d.recorded;
        return t->round > 0 || t->roll > 0 || t->turn_seat != PLAYER;
}

static void paint_felt(void);

static void new_game(void) {
        d.bots = (int)setting("dice.bots", 1);
        if (d.bots < 0 || d.bots > 3) d.bots = 1;
        d.fast = setting("dice.fast", 0) != 0;
        d.players = 1 + d.bots;
        dice_init(&d.table, (1u << d.players) - 1u, app_key());
        for (int s = 0; s < DICE_SEATS; ++s) {
                for (int b = 0; b < DICE_BOXES; ++b) d.box[s][b] = DICE_OPEN;
                d.lucky[s] = 0;
                d.pose[s] = POSE_IDLE;
                d.pose_ticks[s] = 0;
        }
        memset(d.flyer, 0, sizeof(d.flyer));
        d.cursor = 0;
        d.wait = 0;
        d.cup = CUP_IDLE;
        d.party = 0;
        d.over = d.recorded = 0;
        d.high_new = -1;
        d.turn_seat = -1;
        d.rolls = 0;
        d.round = 0;
        d.bot_choice_for = -1;
        d.hint_ticks = 0;
        /* The dice lie in a row in the tray until the first roll scoops them. */
        const bmds_rect_t f = tray_felt();
        for (int i = 0; i < DICE_DICE; ++i) {
                die_t *x = &d.die[i];
                memset(x, 0, sizeof(*x));
                x->state = D_REST;
                x->value = (uint8_t)(i + 1);
                x->x = x->rest_x = (float)(f.x + f.width / 2 + (i - 2) * 64);
                x->y = x->rest_y = (float)(f.y + f.height / 2);
                x->turn = x->rest_turn = (uint16_t)((i * 2900) & 0x1fff);
        }
        paint_felt();
        stage_dirty(&d.stage, rect(0, 0, WIDTH, HEIGHT));
        return;
}

static void play_again(void) {
        new_game();
        return;
}

/* ---- The bots */

static void table_tick(void) {
        dice_table_t *t = &d.table;
        if (!caught_up() || t->phase != DICE_PLAYING || t->turn_seat == PLAYER) {
                d.bot_wait = 0;
                return;
        }
        const int seat = t->turn_seat;
        const int think = d.fast ? 10 : 30;
        if (++d.bot_wait < (t->roll == 0 ? think / 2 : think)) return;
        d.bot_wait = d.fast ? 4 : 14; /* the next hold follows sooner */
        if (t->roll == 0) {
                d.bot_wait = 0;
                (void)dice_roll(t, seat);
                return;
        }
        const int32_t key = (int32_t)(t->round * 64 + seat * 4 + t->roll);
        if (d.bot_choice_for != key) {
                d.bot_choice = dice_bot(t, seat, seat == 2 ? 1 : 2); /* Woody looks only a roll ahead */
                d.bot_choice_for = key;
        }
        const dice_choice_t c = d.bot_choice;
        if (c.box < DICE_BOXES) {
                d.bot_wait = 0;
                (void)dice_score(t, seat, c.box);
                set_pose(seat, POSE_IDLE, 0);
                return;
        }
        for (int i = 0; i < DICE_DICE; ++i) {
                const int want = (c.keep >> i) & 1u;
                if (t->held[i] != want) {
                        (void)dice_hold(t, seat, i, want);
                        return;
                }
        }
        d.bot_wait = 0;
        (void)dice_roll(t, seat);
        return;
}

/* ---- The felt */

static uint16_t rgb565(uint32_t rgb) {
        return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

/* How far (@p x, @p y) is inside the rounded rectangle @p r (corner radius @p rad): negative outside. */
static int32_t inside_by(bmds_rect_t r, int32_t rad, int32_t x, int32_t y) {
        const int32_t x0 = r.x + rad, x1 = r.x + (int32_t)r.width - 1 - rad;
        const int32_t y0 = r.y + rad, y1 = r.y + (int32_t)r.height - 1 - rad;
        const int32_t dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0;
        const int32_t dy = y < y0 ? y0 - y : y > y1 ? y - y1 : 0;
        if (dx == 0 && dy == 0) {
                int32_t m = x - r.x;
                if (r.x + (int32_t)r.width - 1 - x < m) m = r.x + (int32_t)r.width - 1 - x;
                if (y - r.y < m) m = y - r.y;
                if (r.y + (int32_t)r.height - 1 - y < m) m = r.y + (int32_t)r.height - 1 - y;
                return m;
        }
        int32_t dist = 0;
        while ((dist + 1) * (dist + 1) <= dx * dx + dy * dy) ++dist;
        return rad - dist;
}

/* A wooden rail around @p r (@p width wide, rounded in the middle), the felt inside darkened. */
static void paint_rail(bmds_rect_t r, int32_t width) {
        uint16_t *px = (uint16_t *)(void *)app.felt.pixels;
        uint16_t  wood[32];
        for (int32_t i = 0; i < width && i < 32; ++i) {
                const int32_t c = i - width / 2, k = 255 - c * c * 300 / (width * width);
                wood[i] = rgb565(((uint32_t)(0x8a * k / 255) << 16) | ((uint32_t)(0x52 * k / 255) << 8) |
                                 (uint32_t)(0x22 * k / 255));
        }
        const uint16_t edge = rgb565(0x2e1c0c);
        for (int32_t y = r.y; y < r.y + (int32_t)r.height; ++y) {
                for (int32_t x = r.x; x < r.x + (int32_t)r.width; ++x) {
                        const int32_t in = inside_by(r, 22, x, y);
                        if (in < 0) continue;
                        uint16_t *p = &px[(size_t)y * WIDTH + x];
                        if (in < 1 || in == width) {
                                *p = edge;
                        } else if (in < width) {
                                *p = wood[in];
                        } else if (in < width + 6) { /* the felt in the rail's shadow */
                                const uint16_t v = *p;
                                const uint32_t k = 20 + (uint32_t)(in - width) * 2;
                                *p = (uint16_t)((((v >> 11) * k / 32) << 11) | ((((v >> 5) & 63) * k / 32) << 5) |
                                                ((v & 31) * k / 32));
                        }
                }
        }
        return;
}

static void paint_card(bmds_canvas_t *c) {
        const int32_t x = card_x(), w = card_w();
        ui_panel(c, rect(x, CARD_Y, w, card_h()));
        const uint32_t line = bmds_canvas_pixel(c, 0x2e6a44), sums = bmds_canvas_pixel(c, 0x123c24),
                       gold = bmds_canvas_pixel(c, 0xb08028);
        for (int r = 0; r < CARD_ROWS; ++r) {
                const int32_t y = row_top(r), h = row_h(r);
                if (r == ROW_UPPER || r == ROW_BONUS || r == ROW_LUCKY || r == ROW_TOTAL)
                        bmds_canvas_fill(c, x + 6, y + 1, w - 12, h - 1, sums);
                if (r != ROW_TOTAL) bmds_canvas_fill(c, x + 8, y + h, w - 16, 1, line);
                if (r <= DICE_SIXES) {
                        const art_tile_t *t = &art_sprites[ART_DIE_SMALL_1 + r];
                        ui_sprite(c, x + 12, y + (h - t->h) / 2, ART_DIE_SMALL_1 + r, 0);
                }
                (void)ui_text(c, ART_FONT_SMALL, x + (r <= DICE_SIXES ? 46 : 16), row_text_y(r, ART_FONT_SMALL),
                              BOX_LABEL[r]);
        }
        bmds_canvas_fill(c, x + 8, CARD_Y + HEAD_H - 3, w - 16, 2, gold); /* just above the first row */
        for (int s = 0; s < d.players; ++s) {
                bmds_canvas_fill(c, col_x(s), CARD_Y + 8, 1, card_h() - 16, line);
                ui_text_centred(c, ART_FONT_SMALL, col_x(s) + COL_W / 2,
                                CARD_Y + 2 + (HEAD_H - 5 - art_fonts[ART_FONT_SMALL].cap) / 2, SEAT_NAME[s]);
        }
        return;
}

static void paint_felt(void) {
        felt_baize(0x1a8c4a, 0x0a4422);
        bmds_canvas_t *c = &app.felt_canvas;
        paint_rail(tray(), RAIL);
        /* The shelf: a plank under the tray, its five slots felt-lined in gold. */
        const bmds_rect_t t = tray();
        ui_round_fill(c, rect(t.x, BAND_Y - 98, t.width, 88), 10, 0x2e1c0c);
        ui_round_fill(c, rect(t.x + 2, BAND_Y - 96, t.width - 4, 84), 9, 0x6e4220);
        ui_round_fill(c, rect(t.x + 4, BAND_Y - 94, t.width - 8, 6), 3, 0x8e5a2c);
        for (int i = 0; i < DICE_DICE; ++i) {
                float sx, sy;
                slot_middle(i, &sx, &sy);
                ui_round_fill(c, rect((int32_t)sx - 34, (int32_t)sy - 34, 68, 68), 12, 0xb08028);
                ui_round_fill(c, rect((int32_t)sx - 32, (int32_t)sy - 32, 64, 64), 11, 0x0c4424);
        }
        /* The band. */
        bmds_canvas_fill(c, 0, BAND_Y, WIDTH, BAND_H, bmds_canvas_pixel(c, 0x2a1a10));
        bmds_canvas_fill(c, 0, BAND_Y, WIDTH, 2, bmds_canvas_pixel(c, 0xb08028));
        paint_card(c);
        bmds_canvas_damage_clear(c);
        return;
}

/* ---- The frame */

static int my_turn_ready(void) {
        return caught_up() && d.table.phase == DICE_PLAYING && d.table.turn_seat == PLAYER && !d.options;
}

static void show_buttons(void) {
        int n = 0;
        d.buttons[n++] = B_LOBBY;
        d.buttons[n++] = B_NEW;
        d.buttons[n++] = B_OPTIONS;
        d.buttons[n++] = B_RULES;
        if (d.over && caught_up()) {
                d.buttons[n++] = B_AGAIN;
        } else if (my_turn_ready()) {
                d.buttons[n++] = B_HINT;
                if (d.table.roll < DICE_ROLLS) d.buttons[n++] = B_ROLL;
        }
        d.button_count = n;
        int32_t x = WIDTH - 16;
        for (int i = n - 1; i >= 0; --i) {
                const int id = d.buttons[i];
                d.button_at[i].label = BUTTON_LABEL[id];
                d.button_at[i].key = BUTTON_KEY[id];
                if (id <= B_RULES) {
                        d.button_at[i].r = app_system_button(id);
                        continue;
                }
                const int32_t w = ui_text_width(ART_FONT_SMALL, BUTTON_LABEL[id]) + (id == B_ROLL ? 60 : 26);
                x -= w;
                d.button_at[i].r = rect(x, BAND_Y + 13, w, 40);
                x -= 8;
        }
        return;
}

static int bot_sprite(int seat) {
        static const int base[DICE_SEATS] = {0, ART_BOT_PEKIN_IDLE, ART_BOT_WOOD_IDLE, ART_BOT_MALLARD_IDLE};
        return base[seat] + d.pose[seat];
}

/* A die's place on the screen: its middle, raised by its height. */
static void die_screen(const die_t *x, int32_t *sx, int32_t *sy) {
        *sx = (int32_t)x->x;
        *sy = (int32_t)(x->y - x->z * 0.8f);
        return;
}

static void layout_dice(stage_t *st) {
        for (int i = 0; i < DICE_DICE; ++i) {
                const die_t *x = &d.die[i];
                if (x->state == D_CUP || (x->state == D_ROLL && x->delay > 0)) continue;
                int32_t sx, sy;
                die_screen(x, &sx, &sy);
                if (d.hover_die == i && my_turn_ready() && d.table.roll > 0 && d.table.roll < DICE_ROLLS) sy -= 3;
                int      tile;
                uint16_t scale = 0;
                if (x->state == D_ROLL && !x->landed)
                        tile = ART_DIE_TUMBLE_0 + (int)x->tumble % 8;
                else
                        tile = x->value >= 1 && x->value <= 6 ? ART_DIE_1 + x->value - 1 : ART_DIE_1;
                if (x->state == D_SCOOP) { /* shrinking into the cup */
                        scale = (uint16_t)(256 - 150 * x->t / (x->dur ? x->dur : 1));
                } else if (x->z > 0.5f) {
                        scale = (uint16_t)(256 + (int32_t)(x->z * 0.7f));
                }
                const art_tile_t *t = &art_sprites[tile];
                stage_item_t     *it =
                        stage_put(st, (uint32_t)(K_DIE + i), STAGE_SPRITE,
                                  x->state == D_ROLL || x->state == D_HOP ? 46 : 40, sx - t->w / 2, sy - t->h / 2);
                it->tile = (uint16_t)tile;
                it->turn = x->turn;
                it->scale = scale;
                if (x->state == D_SCOOP) continue;
                /* Its shadow: away to the lower right as it rises. */
                const art_tile_t *sh = &art_sprites[ART_DIE_SHADOW];
                it = stage_put(st, (uint32_t)(K_DIE_SHADOW + i), STAGE_SHADE, 30,
                               (int32_t)x->x - sh->w / 2 + 3 + (int32_t)(x->z * 0.35f),
                               (int32_t)x->y - sh->h / 2 + 4 + (int32_t)(x->z * 0.12f));
                it->tile = ART_DIE_SHADOW;
                it->alpha = (uint8_t)(x->z > 60.0f ? 240 : 180 + (int32_t)x->z);
        }
        return;
}

static void cell_text(stage_t *st, int seat, int row, int font, int value) {
        stage_item_t *it = stage_put(st, (uint32_t)(K_CELL + seat * CARD_ROWS + row), STAGE_TEXT, 20,
                                     col_x(seat) + COL_W / 2, row_text_y(row, font));
        it->font = (uint8_t)font;
        it->flags = STAGE_CENTRE;
        snprintf(it->text, sizeof(it->text), "%d", value);
        return;
}

static void layout_card(stage_t *st) {
        const int hints = my_turn_ready() && d.table.roll > 0;
        for (int s = 0; s < d.players; ++s) {
                dice_seat_t shown;
                memset(&shown, 0, sizeof(shown));
                int upper_done = 1;
                for (int b = 0; b < DICE_BOXES; ++b) {
                        shown.box[b] = d.box[s][b];
                        if (d.box[s][b] != DICE_OPEN) {
                                cell_text(st, s, box_row(b), ART_FONT_SMALL, d.box[s][b]);
                        } else {
                                if (b <= DICE_SIXES) upper_done = 0;
                                if (hints && s == PLAYER && dice_can_score(&d.table, PLAYER, b))
                                        cell_text(st, s, box_row(b), ART_FONT_HINT, dice_points(&d.table, PLAYER, b));
                        }
                }
                shown.lucky_bonus = d.lucky[s];
                const int upper = dice_upper(&shown);
                cell_text(st, s, ROW_UPPER, ART_FONT_SMALL, upper);
                if (upper >= DICE_UPPER_TARGET || upper_done)
                        cell_text(st, s, ROW_BONUS, ART_FONT_SMALL, upper >= DICE_UPPER_TARGET ? DICE_UPPER_BONUS : 0);
                if (d.lucky[s] > 0) cell_text(st, s, ROW_LUCKY, ART_FONT_SMALL, d.lucky[s]);
                cell_text(st, s, ROW_TOTAL, ART_FONT_MED, dice_total(&shown));
        }
        /* The column whose turn it is, lit. */
        if (d.turn_seat >= 0) {
                stage_item_t *it = stage_put(st, (uint32_t)(K_COLUMN + d.turn_seat), STAGE_BLEND, 15,
                                             col_x(d.turn_seat) + 2, CARD_Y + 6);
                it->w = COL_W - 3;
                it->h = (int16_t)(card_h() - 12);
                it->rgb = 0xffd040;
                it->alpha = 34;
        }
        /* The box under the pointer, if it can be written. */
        if (d.hover_box >= 0 && my_turn_ready() && dice_can_score(&d.table, PLAYER, d.hover_box)) {
                stage_item_t *it =
                        stage_put(st, K_HOVER_ROW, STAGE_BLEND, 16, card_x() + 6, row_top(box_row(d.hover_box)) + 1);
                it->w = (int16_t)(card_w() - 12);
                it->h = ROW_H - 1;
                it->rgb = 0xffd040;
                it->alpha = 70;
        }
        for (int i = 0; i < FLYERS; ++i) {
                const flyer_t *f = &d.flyer[i];
                if (!f->used) continue;
                stage_item_t *it = stage_put(st, (uint32_t)(K_FLY + i), STAGE_TEXT, 75,
                                             (int32_t)ease_out(f->fx, f->tx, f->t, f->dur),
                                             (int32_t)ease_out(f->fy, f->ty, f->t, f->dur));
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "+%d", f->value);
        }
        return;
}

static void layout_band(stage_t *st) {
        stage_item_t *it;
        char          text[32];
        it = stage_put(st, K_BAND, STAGE_TEXT, 80, 20, BAND_Y + 8);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "ROUND");
        it = stage_put(st, K_BAND + 1, STAGE_TEXT, 80, 20, BAND_Y + 31);
        it->font = ART_FONT_MED;
        snprintf(it->text, sizeof(it->text), "%d OF %d", (d.over ? DICE_ROUNDS - 1 : d.round) + 1, DICE_ROUNDS);
        it = stage_put(st, K_BAND + 2, STAGE_TEXT, 80, 210, BAND_Y + 8);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "ROLLS");
        for (int r = 0; r < DICE_ROLLS; ++r) { /* the rolls left, lit */
                it = stage_put(st, (uint32_t)(K_BAND + 3 + r), STAGE_SPRITE, 80, 210 + r * 32, BAND_Y + 28);
                it->tile = ART_DIE_SMALL_1 + r;
                if (r < d.rolls || d.over) {
                        it = stage_put(st, (uint32_t)(K_BAND + 6 + r), STAGE_SHADE, 81, 210 + r * 32, BAND_Y + 28);
                        it->tile = (uint16_t)(ART_DIE_SMALL_1 + r);
                        it->alpha = 90;
                }
        }
        it = stage_put(st, K_BAND + 9, STAGE_TEXT, 80, 340, BAND_Y + 8);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "HIGH SCORE");
        it = stage_put(st, K_BAND + 10, STAGE_TEXT, 80, 340, BAND_Y + 31);
        it->font = ART_FONT_MED;
        snprintf(it->text, sizeof(it->text), "%d", (int)(d.over ? d.high[0] : kept_score("dice.0", 0)));
        if (d.turn_seat > PLAYER) {
                snprintf(text, sizeof(text), "%s IS PLAYING", SEAT_NAME[d.turn_seat]);
                it = stage_put(st, K_BAND + 11, STAGE_TEXT, 80, 520, BAND_Y + 24);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", text);
        }
        return;
}

static void layout_over(stage_t *st) {
        const bmds_rect_t t = tray();
        /* The seats by total, best first. */
        int order[DICE_SEATS] = {0}, n = 0, highs = 0;
        for (int s = 0; s < d.players; ++s) order[n++] = s;
        while (highs < 5 && d.high[highs] > 0) ++highs;
        const int32_t w = 520, h = 150 + n * 34 + (highs > 0 ? 40 + highs * 24 : 0), x = t.x + t.width / 2 - w / 2,
                      y = TRAY_TOP - 6;
        stage_item_t *it = stage_put(st, K_BOX, STAGE_PANEL, 90, x, y);
        it->w = w;
        it->h = (int16_t)h;
        for (int i = 1; i < n; ++i)
                for (int k = i; k > 0 && dice_total(&d.table.seat[order[k]]) > dice_total(&d.table.seat[order[k - 1]]);
                     --k) {
                        const int tmp = order[k];
                        order[k] = order[k - 1];
                        order[k - 1] = tmp;
                }
        it = stage_put(st, K_BOX + 1, STAGE_TEXT, 91, x + w / 2, y + 26);
        it->font = ART_FONT_BIG;
        it->flags = STAGE_CENTRE;
        if (n == 1)
                snprintf(it->text, sizeof(it->text), "GAME OVER");
        else if (order[0] == PLAYER)
                snprintf(it->text, sizeof(it->text), "YOU WIN!");
        else
                snprintf(it->text, sizeof(it->text), "%s WINS", SEAT_NAME[order[0]]);
        for (int i = 0; i < n; ++i) {
                const int32_t ly = y + 92 + i * 34;
                it = stage_put(st, (uint32_t)(K_BOX + 2 + i * 2), STAGE_TEXT, 91, x + 70, ly);
                it->font = ART_FONT_MED;
                snprintf(it->text, sizeof(it->text), "%d  %s", i + 1, SEAT_NAME[order[i]]);
                it = stage_put(st, (uint32_t)(K_BOX + 3 + i * 2), STAGE_TEXT, 91, x + w - 150, ly);
                it->font = ART_FONT_MED;
                snprintf(it->text, sizeof(it->text), "%d", dice_total(&d.table.seat[order[i]]));
        }
        const int32_t hy = y + 110 + n * 34;
        if (highs == 0) return;
        it = stage_put(st, K_BOX + 12, STAGE_TEXT, 91, x + w / 2, hy);
        it->font = ART_FONT_SMALL;
        it->flags = STAGE_CENTRE;
        snprintf(it->text, sizeof(it->text), "HIGH SCORES");
        for (int i = 0; i < highs; ++i) {
                const int32_t ly = hy + 28 + i * 24;
                it = stage_put(st, (uint32_t)(K_BOX + 13 + i), STAGE_TEXT, 91, x + w / 2 - 70, ly);
                it->font = i == d.high_new ? ART_FONT_HINT : ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%d.", i + 1);
                it = stage_put(st, (uint32_t)(K_BOX + 18 + i), STAGE_TEXT, 91, x + w / 2 - 30, ly);
                it->font = i == d.high_new ? ART_FONT_HINT : ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%d%s", (int)d.high[i], i == d.high_new ? "   NEW!" : "");
        }
        return;
}

/* The options: each line a key and a click. */
#define OPTION_LINES 2
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
        const int bots = (int)setting("dice.bots", 1), fast = setting("dice.fast", 0) != 0;
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
                if (i == 0)
                        snprintf(it->text, sizeof(it->text), "B   BOT DUCKS: %d", bots);
                else
                        snprintf(it->text, sizeof(it->text), "F   FAST BOTS: %s", fast ? "ON" : "OFF");
        }
        it = stage_put(st, K_BOX + 12, STAGE_LABEL, 91, bx + 50, by + 196);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "Click or press a key to change.");
        it = stage_put(st, K_BOX + 13, STAGE_LABEL, 91, bx + 50, by + 216);
        it->rgb = C_DIM;
        snprintf(it->text, sizeof(it->text), "%s",
                 d.players != 1 + bots || dice_scene_busy() ? "New bots sit down next game." : "Esc: close");
        return;
}

static void layout(void) {
        stage_t *st = &d.stage;
        stage_begin(st);
        stage_item_t *it;
        /* Lucky with the cup. */
        int lucky = (d.ticks % 240) < 8 ? ART_LUCKY_BLINK : ART_LUCKY_IDLE;
        if (d.cup == CUP_SHAKE)
                lucky = (d.cup_ticks / 5) % 2 ? ART_LUCKY_SHAKE1 : ART_LUCKY_SHAKE0;
        else if (d.cup == CUP_THROW || d.cup == CUP_SCOOP)
                lucky = d.cup == CUP_THROW ? ART_LUCKY_THROW : ART_LUCKY_SHAKE0;
        else if (d.lucky_mood > 0)
                lucky = d.lucky_pose;
        it = stage_put(st, K_LUCKY, STAGE_SPRITE, 50, LUCKY_X, LUCKY_Y);
        it->tile = (uint16_t)lucky;
        /* The bots, the one playing with an arrow over it. */
        for (int s = 1; s < d.players; ++s) {
                int32_t x, y;
                bot_place(s, &x, &y);
                it = stage_put(st, (uint32_t)(K_BOT + s - 1), STAGE_SPRITE, 20, x, y);
                it->tile = (uint16_t)bot_sprite(s);
                it = stage_put(st, (uint32_t)(K_BOT_NAME + s - 1), STAGE_TEXT, 20, x + 42, y + 92);
                it->font = d.turn_seat == s ? ART_FONT_HINT : ART_FONT_SMALL;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", SEAT_NAME[s]);
                if (d.turn_seat == s) {
                        const int32_t bob = (int32_t)((d.ticks / 4) % 8);
                        it = stage_put(st, K_ARROW, STAGE_SPRITE, 22, x + 42 - art_sprites[ART_ARROW].w / 2,
                                       y - 26 + (bob < 4 ? bob : 8 - bob));
                        it->tile = ART_ARROW;
                }
        }
        const bmds_rect_t f = tray_felt();
        if (d.hint_ticks > 0) { /* Lucky's whisper, at the tray's top */
                it = stage_put(st, K_BUBBLE, STAGE_PANEL, 70, f.x + 10, f.y + 10);
                it->w = (int16_t)(ui_text_width(ART_FONT_SMALL, d.hint) + 40);
                it->h = 44;
                it = stage_put(st, K_BUBBLE_TEXT, STAGE_TEXT, 71, f.x + 30, f.y + 24);
                it->font = ART_FONT_SMALL;
                snprintf(it->text, sizeof(it->text), "%s", d.hint);
        }
        layout_dice(st);
        layout_card(st);
        if (d.pop_ticks > 0) {
                const int32_t t = 90 - (d.pop_ticks > 90 ? 90 : d.pop_ticks), lift = t < 10 ? t * 3 : 30;
                it = stage_put(st, K_POP, STAGE_TEXT, 72, f.x + f.width / 2, f.y + 40 - lift + 30);
                it->font = ART_FONT_MED;
                it->flags = STAGE_CENTRE;
                snprintf(it->text, sizeof(it->text), "%s", d.pop);
        }
        if (d.party > 0) {
                const int32_t t = PARTY - d.party;
                if (t > 16) {
                        const int32_t pop = t - 16, lift = pop < 10 ? pop * 5 : 50 - (pop < 16 ? (pop - 10) * 2 : 12);
                        it = stage_put(st, K_PARTY_TEXT, STAGE_TEXT, 85, f.x + f.width / 2,
                                       f.y + f.height / 2 + 80 - lift);
                        it->font = ART_FONT_BIG;
                        it->flags = STAGE_CENTRE;
                        snprintf(it->text, sizeof(it->text), "LUCKY DUCK!");
                }
                for (int i = 0; i < FEATHERS; ++i) {
                        const feather_t *p = &d.feather[i];
                        if (p->y < -20.0f || p->y > (float)HEIGHT) continue;
                        it = stage_put(st, (uint32_t)(K_FEATHER + i), STAGE_SPRITE, 86, (int32_t)p->x, (int32_t)p->y);
                        it->tile = (uint16_t)(ART_FEATHER_0 + p->kind);
                        it->turn = p->turn;
                }
                for (int i = 0; i < DUCKLINGS; ++i) { /* marching across the tray's foot */
                        const int32_t x = f.x - 60 - i * 58 + t * 4;
                        if (x < f.x || x > f.x + (int32_t)f.width - 56) continue;
                        it = stage_put(st, (uint32_t)(K_DUCKLING + i), STAGE_SPRITE, 84, x, f.y + f.height - 60);
                        it->tile = (uint16_t)(ART_DUCKLING_WALK0 + (t / 6 + i) % 4);
                }
        }
        layout_band(st);
        show_buttons();
        for (int i = 0; i < d.button_count; ++i) {
                const ui_button_t *bt = &d.button_at[i];
                it = stage_put(st, (uint32_t)(K_BUTTON + i), STAGE_BUTTON, 80, bt->r.x, bt->r.y);
                it->w = (int16_t)bt->r.width;
                it->h = (int16_t)bt->r.height;
                it->font = d.hover == i ? UI_HOVER : UI_NORMAL;
                snprintf(it->text, sizeof(it->text), "%s", bt->label);
        }
        if (d.options)
                layout_options(st);
        else if (d.over && caught_up())
                layout_over(st);
        return;
}

/* ---- Input */

static const char *const RULES[] = {
        "Score the most points in 13 rounds: one box of the card a round.",
        "Roll up to three times a turn. Click dice (or press 1 to 5) to hold",
        "them on the shelf between rolls; click again to put them back.",
        "Then click a box on your card to write the dice there (gold numbers",
        "show what each box would score). A box can be used only once.",
        "",
        "ONES to SIXES   the dice of that number, added up",
        "                63 or more in these six: a bonus of 35",
        "3 / 4 OF A KIND three (four) the same: all five dice added up",
        "FULL HOUSE      three of one and two of another: 25",
        "SM STRAIGHT     four in a row: 30;  LG STRAIGHT  five in a row: 40",
        "LUCKY DUCK      five the same: 50",
        "CHANCE          anything: all five dice added up",
        "",
        "Another Lucky Duck pays 100 more (if the box holds 50), and is a joker:",
        "its number's box if open; else any lower box at full value; else 0",
        "in an upper box.",
        "",
        "R, Space or Enter rolls; T: Lucky's tip; O options; N a new game.",
};

static void rules(void) {
        app_info("LUCKY DUCK DICE RULES", RULES, (int)(sizeof(RULES) / sizeof(RULES[0])));
        return;
}

static void new_game_asked(void) {
        if (dice_scene_busy())
                app_confirm("START A NEW GAME?", "The game under way ends if you start again.", "NEW GAME", new_game);
        else
                new_game();
        return;
}

static void roll(void) {
        if (!my_turn_ready() || d.table.roll >= DICE_ROLLS) return;
        (void)dice_roll(&d.table, PLAYER);
        return;
}

static void toggle(int die) {
        dice_table_t *t = &d.table;
        /* Not waiting for hops to land: a die can be sent back while another is on its way. */
        const int ready = !busy_showing() && d.cursor == t->event_count && t->phase == DICE_PLAYING &&
                          t->turn_seat == PLAYER && !d.options;
        if (!ready || t->roll == 0 || t->roll >= DICE_ROLLS || die < 0 || die >= DICE_DICE) return;
        (void)dice_hold(t, PLAYER, die, !t->held[die]);
        return;
}

static void write_box(int box) {
        if (!my_turn_ready() || !dice_can_score(&d.table, PLAYER, box)) return;
        (void)dice_score(&d.table, PLAYER, box);
        d.hint_ticks = 0;
        return;
}

static void hint(void) {
        const dice_table_t *t = &d.table;
        if (t->roll == 0) {
                snprintf(d.hint, sizeof(d.hint), "PSST... ROLL!");
        } else {
                const dice_choice_t c = dice_bot(t, PLAYER, 2);
                if (c.box < DICE_BOXES) {
                        snprintf(d.hint, sizeof(d.hint), "PSST... %s!", BOX_NAME[c.box]);
                } else if (c.keep == 0) {
                        snprintf(d.hint, sizeof(d.hint), "PSST... ROLL THEM ALL");
                } else {
                        /* The dice to keep, the highest first. */
                        uint8_t keep[DICE_DICE];
                        int     n = 0;
                        for (int i = 0; i < DICE_DICE; ++i)
                                if ((c.keep >> i) & 1u) keep[n++] = t->dice[i];
                        for (int i = 1; i < n; ++i)
                                for (int k = i; k > 0 && keep[k] > keep[k - 1]; --k) {
                                        const uint8_t x = keep[k];
                                        keep[k] = keep[k - 1];
                                        keep[k - 1] = x;
                                }
                        int len = snprintf(d.hint, sizeof(d.hint), "PSST... HOLD");
                        for (int i = 0; i < n && len > 0 && len < (int)sizeof(d.hint) - 2; ++i)
                                len += snprintf(d.hint + len, sizeof(d.hint) - (size_t)len, " %d", keep[i]);
                }
        }
        d.hint_ticks = 200;
        sfx(SFX_QUACK2, 0.3f);
        return;
}

static void press(int id) {
        switch (id) {
                case B_LOBBY: app_leave(); break;
                case B_NEW: new_game_asked(); break;
                case B_OPTIONS: d.options = 1; break;
                case B_RULES: rules(); break;
                case B_HINT: hint(); break;
                case B_ROLL: roll(); break;
                case B_AGAIN: play_again(); break;
                default: break;
        }
        return;
}

static void option_change(int line) {
        if (line == 0) {
                set_setting("dice.bots", (setting("dice.bots", 1) + 1) % 4);
                if (!dice_scene_busy()) new_game(); /* nothing under way: they sit down now */
        } else {
                d.fast = !d.fast;
                set_setting("dice.fast", d.fast);
        }
        sfx(SFX_CLICK, 0.4f);
        return;
}

static int die_at(int32_t x, int32_t y) {
        for (int i = DICE_DICE - 1; i >= 0; --i) {
                const die_t *e = &d.die[i];
                if (e->state != D_REST && e->state != D_HELD) continue;
                const int32_t dx = x - (int32_t)e->x, dy = y - (int32_t)e->y;
                if (dx * dx + dy * dy <= 30 * 30) return i;
        }
        return -1;
}

static int box_at(int32_t x, int32_t y) {
        if (x < card_x() + 6 || x >= col_x(PLAYER) + COL_W) return -1;
        for (int b = 0; b < DICE_BOXES; ++b) {
                const int32_t top = row_top(box_row(b));
                if (y >= top && y < top + ROW_H) return b;
        }
        return -1;
}

static void dice_scene_event(const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t  x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const uint32_t pressed = e->data[2] & ~app.buttons;
                d.hover = ui_button_at(d.button_at, d.button_count, x, y);
                d.hover_die = die_at(x, y);
                d.hover_box = box_at(x, y);
                if (d.options) {
                        if ((pressed & 1u) != 0) {
                                int line = -1;
                                for (int i = 0; i < OPTION_LINES; ++i) {
                                        const bmds_rect_t r = option_line(i);
                                        if (contains(&r, x, y)) line = i;
                                }
                                if (line >= 0)
                                        option_change(line);
                                else
                                        d.options = 0;
                        }
                        return;
                }
                if ((pressed & 1u) == 0) return;
                if (d.hover >= 0)
                        press(d.buttons[d.hover]);
                else if (d.hover_die >= 0)
                        toggle(d.hover_die);
                else if (d.hover_box >= 0)
                        write_box(d.hover_box);
                return;
        }
        if (e->type == BMDS_EVENT_ACTIVE && e->data[0] == 0) {
                d.hover = d.hover_die = d.hover_box = -1;
                return;
        }
        if (e->type != BMDS_EVENT_KEY || (e->flags & BMDS_KEY_DOWN) == 0) return;
        const uint32_t usage = e->data[0];
        if (usage == K_M) {
                sound_toggle();
                return;
        }
        if (d.options) {
                if (usage == K_B) option_change(0);
                if (usage == K_F) option_change(1);
                if (usage == K_ESCAPE || usage == K_O || usage == K_ENTER) d.options = 0;
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
        for (int i = 0; i < d.button_count; ++i) {
                const int id = d.buttons[i];
                const int alias = (usage == K_SPACE || usage == K_ENTER || usage == K_KP_ENTER) &&
                                  (id == B_ROLL || id == B_AGAIN);
                if (d.button_at[i].key == usage || alias) {
                        press(id);
                        return;
                }
        }
        return;
}

/* ---- The scene */

static void dice_scene_tick(void) {
        ++d.ticks;
        replay();
        dice_tick_motion();
        flyers_tick();
        party_tick();
        table_tick();
        for (int s = 0; s < DICE_SEATS; ++s)
                if (d.pose_ticks[s] > 0 && --d.pose_ticks[s] == 0) d.pose[s] = POSE_IDLE;
        if (d.lucky_mood > 0) --d.lucky_mood;
        if (d.hint_ticks > 0) --d.hint_ticks;
        if (d.pop_ticks > 0) --d.pop_ticks;
        if (d.clack_wait > 0) --d.clack_wait;
        return;
}

static void dice_scene_draw(bmds_canvas_t *c) {
        layout();
        stage_draw(&d.stage, c);
        return;
}

static void dice_scene_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        layout();
        stage_redraw(&d.stage, c, r);
        return;
}

static int dice_scene_enter(int argc, char **argv) {
        (void)argc;
        (void)argv;
        memset(&d, 0, sizeof(d));
        d.hover = d.hover_die = d.hover_box = -1;
        new_game();
        layout();
        memcpy(d.stage.shown, d.stage.now, sizeof(d.stage.now));
        d.stage.dirty_count = 0;
        return 0;
}

static void dice_scene_leave(void) {
        /* A game over but not yet shown is kept all the same. */
        if (d.table.phase == DICE_OVER) record_game();
        return;
}

const scene_t dice_scene = {
        "dice",          "Lucky Duck Dice", dice_scene_enter,  dice_scene_leave, dice_scene_event,
        dice_scene_tick, dice_scene_draw,   dice_scene_redraw, dice_scene_busy,
};
