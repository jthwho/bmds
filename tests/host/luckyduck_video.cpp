/**
 * @file      luckyduck_video.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's poker hands and Video Poker (programs/luckyduck, devplan/luckyduck.md §21): every
 * five-card hand counted by category, seven cards against brute force, ties and kickers; the
 * machine's deal, holds, draw and pays, each pay table's rows, and perfect play.
 */

#include <doctest.h>

#include <cstring>
#include <initializer_list>
#include "../../programs/luckyduck/hand.h"
#include "../../programs/luckyduck/video_rules.h"

namespace {

        // A card from its name: "AH", "TC", "9S" (rank, then clubs, diamonds, hearts or spades).
        uint8_t card(const char *name) {
                const char *ranks = "A23456789TJQK", *suits = "CDHS";
                const int   r = int(std::strchr(ranks, name[0]) - ranks), s = int(std::strchr(suits, name[1]) - suits);
                return uint8_t(s * DECK_RANKS + r);
        }

        struct Hand {
                        uint8_t c[7];
                        int     n = 0;
                        Hand(std::initializer_list<const char *> names) {
                                for (const char *name : names) c[n++] = card(name);
                        }
        };

        uint32_t value(std::initializer_list<const char *> names) {
                Hand h(names);
                return hand_value(h.c, h.n);
        }

        int row(int machine, std::initializer_list<const char *> names) {
                Hand h(names);
                return vp_row(machine, h.c);
        }

        int row_named(int machine, const char *name) {
                for (int r = 0; r < vp_rows(machine); ++r)
                        if (std::strcmp(vp_row_name(machine, r), name) == 0) return r;
                return -2;
        }

        // Perfect play's answer for five @p names, worked out whole.
        int best_hold(int machine, int coins, std::initializer_list<const char *> names, vp_ev_t &e) {
                Hand h(names);
                vp_ev_start(&e, machine, coins, h.c);
                while (!vp_ev_step(&e, 100000)) {}
                return vp_ev_best(&e);
        }

} // namespace

TEST_CASE("luckyduck hand: every five-card hand counted by category") {
        uint32_t count[HAND_CATEGORIES] = {};
        uint32_t royals = 0;
        uint8_t  h[5];
        for (h[0] = 0; h[0] < DECK_CARDS; ++h[0])
                for (h[1] = uint8_t(h[0] + 1); h[1] < DECK_CARDS; ++h[1])
                        for (h[2] = uint8_t(h[1] + 1); h[2] < DECK_CARDS; ++h[2])
                                for (h[3] = uint8_t(h[2] + 1); h[3] < DECK_CARDS; ++h[3])
                                        for (h[4] = uint8_t(h[3] + 1); h[4] < DECK_CARDS; ++h[4]) {
                                                const uint32_t v = hand_value(h, 5);
                                                ++count[hand_category(v)];
                                                royals += hand_category(v) == HAND_STRAIGHT_FLUSH &&
                                                          hand_rank(v, 0) == HAND_ACE;
                                        }
        CHECK(count[HAND_HIGH_CARD] == 1302540);
        CHECK(count[HAND_PAIR] == 1098240);
        CHECK(count[HAND_TWO_PAIR] == 123552);
        CHECK(count[HAND_THREE] == 54912);
        CHECK(count[HAND_STRAIGHT] == 10200);
        CHECK(count[HAND_FLUSH] == 5108);
        CHECK(count[HAND_FULL_HOUSE] == 3744);
        CHECK(count[HAND_FOUR] == 624);
        CHECK(count[HAND_STRAIGHT_FLUSH] == 40);
        CHECK(royals == 4);
}

TEST_CASE("luckyduck hand: seven cards are their best five") {
        deck_rng_t rng;
        deck_seed(&rng, 7);
        uint8_t deck[DECK_CARDS];
        for (int round = 0; round < 20000; ++round) {
                deck_fill(deck, 1);
                deck_shuffle(&rng, deck, DECK_CARDS);
                const int n = 5 + round % 3;
                uint32_t  best = 0;
                /* Every five: leave out two of seven, one of six (a == b), none of five. */
                for (int a = 0; a < n; ++a)
                        for (int b = a; b < n; ++b) {
                                if (n - 5 != (a == b ? 1 : 2) && n != 5) continue;
                                uint8_t five[5];
                                int     k = 0;
                                for (int i = 0; i < n && k < 5; ++i)
                                        if (n == 5 || (i != a && i != b)) five[k++] = deck[i];
                                const uint32_t v = hand_value(five, 5);
                                if (v > best) best = v;
                        }
                REQUIRE(hand_value(deck, n) == best);
        }
}

TEST_CASE("luckyduck hand: ties, kickers, the wheel") {
        // The wheel is the lowest straight; a straight never wraps past the Ace.
        CHECK(hand_category(value({"AC", "2D", "3H", "4S", "5C"})) == HAND_STRAIGHT);
        CHECK(hand_rank(value({"AC", "2D", "3H", "4S", "5C"}), 0) == 3);
        CHECK(value({"AC", "2D", "3H", "4S", "5C"}) < value({"2C", "3D", "4H", "5S", "6C"}));
        CHECK(hand_category(value({"QC", "KD", "AH", "2S", "3C"})) == HAND_HIGH_CARD);
        CHECK(value({"TC", "JD", "QH", "KS", "AC"}) > value({"9C", "TD", "JH", "QS", "KC"}));
        // Kickers decide; suits never do.
        CHECK(value({"AC", "AD", "KH", "7S", "2C"}) > value({"AH", "AS", "QH", "JS", "TC"}));
        CHECK(value({"AC", "AD", "KH", "7S", "2C"}) == value({"AH", "AS", "KC", "7D", "2S"}));
        CHECK(value({"9C", "9D", "5H", "5S", "KC"}) > value({"9H", "9S", "5C", "5D", "QC"}));
        CHECK(value({"9C", "9D", "5H", "5S", "2C"}) > value({"8H", "8S", "7C", "7D", "AC"}));
        // A full house counts its three first; a four its kicker.
        CHECK(value({"3C", "3D", "3H", "AS", "AC"}) < value({"4C", "4D", "4H", "2S", "2C"}));
        CHECK(value({"7C", "7D", "7H", "7S", "AC"}) > value({"7C", "7D", "7H", "7S", "KC"}));
        // Seven cards: two threes make a full house of the higher; a four takes the best kicker; a
        // flush beats a straight in the same seven; a straight flush beats the four beside it.
        uint32_t v = value({"5C", "5D", "5H", "9S", "9C", "9D", "2H"});
        CHECK(hand_category(v) == HAND_FULL_HOUSE);
        CHECK(hand_rank(v, 0) == 7);
        CHECK(hand_rank(v, 1) == 3);
        v = value({"8C", "8D", "8H", "8S", "KC", "KD", "2H"});
        CHECK(hand_category(v) == HAND_FOUR);
        CHECK(hand_rank(v, 1) == 11);
        CHECK(hand_category(value({"4H", "5H", "6C", "7H", "8D", "JH", "2H"})) == HAND_FLUSH);
        CHECK(hand_category(value({"5S", "6S", "7S", "8S", "9S", "9C", "9D"})) == HAND_STRAIGHT_FLUSH);
        // Three pairs in seven: the best two, the best of the rest as kicker.
        v = value({"2C", "2D", "6H", "6S", "TC", "TD", "3H"});
        CHECK(hand_category(v) == HAND_TWO_PAIR);
        CHECK(hand_rank(v, 0) == 8);
        CHECK(hand_rank(v, 1) == 4);
        CHECK(hand_rank(v, 2) == 1);
        CHECK(std::strcmp(hand_name(HAND_FULL_HOUSE), "FULL HOUSE") == 0);
}

TEST_CASE("luckyduck video: deal, hold, draw, pay") {
        vp_t v;
        vp_init(&v, VP_JACKS, 5, 100);
        CHECK(v.phase == VP_BETTING);
        CHECK(vp_hold(&v, 0, 1) == -1);
        CHECK(vp_draw(&v) == -1);
        CHECK(vp_bet(&v, 0) == -1);
        CHECK(vp_bet(&v, 6) == -1);
        REQUIRE(vp_bet(&v, 5) == 0);
        REQUIRE(vp_deal(&v, deck_key(1)) == 0);
        CHECK(v.stack == 75);
        CHECK(v.phase == VP_DEALT);
        // Five different cards; nothing changes while dealt.
        for (int i = 0; i < VP_CARDS; ++i)
                for (int j = i + 1; j < VP_CARDS; ++j) CHECK(v.cards[i] != v.cards[j]);
        CHECK(vp_bet(&v, 1) == -1);
        CHECK(vp_configure(&v, VP_BONUS, 1) == -1);
        CHECK(vp_deal(&v, deck_key(2)) == -1);
        CHECK(vp_hold(&v, 5, 1) == -1);
        REQUIRE(vp_hold(&v, 1, 1) == 0);
        REQUIRE(vp_hold(&v, 3, 1) == 0);
        REQUIRE(vp_hold(&v, 3, 0) == 0);
        CHECK(v.held == 0x02);
        uint8_t before[VP_CARDS];
        std::memcpy(before, v.cards, sizeof(before));
        REQUIRE(vp_draw(&v) == 0);
        CHECK(v.phase == VP_DONE);
        CHECK(v.drawn == 0x1d);
        CHECK(v.cards[1] == before[1]);
        // The drawn cards are new: none of the five dealt comes back.
        for (int i = 0; i < VP_CARDS; ++i) {
                if (i == 1) continue;
                for (int j = 0; j < VP_CARDS; ++j) CHECK(v.cards[i] != before[j]);
        }
        CHECK(v.row == vp_row(VP_JACKS, v.cards));
        CHECK(v.won == vp_row_pay(VP_JACKS, v.row, 5) * 5);
        CHECK(v.stack == 75 + v.won);
        // Paid: the coins and the machine may change; a bet bigger than the stack isn't dealt.
        CHECK(vp_configure(&v, VP_BONUS, 25) == 0);
        if (v.stack < 125) CHECK(vp_deal(&v, deck_key(3)) == -1);
        // The same key deals the same hand.
        vp_t a, b;
        vp_init(&a, VP_JACKS, 1, 10);
        vp_init(&b, VP_JACKS, 1, 10);
        REQUIRE(vp_deal(&a, deck_key(99)) == 0);
        REQUIRE(vp_deal(&b, deck_key(99)) == 0);
        CHECK(std::memcmp(a.cards, b.cards, VP_CARDS) == 0);
        REQUIRE(vp_draw(&a) == 0);
        REQUIRE(vp_draw(&b) == 0);
        CHECK(std::memcmp(a.cards, b.cards, VP_CARDS) == 0);
}

TEST_CASE("luckyduck video: the pay tables") {
        // Jacks or Better 9/6.
        CHECK(row(VP_JACKS, {"TS", "JS", "QS", "KS", "AS"}) == row_named(VP_JACKS, "ROYAL FLUSH"));
        CHECK(vp_row_royal(VP_JACKS, row_named(VP_JACKS, "ROYAL FLUSH")));
        CHECK(vp_row_pay(VP_JACKS, 0, 1) == 250);
        CHECK(vp_row_pay(VP_JACKS, 0, 4) == 1000);
        CHECK(vp_row_pay(VP_JACKS, 0, 5) == 4000);
        CHECK(row(VP_JACKS, {"9H", "TH", "JH", "QH", "KH"}) == row_named(VP_JACKS, "STRAIGHT FLUSH"));
        CHECK(row(VP_JACKS, {"AH", "2H", "3H", "4H", "5H"}) == row_named(VP_JACKS, "STRAIGHT FLUSH"));
        CHECK(vp_row_pay(VP_JACKS, row_named(VP_JACKS, "FULL HOUSE"), 1) == 9);
        CHECK(vp_row_pay(VP_JACKS, row_named(VP_JACKS, "FLUSH"), 3) == 18);
        CHECK(row(VP_JACKS, {"JC", "JD", "2H", "5S", "9C"}) == row_named(VP_JACKS, "JACKS OR BETTER"));
        CHECK(row(VP_JACKS, {"AC", "AD", "2H", "5S", "9C"}) == row_named(VP_JACKS, "JACKS OR BETTER"));
        CHECK(row(VP_JACKS, {"TC", "TD", "2H", "5S", "9C"}) == -1);
        CHECK(row(VP_JACKS, {"2C", "2D", "3H", "3S", "9C"}) == row_named(VP_JACKS, "TWO PAIR"));
        CHECK(row(VP_JACKS, {"KC", "QD", "2H", "5S", "9C"}) == -1);
        // Bonus Poker: fours by rank.
        CHECK(row(VP_BONUS, {"AC", "AD", "AH", "AS", "9C"}) == row_named(VP_BONUS, "FOUR ACES"));
        CHECK(row(VP_BONUS, {"3C", "3D", "3H", "3S", "9C"}) == row_named(VP_BONUS, "FOUR 2S, 3S, 4S"));
        CHECK(row(VP_BONUS, {"KC", "KD", "KH", "KS", "9C"}) == row_named(VP_BONUS, "FOUR 5S THRU KS"));
        CHECK(vp_row_pay(VP_BONUS, row_named(VP_BONUS, "FOUR ACES"), 1) == 80);
        CHECK(vp_row_pay(VP_BONUS, row_named(VP_BONUS, "FULL HOUSE"), 1) == 8);
        // Double Double Bonus: the kicker counts, and two pair pays 1.
        CHECK(row(VP_DOUBLE_DOUBLE, {"AC", "AD", "AH", "AS", "3C"}) ==
              row_named(VP_DOUBLE_DOUBLE, "FOUR ACES + 2, 3, 4"));
        CHECK(row(VP_DOUBLE_DOUBLE, {"AC", "AD", "AH", "AS", "9C"}) == row_named(VP_DOUBLE_DOUBLE, "FOUR ACES"));
        CHECK(row(VP_DOUBLE_DOUBLE, {"2C", "2D", "2H", "2S", "AC"}) ==
              row_named(VP_DOUBLE_DOUBLE, "FOUR 2-4 + A, 2, 3, 4"));
        CHECK(row(VP_DOUBLE_DOUBLE, {"4C", "4D", "4H", "4S", "3C"}) ==
              row_named(VP_DOUBLE_DOUBLE, "FOUR 2-4 + A, 2, 3, 4"));
        CHECK(row(VP_DOUBLE_DOUBLE, {"4C", "4D", "4H", "4S", "KC"}) == row_named(VP_DOUBLE_DOUBLE, "FOUR 2S, 3S, 4S"));
        CHECK(vp_row_pay(VP_DOUBLE_DOUBLE, row_named(VP_DOUBLE_DOUBLE, "FOUR ACES + 2, 3, 4"), 5) == 2000);
        CHECK(vp_row_pay(VP_DOUBLE_DOUBLE, row_named(VP_DOUBLE_DOUBLE, "TWO PAIR"), 1) == 1);
        // What counts: a pair's two, a four's kicker where it pays, a straight's five.
        Hand h({"2C", "JD", "5H", "JS", "9C"});
        CHECK(vp_scoring(VP_JACKS, h.c, vp_row(VP_JACKS, h.c)) == 0x0a);
        Hand k({"AC", "AD", "3C", "AH", "AS"});
        CHECK(vp_scoring(VP_DOUBLE_DOUBLE, k.c, vp_row(VP_DOUBLE_DOUBLE, k.c)) == 0x1f);
        CHECK(vp_scoring(VP_BONUS, k.c, vp_row(VP_BONUS, k.c)) == 0x1b);
        Hand s({"9C", "TD", "JH", "QS", "KC"});
        CHECK(vp_scoring(VP_JACKS, s.c, vp_row(VP_JACKS, s.c)) == 0x1f);
}

TEST_CASE("luckyduck video: perfect play") {
        vp_ev_t e;
        // A pair of Jacks: hold it. Its return is about 1.54 a coin (playing for the 4,000 royal).
        int hold = best_hold(VP_JACKS, 5, {"JS", "JH", "2C", "7D", "9S"}, e);
        CHECK(hold == 0x03);
        CHECK(vp_ev_return(&e, 0x03) / 5 == doctest::Approx(1.54).epsilon(0.02));
        // Every hold against every draw is every five-card hand once.
        uint64_t draws = 0;
        for (int h = 0; h < VP_HOLDS; ++h) draws += e.draws[h];
        CHECK(draws == 2598960u);
        // A pat flush broken for four to the royal; a low pair given up for four to a flush.
        CHECK(best_hold(VP_JACKS, 5, {"AH", "KH", "QH", "JH", "3H"}, e) == 0x0f);
        CHECK(best_hold(VP_JACKS, 5, {"2H", "5H", "9H", "KH", "2S"}, e) == 0x0f);
        // A pat straight flush stands; nothing at all draws five (or a lone high card).
        CHECK(best_hold(VP_JACKS, 5, {"5S", "6S", "7S", "8S", "9S"}, e) == 0x1f);
        CHECK(vp_ev_return(&e, 0x1f) == doctest::Approx(250.0));
        hold = best_hold(VP_JACKS, 5, {"2C", "4D", "7H", "8S", "TC"}, e); // no card higher than a Ten
        CHECK(hold == 0x00);
}

TEST_CASE("luckyduck video: perfect play's pays agree with the pay tables") {
        // Each hold keeping two cards or more, worked out the slow way (vp_row() on every draw).
        const struct {
                        int                                 machine;
                        std::initializer_list<const char *> cards;
        } cases[] = {{VP_BONUS, {"AC", "AD", "AH", "3C", "9S"}},
                     {VP_DOUBLE_DOUBLE, {"AC", "AD", "AH", "3C", "9S"}},
                     {VP_JACKS, {"TS", "JS", "QS", "9S", "8H"}}};
        for (const auto &c : cases) {
                Hand    h(c.cards);
                vp_ev_t e;
                vp_ev_start(&e, c.machine, 5, h.c);
                while (!vp_ev_step(&e, 100000)) {}
                for (int hold = 0; hold < VP_HOLDS; ++hold) {
                        const int draw = VP_CARDS - __builtin_popcount(unsigned(hold));
                        if (draw > 3) continue;
                        uint8_t hand[VP_CARDS];
                        int     kept = 0;
                        for (int i = 0; i < VP_CARDS; ++i)
                                if (hold & (1 << i)) hand[kept++] = h.c[i];
                        uint64_t  paid = 0, draws = 0;
                        int       idx[3] = {0, 1, 2};
                        const int left = DECK_CARDS - VP_CARDS;
                        for (;;) {
                                for (int j = 0; j < draw; ++j) hand[kept + j] = e.rest[idx[j]];
                                paid += uint64_t(vp_row_pay(c.machine, vp_row(c.machine, hand), 5));
                                ++draws;
                                int j = draw - 1;
                                while (j >= 0 && idx[j] == left - draw + j) --j;
                                if (j < 0) break;
                                ++idx[j];
                                for (int i = j + 1; i < draw; ++i) idx[i] = idx[i - 1] + 1;
                        }
                        CHECK(e.draws[hold] == draws);
                        CHECK(e.paid[hold] == paid);
                }
        }
}
