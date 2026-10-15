/**
 * @file      rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Blocks' rules (devplan/games.md §17). Coordinates are rows down from the top (the first two
 * hidden) and columns from the left; a piece's cells are its box's, turned.
 */

#include <string.h>
#include "rules.h"

/* Each piece's cells unturned, and its box's size (the turns are about the box). */
static const int8_t SHAPES[BLOCKS_PIECES + 1][4][2] = {
        {{0, 0}, {0, 0}, {0, 0}, {0, 0}}, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, /* I */
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},                                   /* O */
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},                                   /* T */
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},                                   /* S */
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},                                   /* Z */
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},                                   /* J */
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},                                   /* L */
};
static const int8_t BOX[BLOCKS_PIECES + 1] = {0, 4, 4, 3, 3, 3, 3, 3};

/* SRS's wall kicks, [from][clockwise], rows down (the guideline's tables have them up). */
static const int8_t KICKS[4][2][5][2] = {
        {{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}, {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
        {{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}, {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}}},
        {{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}, {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}}},
        {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}, {{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
};
static const int8_t KICKS_I[4][2][5][2] = {
        {{{0, 0}, {-1, 0}, {2, 0}, {-1, -2}, {2, 1}}, {{0, 0}, {-2, 0}, {1, 0}, {-2, 1}, {1, -2}}},
        {{{0, 0}, {2, 0}, {-1, 0}, {2, -1}, {-1, 2}}, {{0, 0}, {-1, 0}, {2, 0}, {-1, -2}, {2, 1}}},
        {{{0, 0}, {1, 0}, {-2, 0}, {1, 2}, {-2, -1}}, {{0, 0}, {2, 0}, {-1, 0}, {2, -1}, {-1, 2}}},
        {{{0, 0}, {-2, 0}, {1, 0}, {-2, 1}, {1, -2}}, {{0, 0}, {1, 0}, {-2, 0}, {1, 2}, {-2, -1}}},
};

/* Microseconds a row falls at levels 1-20: the guideline's (0.8 - (n - 1) * 0.007) ^ (n - 1) seconds
   a level on (n = level + 1), so the first falls a row in 0.79 s rather than a whole second. */
static const uint32_t ROW_US[20] = {793000, 617796, 472729, 355201, 262003, 189677, 134734, 93882, 64138, 42950,
                                    28183,  18107,  11390,  7012,   4222,   2485,   1431,   805,   443,   239};

static const uint32_t CLEAR_POINTS[5] = {0, 100, 300, 500, 800};

uint32_t blocks_gravity(uint32_t level) {
        const uint32_t us = ROW_US[(level < 1 ? 1 : level > 20 ? 20 : level) - 1];
        const uint64_t ticks = (uint64_t)us * BLOCKS_TICK_HZ;         /* millionths of a tick a row */
        return (uint32_t)((65536ull * 1000000u + ticks - 1) / ticks); /* up: level 1 a row in 60 ticks */
}

void blocks_cells(const blocks_piece_t *p, int8_t cells[4][2]) {
        const int8_t n = BOX[p->type];
        for (int i = 0; i < 4; ++i) {
                int8_t x = SHAPES[p->type][i][0], y = SHAPES[p->type][i][1];
                if (p->type != BLOCKS_O)
                        for (uint8_t r = 0; r < p->rotation; ++r) {
                                const int8_t t = x;
                                x = (int8_t)(n - 1 - y); /* a quarter turn clockwise */
                                y = t;
                        }
                cells[i][0] = (int8_t)(p->x + x);
                cells[i][1] = (int8_t)(p->y + y);
        }
        return;
}

int blocks_fits(const blocks_t *b, const blocks_piece_t *p) {
        int8_t c[4][2];
        blocks_cells(p, c);
        for (int i = 0; i < 4; ++i) {
                if (c[i][0] < 0 || c[i][0] >= BLOCKS_WIDTH || c[i][1] < 0 || c[i][1] >= BLOCKS_HEIGHT) return 0;
                if (b->field[c[i][1]][c[i][0]] != BLOCKS_NONE) return 0;
        }
        return 1;
}

int32_t blocks_drop_distance(const blocks_t *b) {
        blocks_piece_t p = b->piece;
        int32_t        d = 0;
        for (;;) {
                ++p.y;
                if (!blocks_fits(b, &p)) return d;
                ++d;
        }
}

static uint32_t next_random(blocks_t *b) {
        uint32_t x = b->random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b->random = x;
        return x;
}

/* The bag's next piece, a new shuffled bag when it is empty. */
static uint8_t from_bag(blocks_t *b) {
        if (b->bag_left == 0) {
                for (uint8_t i = 0; i < BLOCKS_PIECES; ++i) b->bag[i] = (uint8_t)(i + 1);
                for (uint8_t i = BLOCKS_PIECES - 1; i > 0; --i) {
                        const uint8_t j = (uint8_t)(next_random(b) % (i + 1u)), t = b->bag[i];
                        b->bag[i] = b->bag[j];
                        b->bag[j] = t;
                }
                b->bag_left = BLOCKS_PIECES;
        }
        return b->bag[--b->bag_left];
}

/* @p type at the top (a row down at once, as the guideline's); over if it can't be. */
static void spawn(blocks_t *b, uint8_t type) {
        const blocks_piece_t p = {type, 0, 3, 0};
        b->piece = p;
        b->fall = 0;
        b->lock_ticks = 0;
        b->lock_resets = 0;
        if (!blocks_fits(b, &b->piece)) {
                b->over = 1;
                b->events |= BLOCKS_OVER;
                return;
        }
        ++b->piece.y;
        if (!blocks_fits(b, &b->piece)) --b->piece.y;
        b->lowest = b->piece.y;
        return;
}

static void spawn_next(blocks_t *b) {
        const uint8_t type = b->next[0];
        memmove(b->next, b->next + 1, BLOCKS_NEXT - 1);
        b->next[BLOCKS_NEXT - 1] = from_bag(b);
        spawn(b, type);
        return;
}

void blocks_start(blocks_t *b, uint32_t level, uint32_t seed) {
        memset(b, 0, sizeof(*b));
        b->random = seed != 0 ? seed : 1;
        b->start_level = b->level = level < 1 ? 1 : level;
        b->gravity = blocks_gravity(b->level);
        b->can_hold = 1;
        for (int i = 0; i < BLOCKS_NEXT; ++i) b->next[i] = from_bag(b);
        spawn_next(b);
        return;
}

/* On the ground, a move or turn puts the lock off (so many times a piece, more on a new low). */
static void moved(blocks_t *b, uint32_t event) {
        b->events |= event;
        blocks_piece_t below = b->piece;
        ++below.y;
        if (!blocks_fits(b, &below) && b->lock_resets < BLOCKS_LOCK_RESETS) {
                b->lock_ticks = 0;
                ++b->lock_resets;
        }
        return;
}

static int shift(blocks_t *b, int8_t dx) {
        blocks_piece_t p = b->piece;
        p.x = (int8_t)(p.x + dx);
        if (!blocks_fits(b, &p)) return 0;
        b->piece = p;
        moved(b, BLOCKS_MOVED);
        return 1;
}

static void turn(blocks_t *b, int clockwise) {
        if (b->piece.type == BLOCKS_O) return;
        const uint8_t from = b->piece.rotation;
        const int8_t(*kicks)[2] = b->piece.type == BLOCKS_I ? KICKS_I[from][clockwise] : KICKS[from][clockwise];
        for (int i = 0; i < 5; ++i) {
                blocks_piece_t p = b->piece;
                p.rotation = (uint8_t)((from + (clockwise ? 1 : 3)) & 3);
                p.x = (int8_t)(p.x + kicks[i][0]);
                p.y = (int8_t)(p.y + kicks[i][1]);
                if (blocks_fits(b, &p)) {
                        b->piece = p;
                        moved(b, BLOCKS_TURNED);
                        return;
                }
        }
        return;
}

static void lock(blocks_t *b) {
        int8_t c[4][2];
        blocks_cells(&b->piece, c);
        int above = 1;
        for (int i = 0; i < 4; ++i) {
                b->field[c[i][1]][c[i][0]] = b->piece.type;
                if (c[i][1] >= BLOCKS_HIDDEN) above = 0;
        }
        b->events |= BLOCKS_LOCKED;
        b->can_hold = 1;
        if (above) { /* locked out: wholly above the field */
                b->over = 1;
                b->events |= BLOCKS_OVER;
                return;
        }
        b->clearing_count = 0;
        for (int y = 0; y < BLOCKS_HEIGHT; ++y) {
                int full = 1;
                for (int x = 0; x < BLOCKS_WIDTH && full; ++x) full = b->field[y][x] != BLOCKS_NONE;
                if (full) b->clearing[b->clearing_count++] = (uint8_t)y;
        }
        if (b->clearing_count == 0) {
                spawn_next(b);
                return;
        }
        b->events |= BLOCKS_CLEARED;
        b->clear_ticks = BLOCKS_CLEAR_TICKS;
        b->score += CLEAR_POINTS[b->clearing_count] * b->level;
        b->lines += b->clearing_count;
        const uint32_t level = b->start_level + b->lines / 10;
        if (level > b->level) {
                b->level = level;
                b->gravity = blocks_gravity(level);
                b->events |= BLOCKS_LEVELLED;
        }
        return;
}

int blocks_flashing(const blocks_t *b) {
        return b->clear_ticks > 0 && (BLOCKS_CLEAR_TICKS - b->clear_ticks) / BLOCKS_BLINK_TICKS % 2 == 0;
}

/* A clear's tick: the rows blink; then, emptied, the rows above drop onto them a row at a time
   (the lowest first), then the next piece comes. */
static void clearing_tick(blocks_t *b) {
        if (b->clear_ticks > 0) {
                if (--b->clear_ticks == 0) {
                        for (uint32_t i = 0; i < b->clearing_count; ++i)
                                memset(b->field[b->clearing[i]], BLOCKS_NONE, BLOCKS_WIDTH);
                        b->settle_ticks = BLOCKS_SETTLE_TICKS;
                }
                return;
        }
        if (--b->settle_ticks > 0) return;
        /* The lowest left (they are kept top to bottom): what is above it drops a row. */
        const int row = b->clearing[--b->clearing_count];
        memmove(b->field[1], b->field[0], (size_t)row * BLOCKS_WIDTH);
        memset(b->field[0], BLOCKS_NONE, BLOCKS_WIDTH);
        for (uint32_t i = 0; i < b->clearing_count; ++i) ++b->clearing[i]; /* above it: down a row too */
        b->events |= BLOCKS_SETTLED;
        b->settle_ticks = BLOCKS_SETTLE_TICKS;
        if (b->clearing_count == 0) spawn_next(b);
        return;
}

void blocks_tick(blocks_t *b, const blocks_input_t *in) {
        b->events = 0;
        if (b->over) return;
        ++b->ticks;
        if (b->clearing_count > 0) {
                clearing_tick(b);
                return;
        }
        if ((in->pressed & BLOCKS_HOLD) != 0 && b->can_hold) {
                const uint8_t held = b->hold;
                b->hold = b->piece.type;
                b->can_hold = 0;
                b->events |= BLOCKS_HELD;
                if (held != BLOCKS_NONE)
                        spawn(b, held);
                else
                        spawn_next(b);
                if (b->over) return;
        }
        if ((in->pressed & BLOCKS_CW) != 0) turn(b, 1);
        if ((in->pressed & BLOCKS_CCW) != 0) turn(b, 0);
        /* Sideways: at once, then auto-repeat while held (the last pressed of the two). */
        if ((in->pressed & (BLOCKS_LEFT | BLOCKS_RIGHT)) != 0) {
                b->repeat_dir = (in->pressed & BLOCKS_RIGHT) != 0 ? BLOCKS_RIGHT : BLOCKS_LEFT;
                b->repeat_ticks = 0;
                (void)shift(b, b->repeat_dir == BLOCKS_LEFT ? -1 : 1);
        } else if (b->repeat_dir != 0 && (in->held & b->repeat_dir) != 0) {
                ++b->repeat_ticks;
                if (b->repeat_ticks >= BLOCKS_DAS_TICKS && (b->repeat_ticks - BLOCKS_DAS_TICKS) % BLOCKS_ARR_TICKS == 0)
                        (void)shift(b, b->repeat_dir == BLOCKS_LEFT ? -1 : 1);
        } else {
                /* Let go: the other one, if held, repeats from its start. */
                const uint32_t other = b->repeat_dir == BLOCKS_LEFT ? BLOCKS_RIGHT : BLOCKS_LEFT;
                b->repeat_dir = b->repeat_dir != 0 && (in->held & other) != 0 ? other : 0;
                b->repeat_ticks = 0;
        }
        if ((in->pressed & BLOCKS_HARD) != 0) {
                const int32_t d = blocks_drop_distance(b);
                b->piece.y = (int8_t)(b->piece.y + d);
                b->score += 2u * (uint32_t)d;
                b->events |= BLOCKS_DROPPED;
                lock(b);
                return;
        }
        /* Gravity (soft drop: 20 times, a point a row). */
        const int      soft = (in->held & BLOCKS_SOFT) != 0;
        uint32_t       g = b->gravity;
        const uint32_t g20 = g * 20 > 65536 ? 65536 : g * 20;
        if (soft && g20 > g) g = g20;
        b->fall += g;
        while (b->fall >= 65536) {
                b->fall -= 65536;
                blocks_piece_t p = b->piece;
                ++p.y;
                if (!blocks_fits(b, &p)) {
                        b->fall = 0;
                        break;
                }
                b->piece = p;
                if (soft) ++b->score;
        }
        if (b->piece.y > b->lowest) { /* a new low: the resets again */
                b->lowest = b->piece.y;
                b->lock_resets = 0;
        }
        blocks_piece_t below = b->piece;
        ++below.y;
        if (blocks_fits(b, &below)) {
                b->lock_ticks = 0;
        } else if (++b->lock_ticks >= BLOCKS_LOCK_TICKS) {
                lock(b);
        }
        return;
}
