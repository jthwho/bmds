/**
 * @file      blackjack_rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Blackjack rules (devplan/luckyduck.md §8b, §16): a table of seats run from
 * actions and a seed, its events logged for the picture (and one day a network), each event
 * readable as a seat may see it. Basic strategy for the bots and the hint. No drawing, no system
 * calls: tested on the host.
 */

#ifndef LUCKYDUCK_BLACKJACK_RULES_H
#define LUCKYDUCK_BLACKJACK_RULES_H

#include <stdint.h>
#include "deck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BJ_SEATS 5
#define BJ_HANDS 4  /* a seat's, with splits */
#define BJ_CARDS 22 /* a hand's at most: 21 Aces are still under 22 */
#define BJ_DECKS 6
#define BJ_SHOE (DECK_CARDS * BJ_DECKS)
#define BJ_EVENTS 256   /* a round's */
#define BJ_DEALER (-1)  /* the dealer, where a seat is asked for */
#define BJ_HIDDEN 0xffu /* a card a seat may not see */

/** @brief The phases of a round. */
enum {
        BJ_BETTING,   /* bets go down; bj_deal() starts the round */
        BJ_INSURANCE, /* the dealer shows an Ace: each seat insures or not */
        BJ_PLAYING,   /* the seat and hand to act: turn_seat, turn_hand */
        BJ_SETTLED    /* the round is over: bets for the next */
};

/** @brief A player's actions. */
enum {
        BJ_HIT,
        BJ_STAND,
        BJ_DOUBLE,
        BJ_SPLIT,
        BJ_SURRENDER,
        BJ_ACTIONS
};

/** @brief A hand's result. */
enum {
        BJ_NONE,
        BJ_WIN,
        BJ_LOSE,
        BJ_PUSH,
        BJ_BLACKJACK_WIN, /* paid 3:2 */
        BJ_SURRENDERED
};

/** @brief Events (`bj_event_t::type`). */
enum {
        BJ_EV_SHUFFLE,   /* the shoe shuffled */
        BJ_EV_BET,       /* seat, amount: a bet placed (hand 0) */
        BJ_EV_DEAL,      /* seat (or BJ_DEALER), hand, card, up */
        BJ_EV_HOLE,      /* the dealer's hole card turned: card */
        BJ_EV_INSURANCE, /* seat, amount (0: declined) */
        BJ_EV_INSURED,   /* seat, amount won (or lost, negative) on insurance */
        BJ_EV_TURN,      /* seat, hand: to act */
        BJ_EV_SPLIT,     /* seat, hand: split into a new hand (amount its index) */
        BJ_EV_DOUBLE,    /* seat, hand, amount: the bet added */
        BJ_EV_SURRENDER, /* seat, hand */
        BJ_EV_BUST,      /* seat (or BJ_DEALER), hand */
        BJ_EV_BLACKJACK, /* seat (or BJ_DEALER), hand */
        BJ_EV_RESULT,    /* seat, hand, result (BJ_WIN...), amount: what comes back (bet included) */
        BJ_EV_ROUND_END
};

/** @brief An event of a round. */
typedef struct {
                uint8_t type;
                int8_t  seat;
                uint8_t hand;
                uint8_t card; /* DEAL, HOLE */
                uint8_t up;   /* DEAL: face up */
                uint8_t result;
                int32_t amount;
} bj_event_t;

/** @brief The table's rules. */
typedef struct {
                int32_t min_bet, max_bet;
                uint8_t hit_soft_17; /* the dealer hits a soft 17 */
                uint8_t surrender;   /* late surrender allowed */
} bj_rules_t;

/** @brief A hand. */
typedef struct {
                uint8_t cards[BJ_CARDS];
                uint8_t count;
                int32_t bet;
                uint8_t doubled, split, split_aces, done, result;
} bj_hand_t;

/** @brief A seat. */
typedef struct {
                uint8_t   playing; /* sits at the table */
                int32_t   stack;   /* chips not on the table */
                bj_hand_t hand[BJ_HANDS];
                uint8_t   hands; /* in play this round */
                int32_t   insurance;
                uint8_t   insured; /* decided this round */
} bj_seat_t;

/** @brief A table. Read freely; changed by the calls. */
typedef struct {
                bj_rules_t rules;
                deck_rng_t rng;
                uint8_t    shoe[BJ_SHOE];
                uint16_t   next, cut; /* the next card's place; the cut card's */
                uint8_t    reshuffle; /* the cut card came out: shuffle before the next round */
                bj_seat_t  seat[BJ_SEATS];
                bj_hand_t  dealer;
                uint8_t    hole_up; /* the hole card (the dealer's second) shown */
                uint8_t    phase;
                int8_t     turn_seat;
                uint8_t    turn_hand;
                bj_event_t events[BJ_EVENTS];
                uint32_t   event_count; /* this round's */
} bj_table_t;

/** @brief Default rules: $5 to $500, the dealer stands on soft 17, surrender allowed. */
bj_rules_t bj_default_rules(void);

/** @brief Makes a table with @p rules, the shoe shuffled from @p key, no one sitting. */
void bj_init(bj_table_t *t, const bj_rules_t *rules, deck_key_t key);

/** @brief Sits a player at @p seat with @p stack chips (or stands one up: @p playing 0). */
void bj_sit(bj_table_t *t, int seat, int playing, int32_t stack);

/**
 * @brief Places a bet of @p amount at @p seat (betting; within the stack; 0 takes it back). The
 *        table's limits are checked when the round is dealt, so a bet can be built up chip by chip.
 * @return 0, or -1.
 */
int bj_bet(bj_table_t *t, int seat, int32_t amount);

/** @brief Whether @p seat's bet is within the table's limits (or none). */
int bj_bet_ok(const bj_table_t *t, int seat);

/** @brief Deals the round (some seat has bet, every bet within the limits). @return 0, or -1. */
int bj_deal(bj_table_t *t);

/** @brief The seat's answer to insurance (taking it costs half the bet). @return 0, or -1. */
int bj_insure(bj_table_t *t, int seat, int take);

/** @brief Whether @p seat may take @p action now (its turn, on its current hand). */
int bj_can(const bj_table_t *t, int seat, int action);

/** @brief Takes @p action for @p seat (its turn). @return 0, or -1 (not allowed). */
int bj_act(bj_table_t *t, int seat, int action);

/** @brief Starts the next round's betting (settled). Bets placed last round are cleared. */
void bj_next_round(bj_table_t *t);

/** @brief The seat to answer insurance next (insurance phase), or -1. */
int bj_insurance_seat(const bj_table_t *t);

/** @brief A hand's total, and whether it is soft (an Ace counted 11). Hidden cards don't count. */
int bj_total(const uint8_t *cards, int count, int *soft);

/** @brief Whether a hand is a blackjack (two cards, 21, not from a split). */
int bj_is_blackjack(const bj_hand_t *h);

/** @brief Event @p i as @p seat may see it (the hole card hidden until turned; BJ_DEALER sees all). */
bj_event_t bj_event_for(const bj_table_t *t, uint32_t i, int seat);

/**
 * @brief Basic strategy's action for @p cards against the dealer's @p up card: what may be done
 *        (@p can_double, @p can_split, @p can_surrender) and the rules (@p hit_soft_17).
 */
int bj_basic(const uint8_t *cards, int count, uint8_t up, int can_double, int can_split, int can_surrender,
             int hit_soft_17);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_BLACKJACK_RULES_H */
