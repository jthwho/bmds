/**
 * @file      luckyduck_blackjack.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's cards and Blackjack rules (programs/luckyduck, devplan/luckyduck.md §16): the
 * random numbers and shuffles, hand values, the deal, every action and its refusals, insurance and
 * the peek, the dealer's play under both soft-17 rules, payouts, the shoe and its cut card, the
 * log as a seat sees it, determinism, and basic strategy.
 */

#include <doctest.h>

#include <cstring>
#include <initializer_list>
#include <vector>
#include "../../programs/luckyduck/blackjack_rules.h"

namespace {

        // Cards by rank (1 the Ace ... 13 the King), spades unless said.
        uint8_t c(int rank, int suit = 3) {
                return uint8_t(suit * 13 + rank - 1);
        }

        // A table with the shoe's next cards set (in the order dealt) and players at @p seats with $1000.
        void stack(bj_table_t &t, std::initializer_list<uint8_t> next, std::initializer_list<int> seats,
                   bj_rules_t rules = bj_default_rules()) {
                bj_init(&t, &rules, 1);
                size_t i = 0;
                for (uint8_t card : next) t.shoe[i++] = card;
                t.next = 0;
                t.cut = BJ_SHOE - 1;
                for (int s : seats) bj_sit(&t, s, 1, 1000);
        }

        int count(const bj_table_t &t, uint8_t type) {
                int n = 0;
                for (uint32_t i = 0; i < t.event_count; ++i) n += t.events[i].type == type;
                return n;
        }

        const bj_event_t *last(const bj_table_t &t, uint8_t type) {
                for (uint32_t i = t.event_count; i-- > 0;)
                        if (t.events[i].type == type) return &t.events[i];
                return nullptr;
        }

} // namespace

TEST_CASE("Lucky Duck deck: seeded randomness, unbiased ranges, shuffles keep the cards") {
        deck_rng_t a, b;
        deck_seed(&a, 42);
        deck_seed(&b, 42);
        for (int i = 0; i < 100; ++i) CHECK(deck_next(&a) == deck_next(&b));
        deck_seed(&b, 43);
        CHECK(deck_next(&a) != deck_next(&b));
        int seen[6] = {};
        for (int i = 0; i < 6000; ++i) ++seen[deck_below(&a, 6)];
        for (int v : seen) CHECK((v > 850 && v < 1150));
        uint8_t cards[BJ_SHOE];
        deck_fill(cards, BJ_DECKS);
        deck_shuffle(&a, cards, BJ_SHOE);
        int each[DECK_CARDS] = {};
        for (uint8_t card : cards) ++each[card];
        for (int n : each) CHECK(n == BJ_DECKS);
}

TEST_CASE("Lucky Duck blackjack: hand totals, soft and hard, blackjacks") {
        int           soft = 0;
        const uint8_t a6[] = {c(1), c(6)};
        CHECK(bj_total(a6, 2, &soft) == 17);
        CHECK(soft == 1);
        const uint8_t a6k[] = {c(1), c(6), c(13)};
        CHECK(bj_total(a6k, 3, &soft) == 17);
        CHECK(soft == 0);
        const uint8_t aa9[] = {c(1), c(1), c(9)};
        CHECK(bj_total(aa9, 3, &soft) == 21);
        CHECK(soft == 1);
        const uint8_t hidden[] = {c(10), BJ_HIDDEN};
        CHECK(bj_total(hidden, 2, nullptr) == 10);
        bj_hand_t h = {};
        h.cards[0] = c(1);
        h.cards[1] = c(12);
        h.count = 2;
        CHECK(bj_is_blackjack(&h));
        h.split = 1;
        CHECK_FALSE(bj_is_blackjack(&h)); // 21 after a split isn't one
}

TEST_CASE("Lucky Duck blackjack: bets, the deal's order, a stand and a win") {
        bj_table_t t;
        // Seats 0 and 2: 0 gets 10 and 9, 2 gets 8 and 8; the dealer 7 up, 10 down.
        stack(t, {c(10), c(8), c(7), c(9), c(8, 1), c(10, 2), c(5), c(2)}, {0, 2});
        CHECK(bj_bet(&t, 0, 1001) == -1); // more than the stack
        CHECK(bj_bet(&t, 1, 10) == -1);   // no one sits there
        CHECK(bj_deal(&t) == -1);         // no bets
        // Under the minimum or over the maximum: placed (chip by chip), but not dealt.
        REQUIRE(bj_bet(&t, 0, 4) == 0);
        CHECK_FALSE(bj_bet_ok(&t, 0));
        CHECK(bj_deal(&t) == -1);
        REQUIRE(bj_bet(&t, 0, 501) == 0);
        CHECK(bj_deal(&t) == -1);
        REQUIRE(bj_bet(&t, 0, 20) == 0);
        REQUIRE(bj_bet(&t, 2, 10) == 0);
        CHECK(t.seat[0].stack == 980);
        REQUIRE(bj_deal(&t) == 0);
        CHECK(t.seat[0].hand[0].cards[0] == c(10));
        CHECK(t.seat[2].hand[0].cards[0] == c(8));
        CHECK(t.dealer.cards[0] == c(7));
        CHECK(t.dealer.cards[1] == c(10, 2));
        CHECK(t.phase == BJ_PLAYING);
        CHECK(t.turn_seat == 0);
        CHECK(bj_act(&t, 2, BJ_STAND) == -1); // not its turn
        REQUIRE(bj_act(&t, 0, BJ_STAND) == 0);
        CHECK(t.turn_seat == 2);
        // 8, 8 against a 7: hit gets the 5 (21: stands by itself), and the dealer stands on 17.
        REQUIRE(bj_act(&t, 2, BJ_HIT) == 0);
        CHECK(t.phase == BJ_SETTLED);
        CHECK(t.seat[0].hand[0].result == BJ_WIN); // 19 against 17
        CHECK(t.seat[0].stack == 1020);
        CHECK(t.seat[2].hand[0].result == BJ_WIN); // 21
        CHECK(t.seat[2].stack == 1010);
        CHECK(count(t, BJ_EV_ROUND_END) == 1);
        bj_next_round(&t);
        CHECK(t.phase == BJ_BETTING);
        CHECK(t.seat[0].hand[0].bet == 0);
}

TEST_CASE("Lucky Duck blackjack: busting, the dealer drawing and busting, pushes") {
        bj_table_t t;
        // Seat 0: 10, 6, then a King (bust). Seat 1: 10, 7 stands. Dealer: 6 up, 10 down, draws 8.
        stack(t, {c(10), c(10, 1), c(6), c(6, 1), c(7), c(10, 2), c(13), c(8)}, {0, 1});
        bj_bet(&t, 0, 10);
        bj_bet(&t, 1, 10);
        REQUIRE(bj_deal(&t) == 0);
        REQUIRE(bj_act(&t, 0, BJ_HIT) == 0);
        CHECK(last(t, BJ_EV_BUST)->seat == 0);
        CHECK(t.turn_seat == 1);
        REQUIRE(bj_act(&t, 1, BJ_STAND) == 0);
        CHECK(t.dealer.count == 3);
        CHECK(last(t, BJ_EV_BUST)->seat == BJ_DEALER);
        CHECK(t.seat[0].hand[0].result == BJ_LOSE); // busted first: loses though the dealer busts
        CHECK(t.seat[1].hand[0].result == BJ_WIN);
        // A push: 10, 8 against 10, 8.
        stack(t, {c(10), c(10, 1), c(8), c(8, 1)}, {0});
        bj_bet(&t, 0, 10);
        bj_deal(&t);
        REQUIRE(bj_act(&t, 0, BJ_STAND) == 0);
        CHECK(t.seat[0].hand[0].result == BJ_PUSH);
        CHECK(t.seat[0].stack == 1000);
}

TEST_CASE("Lucky Duck blackjack: the soft-17 rule") {
        for (int hit = 0; hit < 2; ++hit) {
                bj_rules_t rules = bj_default_rules();
                rules.hit_soft_17 = uint8_t(hit);
                bj_table_t t;
                // Seat 0: 10, 8; the dealer 6 up, an Ace down (soft 17), then a 3.
                stack(t, {c(10), c(6), c(8), c(1), c(3)}, {0}, rules);
                bj_bet(&t, 0, 10);
                REQUIRE(bj_deal(&t) == 0);
                REQUIRE(bj_act(&t, 0, BJ_STAND) == 0);
                CHECK(t.dealer.count == (hit ? 3 : 2));
                CHECK(t.seat[0].hand[0].result == (hit ? BJ_LOSE : BJ_WIN)); // 18 against 20 or 17
        }
}

TEST_CASE("Lucky Duck blackjack: blackjacks pay 3:2, the dealer's peek") {
        bj_table_t t;
        // Seat 0: Ace, King (blackjack); the dealer 9, 7.
        stack(t, {c(1), c(9), c(13), c(7)}, {0});
        bj_bet(&t, 0, 10);
        REQUIRE(bj_deal(&t) == 0);
        CHECK(t.phase == BJ_SETTLED); // nothing to decide
        CHECK(t.seat[0].hand[0].result == BJ_BLACKJACK_WIN);
        CHECK(t.seat[0].stack == 1015);
        CHECK(t.dealer.count == 2); // no hand left to draw against
        // The dealer's ten up and a blackjack under it: the round ends at once, the player's 20 lost.
        stack(t, {c(10), c(10, 1), c(10, 2), c(1)}, {0});
        bj_bet(&t, 0, 10);
        REQUIRE(bj_deal(&t) == 0);
        CHECK(t.phase == BJ_SETTLED);
        CHECK(t.hole_up);
        CHECK(last(t, BJ_EV_BLACKJACK)->seat == BJ_DEALER);
        CHECK(t.seat[0].hand[0].result == BJ_LOSE);
        // Both blackjacks: a push.
        stack(t, {c(1), c(10), c(12), c(1, 1)}, {0});
        bj_bet(&t, 0, 10);
        REQUIRE(bj_deal(&t) == 0);
        CHECK(t.seat[0].hand[0].result == BJ_PUSH);
        CHECK(t.seat[0].stack == 1000);
        // An odd bet's 3:2 rounds down: 5 pays 7.
        stack(t, {c(1), c(9), c(13), c(7)}, {0});
        bj_bet(&t, 0, 5);
        bj_deal(&t);
        CHECK(t.seat[0].stack == 1007);
}

TEST_CASE("Lucky Duck blackjack: insurance against an Ace") {
        bj_table_t t;
        // Seats 0 and 1 with 20s; the dealer Ace up, King down: insurance pays 2:1, the hands lose.
        stack(t, {c(10), c(10, 1), c(1), c(10, 2), c(10, 3), c(13)}, {0, 1});
        bj_bet(&t, 0, 20);
        bj_bet(&t, 1, 20);
        REQUIRE(bj_deal(&t) == 0);
        CHECK(t.phase == BJ_INSURANCE);
        CHECK(bj_insurance_seat(&t) == 0);
        CHECK(bj_insure(&t, 1, 1) == -1); // not its turn to answer
        REQUIRE(bj_insure(&t, 0, 1) == 0);
        CHECK(t.seat[0].stack == 970);
        REQUIRE(bj_insure(&t, 1, 0) == 0);
        CHECK(t.phase == BJ_SETTLED);
        CHECK(t.seat[0].stack == 1000); // the insurance's 30 back, the bet lost: even
        CHECK(t.seat[1].stack == 980);
        CHECK(last(t, BJ_EV_INSURED)->amount == 20);
        // No blackjack under it: the insurance is lost and play goes on.
        stack(t, {c(10), c(1), c(10, 1), c(6)}, {0});
        bj_bet(&t, 0, 20);
        bj_deal(&t);
        REQUIRE(bj_insure(&t, 0, 1) == 0);
        CHECK(t.phase == BJ_PLAYING);
        CHECK(last(t, BJ_EV_INSURED)->amount == -10);
        CHECK(t.seat[0].stack == 970);
}

TEST_CASE("Lucky Duck blackjack: doubling and surrender") {
        bj_table_t t;
        // 6, 5 doubles and gets a 10 (21) against 10, 7.
        stack(t, {c(6), c(10), c(5), c(7), c(10, 1)}, {0});
        bj_bet(&t, 0, 50);
        bj_deal(&t);
        CHECK(bj_can(&t, 0, BJ_DOUBLE));
        REQUIRE(bj_act(&t, 0, BJ_DOUBLE) == 0);
        CHECK(t.seat[0].hand[0].count == 3);
        CHECK(t.seat[0].hand[0].bet == 100);
        CHECK(t.seat[0].hand[0].result == BJ_WIN);
        CHECK(t.seat[0].stack == 1100);
        // A double needs the stack: $10 left can't double a $20 bet.
        stack(t, {c(6), c(10), c(5), c(7)}, {0});
        bj_sit(&t, 0, 1, 30);
        bj_bet(&t, 0, 20);
        bj_deal(&t);
        CHECK_FALSE(bj_can(&t, 0, BJ_DOUBLE));
        // Surrender: half back; not after a hit; not when the rules say no.
        stack(t, {c(10), c(10, 1), c(6), c(7)}, {0});
        bj_bet(&t, 0, 20);
        bj_deal(&t);
        REQUIRE(bj_act(&t, 0, BJ_SURRENDER) == 0);
        CHECK(t.seat[0].hand[0].result == BJ_SURRENDERED);
        CHECK(t.seat[0].stack == 990);
        stack(t, {c(5), c(10), c(4), c(7), c(2)}, {0});
        bj_bet(&t, 0, 20);
        bj_deal(&t);
        bj_act(&t, 0, BJ_HIT);
        CHECK_FALSE(bj_can(&t, 0, BJ_SURRENDER));
        CHECK_FALSE(bj_can(&t, 0, BJ_DOUBLE));
        bj_rules_t no = bj_default_rules();
        no.surrender = 0;
        stack(t, {c(10), c(10, 1), c(6), c(7)}, {0}, no);
        bj_bet(&t, 0, 20);
        bj_deal(&t);
        CHECK_FALSE(bj_can(&t, 0, BJ_SURRENDER));
}

TEST_CASE("Lucky Duck blackjack: splits, split Aces, four hands") {
        bj_table_t t;
        // 8, 8 against 6, 10: split; the first gets a 3 (11) and doubles onto a 9; the second a 10, stands.
        stack(t, {c(8), c(6), c(8, 1), c(10), c(3), c(10, 1), c(9), c(9, 1)}, {0});
        bj_bet(&t, 0, 10);
        bj_deal(&t);
        REQUIRE(bj_act(&t, 0, BJ_SPLIT) == 0);
        CHECK(t.seat[0].hands == 2);
        CHECK(t.seat[0].stack == 980);
        CHECK(t.seat[0].hand[0].count == 2);
        CHECK(t.seat[0].hand[1].count == 2);
        CHECK(t.turn_hand == 0);
        CHECK_FALSE(bj_can(&t, 0, BJ_SURRENDER)); // not after a split
        REQUIRE(bj_act(&t, 0, BJ_DOUBLE) == 0);   // doubling after a split
        CHECK(t.turn_hand == 1);
        REQUIRE(bj_act(&t, 0, BJ_STAND) == 0);
        CHECK(t.phase == BJ_SETTLED);
        // The dealer's 16 draws a 9 and busts: both win (20 at $20, 18 at $10).
        CHECK(t.seat[0].hand[0].result == BJ_WIN);
        CHECK(t.seat[0].hand[1].result == BJ_WIN);
        CHECK(t.seat[0].stack == 1030);
        // Split Aces: one card each, done; 21 there isn't a blackjack (paid 1:1).
        stack(t, {c(1), c(9), c(1, 1), c(8), c(13), c(5), c(10)}, {0});
        bj_bet(&t, 0, 10);
        bj_deal(&t);
        REQUIRE(bj_act(&t, 0, BJ_SPLIT) == 0);
        CHECK(t.phase == BJ_SETTLED);               // both Aces done: the dealer's 17 stands
        CHECK(t.seat[0].hand[0].result == BJ_WIN);  // 21: paid even money
        CHECK(t.seat[0].hand[1].result == BJ_LOSE); // 16
        CHECK(t.seat[0].stack == 1000);
}

TEST_CASE("Lucky Duck blackjack: the cut card, determinism, the log as a seat sees it") {
        bj_rules_t r = bj_default_rules();
        bj_table_t a, b;
        bj_init(&a, &r, 7);
        bj_init(&b, &r, 7);
        CHECK(std::memcmp(a.shoe, b.shoe, sizeof(a.shoe)) == 0);
        CHECK(a.cut > BJ_SHOE * 3 / 4 - 11);
        CHECK(a.cut < BJ_SHOE * 3 / 4 + 11);
        // Rounds until the cut card comes out, each standing: then the next round shuffles first.
        bj_sit(&a, 2, 1, 100000);
        int rounds = 0;
        while (!a.reshuffle && rounds < 200) {
                REQUIRE(bj_bet(&a, 2, 10) == 0);
                REQUIRE(bj_deal(&a) == 0);
                if (a.phase == BJ_INSURANCE) bj_insure(&a, 2, 0);
                while (a.phase == BJ_PLAYING) REQUIRE(bj_act(&a, 2, BJ_STAND) == 0);
                bj_next_round(&a);
                ++rounds;
        }
        CHECK(a.reshuffle);
        bj_bet(&a, 2, 10);
        bj_deal(&a);
        CHECK(a.events[1].type == BJ_EV_SHUFFLE); // after the bet, before the cards
        CHECK(a.next < 20);
        // The hole card: hidden from the seats until turned, never from the dealer.
        bj_table_t t;
        stack(t, {c(10), c(5), c(9), c(12)}, {2});
        bj_bet(&t, 2, 10);
        bj_deal(&t);
        bool found = false;
        for (uint32_t i = 0; i < t.event_count; ++i) {
                const bj_event_t e = bj_event_for(&t, i, 2);
                if (e.type == BJ_EV_DEAL && e.seat == BJ_DEALER && !e.up) {
                        CHECK(e.card == BJ_HIDDEN);
                        CHECK(bj_event_for(&t, i, BJ_DEALER).card == c(12));
                        found = true;
                }
        }
        CHECK(found);
        bj_act(&t, 2, BJ_STAND);
        CHECK(last(t, BJ_EV_HOLE)->card == c(12));
}

TEST_CASE("Lucky Duck blackjack: basic strategy") {
        auto play = [](std::initializer_list<uint8_t> cards, uint8_t up, int h17 = 0, int can_double = 1,
                       int can_split = 1, int can_surrender = 1) {
                std::vector<uint8_t> v(cards);
                return bj_basic(v.data(), int(v.size()), up, can_double, can_split, can_surrender, h17);
        };
        CHECK(play({c(10), c(6)}, c(10)) == BJ_SURRENDER);
        CHECK(play({c(10), c(6)}, c(10), 0, 1, 1, 0) == BJ_HIT);
        CHECK(play({c(10), c(6)}, c(6)) == BJ_STAND);
        CHECK(play({c(10), c(2)}, c(3)) == BJ_HIT);
        CHECK(play({c(10), c(2)}, c(4)) == BJ_STAND);
        CHECK(play({c(6), c(5)}, c(10)) == BJ_DOUBLE);
        CHECK(play({c(6), c(5)}, c(1)) == BJ_HIT);
        CHECK(play({c(6), c(5)}, c(1), 1) == BJ_DOUBLE);
        CHECK(play({c(6), c(5)}, c(10), 0, 0) == BJ_HIT);
        CHECK(play({c(1), c(7)}, c(4)) == BJ_DOUBLE);
        CHECK(play({c(1), c(7)}, c(4), 0, 0) == BJ_STAND);
        CHECK(play({c(1), c(7)}, c(9)) == BJ_HIT);
        CHECK(play({c(1), c(1)}, c(10)) == BJ_SPLIT);
        CHECK(play({c(8), c(8)}, c(10)) == BJ_SPLIT);
        CHECK(play({c(10), c(13)}, c(6)) == BJ_STAND);
        CHECK(play({c(9), c(9)}, c(7)) == BJ_STAND);
        CHECK(play({c(9), c(9)}, c(8)) == BJ_SPLIT);
        CHECK(play({c(5), c(5)}, c(6)) == BJ_DOUBLE);
        CHECK(play({c(10), c(7)}, c(1)) == BJ_STAND);
        CHECK(play({c(10), c(7)}, c(1), 1) == BJ_SURRENDER);
}
