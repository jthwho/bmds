/**
 * @file      hostname.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's host name (devplan/notifications.md §2): what may be configured, and the default
 * from the serial number.
 */

#include <doctest.h>

#include <string>

#include <bmds/hostname.h>

namespace HostName = bmds::HostName;

TEST_CASE("HostName: what may be configured") {
        CHECK(HostName::isValid(""));
        CHECK(HostName::isValid("bmds-a0b1c2d3e4f5"));
        CHECK(HostName::isValid("Studio-2"));
        CHECK(HostName::isValid("x"));
        CHECK_FALSE(HostName::isValid("-lead"));
        CHECK_FALSE(HostName::isValid("trail-"));
        CHECK_FALSE(HostName::isValid("has space"));
        CHECK_FALSE(HostName::isValid("dotted.name"));
        CHECK_FALSE(HostName::isValid("under_score"));
        CHECK(HostName::isValid(std::string(63, 'a').c_str()));
        CHECK_FALSE(HostName::isValid(std::string(64, 'a').c_str()));
}

TEST_CASE("HostName: the configured one, else bmds- and the serial's last six") {
        char out[HostName::MaxBytes + 1];
        CHECK(HostName::resolve("", "e8f60ae2e222", out) == 11);
        CHECK(std::string(out) == "bmds-e2e222");
        CHECK(HostName::resolve("studio", "e8f60ae2e222", out) == 6);
        CHECK(std::string(out) == "studio");
        // Another board's serial: only its letters and digits, in lower case.
        HostName::resolve("", "SN-00:1A:2B", out);
        CHECK(std::string(out) == "bmds-001a2b");
        HostName::resolve("", "42", out);
        CHECK(std::string(out) == "bmds-42");
        HostName::resolve("", "", out); // no serial
        CHECK(std::string(out) == "bmds");
        CHECK(HostName::isValid(out));
}
