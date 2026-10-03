/**
 * @file      result.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <bmds/result.h>
#include <bmds/stringview.h>

using bmds::Error;
using bmds::Result;

namespace {

        struct MoveOnly {
                        MoveOnly() = default;
                        explicit MoveOnly(int v) : value(v) {}
                        MoveOnly(MoveOnly &&) = default;
                        MoveOnly &operator=(MoveOnly &&) = default;
                        MoveOnly(const MoveOnly &) = delete;
                        MoveOnly &operator=(const MoveOnly &) = delete;
                        int       value = 0;
        };

        Result<int> parseDigit(char ch) {
                if (ch < '0' || ch > '9') return bmds::makeError<int>(Error::Invalid);
                return bmds::makeResult(ch - '0');
        }

} // namespace

TEST_CASE("Result") {
        SUBCASE("success") {
                auto r = parseDigit('7');
                CHECK(r.isOk());
                CHECK(r.value() == 7);
                CHECK(r.error() == Error::Ok);
        }

        SUBCASE("failure carries the error and a default value") {
                auto r = parseDigit('x');
                CHECK(r.isError());
                CHECK(r.error() == Error::Invalid);
                CHECK(r.value() == 0);
        }

        SUBCASE("structured bindings") {
                auto [value, err] = parseDigit('3');
                CHECK(value == 3);
                CHECK(err.isOk());
        }

        SUBCASE("move-only values") {
                auto [m, err] = bmds::makeResult(MoveOnly(42));
                CHECK(err.isOk());
                CHECK(m.value == 42);

                Result<MoveOnly> r(MoveOnly(5));
                MoveOnly         taken = std::move(r).value();
                CHECK(taken.value == 5);
        }

        SUBCASE("explicit value and error") {
                Result<bmds::StringView> r(bmds::StringView("partial"), Error::BufferTooSmall);
                CHECK(r.isError());
                CHECK(r.value() == "partial");
        }

        SUBCASE("constexpr") {
                constexpr auto r = bmds::makeResult(10);
                static_assert(r.isOk());
                static_assert(r.value() == 10);
        }
}
