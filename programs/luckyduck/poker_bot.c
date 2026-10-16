/**
 * @file      poker_bot.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's poker ducks (devplan/luckyduck.md §25).
 */

#include <string.h>
#include "hand.h"
#include "poker_bot.h"

/* ---- Draw */

uint32_t pk_bot_discard(const uint8_t *cards) {
        const uint32_t v = hand_value(cards, 5);
        const int      cat = hand_category(v);
        if (cat >= HAND_STRAIGHT) return 0; /* a made hand: stand pat */
        int count[DECK_RANKS] = {0}, suit[4] = {0};
        for (int i = 0; i < 5; ++i) {
                ++count[hand_card_rank(cards[i])];
                ++suit[deck_suit(cards[i])];
        }
        uint32_t keep = 0;
        if (cat >= HAND_PAIR) { /* pairs, trips: keep what matches */
                for (int i = 0; i < 5; ++i)
                        if (count[hand_card_rank(cards[i])] >= 2) keep |= 1u << i;
                return 0x1fu & ~keep;
        }
        for (int s = 0; s < 4; ++s) /* four to a flush */
                if (suit[s] == 4) {
                        for (int i = 0; i < 5; ++i)
                                if ((int)deck_suit(cards[i]) == s) keep |= 1u << i;
                        return 0x1fu & ~keep;
                }
        uint32_t ranks = 0;
        for (int i = 0; i < 5; ++i) ranks |= 1u << hand_card_rank(cards[i]);
        for (int low = 0; low + 3 < HAND_ACE; ++low) { /* four in a row, open at both ends */
                const uint32_t run = 0xfu << low;
                if ((ranks & run) != run) continue;
                for (int i = 0; i < 5; ++i)
                        if (run & (1u << hand_card_rank(cards[i]))) keep |= 1u << i;
                return 0x1fu & ~keep;
        }
        /* High cards: Jacks and up, two at most; else the highest. */
        int kept = 0;
        for (int r = HAND_ACE; r >= 9 && kept < 2; --r)
                for (int i = 0; i < 5 && kept < 2; ++i)
                        if (hand_card_rank(cards[i]) == r) {
                                keep |= 1u << i;
                                ++kept;
                        }
        if (kept == 0) {
                int best = 0;
                for (int i = 1; i < 5; ++i)
                        if (hand_card_rank(cards[i]) > hand_card_rank(cards[best])) best = i;
                keep = 1u << best;
        }
        return 0x1fu & ~keep;
}

/* ---- Equity */

void pk_equity_start(pk_equity_t *e, const pk_view_t *v, deck_key_t key, uint32_t trials) {
        memset(e, 0, sizeof(*e));
        deck_seed_key(&e->rng, key);
        e->game = v->rules.game;
        e->target = trials;
        e->draw_first = (uint8_t)(e->game == PK_DRAW && v->round == 0);
        uint8_t               seen[DECK_CARDS] = {0};
        const pk_view_seat_t *me = &v->seat[v->me];
        for (int i = 0; i < me->count; ++i) {
                e->mine[e->mine_count++] = me->cards[i];
                seen[me->cards[i]] = 1;
        }
        e->mine_final = (uint8_t)(pk_final_cards(e->game) - (e->game == PK_HOLDEM ? PK_BOARD : 0));
        for (int i = 0; i < v->board_count; ++i) {
                e->board[e->board_count++] = v->board[i];
                seen[v->board[i]] = 1;
        }
        for (int i = 0; i < v->dead_count; ++i) seen[v->dead[i]] = 1;
        for (int s = 0; s < PK_SEATS; ++s) {
                const pk_view_seat_t *p = &v->seat[s];
                if (s == v->me || !p->in_hand) continue;
                const int k = e->opps++;
                for (int i = 0; i < p->count; ++i)
                        if (p->cards[i] != PK_HIDDEN) {
                                e->opp_cards[k][e->opp_known[k]++] = p->cards[i];
                                seen[p->cards[i]] = 1;
                        }
        }
        for (int c = 0; c < DECK_CARDS; ++c)
                if (!seen[c]) e->unseen[e->unseen_count++] = (uint8_t)c;
        return;
}

/* The next random unseen card (a partial shuffle in place), or PK_HIDDEN when none are left. */
static uint8_t take(pk_equity_t *e, uint32_t *n) {
        if (*n >= e->unseen_count) return PK_HIDDEN;
        const uint32_t j = *n + deck_below(&e->rng, e->unseen_count - *n);
        const uint8_t  c = e->unseen[j];
        e->unseen[j] = e->unseen[*n];
        e->unseen[*n] = c;
        ++*n;
        return c;
}

/* Fills @p cards (of @p count) up to @p final, then plays Draw's draw if asked. @return Its count. */
static int complete(pk_equity_t *e, uint8_t *cards, int count, int final, int draw, uint32_t *n) {
        while (count < final) {
                const uint8_t c = take(e, n);
                if (c == PK_HIDDEN) break;
                cards[count++] = c;
        }
        if (draw && count == 5) {
                const uint32_t mask = pk_bot_discard(cards);
                for (int i = 0; i < 5; ++i) {
                        if (!(mask & (1u << i))) continue;
                        const uint8_t c = take(e, n);
                        if (c != PK_HIDDEN) cards[i] = c;
                }
        }
        return count;
}

static uint32_t value_with_board(const uint8_t *cards, int count, const uint8_t *board, int board_count) {
        uint8_t all[PK_CARDS + PK_BOARD];
        memcpy(all, cards, (size_t)count);
        memcpy(all + count, board, (size_t)board_count);
        return hand_value(all, count + board_count);
}

int pk_equity_step(pk_equity_t *e, uint32_t count) {
        if (e->opps == 0) {
                e->trials = e->target;
                e->wins = (float)e->target;
                return 1;
        }
        for (uint32_t k = 0; k < count && e->trials < e->target; ++k) {
                uint32_t n = 0;
                uint8_t  board[PK_BOARD];
                memcpy(board, e->board, e->board_count);
                const int bc = e->game == PK_HOLDEM ? complete(e, board, e->board_count, PK_BOARD, 0, &n) : 0;
                uint8_t   mine[PK_CARDS];
                memcpy(mine, e->mine, e->mine_count);
                const int      mc = complete(e, mine, e->mine_count, e->mine_final, e->draw_first, &n);
                const uint32_t me = value_with_board(mine, mc, board, bc);
                int            beaten = 0, ties = 0;
                for (int o = 0; o < e->opps && !beaten; ++o) {
                        uint8_t cards[PK_CARDS];
                        memcpy(cards, e->opp_cards[o], e->opp_known[o]);
                        const int      oc = complete(e, cards, e->opp_known[o], e->mine_final, e->draw_first, &n);
                        const uint32_t them = value_with_board(cards, oc, board, bc);
                        if (them > me) beaten = 1;
                        if (them == me) ++ties;
                }
                if (!beaten) e->wins += 1.0f / (float)(1 + ties);
                ++e->trials;
        }
        return e->trials >= e->target;
}

float pk_equity_value(const pk_equity_t *e) {
        return e->trials > 0 ? e->wins / (float)e->trials : 0.0f;
}

/* ---- Betting */

static pk_choice_t choice(int action, int32_t amount) {
        pk_choice_t c = {(uint8_t)action, amount};
        return c;
}

pk_choice_t pk_bot_decide(const pk_view_t *v, const pk_style_t *style, float equity, uint32_t random) {
        const pk_view_seat_t *me = &v->seat[v->me];
        int                   opps = 0;
        for (int s = 0; s < PK_SEATS; ++s) opps += s != v->me && v->seat[s].in_hand;
        if (opps < 1) opps = 1;
        const float   loose = (float)style->loose / 100.0f, aggro = (float)style->aggro / 100.0f;
        const float   bluff = (float)style->bluff / 100.0f;
        const float   u1 = (float)(random & 0xffffu) / 65536.0f, u2 = (float)(random >> 16) / 65536.0f;
        const int32_t to_call = v->to_call, pot = v->pot;
        const float   odds = to_call > 0 ? (float)to_call / (float)(pot + to_call) : 0.0f;
        /* The equity is against random cards; a bet into the duck says the bettor's are better,
           the bigger the bet the more (a looser duck believes it less). The blinds and the
           bring-in are forced, so they say nothing. */
        const int     stud = v->rules.game == PK_STUD5 || v->rules.game == PK_STUD7;
        const int32_t forced = v->round > 0 ? 0 : stud ? v->rules.bring_in : v->rules.big_blind;
        const int32_t chosen = me->bet + to_call - forced > 0 ? me->bet + to_call - forced : 0;
        const int32_t before = pot - to_call > 1 ? pot - to_call : 1;
        const float   pressure = chosen >= before ? 1.0f : (float)chosen / (float)before;
        const float   eff = equity * (1.0f - 0.35f * pressure * (1.0f - 0.5f * loose));
        const float   fair = 1.0f / (float)(opps + 1), rel = eff / fair;

        int want_raise = rel > 1.6f - 0.5f * aggro && u1 < 0.45f + 0.55f * aggro;
        if (!want_raise && opps <= 2 && v->round > 0 && u2 < bluff * 0.3f && to_call * 3 <= pot) want_raise = 1;
        if (want_raise && v->can_raise) {
                int32_t amount = v->min_raise;
                if (v->rules.no_limit) {
                        /* Half the pot to the whole pot, after calling; a big edge or most of the stack all in. */
                        const float   size = 0.5f + 0.5f * u2;
                        const int32_t to = me->bet + to_call + (int32_t)(size * (float)(pot + to_call));
                        amount = to;
                        if ((rel > 2.4f && eff > 0.75f && v->round > 0) || amount * 10 > v->max_raise * 8)
                                amount = v->max_raise;
                        if (amount < v->min_raise) amount = v->min_raise;
                        if (amount > v->max_raise) amount = v->max_raise;
                }
                return choice(PK_RAISE, amount);
        }
        if (to_call == 0) return choice(PK_CHECK, 0);
        /* Calling: the equity against the pot odds, a looser duck asking less; in the first round
           a share of the fair one too (cheap calls of a bring-in would play every hand; stud's
           chances lie closer together, so it asks more). */
        const float bar = stud ? 1.3f : 1.05f;
        const int   call = eff >= odds * (1.1f - 0.35f * loose) && (v->round > 0 || rel >= bar - 0.5f * loose);
        return call ? choice(PK_CALL, 0) : choice(PK_FOLD, 0);
}
