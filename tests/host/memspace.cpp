/**
 * @file      memspace.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <cstring>
#include <bmds/memspace.h>

TEST_CASE("MemSpace") {
        CHECK(std::strcmp(bmds::memSpaceName(bmds::MemSpace::Internal), "Internal") == 0);
        CHECK(std::strcmp(bmds::memSpaceName(bmds::MemSpace::PsramDma), "PsramDma") == 0);
        CHECK(std::strcmp(bmds::memSpaceName(bmds::MemSpace::Fast), "Fast") == 0);
        CHECK(std::strcmp(bmds::memSpaceName(static_cast<bmds::MemSpace>(200)), "Unknown") == 0);
        CHECK(static_cast<size_t>(bmds::MemSpace::Fast) + 1 == bmds::MemSpaceCount);
}
