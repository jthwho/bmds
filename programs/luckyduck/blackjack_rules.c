/**
 * @file      blackjack_rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Blackjack rules (devplan/luckyduck.md §8b, §16).
 */

#include <string.h>
#include "blackjack_rules.h"

/* ---- Cards */

/* A card's value: the Ace 1 (11 by bj_total() where it fits), the faces 10. */
static int value(uint8_t card) {
        const int r = (int)deck_rank(card) + 1;
        return r > 10 ? 10 : r;
}

int bj_total(const uint8_t *cards, int count, int *soft) {
        int sum = 0, ace = 0;
        for (int i = 0; i < count; ++i) {
                if (cards[i] == BJ_HIDDEN) continue;
                sum += value(cards[i]);
                if (deck_rank(cards[i]) == 0) ace = 1;
        }
        const int s = ace && sum + 10 <= 21;
        if (soft != NULL) *soft = s;
        return s ? sum + 10 : sum;
}

int bj_is_blackjack(const bj_hand_t *h) {
        return h->count == 2 && !h->split && bj_total(h->cards, h->count, NULL) == 21;
}

static int hand_total(const bj_hand_t *h) {
        return bj_total(h->cards, h->count, NULL);
}

/* ---- The log */

static void emit(bj_table_t *t, uint8_t type, int seat, int hand, uint8_t card, uint8_t up, uint8_t result,
                 int32_t amount) {
        if (t->event_count == BJ_EVENTS) return; /* a round can't fill it: 5 seats × 4 hands × ~6 events */
        bj_event_t *e = &t->events[t->event_count++];
        e->type = type;
        e->seat = (int8_t)seat;
        e->hand = (uint8_t)hand;
        e->card = card;
        e->up = up;
        e->result = result;
        e->amount = amount;
        return;
}

bj_event_t bj_event_for(const bj_table_t *t, uint32_t i, int seat) {
        bj_event_t e = t->events[i];
        if (e.type == BJ_EV_DEAL && e.seat == BJ_DEALER && !e.up && seat != BJ_DEALER) e.card = BJ_HIDDEN;
        return e;
}

/* ---- The shoe */

static void shuffle(bj_table_t *t) {
        deck_fill(t->shoe, BJ_DECKS);
        deck_shuffle(&t->rng, t->shoe, BJ_SHOE);
        t->next = 0;
        /* The cut card about three quarters in. */
        t->cut = (uint16_t)(BJ_SHOE * 3 / 4 - 10 + deck_below(&t->rng, 21));
        t->reshuffle = 0;
        emit(t, BJ_EV_SHUFFLE, BJ_DEALER, 0, 0, 0, 0, 0);
        return;
}

static uint8_t draw(bj_table_t *t) {
        if (t->next >= BJ_SHOE) shuffle(t); /* never with the cut card at 75%, but the shoe can't run dry */
        const uint8_t card = t->shoe[t->next++];
        if (t->next == t->cut) t->reshuffle = 1;
        return card;
}

static void deal_to(bj_table_t *t, int seat, int hand, int up) {
        bj_hand_t    *h = seat == BJ_DEALER ? &t->dealer : &t->seat[seat].hand[hand];
        const uint8_t card = draw(t);
        if (h->count < BJ_CARDS) h->cards[h->count++] = card;
        emit(t, BJ_EV_DEAL, seat, hand, card, (uint8_t)up, 0, 0);
        return;
}

/* ---- Setting up */

bj_rules_t bj_default_rules(void) {
        bj_rules_t r;
        memset(&r, 0, sizeof(r));
        r.min_bet = 5;
        r.max_bet = 500;
        r.hit_soft_17 = 0;
        r.surrender = 1;
        return r;
}

void bj_init(bj_table_t *t, const bj_rules_t *rules, deck_key_t key) {
        memset(t, 0, sizeof(*t));
        t->rules = *rules;
        deck_seed_key(&t->rng, key);
        t->phase = BJ_BETTING;
        t->turn_seat = -1;
        shuffle(t);
        return;
}

void bj_sit(bj_table_t *t, int seat, int playing, int32_t stack) {
        if (seat < 0 || seat >= BJ_SEATS) return;
        bj_seat_t *s = &t->seat[seat];
        if (!playing && t->phase == BJ_BETTING && s->hand[0].bet > 0) (void)bj_bet(t, seat, 0);
        s->playing = (uint8_t)playing;
        s->stack = stack;
        return;
}

int bj_bet(bj_table_t *t, int seat, int32_t amount) {
        if (t->phase != BJ_BETTING || seat < 0 || seat >= BJ_SEATS || !t->seat[seat].playing) return -1;
        bj_seat_t    *s = &t->seat[seat];
        const int32_t had = s->hand[0].bet;
        if (amount < 0 || amount > s->stack + had) return -1;
        s->stack += had - amount;
        s->hand[0].bet = amount;
        emit(t, BJ_EV_BET, seat, 0, 0, 0, 0, amount);
        return 0;
}

/* ---- Play */

static int live(const bj_hand_t *h) {
        return h->result != BJ_SURRENDERED && hand_total(h) <= 21;
}

static void settle(bj_table_t *t);

/* The dealer turns the hole card, draws to 17 (a soft 17 too if the rules say) while any hand
   still stands against it, and the round is settled. */
static void dealer_play(bj_table_t *t) {
        t->turn_seat = -1;
        t->hole_up = 1;
        emit(t, BJ_EV_HOLE, BJ_DEALER, 0, t->dealer.cards[1], 1, 0, 0);
        int waiting = 0;
        for (int s = 0; s < BJ_SEATS; ++s)
                for (int h = 0; h < t->seat[s].hands; ++h) {
                        const bj_hand_t *hand = &t->seat[s].hand[h];
                        if (live(hand) && !bj_is_blackjack(hand)) waiting = 1;
                }
        if (waiting) {
                for (;;) {
                        int       soft;
                        const int total = bj_total(t->dealer.cards, t->dealer.count, &soft);
                        if (total > 17 || (total == 17 && !(soft && t->rules.hit_soft_17))) break;
                        deal_to(t, BJ_DEALER, 0, 1);
                }
                if (hand_total(&t->dealer) > 21) emit(t, BJ_EV_BUST, BJ_DEALER, 0, 0, 0, 0, 0);
        }
        settle(t);
        return;
}

/* The next hand to act from seat @p seat's hand @p hand on (that one included): its turn, or the
   dealer's when none is left. */
static void turn_from(bj_table_t *t, int seat, int hand) {
        for (int s = seat; s < BJ_SEATS; ++s, hand = 0) {
                for (int h = hand; h < t->seat[s].hands; ++h) {
                        bj_hand_t *x = &t->seat[s].hand[h];
                        if (x->done) continue;
                        if (bj_is_blackjack(x) || hand_total(x) >= 21) { /* nothing to decide */
                                x->done = 1;
                                continue;
                        }
                        t->phase = BJ_PLAYING;
                        t->turn_seat = (int8_t)s;
                        t->turn_hand = (uint8_t)h;
                        emit(t, BJ_EV_TURN, s, h, 0, 0, 0, 0);
                        return;
                }
        }
        dealer_play(t);
        return;
}

/* Insurance settled, then: the dealer's blackjack ends the round, else the play begins. */
static void peek(bj_table_t *t) {
        const int dealer_bj = bj_total(t->dealer.cards, 2, NULL) == 21;
        for (int s = 0; s < BJ_SEATS; ++s) {
                bj_seat_t *seat = &t->seat[s];
                if (seat->insurance == 0) continue;
                const int32_t won = dealer_bj ? seat->insurance * 2 : -seat->insurance;
                if (dealer_bj) seat->stack += seat->insurance * 3;
                emit(t, BJ_EV_INSURED, s, 0, 0, 0, 0, won);
        }
        if (dealer_bj) {
                t->hole_up = 1;
                emit(t, BJ_EV_HOLE, BJ_DEALER, 0, t->dealer.cards[1], 1, 0, 0);
                emit(t, BJ_EV_BLACKJACK, BJ_DEALER, 0, 0, 0, 0, 0);
                settle(t);
                return;
        }
        for (int s = 0; s < BJ_SEATS; ++s)
                if (t->seat[s].hands > 0 && bj_is_blackjack(&t->seat[s].hand[0]))
                        emit(t, BJ_EV_BLACKJACK, s, 0, 0, 0, 0, 0);
        turn_from(t, 0, 0);
        return;
}

int bj_insurance_seat(const bj_table_t *t) {
        if (t->phase != BJ_INSURANCE) return -1;
        for (int s = 0; s < BJ_SEATS; ++s)
                if (t->seat[s].hands > 0 && !t->seat[s].insured) return s;
        return -1;
}

int bj_bet_ok(const bj_table_t *t, int seat) {
        const int32_t bet = t->seat[seat].hand[0].bet;
        return bet == 0 || (bet >= t->rules.min_bet && bet <= t->rules.max_bet);
}

int bj_deal(bj_table_t *t) {
        if (t->phase != BJ_BETTING) return -1;
        int any = 0;
        for (int s = 0; s < BJ_SEATS; ++s) {
                if (!t->seat[s].playing) continue;
                if (!bj_bet_ok(t, s)) return -1;
                any |= t->seat[s].hand[0].bet > 0;
        }
        if (!any) return -1;
        if (t->reshuffle) shuffle(t);
        for (int s = 0; s < BJ_SEATS; ++s) {
                bj_seat_t    *seat = &t->seat[s];
                const int32_t bet = seat->playing ? seat->hand[0].bet : 0;
                memset(seat->hand, 0, sizeof(seat->hand));
                seat->hand[0].bet = bet;
                seat->hands = bet > 0 ? 1 : 0;
                seat->insurance = 0;
                seat->insured = 0;
        }
        memset(&t->dealer, 0, sizeof(t->dealer));
        t->hole_up = 0;
        /* A card to each, the dealer's up, again to each, the dealer's hole card down. */
        for (int round = 0; round < 2; ++round) {
                for (int s = 0; s < BJ_SEATS; ++s)
                        if (t->seat[s].hands > 0) deal_to(t, s, 0, 1);
                deal_to(t, BJ_DEALER, 0, round == 0);
        }
        const int up = (int)deck_rank(t->dealer.cards[0]);
        if (up == 0) {
                t->phase = BJ_INSURANCE;
                return 0;
        }
        if (value(t->dealer.cards[0]) == 10) {
                peek(t);
                return 0;
        }
        for (int s = 0; s < BJ_SEATS; ++s)
                if (t->seat[s].hands > 0 && bj_is_blackjack(&t->seat[s].hand[0]))
                        emit(t, BJ_EV_BLACKJACK, s, 0, 0, 0, 0, 0);
        turn_from(t, 0, 0);
        return 0;
}

int bj_insure(bj_table_t *t, int seat, int take) {
        if (bj_insurance_seat(t) != seat) return -1;
        bj_seat_t    *s = &t->seat[seat];
        const int32_t amount = take ? s->hand[0].bet / 2 : 0;
        if (amount > s->stack) return -1;
        s->stack -= amount;
        s->insurance = amount;
        s->insured = 1;
        emit(t, BJ_EV_INSURANCE, seat, 0, 0, 0, 0, amount);
        if (bj_insurance_seat(t) < 0) peek(t);
        return 0;
}

int bj_can(const bj_table_t *t, int seat, int action) {
        if (t->phase != BJ_PLAYING || seat != t->turn_seat || action < 0 || action >= BJ_ACTIONS) return 0;
        const bj_seat_t *s = &t->seat[seat];
        const bj_hand_t *h = &s->hand[t->turn_hand];
        switch (action) {
                case BJ_HIT:
                case BJ_STAND: return 1;
                case BJ_DOUBLE: return h->count == 2 && s->stack >= h->bet;
                case BJ_SPLIT:
                        return h->count == 2 && value(h->cards[0]) == value(h->cards[1]) && s->hands < BJ_HANDS &&
                               s->stack >= h->bet;
                case BJ_SURRENDER: return t->rules.surrender && h->count == 2 && s->hands == 1 && !h->split;
                default: return 0;
        }
}

int bj_act(bj_table_t *t, int seat, int action) {
        if (!bj_can(t, seat, action)) return -1;
        bj_seat_t *s = &t->seat[seat];
        const int  hi = t->turn_hand;
        bj_hand_t *h = &s->hand[hi];
        switch (action) {
                case BJ_HIT:
                        deal_to(t, seat, hi, 1);
                        if (hand_total(h) > 21) emit(t, BJ_EV_BUST, seat, hi, 0, 0, 0, 0);
                        if (hand_total(h) >= 21) h->done = 1;
                        break;
                case BJ_STAND: h->done = 1; break;
                case BJ_DOUBLE:
                        s->stack -= h->bet;
                        emit(t, BJ_EV_DOUBLE, seat, hi, 0, 0, 0, h->bet);
                        h->bet *= 2;
                        h->doubled = 1;
                        deal_to(t, seat, hi, 1);
                        if (hand_total(h) > 21) emit(t, BJ_EV_BUST, seat, hi, 0, 0, 0, 0);
                        h->done = 1;
                        break;
                case BJ_SPLIT: {
                        const int  ni = s->hands++;
                        bj_hand_t *n = &s->hand[ni];
                        memset(n, 0, sizeof(*n));
                        n->cards[0] = h->cards[1];
                        n->count = 1;
                        h->count = 1;
                        n->bet = h->bet;
                        s->stack -= h->bet;
                        h->split = n->split = 1;
                        const int aces = deck_rank(h->cards[0]) == 0;
                        h->split_aces = n->split_aces = (uint8_t)aces;
                        emit(t, BJ_EV_SPLIT, seat, hi, 0, 0, 0, ni);
                        deal_to(t, seat, hi, 1);
                        deal_to(t, seat, ni, 1);
                        /* Split Aces take one card each; a hand at 21 has nothing to decide. */
                        if (aces) h->done = n->done = 1;
                        if (hand_total(h) == 21) h->done = 1;
                        if (hand_total(n) == 21) n->done = 1;
                        break;
                }
                case BJ_SURRENDER:
                        h->result = BJ_SURRENDERED;
                        h->done = 1;
                        emit(t, BJ_EV_SURRENDER, seat, hi, 0, 0, 0, 0);
                        break;
                default: return -1;
        }
        if (h->done)
                turn_from(t, seat, hi + 1);
        else
                emit(t, BJ_EV_TURN, seat, hi, 0, 0, 0, 0); /* still this hand's turn (after a split) */
        return 0;
}

static void settle(bj_table_t *t) {
        const int dealer = hand_total(&t->dealer);
        const int dealer_bj = bj_is_blackjack(&t->dealer);
        for (int s = 0; s < BJ_SEATS; ++s) {
                bj_seat_t *seat = &t->seat[s];
                for (int h = 0; h < seat->hands; ++h) {
                        bj_hand_t *x = &seat->hand[h];
                        const int  total = hand_total(x);
                        int32_t    back = 0;
                        uint8_t    result;
                        if (x->result == BJ_SURRENDERED) {
                                result = BJ_SURRENDERED;
                                back = x->bet / 2;
                        } else if (total > 21) {
                                result = BJ_LOSE;
                        } else if (bj_is_blackjack(x)) {
                                result = dealer_bj ? BJ_PUSH : BJ_BLACKJACK_WIN;
                                back = dealer_bj ? x->bet : x->bet + x->bet * 3 / 2;
                        } else if (dealer_bj || (dealer <= 21 && dealer > total)) {
                                result = BJ_LOSE;
                        } else if (dealer > 21 || total > dealer) {
                                result = BJ_WIN;
                                back = 2 * x->bet;
                        } else {
                                result = BJ_PUSH;
                                back = x->bet;
                        }
                        x->result = result;
                        x->done = 1;
                        seat->stack += back;
                        emit(t, BJ_EV_RESULT, s, h, 0, 0, result, back);
                }
        }
        t->phase = BJ_SETTLED;
        t->turn_seat = -1;
        emit(t, BJ_EV_ROUND_END, BJ_DEALER, 0, 0, 0, 0, 0);
        return;
}

void bj_next_round(bj_table_t *t) {
        if (t->phase != BJ_SETTLED) return;
        for (int s = 0; s < BJ_SEATS; ++s) {
                memset(t->seat[s].hand, 0, sizeof(t->seat[s].hand));
                t->seat[s].hands = 0;
                t->seat[s].insurance = 0;
        }
        memset(&t->dealer, 0, sizeof(t->dealer));
        t->hole_up = 0;
        t->event_count = 0;
        t->phase = BJ_BETTING;
        return;
}

/* ---- Basic strategy (several decks, doubles after splits, late surrender) */

int bj_basic(const uint8_t *cards, int count, uint8_t up, int can_double, int can_split, int can_surrender,
             int hit_soft_17) {
        const int d = deck_rank(up) == 0 ? 11 : value(up); /* the dealer's card, the Ace 11 */
        int       soft;
        const int total = bj_total(cards, count, &soft);
        const int pair = count == 2 && value(cards[0]) == value(cards[1]) ? value(cards[0]) : 0;
        /* Surrender first: the worst hands against the best cards. */
        if (can_surrender && count == 2 && !soft) {
                if (pair == 8) {
                        if (hit_soft_17 && d == 11) return BJ_SURRENDER;
                } else if ((total == 16 && d >= 9) || (total == 15 && d == 10) ||
                           (hit_soft_17 && d == 11 && (total == 15 || total == 17))) {
                        return BJ_SURRENDER;
                }
        }
        if (pair && can_split) {
                int split = 0;
                switch (pair) {
                        case 1: split = 1; break;
                        case 9: split = (d >= 2 && d <= 6) || d == 8 || d == 9; break;
                        case 8: split = 1; break;
                        case 7: split = d <= 7; break;
                        case 6: split = d <= 6; break;
                        case 4: split = d == 5 || d == 6; break;
                        case 3:
                        case 2: split = d <= 7; break;
                        default: break; /* tens and fives: never */
                }
                if (split) return BJ_SPLIT;
        }
        if (soft) {
                if (total >= 20) return BJ_STAND;
                if (total == 19) return hit_soft_17 && d == 6 && can_double ? BJ_DOUBLE : BJ_STAND;
                if (total == 18) {
                        if (d >= (hit_soft_17 ? 2 : 3) && d <= 6) return can_double ? BJ_DOUBLE : BJ_STAND;
                        return d <= 8 ? BJ_STAND : BJ_HIT;
                }
                const int low = total == 17 ? 3 : total >= 15 ? 4 : 5;
                return d >= low && d <= 6 && can_double ? BJ_DOUBLE : BJ_HIT;
        }
        if (total >= 17) return BJ_STAND;
        if (total >= 13) return d <= 6 ? BJ_STAND : BJ_HIT;
        if (total == 12) return d >= 4 && d <= 6 ? BJ_STAND : BJ_HIT;
        if (total == 11) return can_double && (d <= 10 || hit_soft_17) ? BJ_DOUBLE : BJ_HIT;
        if (total == 10) return can_double && d <= 9 ? BJ_DOUBLE : BJ_HIT;
        if (total == 9) return can_double && d >= 3 && d <= 6 ? BJ_DOUBLE : BJ_HIT;
        return BJ_HIT;
}
