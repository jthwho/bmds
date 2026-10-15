/**
 * @file      rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Blocks' rules (devplan/games.md §17): the guideline's field, pieces, rotation, gravity, locking,
 * clears and scoring, a tick at a time from that tick's input. No drawing, no system calls: tested
 * on the host.
 */

#ifndef BLOCKS_RULES_H
#define BLOCKS_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLOCKS_WIDTH 10
#define BLOCKS_HEIGHT 22 /* the top two hidden */
#define BLOCKS_HIDDEN 2
#define BLOCKS_NEXT 5
#define BLOCKS_TICK_HZ 60

/** @brief The pieces (a cell's value: 0 empty). */
enum {
        BLOCKS_NONE,
        BLOCKS_I,
        BLOCKS_O,
        BLOCKS_T,
        BLOCKS_S,
        BLOCKS_Z,
        BLOCKS_J,
        BLOCKS_L,
        BLOCKS_PIECES = 7
};

/** @brief Actions: `blocks_input_t`'s bits. */
enum {
        BLOCKS_LEFT = 1u << 0,
        BLOCKS_RIGHT = 1u << 1,
        BLOCKS_SOFT = 1u << 2,
        BLOCKS_HARD = 1u << 3,
        BLOCKS_CW = 1u << 4,
        BLOCKS_CCW = 1u << 5,
        BLOCKS_HOLD = 1u << 6
};

/** @brief What a tick did: `blocks_t::events`' bits (for the sounds). */
enum {
        BLOCKS_MOVED = 1u << 0,
        BLOCKS_TURNED = 1u << 1,
        BLOCKS_LOCKED = 1u << 2,
        BLOCKS_DROPPED = 1u << 3, /* hard */
        BLOCKS_HELD = 1u << 4,
        BLOCKS_CLEARED = 1u << 5, /* `clearing` rows */
        BLOCKS_LEVELLED = 1u << 6,
        BLOCKS_OVER = 1u << 7,
        BLOCKS_SETTLED = 1u << 8 /* the rows above a cleared one dropped onto it */
};

/** @brief A tick's input: the actions held, and those pressed since the last tick. */
typedef struct blocks_input_t {
                uint32_t held;
                uint32_t pressed;
} blocks_input_t;

/** @brief A piece in play. */
typedef struct blocks_piece_t {
                uint8_t type;     /* BLOCKS_I... */
                uint8_t rotation; /* 0-3, clockwise */
                int8_t  x, y;     /* its box's top left */
} blocks_piece_t;

/** @brief A game. Read freely; changed by the calls. */
typedef struct blocks_t {
                uint8_t        field[BLOCKS_HEIGHT][BLOCKS_WIDTH];
                blocks_piece_t piece;
                uint8_t        next[BLOCKS_NEXT];
                uint8_t        hold; /* 0: none */
                uint8_t        can_hold;
                uint8_t        bag[BLOCKS_PIECES];
                uint8_t        bag_left;
                uint32_t       random;
                uint32_t       score, lines, level, start_level;
                uint32_t       gravity; /* rows a tick, in 1/65536ths */
                uint32_t       fall;    /* the gravity collected */
                uint32_t       lock_ticks, lock_resets;
                int8_t         lowest; /* the lowest row the piece's box has reached */
                uint32_t       repeat_ticks;
                uint32_t       repeat_dir;     /* BLOCKS_LEFT or BLOCKS_RIGHT, the last pressed */
                uint8_t        clearing[4];    /* full rows: blinking, then gone a row at a time */
                uint32_t       clearing_count; /* those not gone yet (0: none being cleared) */
                uint32_t       clear_ticks;    /* the blinking's ticks left (0: dropping) */
                uint32_t       settle_ticks;   /* ticks to the next row dropped */
                uint32_t       events;         /* the last tick's */
                uint32_t       ticks;
                int            over;
} blocks_t;

#define BLOCKS_BLINK_TICKS 6 /* a full row's flash, then as many unlit: three times */
#define BLOCKS_CLEAR_TICKS (6 * BLOCKS_BLINK_TICKS)
#define BLOCKS_SETTLE_TICKS 5 /* between the rows above dropping a row each */
#define BLOCKS_LOCK_TICKS 30
#define BLOCKS_LOCK_RESETS 15
#define BLOCKS_DAS_TICKS 10
#define BLOCKS_ARR_TICKS 2

/** @brief Starts a game at @p level (1 or more) with @p seed (not 0) for the bag. */
void blocks_start(blocks_t *b, uint32_t level, uint32_t seed);

/** @brief Runs a tick with @p in. Over: nothing. */
void blocks_tick(blocks_t *b, const blocks_input_t *in);

/** @brief Fills @p cells (4 × [x, y]) with @p p's cells in the field. */
void blocks_cells(const blocks_piece_t *p, int8_t cells[4][2]);

/** @brief Returns true if @p p fits (in the field, on nothing). */
int blocks_fits(const blocks_t *b, const blocks_piece_t *p);

/** @brief Returns the rows the piece would fall to the floor (the ghost's place). */
int32_t blocks_drop_distance(const blocks_t *b);

/** @brief Returns true while the full rows show lit (they blink before they go). */
int blocks_flashing(const blocks_t *b);

/** @brief Rows a tick at @p level, in 1/65536ths. */
uint32_t blocks_gravity(uint32_t level);

#ifdef __cplusplus
}
#endif

#endif /* BLOCKS_RULES_H */
