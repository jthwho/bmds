/**
 * @file      rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Solitaire's rules (devplan/games.md §21): Klondike's deal, moves, draws and passes, Windows'
 * scorings, undo.
 */

#include "rules.h"

#include <string.h>

/* ---- Scoring */

static void add(solitaire_t *s, int32_t points) {
        if (s->scoring == SOL_SCORE_NONE) return;
        s->score += points;
        if (s->scoring == SOL_SCORE_STANDARD && s->score < 0) s->score = 0;
        return;
}

static int is_foundation(uint32_t pile) {
        return pile >= SOL_FOUNDATION && pile < SOL_TABLEAU;
}

static int is_column(uint32_t pile) {
        return pile >= SOL_TABLEAU && pile < SOL_PILES;
}

/* What moving onto @p to from @p from scores. */
static int32_t points_for(const solitaire_t *s, uint32_t from, uint32_t to) {
        const int vegas = s->scoring == SOL_SCORE_VEGAS;
        if (is_foundation(to)) return vegas ? 5 : 10;
        if (is_foundation(from)) return vegas ? -5 : -15;
        if (from == SOL_WASTE && !vegas) return 5;
        return 0;
}

/* The passes through the stock the scoring allows (0: no limit). */
static uint32_t passes_allowed(const solitaire_t *s) {
        if (s->scoring != SOL_SCORE_VEGAS) return 0;
        return s->draw == 1 ? 1 : 3;
}

/* ---- History */

static sol_move_t *remember(solitaire_t *s, uint32_t kind, uint32_t from, uint32_t to, uint32_t count) {
        const uint32_t at = (s->history_first + s->history_count) % SOL_HISTORY;
        if (s->history_count == SOL_HISTORY)
                s->history_first = (s->history_first + 1) % SOL_HISTORY; /* the oldest forgotten */
        else
                ++s->history_count;
        sol_move_t *m = &s->history[at];
        m->kind = (uint8_t)kind;
        m->from = (uint8_t)from;
        m->to = (uint8_t)to;
        m->count = (uint8_t)count;
        m->flipped = 0;
        m->fan = (uint8_t)s->fan;
        m->passes = (uint16_t)s->passes;
        m->score = s->score;
        return m;
}

/* ---- Piles */

static uint8_t pop(sol_pile_t *p) {
        return p->cards[--p->count];
}

static void push(sol_pile_t *p, uint8_t card) {
        p->cards[p->count++] = card;
        return;
}

/* Moves the top @p count cards of @p from onto @p to, in order. */
static void carry(solitaire_t *s, uint32_t from, uint32_t count, uint32_t to) {
        sol_pile_t *f = &s->pile[from], *t = &s->pile[to];
        memcpy(&t->cards[t->count], &f->cards[f->count - count], count);
        t->count = (uint8_t)(t->count + count);
        f->count = (uint8_t)(f->count - count);
        return;
}

uint32_t sol_home(const solitaire_t *s) {
        uint32_t n = 0;
        for (uint32_t f = SOL_FOUNDATION; f < SOL_TABLEAU; ++f) n += s->pile[f].count;
        return n;
}

/* ---- The game */

void sol_deal(solitaire_t *s, uint32_t draw, uint32_t scoring, uint32_t seed) {
        memset(s, 0, sizeof(*s));
        s->draw = draw == 3 ? 3 : 1;
        s->scoring = scoring < SOL_SCORINGS ? scoring : SOL_SCORE_STANDARD;
        s->score = s->scoring == SOL_SCORE_VEGAS ? -52 : 0;
        uint8_t deck[SOL_CARDS];
        for (uint32_t i = 0; i < SOL_CARDS; ++i) deck[i] = (uint8_t)i;
        uint32_t r = seed != 0 ? seed : 1;
        for (uint32_t i = SOL_CARDS - 1; i > 0; --i) { /* Fisher-Yates on xorshift32 */
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                const uint32_t j = r % (i + 1);
                const uint8_t  t = deck[i];
                deck[i] = deck[j];
                deck[j] = t;
        }
        /* A row at a time across the columns, each column's last face up. */
        uint32_t next = 0;
        for (uint32_t row = 0; row < 7; ++row)
                for (uint32_t col = row; col < 7; ++col)
                        push(&s->pile[SOL_TABLEAU + col], (uint8_t)(deck[next++] | (col == row ? SOL_UP : 0)));
        while (next < SOL_CARDS) push(&s->pile[SOL_STOCK], deck[next++]);
        return;
}

void sol_deal_finished(solitaire_t *s, uint32_t draw, uint32_t scoring) {
        sol_deal(s, draw, scoring, 1);
        for (uint32_t p = 0; p < SOL_PILES; ++p) s->pile[p].count = 0;
        /* Spades and hearts alternating in two columns, opposite ways; clubs and diamonds the same. */
        static const uint8_t pairs[4][2] = {{SOL_SPADES, SOL_HEARTS},
                                            {SOL_HEARTS, SOL_SPADES},
                                            {SOL_CLUBS, SOL_DIAMONDS},
                                            {SOL_DIAMONDS, SOL_CLUBS}};
        for (uint32_t col = 0; col < 4; ++col)
                for (uint32_t r = SOL_RANKS; r-- > 0;)
                        push(&s->pile[SOL_TABLEAU + col],
                             (uint8_t)((pairs[col][(SOL_RANKS - 1 - r) % 2] * SOL_RANKS + r) | SOL_UP));
        return;
}

int sol_can_take(const solitaire_t *s, uint32_t from, uint32_t count) {
        if (from >= SOL_PILES || from == SOL_STOCK) return 0;
        const sol_pile_t *p = &s->pile[from];
        if (count == 0 || count > p->count) return 0;
        if (!is_column(from) && count != 1) return 0;
        return (p->cards[p->count - count] & SOL_UP) != 0; /* a column's face-up cards are a run */
}

int sol_can_move(const solitaire_t *s, uint32_t from, uint32_t count, uint32_t to) {
        if (to >= SOL_PILES || to == from || !sol_can_take(s, from, count)) return 0;
        const sol_pile_t *f = &s->pile[from], *t = &s->pile[to];
        const uint8_t     card = f->cards[f->count - count];
        if (is_foundation(to)) {
                if (count != 1) return 0;
                if (t->count == 0) return sol_rank(card) == 0;
                const uint8_t top = t->cards[t->count - 1];
                return sol_suit(top) == sol_suit(card) && sol_rank(top) + 1 == sol_rank(card);
        }
        if (is_column(to)) {
                if (t->count == 0) return sol_rank(card) == SOL_RANKS - 1;
                const uint8_t top = t->cards[t->count - 1];
                return (top & SOL_UP) != 0 && sol_red(top) != sol_red(card) && sol_rank(top) == sol_rank(card) + 1;
        }
        return 0;
}

int sol_move(solitaire_t *s, uint32_t from, uint32_t count, uint32_t to) {
        if (s->won || !sol_can_move(s, from, count, to)) return -1;
        sol_move_t *m = remember(s, SOL_MOVED, from, to, count);
        carry(s, from, count, to);
        add(s, points_for(s, from, to));
        if (from == SOL_WASTE && s->fan > 1) --s->fan;
        sol_pile_t *f = &s->pile[from];
        if (is_column(from) && f->count > 0 && (f->cards[f->count - 1] & SOL_UP) == 0) {
                f->cards[f->count - 1] = (uint8_t)(f->cards[f->count - 1] | SOL_UP);
                m->flipped = 1;
                if (s->scoring == SOL_SCORE_STANDARD) add(s, 5);
        }
        ++s->moves;
        s->started = 1;
        if (sol_home(s) == SOL_CARDS) {
                s->won = 1;
                if (s->scoring == SOL_SCORE_STANDARD && s->seconds >= 30) add(s, (int32_t)(700000 / s->seconds));
        }
        return 0;
}

int sol_can_draw(const solitaire_t *s) {
        if (s->won) return 0;
        if (s->pile[SOL_STOCK].count > 0) return 1;
        const uint32_t allowed = passes_allowed(s);
        return s->pile[SOL_WASTE].count > 0 && (allowed == 0 || s->passes + 1 < allowed);
}

int sol_draw(solitaire_t *s) {
        if (!sol_can_draw(s)) return -1;
        sol_pile_t *stock = &s->pile[SOL_STOCK], *waste = &s->pile[SOL_WASTE];
        ++s->moves;
        s->started = 1;
        if (stock->count > 0) {
                const uint32_t n = stock->count < s->draw ? stock->count : s->draw;
                remember(s, SOL_DREW, SOL_STOCK, SOL_WASTE, n);
                for (uint32_t i = 0; i < n; ++i) push(waste, (uint8_t)(pop(stock) | SOL_UP));
                s->fan = n;
                return (int)n;
        }
        remember(s, SOL_TURNED_AGAIN, SOL_WASTE, SOL_STOCK, waste->count);
        while (waste->count > 0) push(stock, (uint8_t)(pop(waste) & ~SOL_UP));
        if (s->scoring == SOL_SCORE_STANDARD) {
                if (s->draw == 1)
                        add(s, -100);
                else if (s->passes >= 2) /* the fourth pass on */
                        add(s, -20);
        }
        ++s->passes;
        s->fan = 0;
        return 0;
}

int sol_undo(solitaire_t *s) {
        if (s->history_count == 0) return -1;
        --s->history_count;
        const sol_move_t *m = &s->history[(s->history_first + s->history_count) % SOL_HISTORY];
        sol_pile_t       *stock = &s->pile[SOL_STOCK], *waste = &s->pile[SOL_WASTE];
        switch (m->kind) {
                case SOL_MOVED: {
                        sol_pile_t *f = &s->pile[m->from];
                        if (m->flipped) f->cards[f->count - 1] = (uint8_t)(f->cards[f->count - 1] & ~SOL_UP);
                        carry(s, m->to, m->count, m->from);
                        break;
                }
                case SOL_DREW:
                        for (uint32_t i = 0; i < m->count; ++i) push(stock, (uint8_t)(pop(waste) & ~SOL_UP));
                        break;
                default: /* SOL_TURNED_AGAIN */
                        for (uint32_t i = 0; i < m->count; ++i) push(waste, (uint8_t)(pop(stock) | SOL_UP));
                        break;
        }
        s->fan = m->fan;
        s->passes = m->passes;
        s->score = m->score;
        s->won = 0;
        if (s->moves > 0) --s->moves;
        return 0;
}

int sol_foundation_for(const solitaire_t *s, uint32_t from) {
        for (uint32_t f = SOL_FOUNDATION; f < SOL_TABLEAU; ++f)
                if (sol_can_move(s, from, 1, f)) return (int)f;
        return -1;
}

int sol_auto_step(solitaire_t *s, uint32_t *to) {
        if (s->won) return -1;
        static const uint8_t order[] = {SOL_WASTE,       SOL_TABLEAU,     SOL_TABLEAU + 1, SOL_TABLEAU + 2,
                                        SOL_TABLEAU + 3, SOL_TABLEAU + 4, SOL_TABLEAU + 5, SOL_TABLEAU + 6};
        for (uint32_t i = 0; i < sizeof(order); ++i) {
                const int f = sol_foundation_for(s, order[i]);
                if (f < 0) continue;
                (void)sol_move(s, order[i], 1, (uint32_t)f);
                if (to != NULL) *to = (uint32_t)f;
                return order[i];
        }
        return -1;
}

void sol_second(solitaire_t *s) {
        if (!s->started || s->won) return;
        ++s->seconds;
        if (s->scoring == SOL_SCORE_STANDARD && s->seconds % 10 == 0) add(s, -2);
        return;
}
