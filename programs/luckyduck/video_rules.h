/**
 * @file      video_rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Video Poker (devplan/luckyduck.md §8d, §21): a machine dealt from a fresh shuffle
 * each hand, held and drawn, paid by its pay table; and perfect play (every hold's exact expected
 * return), worked out in slices for the hint. No drawing, no system calls: tested on the host.
 */

#ifndef LUCKYDUCK_VIDEO_RULES_H
#define LUCKYDUCK_VIDEO_RULES_H

#include <stdint.h>
#include "deck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VP_CARDS 5
#define VP_MAX_COINS 5
#define VP_HOLDS 32 /* every way to hold five cards */

/** @brief The machines (their pay tables). */
enum {
        VP_JACKS,         /* Jacks or Better 9/6 */
        VP_BONUS,         /* Bonus Poker 8/5 */
        VP_DOUBLE_DOUBLE, /* Double Double Bonus 9/6 */
        VP_MACHINES
};

/** @brief A machine's phases. */
enum {
        VP_BETTING, /* the coins set; vp_deal() deals */
        VP_DEALT,   /* holds; vp_draw() draws and pays */
        VP_DONE     /* paid: the coins may change, vp_deal() deals again */
};

/** @brief A machine. Read freely; changed by the calls. */
typedef struct {
                deck_rng_t rng;
                uint8_t    deck[DECK_CARDS];
                uint8_t    next; /* the next card's place in the deck */
                uint8_t    cards[VP_CARDS];
                uint8_t    held;  /* a bit a card */
                uint8_t    drawn; /* the cards the draw replaced, a bit each */
                uint8_t    phase;
                uint8_t    machine;
                uint8_t    coins; /* bet: 1 to VP_MAX_COINS */
                int32_t    coin;  /* a coin's worth, in dollars */
                int32_t    stack; /* the player's dollars not bet */
                int8_t     row;   /* the pay table's row the draw made, or -1 */
                int32_t    won;   /* dollars the draw paid (the bet included) */
} vp_t;

/** @brief Makes machine @p machine with coins worth @p coin dollars and the player's @p stack. */
void vp_init(vp_t *v, int machine, int32_t coin, int32_t stack);

/** @brief Changes the machine or the coin's worth (not between the deal and the draw). @return 0, or -1. */
int vp_configure(vp_t *v, int machine, int32_t coin);

/** @brief Sets the coins bet, 1 to VP_MAX_COINS (not between the deal and the draw). @return 0, or -1. */
int vp_bet(vp_t *v, int coins);

/** @brief Takes the bet from the stack and deals five from a deck shuffled from @p key. @return 0, or -1. */
int vp_deal(vp_t *v, deck_key_t key);

/** @brief Holds card @p i (or lets it go: @p held 0), between the deal and the draw. @return 0, or -1. */
int vp_hold(vp_t *v, int i, int held);

/** @brief Replaces the cards not held and pays what they make. @return 0, or -1. */
int vp_draw(vp_t *v);

/* ---- The pay tables */

/** @brief Machine @p machine's name ("JACKS OR BETTER"). */
const char *vp_machine_name(int machine);

/** @brief Its pay table's rows, best first. */
int vp_rows(int machine);

/** @brief Row @p row's name ("FULL HOUSE"). */
const char *vp_row_name(int machine, int row);

/** @brief What row @p row pays for @p coins coins, in coins (the bet included). */
int32_t vp_row_pay(int machine, int row, int coins);

/** @brief The row five @p cards make on @p machine, or -1. */
int vp_row(int machine, const uint8_t *cards);

/** @brief Which of five @p cards make row @p row (a bit each): a pair's two, a straight's five. */
uint8_t vp_scoring(int machine, const uint8_t *cards, int row);

/** @brief Whether row @p row is the royal flush. */
int vp_row_royal(int machine, int row);

/* ---- Perfect play: every hold's exact expected return */

/** @brief Perfect play's work: every hold against every draw from the 47 cards left. */
typedef struct {
                uint8_t machine, coins;
                uint8_t cards[VP_CARDS];
                uint8_t rest[DECK_CARDS - VP_CARDS];
                uint8_t hold; /* the hold being worked on */
                uint8_t draw; /* cards it draws */
                uint8_t idx[VP_CARDS];
                uint8_t started, done;
                /* For speed: each card left as its rank's bit and its suit's, the pays by what the
                   rank masks show, and the hold's and the draw's cards but its last as masks. */
                uint16_t rest_bit[DECK_CARDS - VP_CARDS];
                uint8_t  rest_suit[DECK_CARDS - VP_CARDS];
                uint16_t pay_made[9]; /* by hand.h's category (pairs and fours apart) */
                uint16_t pay_royal, pay_pair[13], pay_four[13][13];
                uint32_t part[6]; /* ranks seen once, twice, three, four times; the suits; how many ranks */
                uint8_t  part_ok;
                uint64_t paid[VP_HOLDS]; /* coins paid over all the hold's draws */
                uint32_t draws[VP_HOLDS];
} vp_ev_t;

/** @brief Starts working out @p cards on @p machine at @p coins coins. */
void vp_ev_start(vp_ev_t *e, int machine, int coins, const uint8_t *cards);

/** @brief Works through up to @p budget draws. @return 1 once every hold is done. */
int vp_ev_step(vp_ev_t *e, uint32_t budget);

/** @brief Hold @p hold's expected return in coins (its work done). */
double vp_ev_return(const vp_ev_t *e, int hold);

/** @brief The best hold (its work done): the most expected, the fewest cards drawn on a tie. */
int vp_ev_best(const vp_ev_t *e);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_VIDEO_RULES_H */
