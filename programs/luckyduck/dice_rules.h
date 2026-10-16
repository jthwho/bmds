/**
 * @file      dice_rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck Dice's rules (devplan/luckyduck.md §8c, §18): Yahtzee's 13 boxes for up to four
 * seats, run from actions and a seed, every change logged for the picture (and one day a
 * network). The bots' choices by expected value. No drawing, no system calls: tested on the host.
 */

#ifndef LUCKYDUCK_DICE_RULES_H
#define LUCKYDUCK_DICE_RULES_H

#include <stdint.h>
#include "deck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DICE_SEATS 4
#define DICE_DICE 5
#define DICE_ROLLS 3
#define DICE_ROUNDS 13
#define DICE_EVENTS 256 /* a ring: the reader keeps up */
#define DICE_OPEN (-1)  /* a box not written */

#define DICE_UPPER_BONUS 35
#define DICE_UPPER_TARGET 63
#define DICE_LUCKY_BONUS 100

/** @brief The boxes, in the card's order. */
enum {
        DICE_ONES,
        DICE_TWOS,
        DICE_THREES,
        DICE_FOURS,
        DICE_FIVES,
        DICE_SIXES,
        DICE_THREE_KIND,
        DICE_FOUR_KIND,
        DICE_FULL_HOUSE,
        DICE_SMALL_STRAIGHT,
        DICE_LARGE_STRAIGHT,
        DICE_LUCKY_DUCK, /* five of a kind */
        DICE_CHANCE,
        DICE_BOXES
};

/** @brief The phases of a game. */
enum {
        DICE_PLAYING, /* turn_seat's turn: roll (rolls so far) */
        DICE_OVER     /* every box written */
};

/** @brief Events (`dice_event_t::type`). */
enum {
        DICE_EV_START, /* amount: the seats playing */
        DICE_EV_TURN,  /* seat, round (from 0) */
        DICE_EV_HOLD,  /* seat, die, held */
        DICE_EV_ROLL,  /* seat, roll (1 to 3), dice, held (a mask) */
        DICE_EV_SCORE, /* seat, box, amount: its points */
        DICE_EV_BONUS, /* seat, box (DICE_ONES: the upper bonus; DICE_LUCKY_DUCK: a second Lucky Duck), amount */
        DICE_EV_END    /* seat: the winner (the first of a tie), amount: its total */
};

/** @brief An event. */
typedef struct {
                uint8_t type;
                int8_t  seat;
                uint8_t die;  /* HOLD; SCORE and BONUS: the box */
                uint8_t held; /* HOLD: held; ROLL: a mask of the dice held */
                uint8_t roll; /* ROLL; TURN: the round */
                uint8_t dice[DICE_DICE];
                int32_t amount;
} dice_event_t;

/** @brief A seat's card. */
typedef struct {
                uint8_t playing;
                int16_t box[DICE_BOXES]; /* DICE_OPEN, or the points written */
                int16_t lucky_bonus;     /* the 100s for second Lucky Ducks */
} dice_seat_t;

/** @brief A table. Read freely; changed by the calls. */
typedef struct {
                deck_rng_t   rng;
                dice_seat_t  seat[DICE_SEATS];
                uint8_t      phase;
                int8_t       turn_seat;
                uint8_t      round; /* from 0 */
                uint8_t      roll;  /* rolls this turn */
                uint8_t      dice[DICE_DICE];
                uint8_t      held[DICE_DICE];
                dice_event_t events[DICE_EVENTS];
                uint32_t     event_count; /* since the start: event i is at i % DICE_EVENTS */
} dice_table_t;

/**
 * @brief Starts a game for the seats @p playing (a mask, seat 0 first to play), its rolls from
 *        @p key. Logs the start and the first turn.
 */
void dice_init(dice_table_t *t, uint32_t playing, deck_key_t key);

/** @brief Rolls the dice @p seat doesn't hold (its turn, rolls left). @return 0, or -1. */
int dice_roll(dice_table_t *t, int seat);

/** @brief Holds die @p die (or frees it: @p held 0); after the turn's first roll. @return 0, or -1. */
int dice_hold(dice_table_t *t, int seat, int die, int held);

/** @brief Whether @p seat may write the dice into @p box now (its turn, rolled, the joker rule). */
int dice_can_score(const dice_table_t *t, int seat, int box);

/**
 * @brief Writes the dice into @p box for @p seat (dice_can_score()), with any bonus, and moves
 *        the turn on (or ends the game). @return 0, or -1.
 */
int dice_score(dice_table_t *t, int seat, int box);

/** @brief What the dice would score in @p box for @p seat now (the joker rule applied; 0 if not allowed). */
int dice_points(const dice_table_t *t, int seat, int box);

/** @brief What @p dice score in @p box (@p joker: a second Lucky Duck playing as a joker). */
int dice_box_score(const uint8_t *dice, int box, int joker);

/** @brief A seat's upper boxes' sum, and its total (the bonuses in). */
int dice_upper(const dice_seat_t *s);
int dice_total(const dice_seat_t *s);

/** @brief Event @p i (of those since the start, while still in the ring). */
const dice_event_t *dice_event(const dice_table_t *t, uint32_t i);

/** @brief The bot's choice for @p seat's turn now. */
typedef struct {
                int8_t  box;  /* write this box (DICE_BOXES: roll again) */
                uint8_t keep; /* rolling again: a mask of the dice to hold */
} dice_choice_t;

/**
 * @brief What a bot would do now (its turn, rolled at least once), looking @p ahead rolls ahead
 *        at most (1 or 2: 2 plays better).
 */
dice_choice_t dice_bot(const dice_table_t *t, int seat, int ahead);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_DICE_RULES_H */
