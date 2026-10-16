/**
 * @file      luckyduck_poker.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Poker (programs/luckyduck, devplan/luckyduck.md §25): the five cards making a hand;
 * blinds, the order of play, no-limit's raises and the all-in short of one, fixed limit's sizes
 * and cap; uncalled bets, side pots and split pots; stud's antes, bring-in and who acts first;
 * Draw's discards and its reshuffle; what each seat sees; a tournament's places; the ducks' draw,
 * their equity, and thousands of hands of ducks playing each other with every action legal and no
 * chip made or lost.
 */

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>
#include "../../programs/luckyduck/hand.h"
#include "../../programs/luckyduck/poker_bot.h"
#include "../../programs/luckyduck/poker_rules.h"

namespace {

        // A card from its name: "AH", "TC", "9S" (rank, then clubs, diamonds, hearts or spades).
        uint8_t card(const char *name) {
                const char *ranks = "A23456789TJQK", *suits = "CDHS";
                const int   r = int(std::strchr(ranks, name[0]) - ranks), s = int(std::strchr(suits, name[1]) - suits);
                return uint8_t(s * DECK_RANKS + r);
        }

        std::vector<uint8_t> cards(std::initializer_list<const char *> names) {
                std::vector<uint8_t> v;
                for (const char *n : names) v.push_back(card(n));
                return v;
        }

        // A table of @p game with seats' stacks (0: empty), its button at @p button for the first hand.
        void setup(pk_table_t &t, int game, int no_limit, int32_t big_blind, std::initializer_list<int32_t> stacks,
                   int button, int tournament = 0) {
                const pk_rules_t r = pk_stakes(game, no_limit, big_blind);
                pk_init(&t, &r, deck_key(7), tournament);
                int s = 0;
                for (int32_t stack : stacks) {
                        if (stack > 0) pk_sit(&t, s, 1, stack);
                        ++s;
                }
                t.button = int8_t((button + PK_SEATS - 1) % PK_SEATS); // moves to @p button at the deal
        }

        void start(pk_table_t &t, const std::vector<uint8_t> &deck = {}) {
                if (!deck.empty()) pk_fix_deck(&t, deck.data(), int(deck.size()));
                REQUIRE(pk_start_hand(&t) == 0);
        }

        // Every chip at the table (what is in the pot too, while a hand is played).
        int32_t chips(const pk_table_t &t) {
                int32_t sum = 0;
                for (int s = 0; s < PK_SEATS; ++s) sum += t.seat[s].stack + (t.phase == PK_WAITING ? 0 : t.seat[s].put);
                return sum;
        }

        int count_events(const pk_table_t &t, int type) {
                int n = 0;
                for (uint32_t i = 0; i < t.event_count; ++i) n += t.events[i].type == type;
                return n;
        }

        // Everyone in turn checks or calls until the hand ends.
        void check_down(pk_table_t &t) {
                for (int guard = 0; guard < 200 && t.phase != PK_WAITING; ++guard) {
                        if (t.phase == PK_DRAWING) {
                                REQUIRE(pk_discard(&t, t.turn, 0) == 0);
                                continue;
                        }
                        const int s = t.turn;
                        REQUIRE(pk_act(&t, s, pk_can(&t, s, PK_CHECK) ? PK_CHECK : PK_CALL, 0) == 0);
                }
                REQUIRE(t.phase == PK_WAITING);
        }

} // namespace

TEST_CASE("luckyduck poker: the five cards making a hand") {
        deck_rng_t rng;
        deck_seed(&rng, 3);
        uint8_t deck[DECK_CARDS];
        for (int trial = 0; trial < 3000; ++trial) {
                deck_fill(deck, 1);
                deck_shuffle(&rng, deck, DECK_CARDS);
                const int      n = 5 + trial % 3;
                const uint32_t mask = hand_best(deck, n);
                CHECK(__builtin_popcount(mask) == 5);
                uint8_t five[5];
                int     k = 0;
                for (int i = 0; i < n; ++i)
                        if (mask & (1u << i)) five[k++] = deck[i];
                CHECK(hand_value(five, 5) == hand_value(deck, n));
        }
        // Fewer than five: no straight or flush, pairs still count (stud's cards showing).
        const auto two = cards({"9S", "9H"}), four = cards({"2H", "3H", "4H", "5H"});
        CHECK(hand_category(hand_value(two.data(), 2)) == HAND_PAIR);
        CHECK(hand_category(hand_value(four.data(), 4)) == HAND_HIGH_CARD);
}

TEST_CASE("luckyduck poker: Hold'em's blinds and order of play") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {100, 100, 100, 100}, 0);
        start(t);
        CHECK(t.button == 0);
        CHECK(t.seat[1].bet == 1); // the small blind
        CHECK(t.seat[2].bet == 2); // the big blind
        CHECK(t.turn == 3);        // under the gun
        CHECK(t.seat[0].count == 2);
        CHECK(pk_to_call(&t, 3) == 2);
        CHECK(pk_min_raise(&t, 3) == 4);
        CHECK(pk_max_raise(&t, 3) == 100);
        CHECK(pk_act(&t, 2, PK_CALL, 0) == -1);  // not its turn
        CHECK(pk_act(&t, 3, PK_CHECK, 0) == -1); // a bet to call
        REQUIRE(pk_act(&t, 3, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 0, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        // The big blind's option.
        CHECK(t.turn == 2);
        CHECK(pk_can(&t, 2, PK_CHECK));
        CHECK(pk_can(&t, 2, PK_RAISE));
        REQUIRE(pk_act(&t, 2, PK_CHECK, 0) == 0);
        // The flop: the small blind first.
        CHECK(t.board_count == 3);
        CHECK(t.pot == 8);
        CHECK(t.turn == 1);
        CHECK(pk_min_raise(&t, 1) == 2);
        check_down(t);
        CHECK(t.board_count == 5);
        CHECK(chips(t) == 400);
        // The button moves.
        start(t);
        CHECK(t.button == 1);
        CHECK(t.seat[2].bet == 1);
        CHECK(t.seat[3].bet == 2);
        CHECK(t.turn == 0);
}

TEST_CASE("luckyduck poker: heads-up, the button is the small blind") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {100, 0, 0, 100}, 3);
        start(t);
        CHECK(t.seat[3].bet == 1);
        CHECK(t.seat[0].bet == 2);
        CHECK(t.turn == 3); // first before the flop
        REQUIRE(pk_act(&t, 3, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 0, PK_CHECK, 0) == 0);
        CHECK(t.turn == 0); // last after it
}

TEST_CASE("luckyduck poker: no-limit raises, and an all-in short of one") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {200, 200, 200, 13}, 0);
        start(t);
        // Seat 3 under the gun raises to 6: a raise of 4, so the next must be 4 more.
        CHECK(pk_act(&t, 3, PK_RAISE, 3) == -1); // less than the least
        REQUIRE(pk_act(&t, 3, PK_RAISE, 6) == 0);
        CHECK(pk_min_raise(&t, 0) == 10);
        CHECK(pk_act(&t, 0, PK_RAISE, 9) == -1);
        REQUIRE(pk_act(&t, 0, PK_RAISE, 10) == 0);
        REQUIRE(pk_act(&t, 1, PK_FOLD, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_FOLD, 0) == 0);
        // Seat 3 goes all in for 13: 3 more, short of a full raise of 4.
        CHECK(pk_max_raise(&t, 3) == 13);
        CHECK(pk_min_raise(&t, 3) == 13);
        REQUIRE(pk_act(&t, 3, PK_RAISE, 13) == 0);
        CHECK(t.seat[3].all_in);
        // Seat 0 already acted since the last full raise: it may call, not raise.
        CHECK(t.turn == 0);
        CHECK(pk_to_call(&t, 0) == 3);
        CHECK_FALSE(pk_can(&t, 0, PK_RAISE));
        REQUIRE(pk_act(&t, 0, PK_CALL, 0) == 0);
        // Nobody left to bet: dealt out and shown.
        CHECK(t.phase == PK_WAITING);
        CHECK(t.board_count == 5);
        CHECK(count_events(t, PK_EV_SHOW) == 4);
        CHECK(chips(t) == 613);
}

TEST_CASE("luckyduck poker: fixed limit's sizes and its cap") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 0, 2, {500, 500, 500}, 0);
        start(t); // 0 button, 1 small, 2 big: 0 first
        CHECK(pk_min_raise(&t, 0) == 4);
        CHECK(pk_max_raise(&t, 0) == 4);
        REQUIRE(pk_act(&t, 0, PK_RAISE, 999) == 0); // the amount is the limit's
        CHECK(t.seat[0].bet == 4);
        REQUIRE(pk_act(&t, 1, PK_RAISE, 0) == 0); // 6
        REQUIRE(pk_act(&t, 2, PK_RAISE, 0) == 0); // 8: the fourth bet
        CHECK(t.seat[2].bet == 8);
        CHECK_FALSE(pk_can(&t, 0, PK_RAISE)); // capped
        REQUIRE(pk_act(&t, 0, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        CHECK(t.pot == 24);
        CHECK(pk_min_raise(&t, 1) == 2); // the flop's small bet
        REQUIRE(pk_act(&t, 1, PK_CHECK, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CHECK, 0) == 0);
        REQUIRE(pk_act(&t, 0, PK_CHECK, 0) == 0);
        CHECK(t.board_count == 4);
        CHECK(pk_min_raise(&t, 1) == 4); // the turn's big bet
}

TEST_CASE("luckyduck poker: an uncalled bet comes back") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {100, 100, 100}, 0);
        start(t);
        REQUIRE(pk_act(&t, 0, PK_RAISE, 30) == 0);
        REQUIRE(pk_act(&t, 1, PK_FOLD, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_FOLD, 0) == 0);
        CHECK(t.phase == PK_WAITING);
        CHECK(count_events(t, PK_EV_RETURN) == 1);
        CHECK(count_events(t, PK_EV_SHOW) == 0); // won unseen
        CHECK(t.seat[0].stack == 103);
        CHECK(t.seat[1].stack == 99);
        CHECK(t.seat[2].stack == 98);
}

TEST_CASE("luckyduck poker: side pots") {
        pk_table_t t;
        // Seat 1 (50) has the best hand, seat 2 (100) the second, seat 3 (200) the worst; seat 0
        // folds. Dealt from the button's left (seat 1), a card round each.
        setup(t, PK_HOLDEM, 1, 2, {300, 50, 100, 200}, 0);
        start(t, cards({"AS", "KS", "2C", "4D", "AH", "KH", "7C", "QD", //
                        "AD", "KD", "9H", "3S", "8C"}));
        // Under the gun (seat 3) all in; the button folds, both blinds call all in.
        REQUIRE(pk_act(&t, 3, PK_RAISE, 200) == 0);
        REQUIRE(pk_act(&t, 0, PK_FOLD, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CALL, 0) == 0);
        CHECK(t.phase == PK_WAITING);
        CHECK(count_events(t, PK_EV_POT) == 2);
        CHECK(count_events(t, PK_EV_RETURN) == 1);
        // The main pot, 50 × 3, to seat 1's Aces; the side pot, 50 × 2, to seat 2's Kings; seat 3's
        // 100 beyond came back uncalled.
        CHECK(t.seat[1].stack == 150);
        CHECK(t.seat[2].stack == 100);
        CHECK(t.seat[3].stack == 100);
        CHECK(t.seat[0].stack == 300);
        CHECK(chips(t) == 650);
}

TEST_CASE("luckyduck poker: a split pot's odd dollar") {
        // The board is a straight to the King; the button folds, both blinds play the board.
        const auto deck = cards({"2C", "3D", "KS", "2H", "3S", "KD", "9C", "TS", "JD", "QH", "KC"});
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {100, 100, 100}, 0);
        start(t, deck);
        REQUIRE(pk_act(&t, 0, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_RAISE, 5) == 0);
        REQUIRE(pk_act(&t, 0, PK_FOLD, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        check_down(t);
        CHECK(count_events(t, PK_EV_WIN) == 2);
        CHECK(t.seat[1].stack == 101); // 12 split
        CHECK(t.seat[2].stack == 101);
        // A pot of 33 between the button and the big blind: the extra dollar to the first winner
        // left of the button (the big blind; the small blind folded).
        setup(t, PK_HOLDEM, 1, 2, {100, 100, 100}, 0);
        start(t, deck);
        REQUIRE(pk_act(&t, 0, PK_RAISE, 5) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 1, PK_CHECK, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_RAISE, 4) == 0);
        CHECK(pk_min_raise(&t, 0) == 8);
        REQUIRE(pk_act(&t, 0, PK_RAISE, 9) == 0);
        REQUIRE(pk_act(&t, 1, PK_FOLD, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CALL, 0) == 0);
        check_down(t);
        CHECK(t.seat[2].stack == 103);
        CHECK(t.seat[0].stack == 102);
        CHECK(chips(t) == 300);
}

TEST_CASE("luckyduck poker: stud's antes, bring-in and who acts first") {
        pk_table_t t;
        // Seven-Card Stud, a $2 small bet: antes $1 (at least), bring-in $1. Dealt from seat 1:
        // two down rounds, then the up cards: seat 1 9H, seat 2 2D, seat 0 2C (the lowest: clubs).
        setup(t, PK_STUD7, 0, 4, {100, 100, 100}, 0);
        CHECK(t.rules.ante == 1);
        CHECK(t.rules.bring_in == 2);
        start(t, cards({"AS", "KS", "QS", "AH", "KH", "QH", "9H", "2D", "2C", //
                        "5C", "KD", "3H"}));
        CHECK(t.pot == 3);
        CHECK(t.bring_in == 0);
        CHECK(t.seat[0].bet == 2);
        CHECK(t.turn == 1);
        CHECK(pk_min_raise(&t, 1) == 4); // completing to the small bet
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CALL, 0) == 0);
        // The bring-in has no option: the round is over.
        CHECK(t.round == 1);
        CHECK(t.seat[0].count == 4);
        // Fourth street, the best showing first: seat 1 9H 5C, seat 2 2D KD, seat 0 2C 3H.
        CHECK(t.turn == 2);
        check_down(t);
        CHECK(t.seat[0].count == 7);
        CHECK(chips(t) == 300);
}

TEST_CASE("luckyduck poker: Five-Card Stud and Draw") {
        pk_table_t t;
        setup(t, PK_STUD5, 0, 2, {50, 50, 50, 50, 50, 50}, 0);
        start(t);
        CHECK(t.seat[3].count == 2);
        CHECK(t.seat[3].up == 2);
        check_down(t);
        CHECK(t.seat[3].count == 5);
        CHECK(chips(t) == 300);

        // Draw with six seats each throwing five: the deck runs out and the discards are shuffled in.
        setup(t, PK_DRAW, 0, 2, {50, 50, 50, 50, 50, 50}, 0);
        start(t);
        for (int s = 0; s < 6; ++s) CHECK(t.seat[s].count == 5);
        for (int guard = 0; guard < 20 && t.phase == PK_BETTING; ++guard)
                REQUIRE(pk_act(&t, t.turn, pk_can(&t, t.turn, PK_CHECK) ? PK_CHECK : PK_CALL, 0) == 0);
        REQUIRE(t.phase == PK_DRAWING);
        CHECK(t.turn == 1);
        CHECK(pk_discard(&t, 2, 1) == -1); // not its turn
        bool seen[DECK_CARDS] = {};
        for (int k = 0; k < 6; ++k) REQUIRE(pk_discard(&t, t.turn, 0x1f) == 0);
        for (int s = 0; s < 6; ++s)
                for (int i = 0; i < 5; ++i) {
                        CHECK_FALSE(seen[t.seat[s].cards[i]]); // no card twice
                        seen[t.seat[s].cards[i]] = true;
                }
        CHECK(t.phase == PK_BETTING);
        CHECK(t.turn == 1);
        check_down(t);
        CHECK(chips(t) == 300);
}

TEST_CASE("luckyduck poker: a seat sees only its own down cards") {
        pk_table_t t;
        setup(t, PK_STUD7, 0, 4, {100, 100, 100}, 0);
        start(t);
        for (uint32_t i = 0; i < t.event_count; ++i) {
                const pk_event_t e = pk_event_for(&t, i, 0);
                if (e.type != PK_EV_DEAL) continue;
                if (e.seat == 0 || e.up)
                        CHECK(e.card != PK_HIDDEN);
                else
                        CHECK(e.card == PK_HIDDEN);
        }
        pk_view_t v;
        pk_view(&t, 1, &v);
        CHECK(v.seat[1].cards[0] != PK_HIDDEN);
        CHECK(v.seat[0].cards[0] == PK_HIDDEN);
        CHECK(v.seat[0].cards[2] == t.seat[0].cards[2]); // up
        // Shown at the showdown: everyone's.
        check_down(t);
        int shown = 0;
        for (uint32_t i = 0; i < t.event_count; ++i) shown += pk_event_for(&t, i, 0).type == PK_EV_SHOW;
        CHECK(shown == 9); // three down cards a seat
}

TEST_CASE("luckyduck poker: a tournament's places") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {40, 20, 100}, 0, 1);
        // Seat 2 has the nuts; seats 0 and 1 go all in and lose in the same hand: seat 0 started
        // with more, so finishes higher (second), seat 1 third... then seat 2 has won.
        start(t, cards({"2C", "AS", "3D", "7H", "AH", "8S", "AD", "AC", "KD", "QH", "JS"}));
        REQUIRE(pk_act(&t, 0, PK_RAISE, 40) == 0);
        REQUIRE(pk_act(&t, 1, PK_CALL, 0) == 0);
        REQUIRE(pk_act(&t, 2, PK_CALL, 0) == 0);
        CHECK(t.phase == PK_WAITING);
        CHECK(t.seat[0].place == 2);
        CHECK(t.seat[1].place == 3);
        CHECK(t.seat[2].place == 1);
        CHECK(t.seat[2].stack == 160);
        CHECK(pk_players(&t) == 1);
        CHECK(pk_start_hand(&t) == -1);
}

TEST_CASE("luckyduck poker: the ducks' draw") {
        auto discard = [](std::initializer_list<const char *> names) {
                const auto c = cards(names);
                return pk_bot_discard(c.data());
        };
        CHECK(discard({"2C", "3C", "4C", "5C", "9C"}) == 0);     // a flush stands pat
        CHECK(discard({"9S", "9H", "2C", "5D", "KH"}) == 0x1cu); // a pair: three new
        CHECK(discard({"9S", "9H", "2C", "2D", "KH"}) == 0x10u); // two pair: the kicker
        CHECK(discard({"AS", "3S", "7S", "QS", "KH"}) == 0x10u); // four to a flush
        CHECK(discard({"5S", "6H", "7C", "8D", "KH"}) == 0x10u); // four in a row
        CHECK(discard({"AS", "3D", "7C", "QH", "2H"}) == 0x16u); // the Ace and Queen kept
}

TEST_CASE("luckyduck poker: equity") {
        pk_table_t t;
        setup(t, PK_HOLDEM, 1, 2, {100, 100}, 0);
        start(t, cards({"AS", "7C", "AH", "2D"})); // seat 1 Aces, seat 0 seven-deuce
        pk_view_t   v;
        pk_equity_t e;
        pk_view(&t, 1, &v);
        pk_equity_start(&e, &v, deck_key(5), 6000);
        while (!pk_equity_step(&e, 500)) {}
        CHECK(std::fabs(pk_equity_value(&e) - 0.85f) < 0.03f); // Aces against a random hand
        pk_view(&t, 0, &v);
        pk_equity_start(&e, &v, deck_key(6), 6000);
        while (!pk_equity_step(&e, 500)) {}
        CHECK(std::fabs(pk_equity_value(&e) - 0.35f) < 0.03f);
}

TEST_CASE("luckyduck poker: ducks playing each other") {
        static const pk_style_t styles[PK_SEATS] = {{30, 70, 30}, {70, 30, 10}, {50, 50, 40},
                                                    {80, 80, 60}, {15, 40, 5},  {50, 60, 20}};
        deck_rng_t              rng;
        deck_seed(&rng, 11);
        for (int game = 0; game < PK_GAMES; ++game) {
                for (int nl = 0; nl <= (game == PK_HOLDEM ? 1 : 0); ++nl) {
                        pk_table_t       t;
                        const pk_rules_t r = pk_stakes(game, nl, 4);
                        pk_init(&t, &r, deck_key(uint32_t(100 + game * 2 + nl)), 0);
                        int32_t total = 0;
                        for (int s = 0; s < PK_SEATS; ++s) {
                                const int32_t stack = 20 + int32_t(deck_below(&rng, 400));
                                pk_sit(&t, s, 1, stack);
                                total += stack;
                        }
                        int hands = 0;
                        for (int h = 0; h < 400; ++h) {
                                for (int s = 0; s < PK_SEATS; ++s) // a broke duck rebuys
                                        if (t.seat[s].stack == 0) {
                                                pk_add_chips(&t, s, 200);
                                                total += 200;
                                        }
                                REQUIRE(pk_start_hand(&t) == 0);
                                ++hands;
                                for (int guard = 0; t.phase != PK_WAITING; ++guard) {
                                        REQUIRE(guard < 500);
                                        const int s = t.turn;
                                        if (t.phase == PK_DRAWING) {
                                                REQUIRE(pk_discard(&t, s, pk_bot_discard(t.seat[s].cards)) == 0);
                                                continue;
                                        }
                                        pk_view_t v;
                                        pk_view(&t, s, &v);
                                        const float       equity = float(deck_below(&rng, 1000)) / 1000.0f;
                                        const pk_choice_t c = pk_bot_decide(&v, &styles[s], equity, deck_next(&rng));
                                        REQUIRE(pk_act(&t, s, c.action, c.amount) == 0);
                                }
                                int32_t sum = 0;
                                for (int s = 0; s < PK_SEATS; ++s) sum += t.seat[s].stack;
                                REQUIRE(sum == total);
                                REQUIRE(t.event_count < PK_EVENTS);
                        }
                        CHECK(hands == 400);
                }
        }
}

TEST_CASE("luckyduck poker: tournaments of ducks played out") {
        static const pk_style_t style = {60, 60, 30};
        deck_rng_t              rng;
        deck_seed(&rng, 21);
        for (int game = 0; game < PK_GAMES; ++game) {
                pk_table_t t;
                pk_rules_t r = pk_stakes(game, 1, 20);
                pk_init(&t, &r, deck_key(uint32_t(300 + game)), 1);
                for (int s = 0; s < PK_SEATS; ++s) pk_sit(&t, s, 1, 1500);
                int hands = 0;
                while (pk_players(&t) > 1) {
                        REQUIRE(hands < 3000);
                        if (hands % 8 == 0) { // the blinds rise
                                r = pk_stakes(game, 1, 20 << (hands / 8 < 8 ? hands / 8 : 8));
                                REQUIRE(pk_set_rules(&t, &r) == 0);
                        }
                        REQUIRE(pk_start_hand(&t) == 0);
                        ++hands;
                        while (t.phase != PK_WAITING) {
                                const int s = t.turn;
                                if (t.phase == PK_DRAWING) {
                                        REQUIRE(pk_discard(&t, s, pk_bot_discard(t.seat[s].cards)) == 0);
                                        continue;
                                }
                                pk_view_t v;
                                pk_view(&t, s, &v);
                                const pk_choice_t c = pk_bot_decide(&v, &style, float(deck_below(&rng, 1000)) / 1000.0f,
                                                                    deck_next(&rng));
                                REQUIRE(pk_act(&t, s, c.action, c.amount) == 0);
                        }
                }
                int places = 0;
                for (int s = 0; s < PK_SEATS; ++s) places |= 1 << t.seat[s].place;
                CHECK(places == 0x7e); // 1 to 6, once each
                int32_t sum = 0;
                for (int s = 0; s < PK_SEATS; ++s) sum += t.seat[s].stack;
                CHECK(sum == 9000);
        }
}
