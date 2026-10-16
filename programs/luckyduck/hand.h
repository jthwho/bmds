/**
 * @file      hand.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's poker hands (devplan/luckyduck.md §8e, §21): the best five of 5 to 7 cards as one
 * number, bigger the better, shared by Video Poker and Poker. No drawing, no system calls: tested
 * on the host.
 */

#ifndef LUCKYDUCK_HAND_H
#define LUCKYDUCK_HAND_H

#include <stdint.h>
#include "deck.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A hand's category (hand_category()), worst first. */
enum {
        HAND_HIGH_CARD,
        HAND_PAIR,
        HAND_TWO_PAIR,
        HAND_THREE,
        HAND_STRAIGHT,
        HAND_FLUSH,
        HAND_FULL_HOUSE,
        HAND_FOUR,
        HAND_STRAIGHT_FLUSH,
        HAND_CATEGORIES
};

/** @brief A rank in a hand's value: 0 the deuce up to 12 the Ace (Ace high). */
#define HAND_ACE 12

/**
 * @brief The best five of @p n cards (5 to 7, deck.h's bytes) as one value: its category in bits
 *        20 and up, then five ranks of 4 bits in the order they count (a four's rank, then its
 *        kicker; a full house's three, then its pair; a pair's, then the kickers high to low; a
 *        straight's top card, the wheel's the Five). A bigger value is a better hand; equal is a tie.
 */
uint32_t hand_value(const uint8_t *cards, int n);

/** @brief A value's category (`HAND_...`). */
static inline int hand_category(uint32_t value) {
        return (int)(value >> 20);
}

/** @brief A value's @p i-th rank (0 the one that counts most), 0 the deuce to `HAND_ACE`. */
static inline int hand_rank(uint32_t value, int i) {
        return (int)((value >> (16 - 4 * i)) & 0xfu);
}

/** @brief A card's rank as a hand counts it: 0 the deuce to `HAND_ACE`. */
static inline int hand_card_rank(uint8_t card) {
        const int r = (int)deck_rank(card);
        return r == 0 ? HAND_ACE : r - 1;
}

/** @brief A category's name, as a headline shows it ("FULL HOUSE"). */
const char *hand_name(int category);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_HAND_H */
