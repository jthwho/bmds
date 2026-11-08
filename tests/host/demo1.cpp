/**
 * @file      demo1.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's palette (programs/demo1/pal.c, devplan/demo.md §7): crossfades between parts,
 * flashes over them, cycling, and the overlay's entries kept; its song (song.c) valid and in order.
 */

#include <doctest.h>

#include <string>
#include <vector>

extern "C" {
#include "../../programs/demo1/pal.h"
#include "../../programs/demo1/song.h"
#include "../../sdk/game/seq_internal.h"
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

        // A veil: held at its amount until set again; the overlay's kept.
        pal_veil(&pal, 0x000000, 0.5f);
        shown = pal_update(&pal, 0.5f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0x7f0000);
        CHECK(shown[PAL_PANEL] == 0x101820);
        CHECK(pal_update(&pal, 0.5f) == nullptr); // nothing moved
        pal_veil(&pal, 0x000000, 0.0f);
        shown = pal_update(&pal, 0.016f);
        REQUIRE(shown != nullptr);
        CHECK(shown[10] == 0xff0000);

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

TEST_CASE("demo1: the song is a valid song, 104 bars at 140, its events in order") {
        const bmds_seq_song_t *song = song_get();
        REQUIRE(song == song_get()); // made once
        bmds_seq_t *q = nullptr;
        REQUIRE(bmds_seq_make(song, &q) == 0);
        bmds_seq_start_at(q, 0, 0);
        struct Seen {
                        int64_t last = 0;
                        int     events = 0, ons = 0, out_of_order = 0;
                        int     sounding[16][128] = {};
                        int     doubled = 0; // a note on while the same note sounds on its channel
                        int     hook = 0, bells = 0; // the melody's notes, and the vibraphone's
        } seen;
        const int64_t bar = 16 * 107142857ll; // sixteen steps, each rounded to the ns as the sequencer does
        bmds_seq_schedule(
                q, 104 * bar,
                [](void *arg, int64_t ns, const uint8_t *b, uint32_t) {
                        Seen *x = static_cast<Seen *>(arg);
                        if (ns < x->last) ++x->out_of_order;
                        x->last = ns;
                        ++x->events;
                        const int ch = b[0] & 15;
                        if ((b[0] & 0xf0) == 0x90) {
                                if (x->sounding[ch][b[1]] != 0 && ch != 9) ++x->doubled;
                                ++x->sounding[ch][b[1]];
                                ++x->ons;
                                x->hook += ch == 3;
                                x->bells += ch == 4;
                        }
                        if ((b[0] & 0xf0) == 0x80) --x->sounding[ch][b[1]];
                },
                &seen);
        CHECK(seen.out_of_order == 0);
        CHECK(seen.doubled == 0);
        CHECK(seen.ons > 2000);
        CHECK(seen.hook == 7 * 42);  // drops A and B, the hive (16 bars each), the finale (8): seven times
        CHECK(seen.bells == 6 * 42); // the break, the build, drop B, the finale, the outro
        bmds_seq_position_t pos{};
        pos.size = sizeof(pos);
        REQUIRE(bmds_seq_position(q, 104 * bar - 1, &pos) == 0);
        CHECK(pos.section == SONG_OUTRO);
        CHECK(pos.loops == 0);
        REQUIRE(bmds_seq_position(q, 104 * bar + 1, &pos) == 0);
        CHECK(pos.section == SONG_INTRO);
        CHECK(pos.loops == 1);
        REQUIRE(bmds_seq_position(q, 16 * bar, &pos) == 0);
        CHECK(std::string(pos.cue) == "drop");
        bmds_seq_close(q);
        return;
}
