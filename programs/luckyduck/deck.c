/**
 * @file      deck.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's cards and randomness (devplan/luckyduck.md §16).
 */

#include "deck.h"

static uint32_t rotl(uint32_t x, int k) {
        return (x << k) | (x >> (32 - k));
}

/* splitmix32: a seed's bits spread into the state (never all zero). */
static uint32_t splitmix(uint32_t *x) {
        uint32_t z = (*x += 0x9e3779b9u);
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return z ^ (z >> 16);
}

void deck_seed(deck_rng_t *rng, uint32_t seed) {
        for (int i = 0; i < 4; ++i) rng->s[i] = splitmix(&seed);
        if ((rng->s[0] | rng->s[1] | rng->s[2] | rng->s[3]) == 0) rng->s[0] = 1;
        return;
}

uint32_t deck_next(deck_rng_t *rng) {
        uint32_t      *s = rng->s;
        const uint32_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 9;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 11);
        return r;
}

uint32_t deck_below(deck_rng_t *rng, uint32_t n) {
        if (n <= 1) return 0;
        /* Rejection: the values past the last whole multiple of n would favour the low ones. */
        const uint32_t limit = (uint32_t)(0x100000000ull - 0x100000000ull % n) - 1u;
        uint32_t       r;
        do {
                r = deck_next(rng);
        } while (r > limit);
        return r % n;
}

void deck_fill(uint8_t *cards, uint32_t decks) {
        for (uint32_t i = 0; i < decks * DECK_CARDS; ++i) cards[i] = (uint8_t)(i % DECK_CARDS);
        return;
}

void deck_shuffle(deck_rng_t *rng, uint8_t *cards, uint32_t count) {
        for (uint32_t i = count; i > 1; --i) {
                const uint32_t j = deck_below(rng, i);
                const uint8_t  t = cards[i - 1];
                cards[i - 1] = cards[j];
                cards[j] = t;
        }
        return;
}
