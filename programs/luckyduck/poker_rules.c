/**
 * @file      poker_rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Poker rules (devplan/luckyduck.md §8e, §25).
 */

#include <string.h>
#include "hand.h"
#include "poker_rules.h"

/* ---- The log */

static pk_event_t *emit(pk_table_t *t, uint8_t type, int seat, int index, uint8_t card, int up, int action,
                        int32_t amount, uint32_t value) {
        static pk_event_t spare; /* a hand can't fill the log (six seats' every card and act) */
        if (t->event_count == PK_EVENTS) return &spare;
        pk_event_t *e = &t->events[t->event_count++];
        memset(e, 0, sizeof(*e));
        e->type = type;
        e->seat = (int8_t)seat;
        e->index = (uint8_t)index;
        e->card = card;
        e->up = (uint8_t)up;
        e->action = (uint8_t)action;
        e->amount = amount;
        e->value = value;
        return e;
}

pk_event_t pk_event_for(const pk_table_t *t, uint32_t i, int seat) {
        pk_event_t e = t->events[i];
        if (e.type == PK_EV_DEAL && !e.up && seat != PK_NOBODY && e.seat != seat) e.card = PK_HIDDEN;
        return e;
}

/* ---- Seats */

static int next_of(int s) {
        return (s + 1) % PK_SEATS;
}

static int in_hand_count(const pk_table_t *t) {
        int n = 0;
        for (int s = 0; s < PK_SEATS; ++s) n += t->seat[s].in_hand;
        return n;
}

/* Seats still in that can bet. */
static int able_count(const pk_table_t *t) {
        int n = 0;
        for (int s = 0; s < PK_SEATS; ++s) n += t->seat[s].in_hand && !t->seat[s].all_in;
        return n;
}

/* The next seat in the hand after @p s (clockwise), or PK_NOBODY. */
static int next_in_hand(const pk_table_t *t, int s) {
        for (int i = 1; i <= PK_SEATS; ++i) {
                const int k = (s + i) % PK_SEATS;
                if (t->seat[k].in_hand) return k;
        }
        return PK_NOBODY;
}

/* Whether a seat other than @p s can still bet. */
static int others_able(const pk_table_t *t, int s) {
        for (int k = 0; k < PK_SEATS; ++k)
                if (k != s && t->seat[k].in_hand && !t->seat[k].all_in) return 1;
        return 0;
}

/* Whether @p s has to act before the round can end. */
static int needs(const pk_table_t *t, int s) {
        const pk_seat_t *p = &t->seat[s];
        if (!p->in_hand || p->all_in) return 0;
        return p->bet < t->cur_bet || (!p->acted && others_able(t, s));
}

/* The first seat from @p s on (itself first) that has to act, or PK_NOBODY. */
static int first_from(const pk_table_t *t, int s) {
        for (int i = 0; i < PK_SEATS; ++i) {
                const int k = (s + i) % PK_SEATS;
                if (needs(t, k)) return k;
        }
        return PK_NOBODY;
}

/* ---- Setting up */

pk_rules_t pk_stakes(int game, int no_limit, int32_t big_blind) {
        pk_rules_t r;
        memset(&r, 0, sizeof(r));
        if (big_blind < 2) big_blind = 2;
        r.game = (uint8_t)game;
        r.no_limit = (uint8_t)(game == PK_HOLDEM && no_limit);
        r.big_blind = big_blind;
        r.small_blind = big_blind / 2;
        r.small_bet = big_blind;
        if (game == PK_STUD5 || game == PK_STUD7) {
                r.ante = big_blind / 4 > 0 ? big_blind / 4 : 1;
                r.bring_in = big_blind / 2;
        }
        return r;
}

void pk_init(pk_table_t *t, const pk_rules_t *rules, deck_key_t key, int tournament) {
        memset(t, 0, sizeof(*t));
        t->rules = *rules;
        deck_seed_key(&t->rng, key);
        t->tournament = (uint8_t)(tournament != 0);
        t->button = PK_NOBODY;
        t->turn = PK_NOBODY;
        t->aggressor = PK_NOBODY;
        t->bring_in = PK_NOBODY;
        t->phase = PK_WAITING;
        return;
}

int pk_set_rules(pk_table_t *t, const pk_rules_t *rules) {
        if (t->phase != PK_WAITING) return -1;
        t->rules = *rules;
        return 0;
}

void pk_sit(pk_table_t *t, int seat, int sitting, int32_t stack) {
        if (seat < 0 || seat >= PK_SEATS || t->phase != PK_WAITING) return;
        pk_seat_t *s = &t->seat[seat];
        memset(s, 0, sizeof(*s));
        s->sitting = (uint8_t)(sitting != 0);
        s->stack = sitting ? stack : 0;
        return;
}

void pk_add_chips(pk_table_t *t, int seat, int32_t amount) {
        if (seat < 0 || seat >= PK_SEATS || t->phase != PK_WAITING || amount <= 0) return;
        t->seat[seat].stack += amount;
        return;
}

void pk_fix_deck(pk_table_t *t, const uint8_t *cards, int count) {
        if (count < 0 || count > DECK_CARDS) count = 0;
        memcpy(t->fixed, cards, (size_t)count);
        t->fixed_count = (uint8_t)count;
        return;
}

int pk_players(const pk_table_t *t) {
        int n = 0;
        for (int s = 0; s < PK_SEATS; ++s) n += t->seat[s].sitting && t->seat[s].stack > 0;
        return n;
}

int pk_final_cards(int game) {
        return game == PK_HOLDEM || game == PK_STUD7 ? 7 : 5;
}

/* The betting rounds a hand has. */
static int rounds_of(int game) {
        return game == PK_DRAW ? 2 : game == PK_STUD7 ? 5 : 4;
}

static int is_stud(const pk_table_t *t) {
        return t->rules.game == PK_STUD5 || t->rules.game == PK_STUD7;
}

/* ---- Cards */

static uint8_t draw_card(pk_table_t *t) {
        if (t->next >= DECK_CARDS) {
                /* Draw's deck ran out: the discards (never the drawing seat's own) shuffled in. */
                const uint32_t m = t->muck_count;
                memcpy(&t->deck[DECK_CARDS - m], t->muck, m);
                deck_shuffle(&t->rng, &t->deck[DECK_CARDS - m], m);
                t->next = (uint8_t)(DECK_CARDS - m);
                t->muck_count = 0;
        }
        return t->deck[t->next++];
}

static void deal_to(pk_table_t *t, int seat, int up) {
        pk_seat_t *s = &t->seat[seat];
        if (s->count >= PK_CARDS) return;
        const uint8_t card = draw_card(t);
        const int     i = s->count++;
        s->cards[i] = card;
        if (up) s->up |= (uint8_t)(1u << i);
        emit(t, PK_EV_DEAL, seat, i, card, up, 0, 0, 0);
        return;
}

/* One card to each seat in the hand, from the button's left. */
static void deal_round(pk_table_t *t, int up) {
        int s = t->button;
        for (int i = 0; i < PK_SEATS; ++i) {
                s = next_of(s);
                if (t->seat[s].in_hand) deal_to(t, s, up);
        }
        return;
}

static void deal_board(pk_table_t *t, int count) {
        for (int i = 0; i < count && t->board_count < PK_BOARD; ++i) {
                const uint8_t card = draw_card(t);
                t->board[t->board_count] = card;
                emit(t, PK_EV_BOARD, PK_NOBODY, t->board_count, card, 1, 0, 0, 0);
                ++t->board_count;
        }
        return;
}

static void to_muck(pk_table_t *t, uint8_t card) {
        if (t->muck_count < DECK_CARDS) t->muck[t->muck_count++] = card;
        return;
}

/* The cards round @p round (1 up) starts with: the flop, turn and river; stud's streets. */
static void deal_street(pk_table_t *t, int round) {
        switch (t->rules.game) {
                case PK_HOLDEM: deal_board(t, round == 1 ? 3 : 1); break;
                case PK_STUD5: deal_round(t, 1); break;
                case PK_STUD7: deal_round(t, round < 4); break; /* the seventh street down */
                default: break;
        }
        return;
}

/* ---- Chips */

/* Moves up to @p amount from @p seat's stack to its bet. @return What it moved. */
static int32_t put_chips(pk_table_t *t, int seat, int32_t amount) {
        pk_seat_t *s = &t->seat[seat];
        if (amount > s->stack) amount = s->stack;
        if (amount < 0) amount = 0;
        s->stack -= amount;
        s->bet += amount;
        s->put += amount;
        if (s->stack == 0 && s->in_hand) s->all_in = 1;
        return amount;
}

int32_t pk_bet_size(const pk_table_t *t, int round) {
        const int32_t small = is_stud(t) ? t->rules.small_bet : t->rules.big_blind;
        const int     big_from = t->rules.game == PK_DRAW ? 1 : 2;
        return round >= big_from ? small * 2 : small;
}

int32_t pk_pot_total(const pk_table_t *t) {
        int32_t sum = t->pot;
        for (int s = 0; s < PK_SEATS; ++s) sum += t->seat[s].bet;
        return sum;
}

int32_t pk_to_call(const pk_table_t *t, int seat) {
        if (seat < 0 || seat >= PK_SEATS || t->phase != PK_BETTING) return 0;
        const pk_seat_t *s = &t->seat[seat];
        if (!s->in_hand || s->all_in) return 0;
        const int32_t d = t->cur_bet - s->bet;
        return d <= 0 ? 0 : d < s->stack ? d : s->stack;
}

/* Fixed limit's full raise from the bet now: the round's size (completing a bring-in or a short blind to it). */
static int32_t fixed_target(const pk_table_t *t) {
        const int32_t size = pk_bet_size(t, t->round);
        return t->cur_bet < size ? size : t->cur_bet + size;
}

static int may_raise(const pk_table_t *t, int seat) {
        if (seat < 0 || seat >= PK_SEATS || t->phase != PK_BETTING || t->turn != seat) return 0;
        const pk_seat_t *s = &t->seat[seat];
        if (!s->in_hand || s->all_in || s->acted) return 0;
        if (s->stack <= t->cur_bet - s->bet) return 0; /* no chips beyond a call */
        if (!t->rules.no_limit && t->raises >= 4) return 0;
        return others_able(t, seat); /* nobody left to call it */
}

int32_t pk_min_raise(const pk_table_t *t, int seat) {
        if (!may_raise(t, seat)) return 0;
        const pk_seat_t *s = &t->seat[seat];
        const int32_t    to = t->rules.no_limit ? t->cur_bet + t->last_raise : fixed_target(t);
        return to < s->bet + s->stack ? to : s->bet + s->stack;
}

int32_t pk_max_raise(const pk_table_t *t, int seat) {
        if (!may_raise(t, seat)) return 0;
        const pk_seat_t *s = &t->seat[seat];
        return t->rules.no_limit ? s->bet + s->stack : pk_min_raise(t, seat);
}

int pk_can(const pk_table_t *t, int seat, int action) {
        if (seat < 0 || seat >= PK_SEATS || t->phase != PK_BETTING || t->turn != seat) return 0;
        const pk_seat_t *s = &t->seat[seat];
        if (!s->in_hand || s->all_in) return 0;
        switch (action) {
                case PK_FOLD: return 1;
                case PK_CHECK: return s->bet >= t->cur_bet;
                case PK_CALL: return s->bet < t->cur_bet;
                case PK_RAISE: return may_raise(t, seat);
                default: return 0;
        }
}

/* ---- The hand's course */

static void end_round(pk_table_t *t);
static void showdown(pk_table_t *t);

/* Hands @p seat's turn to it (or ends the round when nobody has to act). */
static void continue_from(pk_table_t *t, int seat) {
        const int k = first_from(t, seat);
        if (k == PK_NOBODY) {
                end_round(t);
                return;
        }
        t->turn = (int8_t)k;
        emit(t, PK_EV_TURN, k, 0, 0, 0, 0, 0, 0);
        return;
}

/* The value of the cards @p s shows (stud's: who acts first). */
static uint32_t showing(const pk_seat_t *s) {
        uint8_t up[PK_CARDS];
        int     n = 0;
        for (int i = 0; i < s->count; ++i)
                if (s->up & (1u << i)) up[n++] = s->cards[i];
        return n > 0 ? hand_value(up, n) : 0;
}

/* The seat to start a round after the first. */
static int round_start(const pk_table_t *t) {
        if (!is_stud(t)) return next_of(t->button);
        int      best = PK_NOBODY;
        uint32_t best_value = 0;
        int      s = t->button;
        for (int i = 0; i < PK_SEATS; ++i) { /* ties: nearest the button's left */
                s = next_of(s);
                if (!t->seat[s].in_hand) continue;
                const uint32_t v = showing(&t->seat[s]);
                if (best == PK_NOBODY || v > best_value) {
                        best = s;
                        best_value = v;
                }
        }
        return best == PK_NOBODY ? next_of(t->button) : best;
}

static void begin_round(pk_table_t *t) {
        t->phase = PK_BETTING;
        t->aggressor = PK_NOBODY;
        continue_from(t, round_start(t));
        return;
}

/* The round's bets: an uncalled one's excess back, the rest into the pot. */
static void collect(pk_table_t *t) {
        int top = PK_NOBODY;
        for (int s = 0; s < PK_SEATS; ++s)
                if (top == PK_NOBODY || t->seat[s].bet > t->seat[top].bet) top = s;
        int32_t second = 0;
        for (int s = 0; s < PK_SEATS; ++s)
                if (s != top && t->seat[s].bet > second) second = t->seat[s].bet;
        if (top != PK_NOBODY && t->seat[top].bet > second) {
                pk_seat_t    *p = &t->seat[top];
                const int32_t back = p->bet - second;
                p->bet -= back;
                p->put -= back;
                p->stack += back;
                if (p->stack > 0) p->all_in = 0;
                emit(t, PK_EV_RETURN, top, 0, 0, 0, 0, back, 0);
        }
        int32_t sum = 0;
        for (int s = 0; s < PK_SEATS; ++s) {
                sum += t->seat[s].bet;
                t->seat[s].bet = 0;
                t->seat[s].acted = 0;
        }
        t->pot += sum;
        if (sum > 0) emit(t, PK_EV_COLLECT, PK_NOBODY, 0, 0, 0, 0, t->pot, 0);
        t->cur_bet = 0;
        t->last_raise = t->rules.big_blind;
        t->raises = 0;
        return;
}

/* After the last action: the hand's chips, ended. */
static void finish(pk_table_t *t) {
        t->pot = 0;
        t->phase = PK_WAITING;
        t->turn = PK_NOBODY;
        /* Who ran out: in a tournament, out, the one who started the hand with more placed higher. */
        int left = 0;
        for (int s = 0; s < PK_SEATS; ++s) left += t->seat[s].sitting && t->seat[s].stack > 0;
        for (;;) {
                int bust = PK_NOBODY, s = t->button;
                for (int i = 0; i < PK_SEATS; ++i) {
                        s = next_of(s);
                        const pk_seat_t *p = &t->seat[s];
                        if (!p->sitting || p->stack > 0 || p->place != 0 || p->started == 0) continue;
                        if (bust == PK_NOBODY || p->started > t->seat[bust].started) bust = s;
                }
                if (bust == PK_NOBODY) break;
                pk_seat_t *p = &t->seat[bust];
                p->started = 0; /* told */
                if (t->tournament) {
                        p->place = (uint8_t)(left + 1);
                        ++left; /* the next one out this hand finishes below */
                        p->sitting = 0;
                        ++t->out;
                }
                emit(t, PK_EV_OUT, bust, t->tournament ? p->place : 0, 0, 0, 0, 0, 0);
        }
        if (t->tournament && pk_players(t) == 1) {
                for (int s = 0; s < PK_SEATS; ++s)
                        if (t->seat[s].sitting && t->seat[s].stack > 0) {
                                t->seat[s].place = 1;
                                emit(t, PK_EV_OUT, s, 1, 0, 0, 0, 0, 0);
                        }
        }
        emit(t, PK_EV_END, PK_NOBODY, 0, 0, 0, 0, 0, 0);
        return;
}

/* Everyone else folded: the last seat takes it all, unseen. */
static void uncontested(pk_table_t *t) {
        collect(t);
        const int w = next_in_hand(t, t->button);
        emit(t, PK_EV_POT, PK_NOBODY, 0, 0, 0, 0, t->pot, 1u << w);
        emit(t, PK_EV_WIN, w, 0, 0, 0, 0, t->pot, 0);
        t->seat[w].stack += t->pot;
        finish(t);
        return;
}

/* Turns up @p seat's cards not yet seen. */
static void show_hand(pk_table_t *t, int seat) {
        pk_seat_t *s = &t->seat[seat];
        for (int i = 0; i < s->count; ++i) {
                if (s->up & (1u << i)) continue;
                s->up |= (uint8_t)(1u << i);
                emit(t, PK_EV_SHOW, seat, i, s->cards[i], 1, 0, 0, 0);
        }
        s->shown = 1;
        return;
}

/* The seat to show first: the last aggressor, else the button's left. */
static int show_start(const pk_table_t *t) {
        if (t->aggressor != PK_NOBODY && t->seat[t->aggressor].in_hand) return t->aggressor;
        return next_in_hand(t, t->button);
}

static void show_all(pk_table_t *t) {
        int s = show_start(t);
        for (int i = 0; i < PK_SEATS; ++i, s = next_of(s))
                if (t->seat[s].in_hand) show_hand(t, s);
        return;
}

/* All in with nobody left to bet: the hands up, the rest dealt, the showdown. */
static void run_out(pk_table_t *t) {
        show_all(t);
        const int rounds = rounds_of(t->rules.game);
        for (; t->round < rounds; ++t->round) deal_street(t, t->round);
        showdown(t);
        return;
}

static void end_round(pk_table_t *t) {
        collect(t);
        ++t->round;
        t->turn = PK_NOBODY;
        if (t->rules.game == PK_DRAW && t->round == 1) {
                t->phase = PK_DRAWING;
                const int s = next_in_hand(t, t->button);
                t->turn = (int8_t)s;
                emit(t, PK_EV_TURN, s, 0, 0, 0, 0, 0, 0);
                return;
        }
        if (t->round >= rounds_of(t->rules.game)) {
                showdown(t);
                return;
        }
        if (able_count(t) <= 1) {
                run_out(t);
                return;
        }
        deal_street(t, t->round);
        begin_round(t);
        return;
}

/* @p seat's best hand, and which of its cards (and the board's) make it. */
static uint32_t seat_value(const pk_table_t *t, int seat, uint16_t *mask) {
        const pk_seat_t *s = &t->seat[seat];
        uint8_t          all[PK_CARDS + PK_BOARD];
        int              n = 0;
        for (int i = 0; i < s->count; ++i) all[n++] = s->cards[i];
        if (t->rules.game == PK_HOLDEM)
                for (int i = 0; i < t->board_count; ++i) all[n++] = t->board[i];
        const uint32_t v = hand_value(all, n);
        if (mask != NULL) {
                const uint32_t best = n >= 5 ? hand_best(all, n) : (1u << n) - 1u;
                uint16_t       m = 0;
                for (int i = 0; i < n; ++i)
                        if (best & (1u << i)) m |= (uint16_t)(1u << (i < s->count ? i : PK_CARDS + i - s->count));
                *mask = m;
        }
        return v;
}

static void showdown(pk_table_t *t) {
        t->phase = PK_BETTING; /* not waiting until finish() */
        t->turn = PK_NOBODY;
        uint32_t value[PK_SEATS] = {0};
        int      s = show_start(t);
        for (int i = 0; i < PK_SEATS; ++i, s = next_of(s)) {
                if (!t->seat[s].in_hand) continue;
                show_hand(t, s);
                uint16_t mask = 0;
                value[s] = seat_value(t, s, &mask);
                emit(t, PK_EV_HAND, s, 0, 0, 0, 0, 0, value[s])->mask = mask;
        }
        /* The pots: cut at each level a seat still in put in, each for those who put in that much. */
        int32_t level = 0;
        int     pots = 0;
        for (;;) {
                int32_t next = 0;
                for (int k = 0; k < PK_SEATS; ++k) {
                        const pk_seat_t *p = &t->seat[k];
                        if (p->in_hand && p->put > level && (next == 0 || p->put < next)) next = p->put;
                }
                if (next == 0) break;
                int32_t  amount = 0;
                uint32_t eligible = 0;
                for (int k = 0; k < PK_SEATS; ++k) {
                        const pk_seat_t *p = &t->seat[k];
                        const int32_t    upto = p->put < next ? p->put : next;
                        if (upto > level) amount += upto - level;
                        if (p->in_hand && p->put >= next) eligible |= 1u << k;
                }
                level = next;
                /* What a folded seat put in beyond everyone still in (never expected: its bet was
                   called or returned) goes with the last pot. */
                int last = 1;
                for (int k = 0; k < PK_SEATS; ++k)
                        if (t->seat[k].in_hand && t->seat[k].put > level) last = 0;
                if (last)
                        for (int k = 0; k < PK_SEATS; ++k)
                                if (t->seat[k].put > level) amount += t->seat[k].put - level;
                emit(t, PK_EV_POT, PK_NOBODY, pots, 0, 0, 0, amount, eligible);
                uint32_t best = 0, winners = 0;
                for (int k = 0; k < PK_SEATS; ++k) {
                        if (!(eligible & (1u << k))) continue;
                        if (winners == 0 || value[k] > best) {
                                best = value[k];
                                winners = 1u << k;
                        } else if (value[k] == best) {
                                winners |= 1u << k;
                        }
                }
                const int     n = __builtin_popcount(winners);
                const int32_t share = amount / n;
                int32_t       odd = amount % n;
                int           w = t->button;
                for (int i = 0; i < PK_SEATS; ++i) { /* the odd dollars to the first winners left of the button */
                        w = next_of(w);
                        if (!(winners & (1u << w))) continue;
                        const int32_t won = share + (odd > 0 ? 1 : 0);
                        if (odd > 0) --odd;
                        t->seat[w].stack += won;
                        emit(t, PK_EV_WIN, w, pots, 0, 0, 0, won, best);
                }
                ++pots;
        }
        finish(t);
        return;
}

/* After an action: the next to act, or the round's end, or the hand's. */
static void advance(pk_table_t *t, int seat) {
        if (in_hand_count(t) == 1) {
                uncontested(t);
                return;
        }
        continue_from(t, next_of(seat));
        return;
}

int pk_act(pk_table_t *t, int seat, int action, int32_t amount) {
        if (!pk_can(t, seat, action)) return -1;
        pk_seat_t *s = &t->seat[seat];
        switch (action) {
                case PK_FOLD:
                        s->in_hand = 0;
                        for (int i = 0; i < s->count; ++i) to_muck(t, s->cards[i]);
                        break;
                case PK_CHECK: s->acted = 1; break;
                case PK_CALL:
                        put_chips(t, seat, t->cur_bet - s->bet);
                        s->acted = 1;
                        break;
                case PK_RAISE: {
                        const int32_t lo = pk_min_raise(t, seat), hi = pk_max_raise(t, seat);
                        if (!t->rules.no_limit) amount = lo;
                        if (amount < lo || amount > hi) return -1;
                        const int32_t raise = amount - t->cur_bet;
                        const int     full = t->rules.no_limit ? raise >= t->last_raise : amount >= fixed_target(t);
                        put_chips(t, seat, amount - s->bet);
                        if (full) {
                                if (t->rules.no_limit) t->last_raise = raise;
                                ++t->raises;
                                for (int k = 0; k < PK_SEATS; ++k) t->seat[k].acted = 0;
                        }
                        t->cur_bet = amount;
                        t->aggressor = (int8_t)seat;
                        s->acted = 1;
                        break;
                }
                default: return -1;
        }
        emit(t, PK_EV_ACT, seat, 0, 0, s->all_in, action, s->bet, 0);
        advance(t, seat);
        return 0;
}

int pk_discard(pk_table_t *t, int seat, uint32_t mask) {
        if (t->phase != PK_DRAWING || seat != t->turn || seat < 0 || (mask & ~0x1fu) != 0) return -1;
        pk_seat_t *s = &t->seat[seat];
        emit(t, PK_EV_DISCARD, seat, 0, 0, 0, 0, __builtin_popcount(mask), mask);
        uint8_t thrown[5];
        int     n = 0;
        for (int i = 0; i < s->count; ++i) {
                if (!(mask & (1u << i))) continue;
                thrown[n++] = s->cards[i];
                s->cards[i] = draw_card(t);
                emit(t, PK_EV_DEAL, seat, i, s->cards[i], 0, 0, 0, 0);
        }
        for (int i = 0; i < n; ++i) to_muck(t, thrown[i]); /* after: never dealt back to this seat */
        s->drawn = 1;
        for (int i = 1; i < PK_SEATS; ++i) {
                const int k = (seat + i) % PK_SEATS;
                if (t->seat[k].in_hand && !t->seat[k].drawn) {
                        t->turn = (int8_t)k;
                        emit(t, PK_EV_TURN, k, 0, 0, 0, 0, 0, 0);
                        return 0;
                }
        }
        if (able_count(t) >= 2)
                begin_round(t);
        else
                showdown(t);
        return 0;
}

/* A forced bet: a blind or the bring-in (into the round's bet), or an ante (straight to the pot). */
static void post(pk_table_t *t, int seat, int32_t amount, int kind) {
        pk_seat_t *s = &t->seat[seat];
        if (kind == PK_POST_ANTE) {
                const int32_t a = amount < s->stack ? amount : s->stack;
                s->stack -= a;
                s->put += a;
                t->pot += a;
                if (s->stack == 0) s->all_in = 1;
                emit(t, PK_EV_POST, seat, 0, 0, 0, kind, a, 0);
                return;
        }
        const int32_t a = put_chips(t, seat, amount);
        emit(t, PK_EV_POST, seat, 0, 0, s->all_in, kind, a, 0);
        return;
}

/* Stud's bring-in: the lowest up card (the deuce lowest; ties clubs, diamonds, hearts, spades). */
static int lowest_up(const pk_table_t *t) {
        int best = PK_NOBODY, best_key = 0;
        for (int s = 0; s < PK_SEATS; ++s) {
                const pk_seat_t *p = &t->seat[s];
                if (!p->in_hand) continue;
                const uint8_t c = p->cards[p->count - 1];
                const int     key = hand_card_rank(c) * 4 + (int)deck_suit(c);
                if (best == PK_NOBODY || key < best_key) {
                        best = s;
                        best_key = key;
                }
        }
        return best;
}

int pk_start_hand(pk_table_t *t) {
        if (t->phase != PK_WAITING || pk_players(t) < 2) return -1;
        t->event_count = 0;
        ++t->hands;
        for (int s = 0; s < PK_SEATS; ++s) {
                pk_seat_t *p = &t->seat[s];
                p->in_hand = (uint8_t)(p->sitting && p->stack > 0);
                p->all_in = p->acted = p->drawn = p->shown = 0;
                p->count = p->up = 0;
                p->bet = p->put = 0;
                p->started = p->stack;
        }
        t->board_count = 0;
        t->round = 0;
        t->raises = 0;
        t->pot = 0;
        t->cur_bet = 0;
        t->last_raise = t->rules.big_blind;
        t->aggressor = PK_NOBODY;
        t->bring_in = PK_NOBODY;
        t->muck_count = 0;
        if (t->button == PK_NOBODY) { /* the first hand's button: anyone's */
                int k = (int)deck_below(&t->rng, (uint32_t)in_hand_count(t));
                for (int s = 0; s < PK_SEATS; ++s)
                        if (t->seat[s].in_hand && k-- == 0) t->button = (int8_t)s;
        } else {
                t->button = (int8_t)next_in_hand(t, t->button);
        }
        deck_fill(t->deck, 1);
        deck_shuffle(&t->rng, t->deck, DECK_CARDS);
        for (int i = 0; i < t->fixed_count; ++i) /* the fixed cards to the front, in order */
                for (int j = i; j < DECK_CARDS; ++j)
                        if (t->deck[j] == t->fixed[i]) {
                                t->deck[j] = t->deck[i];
                                t->deck[i] = t->fixed[i];
                                break;
                        }
        t->fixed_count = 0;
        t->next = 0;
        t->phase = PK_BETTING;
        emit(t, PK_EV_START, t->button, 0, 0, 0, 0, (int32_t)t->hands, 0);
        if (!is_stud(t)) {
                const int sb = in_hand_count(t) == 2 ? t->button : next_in_hand(t, t->button);
                const int bb = next_in_hand(t, sb);
                post(t, sb, t->rules.small_blind, PK_POST_SMALL);
                post(t, bb, t->rules.big_blind, PK_POST_BIG);
                t->cur_bet = t->rules.big_blind;
                t->raises = 1; /* the big blind is the first bet */
                const int cards = t->rules.game == PK_HOLDEM ? 2 : 5;
                for (int i = 0; i < cards; ++i) deal_round(t, 0);
                continue_from(t, next_of(bb));
                return 0;
        }
        for (int s = 0; s < PK_SEATS; ++s)
                if (t->seat[s].in_hand) post(t, s, t->rules.ante, PK_POST_ANTE);
        emit(t, PK_EV_COLLECT, PK_NOBODY, 0, 0, 0, 0, t->pot, 0);
        deal_round(t, 0);
        if (t->rules.game == PK_STUD7) deal_round(t, 0);
        deal_round(t, 1);
        const int low = lowest_up(t);
        t->bring_in = (int8_t)low;
        if (!t->seat[low].all_in) {
                post(t, low, t->rules.bring_in, PK_POST_BRING_IN);
                t->cur_bet = t->seat[low].bet;
                t->seat[low].acted = 1; /* no option: calls all round end the round */
        }
        continue_from(t, next_of(low));
        return 0;
}

/* ---- Views */

void pk_view(const pk_table_t *t, int seat, pk_view_t *v) {
        memset(v, 0, sizeof(*v));
        v->rules = t->rules;
        v->me = (int8_t)seat;
        v->button = t->button;
        v->turn = t->turn;
        v->phase = t->phase;
        v->round = t->round;
        v->board_count = t->board_count;
        memcpy(v->board, t->board, sizeof(v->board));
        for (int s = 0; s < PK_SEATS; ++s) {
                const pk_seat_t *p = &t->seat[s];
                pk_view_seat_t  *q = &v->seat[s];
                q->sitting = p->sitting;
                q->in_hand = p->in_hand;
                q->all_in = p->all_in;
                q->stack = p->stack;
                q->bet = p->bet;
                q->put = p->put;
                if (!p->in_hand) { /* folded: its up cards remembered */
                        for (int i = 0; i < p->count; ++i)
                                if ((p->up & (1u << i)) && v->dead_count < DECK_CARDS)
                                        v->dead[v->dead_count++] = p->cards[i];
                        continue;
                }
                q->count = p->count;
                for (int i = 0; i < p->count; ++i)
                        q->cards[i] = s == seat || (p->up & (1u << i)) ? p->cards[i] : PK_HIDDEN;
        }
        v->pot = pk_pot_total(t);
        v->to_call = pk_to_call(t, seat);
        v->min_raise = pk_min_raise(t, seat);
        v->max_raise = pk_max_raise(t, seat);
        v->can_raise = (uint8_t)pk_can(t, seat, PK_RAISE);
        return;
}
