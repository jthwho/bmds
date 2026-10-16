/**
 * @file      deck.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's cards and randomness (devplan/luckyduck.md §16): a card is a byte (suit × 13 +
 * rank, rank 0 the Ace), shoes are shuffled with xoshiro128** from a seed, so a game is the same
 * from the same seed and actions. No drawing, no system calls: tested on the host.
 */

#ifndef LUCKYDUCK_DECK_H
#define LUCKYDUCK_DECK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_CARDS 52
#define DECK_RANKS 13

/** @brief The random numbers' state (xoshiro128**). */
typedef struct {
                uint32_t s[4];
} deck_rng_t;

/** @brief Seeds @p rng from @p seed (any value, 0 too). */
void deck_seed(deck_rng_t *rng, uint32_t seed);

/** @brief The next 32 random bits. */
uint32_t deck_next(deck_rng_t *rng);

/** @brief A number from 0 to @p n - 1 (unbiased). */
uint32_t deck_below(deck_rng_t *rng, uint32_t n);

/** @brief Fills @p cards with @p decks decks in order (52 × @p decks bytes). */
void deck_fill(uint8_t *cards, uint32_t decks);

/** @brief Shuffles @p count cards (Fisher-Yates). */
void deck_shuffle(deck_rng_t *rng, uint8_t *cards, uint32_t count);

/** @brief Returns @p card's rank (0 the Ace, 12 the King) and suit (clubs, diamonds, hearts, spades). */
static inline uint32_t deck_rank(uint8_t card) {
        return (uint32_t)card % DECK_RANKS;
}
static inline uint32_t deck_suit(uint8_t card) {
        return (uint32_t)card / DECK_RANKS;
}

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_DECK_H */
