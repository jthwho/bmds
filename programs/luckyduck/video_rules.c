/**
 * @file      video_rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Video Poker (devplan/luckyduck.md §21).
 */

#include "video_rules.h"
#include <string.h>
#include "hand.h"

/* What a pay table's row matches. */
enum {
        ROW_ROYAL,
        ROW_STRAIGHT_FLUSH,
        ROW_FOUR,
        ROW_FOUR_ACES,
        ROW_FOUR_LOW,       /* 2s, 3s or 4s */
        ROW_FOUR_MID,       /* 5s to Kings */
        ROW_FOUR_ACES_KICK, /* Aces with a 2, 3 or 4 */
        ROW_FOUR_LOW_KICK,  /* 2s, 3s or 4s with an Ace, 2, 3 or 4 */
        ROW_FULL_HOUSE,
        ROW_FLUSH,
        ROW_STRAIGHT,
        ROW_THREE,
        ROW_TWO_PAIR,
        ROW_JACKS /* a pair of Jacks or better */
};

typedef struct {
                uint8_t     kind;
                uint16_t    pay; /* a coin's */
                const char *name;
} row_t;

#define ROWS_MAX 13
#define ROYAL_MAX_COINS 4000 /* the royal flush at five coins: 800 a coin */

typedef struct {
                const char *name;
                uint8_t     rows;
                row_t       row[ROWS_MAX];
} machine_t;

static const machine_t MACHINES[VP_MACHINES] = {
        {"JACKS OR BETTER",
         9,
         {{ROW_ROYAL, 250, "ROYAL FLUSH"},
          {ROW_STRAIGHT_FLUSH, 50, "STRAIGHT FLUSH"},
          {ROW_FOUR, 25, "FOUR OF A KIND"},
          {ROW_FULL_HOUSE, 9, "FULL HOUSE"},
          {ROW_FLUSH, 6, "FLUSH"},
          {ROW_STRAIGHT, 4, "STRAIGHT"},
          {ROW_THREE, 3, "THREE OF A KIND"},
          {ROW_TWO_PAIR, 2, "TWO PAIR"},
          {ROW_JACKS, 1, "JACKS OR BETTER"}}},
        {"BONUS POKER",
         11,
         {{ROW_ROYAL, 250, "ROYAL FLUSH"},
          {ROW_STRAIGHT_FLUSH, 50, "STRAIGHT FLUSH"},
          {ROW_FOUR_ACES, 80, "FOUR ACES"},
          {ROW_FOUR_LOW, 40, "FOUR 2S, 3S, 4S"},
          {ROW_FOUR_MID, 25, "FOUR 5S THRU KS"},
          {ROW_FULL_HOUSE, 8, "FULL HOUSE"},
          {ROW_FLUSH, 5, "FLUSH"},
          {ROW_STRAIGHT, 4, "STRAIGHT"},
          {ROW_THREE, 3, "THREE OF A KIND"},
          {ROW_TWO_PAIR, 2, "TWO PAIR"},
          {ROW_JACKS, 1, "JACKS OR BETTER"}}},
        {"DOUBLE DOUBLE BONUS",
         13,
         {{ROW_ROYAL, 250, "ROYAL FLUSH"},
          {ROW_STRAIGHT_FLUSH, 50, "STRAIGHT FLUSH"},
          {ROW_FOUR_ACES_KICK, 400, "FOUR ACES + 2, 3, 4"},
          {ROW_FOUR_LOW_KICK, 160, "FOUR 2-4 + A, 2, 3, 4"},
          {ROW_FOUR_ACES, 160, "FOUR ACES"},
          {ROW_FOUR_LOW, 80, "FOUR 2S, 3S, 4S"},
          {ROW_FOUR_MID, 50, "FOUR 5S THRU KS"},
          {ROW_FULL_HOUSE, 9, "FULL HOUSE"},
          {ROW_FLUSH, 6, "FLUSH"},
          {ROW_STRAIGHT, 4, "STRAIGHT"},
          {ROW_THREE, 3, "THREE OF A KIND"},
          {ROW_TWO_PAIR, 1, "TWO PAIR"},
          {ROW_JACKS, 1, "JACKS OR BETTER"}}},
};

#define RANK_JACK 9 /* hand.h's ranks: the deuce 0 */
#define RANK_FOUR 2

static int low(int r) {
        return r <= RANK_FOUR;
}

static int matches(int kind, uint32_t value) {
        const int c = hand_category(value), r0 = hand_rank(value, 0), r1 = hand_rank(value, 1);
        switch (kind) {
                case ROW_ROYAL: return c == HAND_STRAIGHT_FLUSH && r0 == HAND_ACE;
                case ROW_STRAIGHT_FLUSH: return c == HAND_STRAIGHT_FLUSH;
                case ROW_FOUR: return c == HAND_FOUR;
                case ROW_FOUR_ACES: return c == HAND_FOUR && r0 == HAND_ACE;
                case ROW_FOUR_LOW: return c == HAND_FOUR && low(r0);
                case ROW_FOUR_MID: return c == HAND_FOUR && !low(r0) && r0 != HAND_ACE;
                case ROW_FOUR_ACES_KICK: return c == HAND_FOUR && r0 == HAND_ACE && low(r1);
                case ROW_FOUR_LOW_KICK: return c == HAND_FOUR && low(r0) && (low(r1) || r1 == HAND_ACE);
                case ROW_FULL_HOUSE: return c == HAND_FULL_HOUSE;
                case ROW_FLUSH: return c == HAND_FLUSH;
                case ROW_STRAIGHT: return c == HAND_STRAIGHT;
                case ROW_THREE: return c == HAND_THREE;
                case ROW_TWO_PAIR: return c == HAND_TWO_PAIR;
                case ROW_JACKS: return c == HAND_PAIR && r0 >= RANK_JACK;
                default: return 0;
        }
}

static int row_of(const machine_t *m, uint32_t value) {
        for (int i = 0; i < m->rows; ++i)
                if (matches(m->row[i].kind, value)) return i;
        return -1;
}

static int machine_ok(int machine) {
        return machine >= 0 && machine < VP_MACHINES;
}

/* ---- The pay tables */

const char *vp_machine_name(int machine) {
        return machine_ok(machine) ? MACHINES[machine].name : "";
}

int vp_rows(int machine) {
        return machine_ok(machine) ? MACHINES[machine].rows : 0;
}

const char *vp_row_name(int machine, int row) {
        return machine_ok(machine) && row >= 0 && row < MACHINES[machine].rows ? MACHINES[machine].row[row].name : "";
}

int32_t vp_row_pay(int machine, int row, int coins) {
        if (!machine_ok(machine) || row < 0 || row >= MACHINES[machine].rows) return 0;
        const row_t *r = &MACHINES[machine].row[row];
        if (r->kind == ROW_ROYAL && coins == VP_MAX_COINS) return ROYAL_MAX_COINS;
        return (int32_t)r->pay * coins;
}

int vp_row_royal(int machine, int row) {
        return machine_ok(machine) && row >= 0 && row < MACHINES[machine].rows &&
               MACHINES[machine].row[row].kind == ROW_ROYAL;
}

int vp_row(int machine, const uint8_t *cards) {
        if (!machine_ok(machine)) return -1;
        return row_of(&MACHINES[machine], hand_value(cards, VP_CARDS));
}

uint8_t vp_scoring(int machine, const uint8_t *cards, int row) {
        if (!machine_ok(machine) || row < 0 || row >= MACHINES[machine].rows) return 0;
        const uint32_t value = hand_value(cards, VP_CARDS);
        const int      kind = MACHINES[machine].row[row].kind;
        int            a = -1, b = -1; /* the ranks that count; none: all five */
        switch (kind) {
                case ROW_FOUR:
                case ROW_FOUR_ACES:
                case ROW_FOUR_LOW:
                case ROW_FOUR_MID:
                case ROW_THREE:
                case ROW_JACKS: a = hand_rank(value, 0); break;
                case ROW_FOUR_ACES_KICK: /* the kicker counts */
                case ROW_FOUR_LOW_KICK:
                case ROW_TWO_PAIR:
                        a = hand_rank(value, 0);
                        b = hand_rank(value, 1);
                        break;
                default: return (1u << VP_CARDS) - 1;
        }
        uint8_t mask = 0;
        for (int i = 0; i < VP_CARDS; ++i) {
                const int r = hand_card_rank(cards[i]);
                if (r == a || r == b) mask |= (uint8_t)(1u << i);
        }
        return mask;
}

/* ---- The machine */

void vp_init(vp_t *v, int machine, int32_t coin, int32_t stack) {
        memset(v, 0, sizeof(*v));
        v->machine = (uint8_t)(machine_ok(machine) ? machine : VP_JACKS);
        v->coin = coin > 0 ? coin : 1;
        v->coins = 1;
        v->stack = stack;
        v->phase = VP_BETTING;
        v->row = -1;
        return;
}

int vp_configure(vp_t *v, int machine, int32_t coin) {
        if (v->phase == VP_DEALT || !machine_ok(machine) || coin <= 0) return -1;
        v->machine = (uint8_t)machine;
        v->coin = coin;
        return 0;
}

int vp_bet(vp_t *v, int coins) {
        if (v->phase == VP_DEALT || coins < 1 || coins > VP_MAX_COINS) return -1;
        v->coins = (uint8_t)coins;
        return 0;
}

int vp_deal(vp_t *v, deck_key_t key) {
        const int32_t bet = v->coin * v->coins;
        if (v->phase == VP_DEALT || bet > v->stack) return -1;
        v->stack -= bet;
        deck_seed_key(&v->rng, key);
        deck_fill(v->deck, 1);
        deck_shuffle(&v->rng, v->deck, DECK_CARDS);
        memcpy(v->cards, v->deck, VP_CARDS);
        v->next = VP_CARDS;
        v->held = 0;
        v->drawn = 0;
        v->row = -1;
        v->won = 0;
        v->phase = VP_DEALT;
        return 0;
}

int vp_hold(vp_t *v, int i, int held) {
        if (v->phase != VP_DEALT || i < 0 || i >= VP_CARDS) return -1;
        if (held)
                v->held |= (uint8_t)(1u << i);
        else
                v->held &= (uint8_t) ~(1u << i);
        return 0;
}

int vp_draw(vp_t *v) {
        if (v->phase != VP_DEALT) return -1;
        v->drawn = (uint8_t)(~v->held & ((1u << VP_CARDS) - 1));
        for (int i = 0; i < VP_CARDS; ++i)
                if (v->drawn & (1u << i)) v->cards[i] = v->deck[v->next++];
        v->row = (int8_t)vp_row(v->machine, v->cards);
        v->won = vp_row_pay(v->machine, v->row, v->coins) * v->coin;
        v->stack += v->won;
        v->phase = VP_DONE;
        return 0;
}

/* ---- Perfect play */

static uint16_t pay_of(const machine_t *m, int machine, int coins, int category, int r0, int r1) {
        const uint32_t value = (uint32_t)category << 20 | (uint32_t)r0 << 16 | (uint32_t)r1 << 12;
        return (uint16_t)vp_row_pay(machine, row_of(m, value), coins);
}

void vp_ev_start(vp_ev_t *e, int machine, int coins, const uint8_t *cards) {
        memset(e, 0, sizeof(*e));
        e->machine = (uint8_t)(machine_ok(machine) ? machine : VP_JACKS);
        e->coins = (uint8_t)coins;
        memcpy(e->cards, cards, VP_CARDS);
        int n = 0;
        for (int c = 0; c < DECK_CARDS; ++c) {
                if (memchr(cards, c, VP_CARDS) != NULL) continue;
                e->rest[n] = (uint8_t)c;
                e->rest_bit[n] = (uint16_t)(1u << hand_card_rank((uint8_t)c));
                e->rest_suit[n] = (uint8_t)(1u << deck_suit((uint8_t)c));
                ++n;
        }
        /* The pays by what matters to the rows: a pair's rank, a four's and its kicker's. */
        const machine_t *m = &MACHINES[e->machine];
        for (int c = 0; c < HAND_CATEGORIES; ++c) e->pay_made[c] = pay_of(m, e->machine, coins, c, 0, 0);
        e->pay_made[HAND_STRAIGHT_FLUSH] = pay_of(m, e->machine, coins, HAND_STRAIGHT_FLUSH, HAND_ACE - 1, 0);
        e->pay_royal = pay_of(m, e->machine, coins, HAND_STRAIGHT_FLUSH, HAND_ACE, 0);
        for (int r = 0; r <= HAND_ACE; ++r) {
                e->pay_pair[r] = pay_of(m, e->machine, coins, HAND_PAIR, r, 0);
                for (int k = 0; k <= HAND_ACE; ++k) e->pay_four[r][k] = pay_of(m, e->machine, coins, HAND_FOUR, r, k);
        }
        return;
}

/* A one-bit mask's bit (de Bruijn): the chip may have no instruction to count bits. */
static int bit_of(uint32_t m) {
        static const uint8_t index[32] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                          31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
        return index[(m * 0x077cb531u) >> 27];
}

/* What five cards pay: their ranks seen once (@p distinct of them), twice, three and four times,
   and their suits. */
static uint32_t pay_masks(const vp_ev_t *e, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, uint32_t suits,
                          uint32_t distinct) {
        switch (distinct) {
                case 5: {
                        const int      flush = (suits & (suits - 1)) == 0;
                        const uint32_t runs = c1 & (c1 >> 1) & (c1 >> 2) & (c1 >> 3) & (c1 >> 4);
                        const int      straight = runs != 0 || c1 == 0x100fu;
                        if (flush && straight)
                                return runs == 1u << (HAND_ACE - 4) ? e->pay_royal : e->pay_made[HAND_STRAIGHT_FLUSH];
                        if (flush) return e->pay_made[HAND_FLUSH];
                        return straight ? e->pay_made[HAND_STRAIGHT] : 0;
                }
                case 4: return e->pay_pair[bit_of(c2)];
                case 3: return e->pay_made[c3 != 0 ? HAND_THREE : HAND_TWO_PAIR];
                default:
                        if (c4 != 0) return e->pay_four[bit_of(c4)][bit_of(c1 & ~c4)];
                        return e->pay_made[HAND_FULL_HOUSE];
        }
}

static void add_card(uint32_t *p, uint32_t bit, uint32_t suit) {
        p[5] += (p[0] & bit) == 0;
        p[3] |= p[2] & bit;
        p[2] |= p[1] & bit;
        p[1] |= p[0] & bit;
        p[0] |= bit;
        p[4] |= suit;
        return;
}

int vp_ev_step(vp_ev_t *e, uint32_t budget) {
        const int left = DECK_CARDS - VP_CARDS;
        while (!e->done && budget > 0) {
                if (!e->started) {
                        e->draw = (uint8_t)(VP_CARDS - __builtin_popcount(e->hold));
                        for (int j = 0; j < e->draw; ++j) e->idx[j] = (uint8_t)j;
                        e->started = 1;
                        e->part_ok = 0;
                }
                const int k = e->draw;
                /* The hold's cards and the draw's but its last, as masks: each draw adds one card. */
                if (!e->part_ok) {
                        memset(e->part, 0, sizeof(e->part));
                        for (int i = 0; i < VP_CARDS; ++i)
                                if (e->hold & (1u << i))
                                        add_card(e->part, 1u << hand_card_rank(e->cards[i]),
                                                 1u << deck_suit(e->cards[i]));
                        for (int j = 0; j + 1 < k; ++j)
                                add_card(e->part, e->rest_bit[e->idx[j]], e->rest_suit[e->idx[j]]);
                        e->part_ok = 1;
                }
                if (k == 0) { /* standing pat: one hand */
                        e->paid[e->hold] +=
                                pay_masks(e, e->part[0], e->part[1], e->part[2], e->part[3], e->part[4], e->part[5]);
                        ++e->draws[e->hold];
                        --budget;
                        e->started = 0;
                        if (++e->hold == VP_HOLDS) e->done = 1;
                        continue;
                }
                /* The last card through the rest of the deck, as far as the budget goes. */
                const uint32_t c1 = e->part[0], c2 = e->part[1], c3 = e->part[2], c4 = e->part[3], suits = e->part[4],
                               distinct = e->part[5];
                const int from = e->idx[k - 1];
                int       to = left;
                if ((uint32_t)(to - from) > budget) to = from + (int)budget;
                uint64_t paid = 0;
                for (int x = from; x < to; ++x) {
                        const uint32_t bit = e->rest_bit[x];
                        paid += pay_masks(e, c1 | bit, c2 | (c1 & bit), c3 | (c2 & bit), c4 | (c3 & bit),
                                          suits | e->rest_suit[x], distinct + ((c1 & bit) == 0));
                }
                e->paid[e->hold] += paid;
                e->draws[e->hold] += (uint32_t)(to - from);
                budget -= (uint32_t)(to - from);
                if (to < left) {
                        e->idx[k - 1] = (uint8_t)to;
                        continue;
                }
                /* The last card has been through the deck: the next of the others. */
                int j = k - 2;
                while (j >= 0 && e->idx[j] == left - k + j) --j;
                if (j < 0) { /* this hold's draws are done */
                        e->started = 0;
                        if (++e->hold == VP_HOLDS) e->done = 1;
                        continue;
                }
                ++e->idx[j];
                for (int i = j + 1; i < k; ++i) e->idx[i] = (uint8_t)(e->idx[i - 1] + 1);
                e->part_ok = 0;
        }
        return e->done;
}

double vp_ev_return(const vp_ev_t *e, int hold) {
        if (hold < 0 || hold >= VP_HOLDS || e->draws[hold] == 0) return 0.0;
        return (double)e->paid[hold] / (double)e->draws[hold];
}

int vp_ev_best(const vp_ev_t *e) {
        int    best = 0;
        double most = vp_ev_return(e, 0);
        for (int h = 1; h < VP_HOLDS; ++h) {
                const double r = vp_ev_return(e, h);
                /* Equal (within rounding): the hold drawing fewer wins. */
                if (r > most + 1e-9 ||
                    (r > most - 1e-9 && __builtin_popcount((unsigned)h) > __builtin_popcount((unsigned)best))) {
                        best = h;
                        most = r;
                }
        }
        return best;
}
