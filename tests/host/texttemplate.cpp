/**
 * @file      texttemplate.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text templates (devplan/templates.md): the syntax, the names, the errors.
 */

#include <doctest.h>

#include <string>

#include <bmds/texttemplate.h>

namespace TextTemplate = bmds::TextTemplate;
using bmds::Error;

namespace {

        bmds::LocalTime afternoon() {
                bmds::LocalTime t;
                t.local.year = 2026;
                t.local.month = 10;
                t.local.day = 10;
                t.local.hour = 14;
                t.local.minute = 3;
                t.local.second = 27;
                t.local.weekday = 6;
                return t;
        }

        std::string expand(const char *text, Error *err = nullptr) {
                const bmds::LocalTime t = afternoon();
                TextTemplate::Values  v;
                v.home = "/home";
                v.hostname = "bmds-e2e222";
                v.serial = "e8f60ae2e222";
                v.time = &t;
                bmds::String out(bmds::MemSpace::Psram);
                const Error  e = TextTemplate::expand(text, v, out);
                if (err != nullptr) *err = e;
                return e.isOk() ? std::string(out.cStr()) : std::string("<error>");
        }

} // namespace

TEST_CASE("TextTemplate: the names") {
        CHECK(expand("${HOME}/Images/Screenshots/${HOSTNAME}_${DATE}_${TIME}.png") ==
              "/home/Images/Screenshots/bmds-e2e222_2026-10-10_14-03-27.png");
        CHECK(expand("${SERIAL}") == "e8f60ae2e222");
        CHECK(expand("${HOME}/logs/${DATE:%Y/%m}/run-${TIME:%H%M}.txt") == "/home/logs/2026/10/run-1403.txt");
        CHECK(expand("${DATE:%a %-d %b}") == "Sat 10 Oct");
        CHECK(expand("${TIME:}") == ""); // an empty format: nothing
}

TEST_CASE("TextTemplate: literals and $$") {
        CHECK(expand("") == "");
        CHECK(expand("plain/text.png") == "plain/text.png");
        CHECK(expand("cost $$5 ${HOME}$$") == "cost $5 /home$");
}

TEST_CASE("TextTemplate: errors") {
        const char *bad[] = {
                "$",       "a$b",     "$HOME",     "${",    "${HOME",    "${}",
                "${home}", "${NOPE}", "${HOME:x}", "${1A}", "${DATE:%Y", "${HOME }",
        };
        for (const char *t : bad) {
                CAPTURE(t);
                CHECK(TextTemplate::check(t) == Error::Invalid);
                Error err;
                CHECK(expand(t, &err) == "<error>");
                CHECK(err == Error::Invalid);
        }
        CHECK(TextTemplate::check("${HOME}/${HOSTNAME}_${SERIAL}_${DATE}_${TIME:%H}") == Error::Ok);
        CHECK(TextTemplate::check("$$") == Error::Ok);
}
