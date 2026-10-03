/**
 * @file      span.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <array>
#include <vector>
#include <bmds/span.h>

using bmds::Span;

namespace {

        int sum(Span<const int> values) {
                int total = 0;
                for (int v : values) total += v;
                return total;
        }

} // namespace

TEST_CASE("Span") {
        int                array[] = {1, 2, 3, 4, 5};
        std::array<int, 3> stdArray = {10, 20, 30};
        std::vector<int>   vec = {7, 8};

        SUBCASE("construction from containers") {
                CHECK(Span<int>().isEmpty());
                CHECK(Span<int>(array).size() == 5);
                CHECK(Span<int>(stdArray).size() == 3);
                CHECK(Span<int>(vec).size() == 2);
                CHECK(Span<int>(array + 1, 2)[0] == 2);
        }

        SUBCASE("implicit conversion to const") {
                Span<int> s(array);
                CHECK(sum(s) == 15);
                CHECK(sum(array) == 15);
                CHECK(sum(stdArray) == 60);
                CHECK(sum(vec) == 15);
                const std::array<int, 3> &constArray = stdArray;
                CHECK(sum(constArray) == 60);
        }

        SUBCASE("writes go through") {
                Span<int> s(array);
                s[0] = 100;
                CHECK(array[0] == 100);
        }

        SUBCASE("sub-spans clamp") {
                Span<int> s(array);
                CHECK(s.first(2).size() == 2);
                CHECK(s.first(99).size() == 5);
                CHECK(s.last(2).front() == 4);
                CHECK(s.last(99).size() == 5);
                CHECK(s.mid(1, 2).front() == 2);
                CHECK(s.mid(1, 2).back() == 3);
                CHECK(s.mid(3).size() == 2);
                CHECK(s.mid(4, 99).size() == 1);
                CHECK(s.mid(5).isEmpty());
                CHECK(s.mid(99).isEmpty());
        }

        SUBCASE("bytes") {
                Span<int> s(array);
                CHECK(s.sizeBytes() == 5 * sizeof(int));
                CHECK(s.asBytes().size() == 5 * sizeof(int));
                CHECK(s.asWritableBytes().size() == 5 * sizeof(int));
        }

        SUBCASE("constexpr") {
                static constexpr int      values[] = {1, 2, 3};
                constexpr Span<const int> s(values);
                static_assert(s.size() == 3);
                static_assert(s.mid(1).front() == 2);
        }
}
