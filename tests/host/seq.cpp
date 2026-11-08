/**
 * @file      seq.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's sequencer (sdk/game/seq.c, devplan/sequencer.md): the timeline, swing, the
 * scheduler's order and its note-offs, the hits and the position, with no port and no thread.
 */

#include <doctest.h>

#include <string>
#include <vector>

extern "C" {
#include "../../sdk/game/seq_internal.h"
}

namespace {

        struct Event {
                        int64_t              ns;
                        std::vector<uint8_t> bytes;
        };

        void collect(void *arg, int64_t heard_ns, const uint8_t *bytes, uint32_t length) {
                static_cast<std::vector<Event> *>(arg)->push_back(
                        {heard_ns, std::vector<uint8_t>(bytes, bytes + length)});
                return;
        }

        constexpr int64_t Ms = 1000000;

        // 120 BPM, sixteenths: a step is 125 ms, a bar 2 s.
        const bmds_seq_step_t kick[4] = {{36, 100, 1, 0}, {}, {}, {}};
        const bmds_seq_step_t bass[2] = {{45, 90, 2, 0}, {}};
        const uint8_t         cutoff[2] = {10, 0xff};
        const bmds_seq_lane_t lanes[1] = {{74, {0, 0, 0}, cutoff}};

        const bmds_seq_pattern_t drum_patterns[1] = {{sizeof(bmds_seq_pattern_t), 4, kick, 0, nullptr}};
        const bmds_seq_pattern_t bass_patterns[1] = {{sizeof(bmds_seq_pattern_t), 2, bass, 1, lanes}};
        const bmds_seq_track_t   tracks[2] = {
                {sizeof(bmds_seq_track_t), "drums", 9, 0, 100, 64, 1, drum_patterns},
                {sizeof(bmds_seq_track_t), "bass", 0, 38, 100, 64, 1, bass_patterns},
        };
        const int8_t             both[2] = {0, 0}, drums_only[2] = {0, -1};
        const bmds_seq_section_t sections[2] = {
                {sizeof(bmds_seq_section_t), "intro", 1, 0.0f, 0, drums_only},
                {sizeof(bmds_seq_section_t), "main", 1, 0.0f, 12, both},
        };

        bmds_seq_song_t song(float swing, int32_t loop) {
                return {sizeof(bmds_seq_song_t), "test", 120.0f, 0, 0, swing, 2, tracks, 2, sections, loop};
        }

} // namespace

TEST_CASE("seq: steps in time order, offs before the next step, transposed but not the drums") {
        const bmds_seq_song_t s = song(0.0f, -1);
        bmds_seq_t           *q = nullptr;
        REQUIRE(bmds_seq_make(&s, &q) == 0);
        bmds_seq_start_at(q, 1000 * Ms, 20 * Ms);
        std::vector<Event> all;
        bmds_seq_schedule(q, INT64_MAX, collect, &all);
        for (size_t i = 1; i < all.size(); ++i) CHECK(all[i].ns >= all[i - 1].ns);

        // The intro: a kick each beat, its off a millisecond before the next step.
        REQUIRE(all.size() >= 2);
        CHECK(all[0].ns == 1000 * Ms);
        CHECK(all[0].bytes == std::vector<uint8_t>{0x99, 36, 100});
        CHECK(all[1].ns == 1124 * Ms);
        CHECK(all[1].bytes == std::vector<uint8_t>{0x89, 36, 0});

        // The main section, 2 s in: the bass an octave up (A2 + 12), its cutoff on even steps only,
        // held two steps; the drums as they were.
        int ons = 0, ccs = 0, bass_offs = 0;
        for (const Event &e : all) {
                if (e.ns < 3000 * Ms) continue;
                if (e.bytes[0] == 0x90) {
                        CHECK(e.bytes[1] == 57);
                        ++ons;
                }
                if (e.bytes[0] == 0xb0) {
                        CHECK(e.bytes == std::vector<uint8_t>{0xb0, 74, 10});
                        CHECK((e.ns - 3000 * Ms) % (250 * Ms) == 0);
                        ++ccs;
                }
                if (e.bytes[0] == 0x80) {
                        CHECK((e.ns - 3000 * Ms) % (250 * Ms) == 249 * Ms);
                        ++bass_offs;
                }
                if (e.bytes[0] == 0x99) CHECK(e.bytes[1] == 36);
        }
        CHECK(ons == 8);
        CHECK(ccs == 8);
        CHECK(bass_offs == 8); // the song ends: every note off, the last at the end
        CHECK(all.back().ns <= 5000 * Ms);
        bmds_seq_close(q);
        return;
}

TEST_CASE("seq: scheduled in pieces as in one") {
        const bmds_seq_song_t s = song(0.25f, 0);
        bmds_seq_t           *a = nullptr, *b = nullptr;
        REQUIRE(bmds_seq_make(&s, &a) == 0);
        REQUIRE(bmds_seq_make(&s, &b) == 0);
        bmds_seq_start_at(a, 0, 0);
        bmds_seq_start_at(b, 0, 0);
        std::vector<Event> once, pieces;
        bmds_seq_schedule(a, 9000 * Ms, collect, &once);
        for (int64_t h = 20 * Ms; h <= 9000 * Ms; h += 20 * Ms) bmds_seq_schedule(b, h, collect, &pieces);
        REQUIRE(once.size() == pieces.size());
        for (size_t i = 0; i < once.size(); ++i) {
                CHECK(once[i].ns == pieces[i].ns);
                CHECK(once[i].bytes == pieces[i].bytes);
        }
        bmds_seq_close(a);
        bmds_seq_close(b);
        return;
}

TEST_CASE("seq: position and hits, round the loop") {
        const bmds_seq_song_t s = song(0.0f, 1);
        bmds_seq_t           *q = nullptr;
        REQUIRE(bmds_seq_make(&s, &q) == 0);
        bmds_seq_position_t pos{};
        pos.size = sizeof(pos);
        CHECK(bmds_seq_position(q, 0, &pos) == -BMDS_EAGAIN);
        bmds_seq_start_at(q, 100 * Ms, 0);
        CHECK(bmds_seq_position(q, 99 * Ms, &pos) == -BMDS_EAGAIN);

        REQUIRE(bmds_seq_position(q, 100 * Ms + 1625 * Ms, &pos) == 0); // 13 steps in
        CHECK(pos.section == 0);
        CHECK(pos.bar == 0);
        CHECK(pos.beat == 3);
        CHECK(pos.step == 1);
        CHECK(pos.beat_phase == doctest::Approx(0.25));
        CHECK(pos.bar_phase == doctest::Approx(0.8125));
        CHECK(pos.loops == 0);

        // Past the end (4 s): round the main section again.
        REQUIRE(bmds_seq_position(q, 100 * Ms + 4500 * Ms, &pos) == 0);
        CHECK(pos.section == 1);
        CHECK(std::string(pos.cue) == "main");
        CHECK(pos.beat == 1);
        CHECK(pos.loops == 1);
        CHECK(pos.ended == 0);
        REQUIRE(bmds_seq_position(q, 100 * Ms + 6100 * Ms, &pos) == 0);
        CHECK(pos.loops == 2);
        CHECK(pos.seconds == doctest::Approx(6.1));

        // The hits: what the scheduler made, by track and by time.
        std::vector<Event> all;
        bmds_seq_schedule(q, 100 * Ms + 4100 * Ms, collect, &all);
        bmds_seq_hit_t hits[64];
        const uint32_t kicks = bmds_seq_hits(q, 0, INT64_MAX, 1u << 0, hits, 64);
        CHECK(kicks == 9); // 4 a bar, two bars, and the loop's first
        for (uint32_t i = 1; i < kicks; ++i) CHECK(hits[i].time_ns - hits[i - 1].time_ns == 500 * Ms);
        CHECK(bmds_seq_hits(q, 100 * Ms, 600 * Ms, 1u << 0, hits, 64) == 1); // (from, to]: the second only
        CHECK(bmds_seq_hits(q, 0, INT64_MAX, 1u << 1, hits, 64) == 9);       // every other step, and the loop's first
        bmds_seq_hit_t last{};
        REQUIRE(bmds_seq_last_hit(q, 100 * Ms + 2300 * Ms, 1, &last) == 0);
        CHECK(last.time_ns == 100 * Ms + 2250 * Ms);
        CHECK(last.note == 57);
        CHECK(bmds_seq_last_hit(q, 100 * Ms + 1999 * Ms, 1, &last) == -BMDS_ENOENT);
        bmds_seq_close(q);
        return;
}

TEST_CASE("seq: when a cue is heard, round the loop") {
        const bmds_seq_song_t s = song(0.0f, 1);
        bmds_seq_t           *q = nullptr;
        REQUIRE(bmds_seq_make(&s, &q) == 0);
        int64_t t = 0;
        CHECK(bmds_seq_time_of(q, 1, 0, 0, 0, &t) == -BMDS_EAGAIN);
        bmds_seq_start_at(q, 100 * Ms, 0);
        // A bar is 2 s: main's beat 2 at 2 s + 1 s.
        REQUIRE(bmds_seq_time_of(q, 1, 0, 2, 0, &t) == 0);
        CHECK(t == 100 * Ms + 3000 * Ms);
        // Its position says so the other way round.
        bmds_seq_position_t pos{};
        pos.size = sizeof(pos);
        REQUIRE(bmds_seq_position(q, t, &pos) == 0);
        CHECK(pos.section == 1);
        CHECK(pos.beat == 2);
        // Passed: the loop's next time round (2 s each); the intro isn't played again.
        REQUIRE(bmds_seq_time_of(q, 1, 0, 2, t + 1, &t) == 0);
        CHECK(t == 100 * Ms + 5000 * Ms);
        CHECK(bmds_seq_time_of(q, 0, 0, 0, 200 * Ms, &t) == -BMDS_ENOENT);
        // No such bar or beat.
        CHECK(bmds_seq_time_of(q, 1, 1, 0, 0, &t) == -BMDS_EINVAL);
        CHECK(bmds_seq_time_of(q, 1, 0, 4, 0, &t) == -BMDS_EINVAL);
        CHECK(bmds_seq_time_of(q, 2, 0, 0, 0, &t) == -BMDS_EINVAL);
        bmds_seq_close(q);
        return;
}

TEST_CASE("seq: a bad song is refused") {
        bmds_seq_t     *q = nullptr;
        bmds_seq_song_t s = song(0.0f, -1);
        s.swing = 0.6f;
        CHECK(bmds_seq_make(&s, &q) == -BMDS_EINVAL);
        s = song(0.0f, 2);
        CHECK(bmds_seq_make(&s, &q) == -BMDS_EINVAL);
        const int8_t             bad[2] = {1, 0};
        const bmds_seq_section_t one[1] = {{sizeof(bmds_seq_section_t), nullptr, 1, 0.0f, 0, bad}};
        s = song(0.0f, -1);
        s.sections = one;
        s.section_count = 1;
        CHECK(bmds_seq_make(&s, &q) == -BMDS_EINVAL);
        CHECK(q == nullptr);
        return;
}

TEST_CASE("seq: swing puts each odd step that part of a step late") {
        const bmds_seq_step_t    every[2] = {{42, 80, 1, 0}, {42, 80, 1, BMDS_SEQ_ACCENT}};
        const bmds_seq_pattern_t pattern[1] = {{sizeof(bmds_seq_pattern_t), 2, every, 0, nullptr}};
        const bmds_seq_track_t   hats[1] = {{sizeof(bmds_seq_track_t), "hats", 9, 0, 100, 64, 1, pattern}};
        const int8_t             on[1] = {0};
        const bmds_seq_section_t one[1] = {{sizeof(bmds_seq_section_t), nullptr, 1, 0.0f, 0, on}};
        const bmds_seq_song_t    s = {sizeof(bmds_seq_song_t), "hats", 120.0f, 0, 0, 0.25f, 1, hats, 1, one, -1};
        bmds_seq_t              *q = nullptr;
        REQUIRE(bmds_seq_make(&s, &q) == 0);
        bmds_seq_start_at(q, 0, 0);
        std::vector<Event> all;
        bmds_seq_schedule(q, INT64_MAX, collect, &all);
        int ons = 0;
        for (const Event &e : all) {
                if (e.bytes[0] != 0x99) continue;
                const int64_t step = e.ns / (125 * Ms), into = e.ns % (125 * Ms);
                CHECK(into == ((step & 1) != 0 ? 31250000 : 0));
                CHECK(e.bytes[2] == ((step & 1) != 0 ? 104 : 80)); // the accent: + 24
                ++ons;
        }
        CHECK(ons == 16);
        for (size_t i = 1; i < all.size(); ++i) CHECK(all[i].ns >= all[i - 1].ns);
        bmds_seq_close(q);
        return;
}

TEST_CASE("seq: fades, in steps among the notes, a later one replacing what it overlaps") {
        const bmds_seq_song_t s = song(0.0f, -1);
        bmds_seq_t           *q = nullptr;
        REQUIRE(bmds_seq_make(&s, &q) == 0);
        bmds_seq_start_at(q, 0, 0);
        CHECK(bmds_seq_fade(q, 1.5f, 100 * Ms, 0) == -BMDS_EINVAL);
        CHECK(bmds_seq_fade(q, 0.5f, 100 * Ms, -1) == -BMDS_EINVAL);
        // Down to a quarter (half the volume: 40 log10) from 0.5 s over 0.2 s: 5 steps of 40 ms.
        REQUIRE(bmds_seq_fade(q, 0.25f, 500 * Ms, 200 * Ms) == 0);
        std::vector<Event> all;
        for (int64_t h = 20 * Ms; h <= 1000 * Ms; h += 20 * Ms) bmds_seq_schedule(q, h, collect, &all);
        std::vector<Event> volumes;
        for (size_t i = 0; i < all.size(); ++i) {
                if (i > 0) CHECK(all[i].ns >= all[i - 1].ns);
                if ((all[i].bytes[0] & 0xf0) == 0xb0 && all[i].bytes[1] == 7) volumes.push_back(all[i]);
        }
        REQUIRE(volumes.size() == 10); // two tracks a step
        CHECK(volumes[0].ns == 540 * Ms);
        CHECK(volumes[9].ns == 700 * Ms);
        CHECK(volumes[8].bytes == std::vector<uint8_t>{0xb9, 7, 50});
        CHECK(volumes[9].bytes == std::vector<uint8_t>{0xb0, 7, 50});

        // Back up at 1.2 s over 0.4 s, but another at 1.3 s replaces its steps from then on.
        REQUIRE(bmds_seq_fade(q, 1.0f, 1200 * Ms, 400 * Ms) == 0);
        REQUIRE(bmds_seq_fade(q, 0.0f, 1300 * Ms, 0) == 0);
        all.clear();
        bmds_seq_schedule(q, 3000 * Ms, collect, &all);
        volumes.clear();
        for (const Event &e : all)
                if ((e.bytes[0] & 0xf0) == 0xb0 && e.bytes[1] == 7) volumes.push_back(e);
        REQUIRE(volumes.size() == 6); // 1.24 s, 1.28 s, then silence at 1.3 s
        CHECK(volumes[1].bytes[2] > 50);
        CHECK(volumes[4].ns == 1300 * Ms);
        CHECK(volumes[5].bytes == std::vector<uint8_t>{0xb0, 7, 0});

        // One asked for a time already sent goes at once, never before what was sent (a port
        // refuses an event out of order); the offs and steps stay in order with it.
        REQUIRE(bmds_seq_fade(q, 1.0f, 1000 * Ms, 0) == 0);
        all.clear();
        bmds_seq_schedule(q, 3500 * Ms, collect, &all);
        REQUIRE(!all.empty());
        CHECK(all[0].ns == 3000 * Ms - 1 * Ms); // the last sent: the bass's off before 3 s
        CHECK(all[0].bytes == std::vector<uint8_t>{0xb9, 7, 100});
        for (size_t i = 1; i < all.size(); ++i) CHECK(all[i].ns >= all[i - 1].ns);
        bmds_seq_close(q);
        return;
}
