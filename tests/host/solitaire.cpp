/**
 * @file      solitaire.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Solitaire's rules (programs/solitaire/rules.c, devplan/games.md §21): the deal, moves allowed
 * and refused, turning over, draws and passes, both scorings, undo back to the deal, the win.
 */

#include <doctest.h>

#include <cstring>
#include <initializer_list>
#include <set>
#include "../../programs/solitaire/rules.h"

namespace {

        uint8_t card(uint32_t suit, uint32_t rank, bool up = true) {
                return uint8_t(suit * SOL_RANKS + rank) | (up ? SOL_UP : 0);
        }

        // An empty game (no cards anywhere) to set piles up by hand.
        void empty(solitaire_t &s, uint32_t draw = 1, uint32_t scoring = SOL_SCORE_STANDARD) {
                sol_deal(&s, draw, scoring, 1);
                for (auto &p : s.pile) p.count = 0;
                s.score = 0;
                return;
        }

        void put(solitaire_t &s, uint32_t pile, std::initializer_list<uint8_t> cards) {
                for (uint8_t c : cards) s.pile[pile].cards[s.pile[pile].count++] = c;
                return;
        }

        uint8_t top(const solitaire_t &s, uint32_t pile) {
                return s.pile[pile].cards[s.pile[pile].count - 1];
        }

} // namespace

TEST_CASE("Solitaire: the deal") {
        solitaire_t s;
        sol_deal(&s, 3, SOL_SCORE_VEGAS, 42);
        std::set<int> seen;
        for (uint32_t col = 0; col < 7; ++col) {
                const sol_pile_t &p = s.pile[SOL_TABLEAU + col];
                REQUIRE(p.count == col + 1);
                for (uint32_t i = 0; i < p.count; ++i) {
                        CHECK(((p.cards[i] & SOL_UP) != 0) == (i + 1 == p.count));
                        seen.insert(p.cards[i] & ~SOL_UP);
                }
        }
        CHECK(s.pile[SOL_STOCK].count == 24);
        for (uint32_t i = 0; i < 24; ++i) {
                CHECK((s.pile[SOL_STOCK].cards[i] & SOL_UP) == 0);
                seen.insert(s.pile[SOL_STOCK].cards[i]);
        }
        CHECK(seen.size() == 52);
        CHECK(s.score == -52);
        CHECK(s.draw == 3);
        CHECK(!s.started);
        solitaire_t other;
        sol_deal(&other, 3, SOL_SCORE_VEGAS, 43);
        CHECK(std::memcmp(s.pile, other.pile, sizeof(s.pile)) != 0);
}

TEST_CASE("Solitaire: columns take one lower of the other colour, a King when empty") {
        solitaire_t s;
        empty(s);
        put(s, SOL_TABLEAU, {card(SOL_SPADES, 7)});                         // 8 of spades
        put(s, SOL_TABLEAU + 1, {card(SOL_HEARTS, 6), card(SOL_CLUBS, 5)}); // 7 of hearts, 6 of clubs
        put(s, SOL_TABLEAU + 2, {card(SOL_CLUBS, 6)});                      // 7 of clubs
        put(s, SOL_TABLEAU + 3, {card(SOL_DIAMONDS, 12, false), card(SOL_SPADES, 12)});
        CHECK(sol_can_take(&s, SOL_TABLEAU + 1, 2));
        CHECK(!sol_can_take(&s, SOL_TABLEAU + 3, 2));                  // a face-down card
        CHECK(!sol_can_move(&s, SOL_TABLEAU + 2, 1, SOL_TABLEAU));     // the same colour
        CHECK(!sol_can_move(&s, SOL_TABLEAU + 1, 1, SOL_TABLEAU));     // a 6 on an 8
        CHECK(sol_can_move(&s, SOL_TABLEAU + 1, 2, SOL_TABLEAU));      // the run
        CHECK(!sol_can_move(&s, SOL_TABLEAU + 1, 1, SOL_TABLEAU + 4)); // not a King
        CHECK(sol_can_move(&s, SOL_TABLEAU + 3, 1, SOL_TABLEAU + 4));
        CHECK(!sol_can_move(&s, SOL_TABLEAU, 1, SOL_WASTE));
        CHECK(!sol_can_move(&s, SOL_TABLEAU, 1, SOL_STOCK));

        REQUIRE(sol_move(&s, SOL_TABLEAU + 1, 2, SOL_TABLEAU) == 0);
        CHECK(s.pile[SOL_TABLEAU].count == 3);
        CHECK(top(s, SOL_TABLEAU) == card(SOL_CLUBS, 5));
        CHECK(s.pile[SOL_TABLEAU + 1].count == 0);
        CHECK(s.score == 0); // column to column
        CHECK(s.started);

        // The King leaves; the card under it turns over (5 points).
        REQUIRE(sol_move(&s, SOL_TABLEAU + 3, 1, SOL_TABLEAU + 1) == 0);
        CHECK(top(s, SOL_TABLEAU + 3) == card(SOL_DIAMONDS, 12));
        CHECK(s.score == 5);
        CHECK(sol_move(&s, SOL_TABLEAU + 2, 1, SOL_TABLEAU) == -1);
        CHECK(s.moves == 2);
}

TEST_CASE("Solitaire: foundations build up by suit from the Ace") {
        solitaire_t s;
        empty(s);
        put(s, SOL_WASTE, {card(SOL_HEARTS, 1), card(SOL_HEARTS, 0)});
        put(s, SOL_TABLEAU, {card(SOL_CLUBS, 1)});
        s.fan = 1;
        CHECK(sol_foundation_for(&s, SOL_TABLEAU) == -1);
        CHECK(sol_foundation_for(&s, SOL_WASTE) == SOL_FOUNDATION);
        REQUIRE(sol_move(&s, SOL_WASTE, 1, SOL_FOUNDATION + 2) == 0);
        CHECK(s.score == 10);
        CHECK(!sol_can_move(&s, SOL_TABLEAU, 1, SOL_FOUNDATION + 2)); // another suit
        CHECK(sol_foundation_for(&s, SOL_WASTE) == SOL_FOUNDATION + 2);
        REQUIRE(sol_move(&s, SOL_WASTE, 1, SOL_FOUNDATION + 2) == 0);
        CHECK(s.score == 20);
        CHECK(sol_home(&s) == 2);

        // Two at once never go up; back down to a column costs 15.
        put(s, SOL_TABLEAU + 1, {card(SOL_SPADES, 2), card(SOL_HEARTS, 1)});
        s.pile[SOL_FOUNDATION + 2].count = 1; // the 2 of hearts back on the column instead
        CHECK(!sol_can_move(&s, SOL_TABLEAU + 1, 2, SOL_FOUNDATION + 2));
        REQUIRE(sol_move(&s, SOL_TABLEAU + 1, 1, SOL_FOUNDATION + 2) == 0);
        REQUIRE(sol_move(&s, SOL_FOUNDATION + 2, 1, SOL_TABLEAU + 1) == 0);
        CHECK(s.score == 30 - 15);
}

TEST_CASE("Solitaire: drawing three, the fan, passes and their cost") {
        solitaire_t s;
        empty(s, 3);
        for (uint32_t r = 0; r < 7; ++r) put(s, SOL_STOCK, {card(SOL_CLUBS, r, false)});
        CHECK(sol_draw(&s) == 3);
        CHECK(s.pile[SOL_WASTE].count == 3);
        CHECK(top(s, SOL_WASTE) == card(SOL_CLUBS, 4)); // the third drawn on top
        CHECK(s.fan == 3);
        put(s, SOL_TABLEAU, {card(SOL_HEARTS, 5)});
        REQUIRE(sol_move(&s, SOL_WASTE, 1, SOL_TABLEAU) == 0); // the 5 of clubs on the 6 of hearts
        CHECK(s.fan == 2);
        CHECK(s.score == 5);
        CHECK(sol_draw(&s) == 3);
        CHECK(sol_draw(&s) == 1); // the last
        CHECK(s.fan == 1);
        CHECK(s.pile[SOL_STOCK].count == 0);
        // Turned again: free for the first two passes drawing three, then 20 each.
        for (int pass = 0; pass < 4; ++pass) {
                const int32_t before = s.score;
                CHECK(sol_draw(&s) == 0);
                CHECK(s.pile[SOL_WASTE].count == 0);
                CHECK(s.pile[SOL_STOCK].count == 6);
                CHECK((top(s, SOL_STOCK) & SOL_UP) == 0);
                CHECK(s.score == (pass < 2 ? before : before - 20 < 0 ? 0 : before - 20));
                while (s.pile[SOL_STOCK].count > 0) sol_draw(&s);
        }
        CHECK(s.passes == 4);

        // Drawing one: every pass after the first costs 100 (never below 0).
        empty(s, 1);
        s.score = 150;
        put(s, SOL_STOCK, {card(SOL_CLUBS, 0, false)});
        CHECK(sol_draw(&s) == 1);
        CHECK(sol_draw(&s) == 0);
        CHECK(s.score == 50);
        CHECK(sol_draw(&s) == 1);
        CHECK(sol_draw(&s) == 0);
        CHECK(s.score == 0);
        empty(s, 1);
        CHECK(!sol_can_draw(&s));
        CHECK(sol_draw(&s) == -1);
}

TEST_CASE("Solitaire: Vegas") {
        solitaire_t s;
        sol_deal(&s, 1, SOL_SCORE_VEGAS, 7);
        CHECK(s.score == -52);
        while (s.pile[SOL_STOCK].count > 0) CHECK(sol_draw(&s) == 1);
        CHECK(!sol_can_draw(&s)); // one pass drawing one
        CHECK(sol_draw(&s) == -1);

        empty(s, 3, SOL_SCORE_VEGAS);
        s.score = -52;
        put(s, SOL_STOCK, {card(SOL_CLUBS, 0, false), card(SOL_HEARTS, 3, false)}); // the Ace drawn last: on top
        CHECK(sol_draw(&s) == 2);
        REQUIRE(sol_move(&s, SOL_WASTE, 1, SOL_FOUNDATION) == 0); // the Ace of clubs
        CHECK(s.score == -47);
        put(s, SOL_TABLEAU, {card(SOL_HEARTS, 1)});
        REQUIRE(sol_move(&s, SOL_FOUNDATION, 1, SOL_TABLEAU) == 0);
        CHECK(s.score == -52);
        CHECK(sol_draw(&s) == 0); // three passes drawing three: turned again twice
        CHECK(sol_draw(&s) == 1);
        CHECK(sol_draw(&s) == 0);
        CHECK(sol_draw(&s) == 1);
        CHECK(!sol_can_draw(&s));

        // No time penalty.
        s.started = 1;
        for (int i = 0; i < 60; ++i) sol_second(&s);
        CHECK(s.score == -52);
        CHECK(s.seconds == 60);
}

TEST_CASE("Solitaire: the time costs 2 every 10 seconds once started") {
        solitaire_t s;
        sol_deal(&s, 1, SOL_SCORE_STANDARD, 9);
        for (int i = 0; i < 30; ++i) sol_second(&s);
        CHECK(s.seconds == 0);
        s.started = 1;
        s.score = 5;
        for (int i = 0; i < 30; ++i) sol_second(&s);
        CHECK(s.seconds == 30);
        CHECK(s.score == 0);
}

TEST_CASE("Solitaire: undo goes back to the deal") {
        for (uint32_t seed = 1; seed < 40; ++seed) {
                for (uint32_t draw : {1u, 3u}) {
                        solitaire_t s;
                        sol_deal(&s, draw, SOL_SCORE_STANDARD, seed);
                        const solitaire_t dealt = s;
                        uint32_t          made = 0, r = seed * 2654435761u;
                        // Play at random: up when possible, else a random legal column move, else draw.
                        for (int step = 0; step < 400 && !s.won; ++step) {
                                if (sol_auto_step(&s, nullptr) >= 0) {
                                        ++made;
                                        continue;
                                }
                                bool moved = false;
                                r = r * 1664525u + 1013904223u;
                                for (uint32_t k = 0; k < SOL_PILES * SOL_PILES && !moved; ++k) {
                                        const uint32_t from = (r + k) % SOL_PILES, to = (r / 7 + k * 5) % SOL_PILES;
                                        if (to < SOL_TABLEAU) continue;
                                        for (uint32_t n = 1; n <= s.pile[from].count && !moved; ++n)
                                                if (sol_move(&s, from, n, to) == 0) moved = true;
                                }
                                if (!moved && sol_draw(&s) < 0) break;
                                ++made;
                        }
                        CHECK(made > 0);
                        while (sol_undo(&s) == 0) --made;
                        CHECK(made == 0);
                        for (uint32_t p = 0; p < SOL_PILES; ++p) { // (bytes past a count are left over)
                                REQUIRE(s.pile[p].count == dealt.pile[p].count);
                                CHECK(std::memcmp(s.pile[p].cards, dealt.pile[p].cards, s.pile[p].count) == 0);
                        }
                        CHECK(s.score == 0);
                        CHECK(s.passes == 0);
                        CHECK(s.fan == 0);
                        CHECK(s.moves == 0);
                }
        }
}

TEST_CASE("Solitaire: the win, its bonus, and undoing it") {
        solitaire_t s;
        empty(s);
        for (uint32_t suit = 0; suit < 4; ++suit)
                for (uint32_t r = 0; r < SOL_RANKS - (suit == SOL_SPADES ? 1 : 0); ++r)
                        put(s, SOL_FOUNDATION + suit, {card(suit, r)});
        put(s, SOL_TABLEAU + 6, {card(SOL_SPADES, 12)});
        s.started = 1;
        s.seconds = 100;
        s.score = 500;
        CHECK(sol_home(&s) == 51);
        uint32_t to = 0;
        CHECK(sol_auto_step(&s, &to) == SOL_TABLEAU + 6);
        CHECK(to == SOL_FOUNDATION + uint32_t(SOL_SPADES));
        CHECK(s.won);
        CHECK(s.score == 500 + 10 + 7000);
        CHECK(sol_auto_step(&s, &to) == -1);
        CHECK(!sol_can_draw(&s));
        sol_second(&s);
        CHECK(s.seconds == 100); // stopped
        REQUIRE(sol_undo(&s) == 0);
        CHECK(!s.won);
        CHECK(s.score == 500);
        CHECK(top(s, SOL_TABLEAU + 6) == card(SOL_SPADES, 12));
}

TEST_CASE("Solitaire: undo is kept for the last moves only") {
        solitaire_t s;
        empty(s);
        put(s, SOL_TABLEAU, {card(SOL_SPADES, 12)});
        for (uint32_t i = 0; i < SOL_HISTORY + 10; ++i)
                REQUIRE(sol_move(&s, SOL_TABLEAU + (i % 2), 1, SOL_TABLEAU + 1 - (i % 2)) == 0);
        uint32_t undone = 0;
        while (sol_undo(&s) == 0) ++undone;
        CHECK(undone == SOL_HISTORY);
}

TEST_CASE("Solitaire: a game played out finishes by itself") {
        solitaire_t s;
        sol_deal_finished(&s, 1, SOL_SCORE_STANDARD);
        std::set<int> seen;
        for (uint32_t col = 0; col < 4; ++col) {
                const sol_pile_t &p = s.pile[SOL_TABLEAU + col];
                REQUIRE(p.count == 13);
                for (uint32_t i = 0; i < p.count; ++i) seen.insert(p.cards[i] & ~SOL_UP);
                for (uint32_t i = 1; i < p.count; ++i) // a run: one lower of the other colour
                        CHECK((sol_rank(p.cards[i]) + 1 == sol_rank(p.cards[i - 1]) &&
                               sol_red(p.cards[i]) != sol_red(p.cards[i - 1])));
        }
        CHECK(seen.size() == 52);
        int steps = 0;
        while (sol_auto_step(&s, nullptr) >= 0) ++steps;
        CHECK(steps == 52);
        CHECK(s.won);
}
