/**
 * @file      hand.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's poker hands (devplan/luckyduck.md §21): from rank masks, one bit a rank (bit 0 the
 * deuce, bit 12 the Ace), one mask a suit, and masks of the ranks seen at least once, twice, three
 * and four times. Every category is a few operations on those.
 */

#include "hand.h"

static int top_bit(uint32_t m) {
        return 31 - __builtin_clz(m);
}

/* The highest @p count ranks of @p m packed from rank slot @p slot on. */
static uint32_t pack(uint32_t value, uint32_t m, int slot, int count) {
        for (int i = 0; i < count && m != 0; ++i, ++slot) {
                const int r = top_bit(m);
                value |= (uint32_t)r << (16 - 4 * slot);
                m &= ~(1u << r);
        }
        return value;
}

/* The top card of the best straight in @p m, or -1: five ranks in a row, or the wheel (A-2-3-4-5,
   whose top is the Five). */
static int straight_top(uint32_t m) {
        const uint32_t runs = m & (m >> 1) & (m >> 2) & (m >> 3) & (m >> 4);
        if (runs != 0) return top_bit(runs) + 4;
        if ((m & 0x100fu) == 0x100fu) return 3;
        return -1;
}

static uint32_t category(int c) {
        return (uint32_t)c << 20;
}

uint32_t hand_value(const uint8_t *cards, int n) {
        uint32_t suit[4] = {0, 0, 0, 0};
        uint32_t c1 = 0, c2 = 0, c3 = 0, c4 = 0; /* ranks seen at least once, twice... */
        for (int i = 0; i < n; ++i) {
                const uint32_t b = 1u << hand_card_rank(cards[i]);
                suit[deck_suit(cards[i]) & 3u] |= b;
                c4 |= c3 & b;
                c3 |= c2 & b;
                c2 |= c1 & b;
                c1 |= b;
        }
        int flush = -1;
        for (int s = 0; s < 4; ++s)
                if (__builtin_popcount(suit[s]) >= 5) flush = s;
        if (flush >= 0) {
                const int top = straight_top(suit[flush]);
                if (top >= 0) return category(HAND_STRAIGHT_FLUSH) | (uint32_t)top << 16;
        }
        if (c4 != 0) {
                const int q = top_bit(c4);
                return pack(category(HAND_FOUR) | (uint32_t)q << 16, c1 & ~(1u << q), 1, 1);
        }
        if (c3 != 0) {
                const int      t = top_bit(c3);
                const uint32_t pairs = c2 & ~(1u << t);
                if (pairs != 0) return category(HAND_FULL_HOUSE) | (uint32_t)t << 16 | (uint32_t)top_bit(pairs) << 12;
        }
        if (flush >= 0) return pack(category(HAND_FLUSH), suit[flush], 0, 5);
        const int top = straight_top(c1);
        if (top >= 0) return category(HAND_STRAIGHT) | (uint32_t)top << 16;
        if (c3 != 0) {
                const int t = top_bit(c3);
                return pack(category(HAND_THREE) | (uint32_t)t << 16, c1 & ~(1u << t), 1, 2);
        }
        if (c2 != 0) {
                const int      hi = top_bit(c2);
                const uint32_t rest = c2 & ~(1u << hi);
                if (rest != 0) {
                        const int lo = top_bit(rest);
                        return pack(category(HAND_TWO_PAIR) | (uint32_t)hi << 16 | (uint32_t)lo << 12,
                                    c1 & ~((1u << hi) | (1u << lo)), 2, 1);
                }
                return pack(category(HAND_PAIR) | (uint32_t)hi << 16, c1 & ~(1u << hi), 1, 3);
        }
        return pack(category(HAND_HIGH_CARD), c1, 0, 5);
}

const char *hand_name(int c) {
        static const char *const names[HAND_CATEGORIES] = {
                "HIGH CARD", "PAIR",       "TWO PAIR",       "THREE OF A KIND", "STRAIGHT",
                "FLUSH",     "FULL HOUSE", "FOUR OF A KIND", "STRAIGHT FLUSH",
        };
        return c >= 0 && c < HAND_CATEGORIES ? names[c] : "";
}
