/**
 * @file      blocks.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Blocks' rules (programs/blocks/rules.c, devplan/games.md §17): the bag, rotation and kicks,
 * gravity, locking, clears and scoring, auto-repeat, hold, the game's end.
 */

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "../../programs/blocks/rules.h"

namespace {

        const blocks_input_t None = {0, 0};

        void press(blocks_t &b, uint32_t action) {
                const blocks_input_t in = {action, action};
                blocks_tick(&b, &in);
        }

        // The piece's cells as "x,y;" sorted.
        std::string cells(const blocks_piece_t &p) {
                int8_t c[4][2];
                blocks_cells(&p, c);
                std::vector<std::string> v;
                for (auto &x : c) v.push_back(std::to_string(x[0]) + "," + std::to_string(x[1]));
                std::sort(v.begin(), v.end());
                std::string out;
                for (auto &s : v) out += s + ";";
                return out;
        }

} // namespace

TEST_CASE("Blocks: the bag gives each piece once in seven, spawned in the middle") {
        blocks_t b;
        blocks_start(&b, 1, 12345);
        std::vector<int> seen;
        for (int i = 0; i < 21; ++i) {
                seen.push_back(b.piece.type);
                CHECK(b.piece.x == 3);
                CHECK(b.piece.y == 1); // a row down at once
                std::memset(b.field, 0, sizeof(b.field));
                press(b, BLOCKS_HARD);
                CHECK((b.events & BLOCKS_LOCKED) != 0);
        }
        for (int bag = 0; bag < 3; ++bag) {
                std::set<int> s(seen.begin() + bag * 7, seen.begin() + bag * 7 + 7);
                CHECK(s.size() == 7);
                CHECK(*s.begin() == BLOCKS_I);
                CHECK(*s.rbegin() == BLOCKS_L);
        }
}

TEST_CASE("Blocks: turns and SRS's wall kicks") {
        blocks_t b;
        blocks_start(&b, 1, 7);
        b.piece = {BLOCKS_T, 0, 3, 5};
        CHECK(cells(b.piece) == "3,6;4,5;4,6;5,6;");
        press(b, BLOCKS_CW);
        CHECK((b.events & BLOCKS_TURNED) != 0);
        CHECK(b.piece.rotation == 1);
        CHECK(cells(b.piece) == "4,5;4,6;4,7;5,6;");
        press(b, BLOCKS_CCW);
        CHECK(b.piece.rotation == 0);
        // An I standing at the left wall: turned, it is kicked right (0 -> R's second test: R->2).
        b.piece = {BLOCKS_I, 1, -2, 5};
        REQUIRE(blocks_fits(&b, &b.piece));
        CHECK(cells(b.piece) == "0,5;0,6;0,7;0,8;");
        press(b, BLOCKS_CW);
        CHECK(b.piece.rotation == 2);
        CHECK(cells(b.piece) == "0,7;1,7;2,7;3,7;");
        // The O doesn't turn.
        b.piece = {BLOCKS_O, 0, 3, 5};
        press(b, BLOCKS_CW);
        CHECK((b.events & BLOCKS_TURNED) == 0);
}

TEST_CASE("Blocks: gravity, soft drop, the lock delay") {
        CHECK(blocks_gravity(1) == 1378); // a row in 0.793 s (47.6 ticks), up
        CHECK(blocks_gravity(20) > 65536);
        blocks_t b;
        blocks_start(&b, 1, 99);
        const int8_t y = b.piece.y;
        for (int i = 0; i < 47; ++i) blocks_tick(&b, &None);
        CHECK(b.piece.y == y);
        blocks_tick(&b, &None);
        CHECK(b.piece.y == y + 1);
        // Soft drop: 20 times, a point a row.
        const blocks_input_t soft = {BLOCKS_SOFT, 0};
        for (int i = 0; i < 3; ++i) blocks_tick(&b, &soft);
        CHECK(b.piece.y == y + 2);
        CHECK(b.score == 1);
        // On the floor: it locks after the delay.
        b.piece.y = (int8_t)(b.piece.y + blocks_drop_distance(&b));
        for (int i = 0; i < BLOCKS_LOCK_TICKS - 1; ++i) {
                blocks_tick(&b, &None);
                CHECK((b.events & BLOCKS_LOCKED) == 0);
        }
        blocks_tick(&b, &None);
        CHECK((b.events & BLOCKS_LOCKED) != 0);
}

TEST_CASE("Blocks: moves put the lock off, so many times") {
        blocks_t b;
        blocks_start(&b, 1, 5);
        b.piece = {BLOCKS_T, 0, 3, 20}; // its bottom on the floor
        REQUIRE(blocks_drop_distance(&b) == 0);
        int ticks = 0;
        for (int i = 0; i < 100 && (b.events & BLOCKS_LOCKED) == 0; ++i, ++ticks) {
                press(b, i % 2 == 0 ? BLOCKS_LEFT : BLOCKS_RIGHT);
                for (int k = 0; k < 20 && (b.events & BLOCKS_LOCKED) == 0; ++k, ++ticks) blocks_tick(&b, &None);
        }
        CHECK((b.events & BLOCKS_LOCKED) != 0);
        CHECK(ticks > BLOCKS_LOCK_RESETS * 20); // put off each time, then not
        CHECK(ticks < 100 * 21);
}

TEST_CASE("Blocks: auto-repeat") {
        blocks_t b;
        blocks_start(&b, 1, 3);
        b.piece = {BLOCKS_T, 0, 5, 5};
        b.gravity = 0;
        press(b, BLOCKS_LEFT);
        CHECK(b.piece.x == 4);
        const blocks_input_t held = {BLOCKS_LEFT, 0};
        for (int i = 1; i < BLOCKS_DAS_TICKS; ++i) blocks_tick(&b, &held);
        CHECK(b.piece.x == 4);
        blocks_tick(&b, &held); // the delay over
        CHECK(b.piece.x == 3);
        blocks_tick(&b, &held);
        CHECK(b.piece.x == 3);
        blocks_tick(&b, &held); // then every ARR
        CHECK(b.piece.x == 2);
        for (int i = 0; i < 20; ++i) blocks_tick(&b, &held);
        CHECK(b.piece.x == 0); // at the wall
        blocks_tick(&b, &None);
        CHECK(b.repeat_dir == 0);
}

TEST_CASE("Blocks: clears, scoring, levels") {
        blocks_t b;
        blocks_start(&b, 1, 11);
        // Four rows full but for column 0; an I stood there.
        for (int y = BLOCKS_HEIGHT - 4; y < BLOCKS_HEIGHT; ++y)
                for (int x = 1; x < BLOCKS_WIDTH; ++x) b.field[y][x] = BLOCKS_O;
        b.field[BLOCKS_HEIGHT - 5][5] = BLOCKS_T; // above them: moves down 4
        b.piece = {BLOCKS_I, 1, -2, 5};
        b.lines = 8;
        press(b, BLOCKS_HARD);
        CHECK((b.events & BLOCKS_CLEARED) != 0);
        CHECK((b.events & BLOCKS_LEVELLED) != 0);
        CHECK(b.clearing_count == 4);
        CHECK(b.score == 800 * 1 + 2 * 13); // at the level it was cleared at; the drop's 13 rows
        CHECK(b.level == 2);
        CHECK(b.lines == 12);
        // Three blinks, lit first.
        std::string blinks;
        for (int i = 0; i < BLOCKS_CLEAR_TICKS; ++i) {
                if (i % BLOCKS_BLINK_TICKS == 0) blinks += blocks_flashing(&b) ? '#' : '.';
                blocks_tick(&b, &None);
        }
        CHECK(blinks == "#.#.#.");
        // Emptied, then the rows above drop a row at a time; then the next piece.
        CHECK(b.field[BLOCKS_HEIGHT - 1][1] == BLOCKS_NONE);
        CHECK(b.field[BLOCKS_HEIGHT - 5][5] == BLOCKS_T);
        int settled = 0;
        for (int i = 0; i < 4 * BLOCKS_SETTLE_TICKS; ++i) {
                blocks_tick(&b, &None);
                if ((b.events & BLOCKS_SETTLED) != 0) ++settled;
                if (i == BLOCKS_SETTLE_TICKS - 1) CHECK(b.field[BLOCKS_HEIGHT - 4][5] == BLOCKS_T); // a row down
        }
        CHECK(settled == 4);
        CHECK(b.clearing_count == 0);
        CHECK(b.field[BLOCKS_HEIGHT - 1][5] == BLOCKS_T);
        for (int x = 0; x < BLOCKS_WIDTH; ++x)
                if (x != 5) CHECK(b.field[BLOCKS_HEIGHT - 1][x] == BLOCKS_NONE);
}

TEST_CASE("Blocks: hold, once a piece") {
        blocks_t b;
        blocks_start(&b, 1, 21);
        const uint8_t first = b.piece.type, second = b.next[0];
        press(b, BLOCKS_HOLD);
        CHECK(b.hold == first);
        CHECK(b.piece.type == second);
        press(b, BLOCKS_HOLD);
        CHECK(b.hold == first); // not again until it locks
        press(b, BLOCKS_HARD);
        press(b, BLOCKS_HOLD);
        CHECK(b.piece.type == first);
}

TEST_CASE("Blocks: the game ends when a piece can't come in") {
        blocks_t b;
        blocks_start(&b, 1, 8);
        for (int y = 2; y < BLOCKS_HEIGHT; ++y)
                for (int x = 0; x < BLOCKS_WIDTH; ++x) b.field[y][x] = (x + y) % 2 == 0 ? BLOCKS_S : BLOCKS_NONE;
        for (int x = 0; x < BLOCKS_WIDTH; ++x) b.field[1][x] = x == 0 ? BLOCKS_NONE : BLOCKS_Z;
        press(b, BLOCKS_HARD);
        CHECK(b.over);
        CHECK((b.events & BLOCKS_OVER) != 0);
        press(b, BLOCKS_HARD);
        CHECK(b.events == 0);
}
