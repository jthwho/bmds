/**
 * @file      language.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Unit tests for the interpreter's building blocks: Glob, Arith, Variables.
 */

#include <doctest.h>

#include <string>
#include <bmds/arith.h>
#include <bmds/glob.h>
#include <bmds/variables.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::Variables;
namespace Glob = bmds::Glob;

TEST_CASE("Glob: match") {
        CHECK(Glob::match("", ""));
        CHECK(Glob::match("*", ""));
        CHECK(Glob::match("*", "anything"));
        CHECK(Glob::match("a*c", "abbbc"));
        CHECK_FALSE(Glob::match("a*c", "abbbd"));
        CHECK(Glob::match("*.c", "main.c"));
        CHECK(Glob::match("?.?", "a.b"));
        CHECK_FALSE(Glob::match("?", ""));
        CHECK(Glob::match("[abc]x", "bx"));
        CHECK(Glob::match("[a-c][0-9]", "c7"));
        CHECK(Glob::match("[!a-c]", "d"));
        CHECK(Glob::match("[^a-c]", "d"));
        CHECK_FALSE(Glob::match("[!a-c]", "b"));
        CHECK(Glob::match("[]x]", "]"));
        CHECK(Glob::match("\\*", "*"));
        CHECK_FALSE(Glob::match("\\*", "a"));
        CHECK(Glob::match("[ab", "[ab")); // unterminated: literal
        CHECK(Glob::match("*a*b*c*", "xxaxxbxxcxx"));
        CHECK_FALSE(Glob::match("*a*b*c*", "xxaxxcxxbxx"));
        CHECK(Glob::hasMagic("a*"));
        CHECK_FALSE(Glob::hasMagic("a\\*"));
}

TEST_CASE("Glob: prefix and suffix") {
        const bmds::StringView p = "/a/b/c.tar.gz";
        CHECK(Glob::matchPrefix("*/", p, false) == 1);
        CHECK(Glob::matchPrefix("*/", p, true) == 5);
        CHECK(Glob::matchSuffix(".*", p, false) == 3);
        CHECK(Glob::matchSuffix(".*", p, true) == 7);
        CHECK(Glob::matchPrefix("x*", p, false) == bmds::StringView::NPos);
}

namespace {

        long long eval(const char *expr, Variables &vars) {
                bmds::Arith::Failure failure;
                auto [v, err] = bmds::Arith::evaluate(expr, vars, failure);
                REQUIRE(err.isOk());
                return v;
        }

        std::string failure(const char *expr) {
                Variables            vars;
                bmds::Arith::Failure f;
                auto [v, err] = bmds::Arith::evaluate(expr, vars, f);
                REQUIRE(err == Error::Invalid);
                return f.message;
        }

} // namespace

TEST_CASE("Arith") {
        Variables vars;
        CHECK(eval("", vars) == 0);
        CHECK(eval("1 + 2 * 3 - 4 / 2", vars) == 5);
        CHECK(eval("-2 ** 2", vars) == 4); // unary binds tighter, as in bash
        CHECK(eval("2 ** 3 ** 2", vars) == 512);
        CHECK(eval("7 % -3", vars) == 1);
        CHECK(eval("1 < 2 && 2 < 3", vars) == 1);
        CHECK(eval("0x1F | 0100 | 2#1", vars) == (0x1F | 64 | 1));
        CHECK(eval("36#z + 64#_", vars) == 35 + 63);
        CHECK(eval("5 > 3 ? 10 : 20", vars) == 10);
        CHECK(eval("(1, 2, 3)", vars) == 3);
        CHECK(eval("x = 5, y = x * 2, y", vars) == 10);
        CHECK(*vars.value("y") == "10");
        CHECK(eval("x += 3", vars) == 8);
        CHECK(eval("x++ + ++x", vars) == 18);
        CHECK(eval("x", vars) == 10);
        CHECK(eval("~0 << 4 >> 4", vars) == -1);
        CHECK(eval("9223372036854775807 + 1", vars) < 0);
        CHECK(eval("1 || (z = 1)", vars) == 1);
        CHECK(vars.value("z") == nullptr); // skipped side effect
        CHECK(eval("0 ? (z = 1) : 2", vars) == 2);
        CHECK(vars.value("z") == nullptr);
        CHECK(vars.set("e", "x * 2").isOk());
        CHECK(eval("e + 1", vars) == 21);
        CHECK(eval("unset_variable + 1", vars) == 1);

        CHECK(failure("1 / 0") == "division by 0");
        CHECK(failure("1 +") == "syntax error: operand expected");
        CHECK(failure("(1") == "missing `)'");
        CHECK(failure("08") == "value too great for base");
        CHECK(failure("2 ** -1") == "exponent less than 0");
        CHECK(failure("3 = 4") == "attempted assignment to non-variable");
        CHECK(failure("1 2") == "syntax error in expression");
        Variables loop;
        REQUIRE(loop.set("a", "a").isOk());
        bmds::Arith::Failure f;
        CHECK(bmds::Arith::evaluate("a", loop, f).error() == Error::Invalid);
        CHECK(std::string(f.message) == "expression recursion level exceeded");
}

TEST_CASE("Variables") {
        const size_t base = HostMemory::liveAllocations();
        {
                Variables v;
                CHECK(v.value("x") == nullptr);
                CHECK(v.set("b", "2").isOk());
                CHECK(v.set("a", "1").isOk());
                CHECK(v.set("c", "a long value that needs the heap").isOk());
                CHECK(v.entries().size() == 3);
                CHECK(v.entries()[0].name == "a"); // sorted
                CHECK(*v.value("b") == "2");
                CHECK(v.addFlags("d", Variables::Exported).isOk());
                CHECK(v.find("d") != nullptr);
                CHECK(v.value("d") == nullptr); // exists, but unset
                CHECK(v.addFlags("a", Variables::ReadOnly).isOk());
                CHECK(v.set("a", "x") == Error::ReadOnly);
                CHECK(v.unset("a") == Error::ReadOnly);
                CHECK(v.restore("a", true, "restored", Variables::IsSet).isOk()); // `local` scope end ignores read-only
                CHECK(*v.value("a") == "restored");
                CHECK(v.unset("b").isOk());
                CHECK(v.unset("nope").isOk());
                auto [copy, err] = v.clone();
                CHECK(err.isOk());
                CHECK(copy.entries().size() == v.entries().size());
                CHECK(*copy.value("c") == *v.value("c"));
                CHECK(copy.value("c")->data() != v.value("c")->data());
        }
        CHECK(HostMemory::liveAllocations() == base);
}
