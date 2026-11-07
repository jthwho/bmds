/**
 * @file      song.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's track (devplan/demo.md §4): 140 BPM, A minor, Am-F-C-G a bar each. Four on the
 * floor, a clap on 2 and 4, sixteenth hats under off-beat open ones, an acid bass following the
 * chords with its filter (controller 74) opening and closing over four bars, a sawtooth arpeggio,
 * a warm pad of three voices, a crash every eight bars, and the hook: an eight-bar melody on a
 * square lead in 3-3-2 (dotted eighths and an eighth), answered and lifted in its second half,
 * sung again an octave up on a vibraphone in the break. The patterns are written out by code at
 * the first call (the rules are shorter than the steps), into static arrays the sequencer reads.
 */

#include <math.h>
#include <string.h>
#include "song.h"

#define BAR 16           /* steps */
#define PHRASE (4 * BAR) /* the chords' round */
#define BUILD (8 * BAR)
#define HOOK (8 * BAR) /* the melody's round */

/* Notes. */
#define KICK 36
#define CLAP 39
#define CLOSED_HAT 42
#define OPEN_HAT 46
#define CRASH 49

/* Am, F, C, G: the bass's root, the arpeggio's and the pad's voicings. */
static const uint8_t roots[4] = {33, 29, 36, 31};                                           /* A1 F1 C2 G1 */
static const uint8_t arps[4][3] = {{69, 72, 76}, {65, 69, 72}, {67, 72, 76}, {67, 71, 74}}; /* close, up */
static const uint8_t pads[4][3] = {{57, 60, 64}, {57, 60, 65}, {55, 60, 64}, {55, 59, 62}}; /* led voices */

static struct {
                bmds_seq_step_t    kick[BAR], kick_build[BUILD], clap[BAR], hats_open[BAR], hats[BAR];
                bmds_seq_step_t    bass[PHRASE], bass_busy[PHRASE], lead[PHRASE], pad[3][PHRASE], crash[BUILD];
                bmds_seq_step_t    melody[HOOK], bells[HOOK];
                uint8_t            sweep[PHRASE];
                bmds_seq_lane_t    bass_lanes[1];
                bmds_seq_pattern_t kick_p[2], clap_p[1], hats_p[2], bass_p[2], lead_p[1], pad_p[3][1], crash_p[1];
                bmds_seq_pattern_t melody_p[1], bells_p[1];
                bmds_seq_track_t   tracks[SONG_TRACKS];
                bmds_seq_section_t sections[SONG_SECTIONS];
                bmds_seq_song_t    song;
                int                made;
} s;

/* Each track's pattern a section (-1: silent), in the order of the enums. */
static const int8_t arrangement[SONG_SECTIONS][SONG_TRACKS] = {
        /*               kick clap hats bass lead pad1 pad2 pad3 crash melody bells */
        [SONG_INTRO] = {0, -1, 0, -1, -1, 0, 0, 0, 0, -1, -1}, [SONG_BUILD] = {1, 0, 1, 0, -1, -1, -1, -1, 0, -1, -1},
        [SONG_DROP_A] = {0, 0, 1, 0, 0, -1, -1, -1, 0, 0, -1}, [SONG_GROOVE] = {0, 0, 1, 1, 0, -1, -1, -1, 0, -1, -1},
        [SONG_BREAK] = {-1, -1, 0, -1, -1, 0, 0, 0, 0, -1, 0}, [SONG_BUILD_2] = {1, 0, 1, 0, 0, -1, -1, -1, 0, -1, 0},
        [SONG_DROP_B] = {0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0},     [SONG_FINALE] = {0, 0, 1, 1, 0, -1, -1, -1, 0, 0, 0},
        [SONG_OUTRO] = {0, -1, 0, -1, -1, 0, 0, 0, 0, -1, 0},
};

static const struct {
                const char *cue;
                uint32_t    bars;
} section_info[SONG_SECTIONS] = {
        {"intro", 8}, {"build", 8}, {"drop", 16},  {"groove", 8}, {"break", 8},
        {"build", 8}, {"drop", 16}, {"finale", 8}, {"outro", 8},
};

static bmds_seq_step_t note(uint32_t n, uint32_t velocity, uint32_t length, uint32_t flags) {
        return (bmds_seq_step_t){(uint8_t)n, (uint8_t)velocity, (uint8_t)length, (uint8_t)flags};
}

static bmds_seq_pattern_t pattern(const bmds_seq_step_t *steps, uint32_t count, const bmds_seq_lane_t *lanes,
                                  uint32_t lane_count) {
        return (bmds_seq_pattern_t){sizeof(bmds_seq_pattern_t), count, steps, lane_count, lanes};
}

static bmds_seq_track_t track(const char *name, uint32_t channel, uint32_t program, uint32_t volume, uint32_t pan,
                              const bmds_seq_pattern_t *patterns, uint32_t count) {
        return (bmds_seq_track_t){sizeof(bmds_seq_track_t), name, channel, program, volume, pan, count, patterns};
}

static void write_drums(void) {
        for (int i = 0; i < BAR; ++i) {
                if (i % 4 == 0) s.kick[i] = note(KICK, 115, 1, 0);
                if (i == 4 || i == 12) s.clap[i] = note(CLAP, 100, 1, 0);
                if (i % 4 == 2) s.hats_open[i] = note(OPEN_HAT, 60, 1, 0);
                /* Sixteenths: the off-beat open, the rest closed, louder on the eighths. */
                static const uint8_t closed[BAR] = {44, 26, 0, 30, 40, 24, 0, 34, 44, 26, 0, 30, 40, 28, 0, 38};
                s.hats[i] = i % 4 == 2 ? note(OPEN_HAT, 66, 1, 0) : note(CLOSED_HAT, closed[i], 1, 0);
        }
        /* The build's kick: quarters, eighths, sixteenths rising, a beat of silence before the drop. */
        for (int i = 0; i < BUILD; ++i) {
                const int bar = i / BAR;
                const int every = bar < 4 ? 4 : bar < 6 ? 2 : 1;
                if (i >= BUILD - 4 || i % every != 0) continue;
                const uint32_t velocity = bar < 6 ? 110 : 70 + (uint32_t)(i - 6 * BAR) * 50 / (2 * BAR);
                s.kick_build[i] = note(KICK, velocity, 1, 0);
        }
        s.crash[0] = note(CRASH, 100, 1, 0);
        return;
}

static void write_tonal(void) {
        /* The acid: semitones over the bar's root (-1 a rest), accents, a held note. */
        static const int8_t riff[BAR] = {0, -1, 0, 12, 0, -1, 7, 0, 0, 12, 0, 7, 0, 10, 12, -1};
        static const int8_t busy[BAR] = {0, 0, 12, 0, 7, 0, 12, 10, 0, 0, 12, 0, 7, 12, 10, 7};
        static const int8_t up[16] = {0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0, 2, 1, 3}; /* 3: the root an octave up */
        for (int i = 0; i < PHRASE; ++i) {
                const int      chord = i / BAR, k = i % BAR;
                const uint32_t accent = k == 0 || k == 8 || k == 14 ? BMDS_SEQ_ACCENT : 0;
                if (riff[k] >= 0) s.bass[i] = note(roots[chord] + (uint32_t)riff[k], 92, k == 14 ? 2 : 1, accent);
                s.bass_busy[i] = note(roots[chord] + (uint32_t)busy[k], 88, 1, k % 4 == 0 ? BMDS_SEQ_ACCENT : 0);
                const uint32_t a = up[k] == 3 ? arps[chord][0] + 12u : arps[chord][up[k]];
                s.lead[i] = note(a, k % 8 == 0 ? 78 : 62, 1, 0);
                for (int v = 0; v < 3; ++v)
                        if (k == 0) s.pad[v][i] = note(pads[chord][v], 56, BAR, 0);
                /* The filter: closed to open and back over the four bars. */
                s.sweep[i] = (uint8_t)(36.0f + 60.0f * (0.5f - 0.5f * cosf(6.2831853f * (float)i / PHRASE)));
        }
        return;
}

/* The hook, a bar a line: step, note, length. Over Am, F, C, G twice; the second half answers the
 * first and climbs, ending on A held over G (back to Am at the top). */
#define A4 69
#define B4 71
#define C5 72
#define D5 74
#define E5 76
#define F5 77
#define G5 79
#define A5 81
#define B5 83
#define C6 84
static const uint8_t hook[][4] = {
        /* bar, step, note, length */
        {0, 0, E5, 3}, {0, 3, E5, 3}, {0, 6, D5, 2}, {0, 8, C5, 3},  {0, 11, D5, 3}, {0, 14, E5, 2}, /* Am */
        {1, 0, F5, 3}, {1, 3, E5, 3}, {1, 6, C5, 2}, {1, 8, A4, 6},  {1, 14, C5, 2},                 /* F */
        {2, 0, G5, 3}, {2, 3, E5, 3}, {2, 6, C5, 2}, {2, 8, E5, 3},  {2, 11, G5, 3}, {2, 14, A5, 2}, /* C */
        {3, 0, G5, 6}, {3, 6, D5, 2}, {3, 8, B4, 4}, {3, 12, D5, 4},                                 /* G */
        {4, 0, E5, 3}, {4, 3, E5, 3}, {4, 6, D5, 2}, {4, 8, C5, 3},  {4, 11, D5, 3}, {4, 14, E5, 2}, /* Am */
        {5, 0, F5, 3}, {5, 3, E5, 3}, {5, 6, C5, 2}, {5, 8, A4, 3},  {5, 11, C5, 3}, {5, 14, F5, 2}, /* F */
        {6, 0, G5, 3}, {6, 3, E5, 3}, {6, 6, G5, 2}, {6, 8, C6, 6},  {6, 14, B5, 2},                 /* C */
        {7, 0, B5, 3}, {7, 3, A5, 3}, {7, 6, G5, 2}, {7, 8, A5, 8},                                  /* G */
};

static void write_hook(void) {
        for (size_t i = 0; i < sizeof(hook) / sizeof(hook[0]); ++i) {
                const uint32_t at = hook[i][0] * BAR + hook[i][1], strong = hook[i][1] % 8 == 0;
                s.melody[at] = note(hook[i][2], strong ? 104 : 90, hook[i][3], 0);
                s.bells[at] = note(hook[i][2] + 12u, strong ? 84 : 70, hook[i][3], 0);
        }
        return;
}

const bmds_seq_song_t *song_get(void) {
        if (s.made) return &s.song;
        write_drums();
        write_tonal();
        write_hook();
        s.bass_lanes[0] = (bmds_seq_lane_t){74, {0, 0, 0}, s.sweep};
        s.kick_p[0] = pattern(s.kick, BAR, NULL, 0);
        s.kick_p[1] = pattern(s.kick_build, BUILD, NULL, 0);
        s.clap_p[0] = pattern(s.clap, BAR, NULL, 0);
        s.hats_p[0] = pattern(s.hats_open, BAR, NULL, 0);
        s.hats_p[1] = pattern(s.hats, BAR, NULL, 0);
        s.bass_p[0] = pattern(s.bass, PHRASE, s.bass_lanes, 1);
        s.bass_p[1] = pattern(s.bass_busy, PHRASE, s.bass_lanes, 1);
        s.lead_p[0] = pattern(s.lead, PHRASE, NULL, 0);
        for (int v = 0; v < 3; ++v) s.pad_p[v][0] = pattern(s.pad[v], PHRASE, NULL, 0);
        s.crash_p[0] = pattern(s.crash, BUILD, NULL, 0);
        s.melody_p[0] = pattern(s.melody, HOOK, NULL, 0);
        s.bells_p[0] = pattern(s.bells, HOOK, NULL, 0);

        /* GM programs from 0: Synth Bass 2, Lead 2 (sawtooth), Pad 2 (warm), Lead 1 (square), Vibraphone. */
        s.tracks[SONG_KICK] = track("kick", 9, 0, 110, 64, s.kick_p, 2);
        s.tracks[SONG_CLAP] = track("clap", 9, 0, 110, 64, s.clap_p, 1);
        s.tracks[SONG_HATS] = track("hats", 9, 0, 110, 64, s.hats_p, 2);
        s.tracks[SONG_BASS] = track("bass", 0, 39, 100, 64, s.bass_p, 2);
        s.tracks[SONG_LEAD] = track("lead", 1, 81, 72, 84, s.lead_p, 1);
        s.tracks[SONG_PAD1] = track("pad", 2, 89, 76, 48, s.pad_p[0], 1);
        s.tracks[SONG_PAD2] = track("pad", 2, 89, 76, 48, s.pad_p[1], 1);
        s.tracks[SONG_PAD3] = track("pad", 2, 89, 76, 48, s.pad_p[2], 1);
        s.tracks[SONG_CRASH] = track("crash", 9, 0, 110, 64, s.crash_p, 1);
        s.tracks[SONG_MELODY] = track("melody", 3, 80, 96, 60, s.melody_p, 1);
        s.tracks[SONG_BELLS] = track("bells", 4, 11, 88, 72, s.bells_p, 1);
        for (int i = 0; i < SONG_SECTIONS; ++i)
                s.sections[i] = (bmds_seq_section_t){
                        sizeof(bmds_seq_section_t), section_info[i].cue, section_info[i].bars, 0.0f, 0, arrangement[i]};
        s.song = (bmds_seq_song_t){sizeof(bmds_seq_song_t), "BMDS Demo #1", 140.0f, 4, 4, 0.0f, SONG_TRACKS, s.tracks,
                                   SONG_SECTIONS,           s.sections,     0};
        s.made = 1;
        return &s.song;
}
