/**
 * @file      poker_bot.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's poker ducks (devplan/luckyduck.md §25): each decides from its seat's view alone. Its
 * chance of winning is estimated by dealing the cards it can't see at random (stepped, so the work
 * spreads over frames), then weighed against the pot odds with the duck's personality. No drawing,
 * no system calls: tested on the host.
 */

#ifndef LUCKYDUCK_POKER_BOT_H
#define LUCKYDUCK_POKER_BOT_H

#include <stdint.h>
#include "poker_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A duck's personality: each 0 to 100. */
typedef struct {
                uint8_t loose; /* how many hands it plays */
                uint8_t aggro; /* how often it bets and raises rather than calls */
                uint8_t bluff; /* how often it bets without the cards */
} pk_style_t;

/** @brief An equity estimate under way. */
typedef struct {
                deck_rng_t rng;
                uint8_t    game;
                uint8_t    draw_first; /* Draw before the draw: both sides' draws played out */
                uint8_t    mine[PK_CARDS], mine_count, mine_final;
                uint8_t    board[PK_BOARD], board_count;
                uint8_t    opp_cards[PK_SEATS][PK_CARDS]; /* each opponent's known (up) cards */
                uint8_t    opp_known[PK_SEATS], opps;
                uint8_t    unseen[DECK_CARDS], unseen_count;
                uint32_t   trials, target;
                float      wins; /* ties count their share */
} pk_equity_t;

/** @brief Starts estimating @p v's seat's chance of winning over @p trials random deals. */
void pk_equity_start(pk_equity_t *e, const pk_view_t *v, deck_key_t key, uint32_t trials);

/** @brief Plays up to @p count more deals. @return 1 when all are done. */
int pk_equity_step(pk_equity_t *e, uint32_t count);

/** @brief The chance of winning so far (0 to 1). */
float pk_equity_value(const pk_equity_t *e);

/** @brief What a duck does (pk_act()'s action and amount). */
typedef struct {
                uint8_t action;
                int32_t amount;
} pk_choice_t;

/**
 * @brief A duck's action for @p v (its turn to bet) with @p equity, its personality @p style,
 *        @p random 32 random bits. Always legal at that view.
 */
pk_choice_t pk_bot_decide(const pk_view_t *v, const pk_style_t *style, float equity, uint32_t random);

/** @brief Draw: which of five cards to throw (a mask): made hands kept, then draws, then high cards. */
uint32_t pk_bot_discard(const uint8_t *cards);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_POKER_BOT_H */
