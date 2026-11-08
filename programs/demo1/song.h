/**
 * @file      song.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's track (devplan/demo.md §4): 140 BPM in A minor, sequencer data (bmds/seq.h).
 */

#ifndef DEMO1_SONG_H
#define DEMO1_SONG_H

#include <bmds/seq.h>

/** @brief The tracks, in order (bmds_seq_hit_t::track). */
enum {
        SONG_KICK,
        SONG_CLAP,
        SONG_HATS,
        SONG_BASS,
        SONG_LEAD,
        SONG_PAD1,
        SONG_PAD2,
        SONG_PAD3,
        SONG_CRASH,
        SONG_MELODY,
        SONG_BELLS,
        SONG_TRACKS
};

/** @brief The sections, in order (bmds_seq_position_t::section). */
enum {
        SONG_INTRO,
        SONG_BUILD,
        SONG_DROP_A,
        SONG_GROOVE,
        SONG_HIVE,
        SONG_BREAK,
        SONG_BUILD_2,
        SONG_DROP_B,
        SONG_FINALE,
        SONG_OUTRO,
        SONG_SECTIONS
};

/** @brief The song: its patterns written out on the first call (static; read in place while it plays). */
const bmds_seq_song_t *song_get(void);

#endif // DEMO1_SONG_H
