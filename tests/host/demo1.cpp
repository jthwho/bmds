/**
 * @file      demo1.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's palette (programs/demo1/pal.c, devplan/demo.md §7): crossfades between parts,
 * flashes over them, cycling, and the overlay's entries kept.
 */

#include <doctest.h>

#include <vector>

extern "C" {
#include "../../programs/demo1/pal.h"
}

TEST_CASE("demo1: the palette fades, flashes and cycles, the overlay's entries kept") {
        pal_t pal;
        pal_init(&pal);
        const uint32_t *shown = pal_update(&pal, 0.0f);
        REQUIRE(shown != nullptr);
        CHECK(shown[0] == 0x000000);
        CHECK(shown[PAL_TEXT] == 0xf0f4f8);
        CHECK(pal_update(&pal, 0.016f) == nullptr); // nothing changed

        // A part's palette at once, then another faded to over a second.
        std::vector<uint32_t> white(PAL_PARTS, 0xffffff), red(PAL_PARTS, 0xff0000);
        pal_set_base(&pal, white.data(), 0.0f);
        shown = pal_update(&pal, 0.016f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xffffff);
        pal_set_base(&pal, red.data(), 1.0f);
        shown = pal_update(&pal, 0.5f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xff7f7f); // half way (127.5 rounds to the side it left)
        CHECK(shown[PAL_TEXT] == 0xf0f4f8);
        shown = pal_update(&pal, 0.6f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xff0000); // there, exactly
        CHECK(pal_update(&pal, 0.016f) == nullptr);

        // A flash: white over everything, gone in half a second at 2 a second.
        pal_flash(&pal, 0xffffff, 1.0f, 2.0f);
        shown = pal_update(&pal, 0.0f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xffffff);
        CHECK(shown[PAL_PANEL] == 0x101820); // the overlay's own colours stay
        shown = pal_update(&pal, 0.25f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xff8080);
        shown = pal_update(&pal, 0.3f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xff0000);
        CHECK(pal_update(&pal, 0.016f) == nullptr);

        // Cycling: entries move, the overlay's don't, and the edge of the parts' range holds.
        std::vector<uint32_t> ramp(PAL_PARTS);
        for (uint32_t i = 0; i < PAL_PARTS; ++i) ramp[i] = i;
        pal_set_base(&pal, ramp.data(), 0.0f);
        pal_rotate(&pal, 0, PAL_PARTS + 10, 1);
        shown = pal_update(&pal, 0.016f);
        REQUIRE(shown != nullptr);
        CHECK(shown[0] == PAL_PARTS - 1);
        CHECK(shown[1] == 0);
        CHECK(shown[PAL_TEXT] == 0xf0f4f8);
        const uint32_t three[3] = {7, 8, 9};
        pal_set_entries(&pal, PAL_PARTS - 1, 3, three); // cut at the parts' end
        shown = pal_update(&pal, 0.016f);
        REQUIRE(shown != nullptr);
        CHECK(shown[PAL_PARTS - 1] == 7);
        CHECK(shown[PAL_ACCENT] == 0x40e0ff);
}
