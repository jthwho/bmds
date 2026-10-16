/**
 * @file      luckyduck_dice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck Dice's rules (programs/luckyduck, devplan/luckyduck.md §18): every box's score, the
 * upper bonus, second Lucky Ducks and the joker rule, a turn's rolls and holds and their refusals,
 * the turns and rounds to the end, the log, determinism, and the bots (legal, and good).
 */

#include <doctest.h>

#include <cstring>
#include <initializer_list>
#include "../../programs/luckyduck/dice_rules.h"

namespace {

        // The dice set to @p faces as if just rolled (the turn's first roll unless said).
        void set_dice(dice_table_t &t, std::initializer_list<int> faces, int roll = 1) {
                int i = 0;
                for (int f : faces) t.dice[i++] = uint8_t(f);
                t.roll = uint8_t(roll);
        }

        int score(std::initializer_list<int> faces, int box, int joker = 0) {
                uint8_t d[DICE_DICE];
                int     i = 0;
                for (int f : faces) d[i++] = uint8_t(f);
                return dice_box_score(d, box, joker);
        }

        int count(const dice_table_t &t, uint8_t type, uint32_t from = 0) {
                int n = 0;
                for (uint32_t i = from; i < t.event_count; ++i) n += dice_event(&t, i)->type == type;
                return n;
        }

        const dice_event_t *last(const dice_table_t &t, uint8_t type) {
                for (uint32_t i = t.event_count; i-- > 0;)
                        if (dice_event(&t, i)->type == type) return dice_event(&t, i);
                return nullptr;
        }

        // Plays a whole game with every seat a bot. @return The total of seat 0.
        int bot_game(dice_table_t &t, uint32_t playing, uint32_t seed) {
                dice_init(&t, playing, deck_key(seed));
                int guard = 0;
                while (t.phase == DICE_PLAYING && ++guard < 10000) {
                        const int seat = t.turn_seat;
                        if (t.roll == 0) {
                                REQUIRE(dice_roll(&t, seat) == 0);
                                continue;
                        }
                        const dice_choice_t c = dice_bot(&t, seat, 2);
                        if (c.box < DICE_BOXES) {
                                REQUIRE(c.box >= 0);
                                REQUIRE(dice_can_score(&t, seat, c.box));
                                REQUIRE(dice_score(&t, seat, c.box) == 0);
                                continue;
                        }
                        REQUIRE(t.roll < DICE_ROLLS);
                        for (int d = 0; d < DICE_DICE; ++d) REQUIRE(dice_hold(&t, seat, d, (c.keep >> d) & 1) == 0);
                        REQUIRE(dice_roll(&t, seat) == 0);
                }
                return dice_total(&t.seat[0]);
        }

} // namespace

TEST_CASE("Lucky Duck Dice: each box's score") {
        CHECK(score({1, 1, 2, 3, 1}, DICE_ONES) == 3);
        CHECK(score({6, 6, 2, 6, 1}, DICE_SIXES) == 18);
        CHECK(score({2, 3, 4, 5, 6}, DICE_FOURS) == 4);
        CHECK(score({2, 2, 2, 5, 6}, DICE_THREE_KIND) == 17);
        CHECK(score({2, 2, 4, 5, 6}, DICE_THREE_KIND) == 0);
        CHECK(score({3, 3, 3, 3, 6}, DICE_FOUR_KIND) == 18);
        CHECK(score({3, 3, 3, 6, 6}, DICE_FOUR_KIND) == 0);
        CHECK(score({3, 3, 3, 6, 6}, DICE_FULL_HOUSE) == 25);
        CHECK(score({3, 3, 3, 3, 3}, DICE_FULL_HOUSE) == 0); // five of a kind isn't one (but a joker is)
        CHECK(score({3, 3, 3, 3, 3}, DICE_FULL_HOUSE, 1) == 25);
        CHECK(score({1, 2, 3, 4, 6}, DICE_SMALL_STRAIGHT) == 30);
        CHECK(score({3, 4, 5, 6, 6}, DICE_SMALL_STRAIGHT) == 30);
        CHECK(score({1, 2, 3, 5, 6}, DICE_SMALL_STRAIGHT) == 0);
        CHECK(score({5, 4, 3, 2, 1}, DICE_LARGE_STRAIGHT) == 40);
        CHECK(score({2, 3, 4, 5, 6}, DICE_LARGE_STRAIGHT) == 40);
        CHECK(score({1, 2, 3, 4, 6}, DICE_LARGE_STRAIGHT) == 0);
        CHECK(score({4, 4, 4, 4, 4}, DICE_LUCKY_DUCK) == 50);
        CHECK(score({4, 4, 4, 4, 5}, DICE_LUCKY_DUCK) == 0);
        CHECK(score({1, 2, 3, 4, 6}, DICE_CHANCE) == 16);
}

TEST_CASE("Lucky Duck Dice: a turn's rolls and holds, and what is refused") {
        dice_table_t t;
        dice_init(&t, 0x3u, deck_key(7));
        CHECK(t.phase == DICE_PLAYING);
        CHECK(t.turn_seat == 0);
        CHECK(count(t, DICE_EV_START) == 1);
        CHECK(count(t, DICE_EV_TURN) == 1);
        CHECK(dice_hold(&t, 0, 0, 1) == -1); // nothing rolled yet
        CHECK(dice_score(&t, 0, DICE_CHANCE) == -1);
        CHECK(dice_roll(&t, 1) == -1); // not its turn
        REQUIRE(dice_roll(&t, 0) == 0);
        for (int i = 0; i < DICE_DICE; ++i) CHECK((t.dice[i] >= 1 && t.dice[i] <= 6));
        const uint8_t kept = t.dice[2];
        CHECK(dice_hold(&t, 0, 2, 1) == 0);
        CHECK(dice_hold(&t, 0, 5, 1) == -1);
        CHECK(dice_hold(&t, 1, 2, 1) == -1);
        const uint32_t holds = (uint32_t)count(t, DICE_EV_HOLD);
        CHECK(dice_hold(&t, 0, 2, 1) == 0); // already held: nothing logged
        CHECK((uint32_t)count(t, DICE_EV_HOLD) == holds);
        for (int r = 2; r <= 3; ++r) {
                REQUIRE(dice_roll(&t, 0) == 0);
                CHECK(t.dice[2] == kept);
                const dice_event_t *e = last(t, DICE_EV_ROLL);
                REQUIRE(e != nullptr);
                CHECK(e->roll == r);
                CHECK(e->held == 0x4u);
                CHECK(std::memcmp(e->dice, t.dice, DICE_DICE) == 0);
        }
        CHECK(dice_roll(&t, 0) == -1); // three rolls
        CHECK(dice_hold(&t, 0, 1, 1) == -1);
        const int sum = t.dice[0] + t.dice[1] + t.dice[2] + t.dice[3] + t.dice[4];
        REQUIRE(dice_score(&t, 0, DICE_CHANCE) == 0);
        CHECK(t.seat[0].box[DICE_CHANCE] == sum);
        CHECK(t.turn_seat == 1);
        CHECK(t.roll == 0);
        CHECK(t.held[2] == 0);
        CHECK(t.round == 0);
        REQUIRE(dice_roll(&t, 1) == 0);
        CHECK(dice_score(&t, 1, DICE_CHANCE) == 0);
        CHECK(t.turn_seat == 0);
        CHECK(t.round == 1);
        CHECK(last(t, DICE_EV_TURN)->roll == 1);
        REQUIRE(dice_roll(&t, 0) == 0);
        CHECK(dice_can_score(&t, 0, DICE_CHANCE) == 0); // written
        CHECK(dice_score(&t, 0, DICE_CHANCE) == -1);
}

TEST_CASE("Lucky Duck Dice: the upper bonus at 63") {
        dice_table_t t;
        dice_init(&t, 0x1u, deck_key(1));
        t.seat[0].box[DICE_ONES] = 3;
        t.seat[0].box[DICE_TWOS] = 6;
        t.seat[0].box[DICE_THREES] = 9;
        t.seat[0].box[DICE_FOURS] = 12;
        t.seat[0].box[DICE_FIVES] = 15; // 45: sixes need 18
        set_dice(t, {6, 6, 6, 2, 3});
        const uint32_t from = t.event_count;
        REQUIRE(dice_score(&t, 0, DICE_SIXES) == 0);
        CHECK(dice_upper(&t.seat[0]) == 63);
        CHECK(count(t, DICE_EV_BONUS, from) == 1);
        CHECK(last(t, DICE_EV_BONUS)->die == DICE_ONES);
        CHECK(last(t, DICE_EV_BONUS)->amount == DICE_UPPER_BONUS);
        CHECK(dice_total(&t.seat[0]) == 63 + 35);
        // Below the target: none.
        dice_init(&t, 0x1u, deck_key(1));
        set_dice(t, {6, 6, 6, 6, 6});
        REQUIRE(dice_score(&t, 0, DICE_SIXES) == 0);
        CHECK(count(t, DICE_EV_BONUS) == 0);
        CHECK(dice_total(&t.seat[0]) == 30);
}

TEST_CASE("Lucky Duck Dice: second Lucky Ducks and the joker rule") {
        dice_table_t t;
        dice_init(&t, 0x1u, deck_key(1));
        set_dice(t, {4, 4, 4, 4, 4});
        // The first: any box (the Lucky Duck box among them).
        CHECK(dice_can_score(&t, 0, DICE_CHANCE));
        CHECK(dice_points(&t, 0, DICE_FULL_HOUSE) == 0); // not a joker yet
        REQUIRE(dice_score(&t, 0, DICE_LUCKY_DUCK) == 0);
        CHECK(t.seat[0].box[DICE_LUCKY_DUCK] == 50);
        // A second, its upper box open: only that box, and 100 more.
        set_dice(t, {4, 4, 4, 4, 4});
        CHECK(dice_can_score(&t, 0, DICE_FOURS));
        CHECK_FALSE(dice_can_score(&t, 0, DICE_CHANCE));
        CHECK_FALSE(dice_can_score(&t, 0, DICE_FULL_HOUSE));
        uint32_t from = t.event_count;
        REQUIRE(dice_score(&t, 0, DICE_FOURS) == 0);
        CHECK(t.seat[0].box[DICE_FOURS] == 20);
        CHECK(t.seat[0].lucky_bonus == 100);
        CHECK(count(t, DICE_EV_BONUS, from) == 1);
        CHECK(last(t, DICE_EV_BONUS)->die == DICE_LUCKY_DUCK);
        // A third, its upper box written: any lower box at full value.
        set_dice(t, {4, 4, 4, 4, 4});
        CHECK_FALSE(dice_can_score(&t, 0, DICE_ONES));
        CHECK(dice_points(&t, 0, DICE_FULL_HOUSE) == 25);
        CHECK(dice_points(&t, 0, DICE_SMALL_STRAIGHT) == 30);
        CHECK(dice_points(&t, 0, DICE_LARGE_STRAIGHT) == 40);
        CHECK(dice_points(&t, 0, DICE_FOUR_KIND) == 20);
        REQUIRE(dice_score(&t, 0, DICE_LARGE_STRAIGHT) == 0);
        CHECK(t.seat[0].box[DICE_LARGE_STRAIGHT] == 40);
        CHECK(t.seat[0].lucky_bonus == 200);
        // Every lower box written: a zero in an open upper box.
        for (int b = DICE_THREE_KIND; b < DICE_BOXES; ++b)
                if (t.seat[0].box[b] == DICE_OPEN) t.seat[0].box[b] = 0;
        set_dice(t, {4, 4, 4, 4, 4});
        CHECK(dice_can_score(&t, 0, DICE_SIXES));
        CHECK(dice_points(&t, 0, DICE_SIXES) == 0);
        CHECK_FALSE(dice_can_score(&t, 0, DICE_FOURS));
        // The Lucky Duck box scratched (0): the joker rule still, but no 100.
        dice_init(&t, 0x1u, deck_key(1));
        t.seat[0].box[DICE_LUCKY_DUCK] = 0;
        set_dice(t, {2, 2, 2, 2, 2});
        CHECK_FALSE(dice_can_score(&t, 0, DICE_CHANCE));
        from = t.event_count;
        REQUIRE(dice_score(&t, 0, DICE_TWOS) == 0);
        CHECK(t.seat[0].lucky_bonus == 0);
        CHECK(count(t, DICE_EV_BONUS, from) == 0);
}

TEST_CASE("Lucky Duck Dice: 13 rounds, the end and its winner, the same game from the same seed") {
        dice_table_t a, b;
        bot_game(a, 0xfu, 99);
        bot_game(b, 0xfu, 99);
        CHECK(a.phase == DICE_OVER);
        CHECK(a.turn_seat == -1);
        CHECK(a.round == DICE_ROUNDS);
        for (int s = 0; s < DICE_SEATS; ++s) {
                for (int box = 0; box < DICE_BOXES; ++box) CHECK(a.seat[s].box[box] != DICE_OPEN);
                CHECK(dice_total(&a.seat[s]) == dice_total(&b.seat[s]));
        }
        CHECK(count(a, DICE_EV_END, a.event_count - 4) == 1);
        const dice_event_t *end = last(a, DICE_EV_END);
        REQUIRE(end != nullptr);
        int best = 0;
        for (int s = 0; s < DICE_SEATS; ++s)
                if (dice_total(&a.seat[s]) > best) best = dice_total(&a.seat[s]);
        CHECK(end->amount == best);
        CHECK(dice_total(&a.seat[end->seat]) == best);
        // Seats not starting at 0, and one alone.
        dice_init(&a, 0x6u, deck_key(3));
        CHECK(a.turn_seat == 1);
        bot_game(a, 0x8u, 5);
        CHECK(a.phase == DICE_OVER);
        CHECK(last(a, DICE_EV_END)->seat == 3);
        dice_init(&a, 0u, deck_key(5));
        CHECK(a.phase == DICE_OVER);
}

TEST_CASE("Lucky Duck Dice: the bots choose well") {
        dice_table_t t;
        dice_init(&t, 0x1u, deck_key(1));
        set_dice(t, {6, 6, 6, 6, 6});
        dice_choice_t c = dice_bot(&t, 0, 2);
        CHECK(c.box == DICE_LUCKY_DUCK);
        set_dice(t, {2, 3, 4, 5, 6});
        c = dice_bot(&t, 0, 2);
        CHECK(c.box == DICE_LARGE_STRAIGHT);
        // Three sixes on the first roll: keep them.
        set_dice(t, {6, 1, 6, 3, 6});
        c = dice_bot(&t, 0, 2);
        CHECK(c.box == DICE_BOXES);
        CHECK(c.keep == 0x15u);
        // The last roll: a box, the best one.
        set_dice(t, {3, 3, 3, 5, 5}, 3);
        c = dice_bot(&t, 0, 2);
        CHECK(c.box == DICE_FULL_HOUSE);
        // Whole games: good on average (well played solitaire averages about 250).
        int sum = 0;
        for (uint32_t seed = 1; seed <= 12; ++seed) sum += bot_game(t, 0x1u, seed);
        CHECK(sum / 12 > 190);
}
