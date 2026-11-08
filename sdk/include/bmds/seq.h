/**
 * @file      seq.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's sequencer (devplan/sequencer.md): songs as patterns of steps arranged in
 * sections, played live through a MIDI port's scheduled output, and beat sync: where the song is,
 * as heard, at any moment, and which notes were heard between two moments. A frame drawn for the
 * moment it will be seen (bmds_game_frame_ns()) asks those of that moment, and draws in time with
 * the music.
 *
 * A song is plain data (static arrays work), read in place for as long as it plays. Time is
 * `BMDS_CLOCK_MONOTONIC` nanoseconds, as heard: each event is sent its output's latency ahead
 * (`BMDS_MIDI_LATENCY`), so the song's clock is what the listener hears.
 *
 * Thread safety: one thread plays (the library's); the calls may come from any one other thread.
 * A static library, as the rest of libbmdsgame: no ABI promise; structs the caller fills begin with
 * `size`.
 */

#ifndef BMDS_SEQ_H
#define BMDS_SEQ_H

#include <stdint.h>
#include <bmds/sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `bmds_seq_step_t::flags`. */
#define BMDS_SEQ_ACCENT 0x1u /**< Louder (velocity + 24, at most 127). */

/** @brief A step of a pattern: a note, or a rest (velocity 0). */
typedef struct bmds_seq_step_t {
                uint8_t note;     /**< MIDI note (a drum on channel 10, unmoved by transposition). */
                uint8_t velocity; /**< 1-127; 0: a rest. */
                uint8_t length;   /**< In steps (0: 1); held into the next steps. */
                uint8_t flags;    /**< `BMDS_SEQ_...`. */
} bmds_seq_step_t;

/** @brief A controller's value a step (a filter's sweep). */
typedef struct bmds_seq_lane_t {
                uint8_t        controller; /**< 0-119. */
                uint8_t        reserved[3];
                const uint8_t *values; /**< One a step of the pattern; 0xff: unchanged. */
} bmds_seq_lane_t;

/** @brief A pattern: steps played round as long as its section lasts. */
typedef struct bmds_seq_pattern_t {
                uint32_t               size;  /**< `sizeof(bmds_seq_pattern_t)`. */
                uint32_t               steps; /**< 1-256. */
                const bmds_seq_step_t *notes; /**< `steps` of them. */
                uint32_t               lane_count;
                const bmds_seq_lane_t *lanes;
} bmds_seq_pattern_t;

/** @brief A track: a channel's sound and its patterns. */
typedef struct bmds_seq_track_t {
                uint32_t                  size; /**< `sizeof(bmds_seq_track_t)`. */
                const char               *name;
                uint32_t                  channel; /**< 0-15 (9: GM drums). */
                uint32_t                  program; /**< GM program, 0-127 (ignored on channel 10). */
                uint32_t                  volume;  /**< Controller 7, 0-127. */
                uint32_t                  pan;     /**< Controller 10, 0-127 (64: the middle). */
                uint32_t                  pattern_count;
                const bmds_seq_pattern_t *patterns;
} bmds_seq_track_t;

/** @brief A section of the arrangement: a number of bars, each track's pattern in it. */
typedef struct bmds_seq_section_t {
                uint32_t      size; /**< `sizeof(bmds_seq_section_t)`. */
                const char   *cue;  /**< A name the program reads (bmds_seq_position_t::cue). */
                uint32_t      bars;
                float         bpm;       /**< 0: the song's. */
                int32_t       transpose; /**< Semitones (not channel 10). */
                const int8_t *patterns;  /**< Per track: an index into its patterns, or -1 (silent). */
} bmds_seq_section_t;

/** @brief A song. */
typedef struct bmds_seq_song_t {
                uint32_t                  size; /**< `sizeof(bmds_seq_song_t)`. */
                const char               *name;
                float                     bpm;            /**< Beats a minute. */
                uint32_t                  steps_per_beat; /**< 0: 4 (sixteenths). */
                uint32_t                  beats_per_bar;  /**< 0: 4. */
                float                     swing;          /**< 0-0.5: each odd step that part of a step late. */
                uint32_t                  track_count;
                const bmds_seq_track_t   *tracks; /**< Up to 16. */
                uint32_t                  section_count;
                const bmds_seq_section_t *sections;
                int32_t                   loop_section; /**< At the end, on from this section; -1: it ends. */
} bmds_seq_song_t;

/** @brief A sequencer (bmds_seq_open()). */
typedef struct bmds_seq_t bmds_seq_t;

/** @brief bmds_seq_open()'s settings. */
typedef struct bmds_seq_config_t {
                uint32_t    size;         /**< `sizeof(bmds_seq_config_t)`. */
                const char *port;         /**< A MIDI port's file ("/dev/midi3"), or a port's name ("wavetable"); NULL:
                                     "/dev/midi", the system synth. */
                uint32_t    lookahead_ms; /**< Events written this far ahead of when they are sent (0: 80). */
                int32_t     latency_us;   /**< The output's latency; -1: asked of the port (`BMDS_MIDI_LATENCY`). */
                /** Latency the port may take (`BMDS_MIDI_ALLOW_LATENCY`, asked before the latency): a built-in synth
                 *  then renders ahead, keeping time beside a busy program (0: as low as it goes, real time). Appended:
                 *  a config of the older `size` leaves it 0. */
                uint32_t allow_latency_us;
} bmds_seq_config_t;

/**
 * @brief Opens @p config's port for @p song (checked once here: indices in range, sizes sane).
 * @return 0, or `-BMDS_EINVAL` (a bad song), the port's error, `-BMDS_ENOMEM`.
 */
int bmds_seq_open(const bmds_seq_config_t *config, const bmds_seq_song_t *song, bmds_seq_t **out);

/**
 * @brief Plays from the song's start, heard at @p start_ns (0: as soon as it can be: now, plus the
 *        latency and the lookahead): each track's sound set, the thread started.
 * @return 0, or `-BMDS_EINVAL` (playing already), `-BMDS_ENOMEM` (no thread).
 */
int bmds_seq_play(bmds_seq_t *seq, int64_t start_ns);

/** @brief Stops: what is scheduled cancelled, every note off. */
void bmds_seq_stop(bmds_seq_t *seq);

/** @brief Stops and frees @p seq (NULL is ignored). */
void bmds_seq_close(bmds_seq_t *seq);

/** @brief Returns the latency used, in ns (the port's, or the config's). */
int64_t bmds_seq_latency_ns(const bmds_seq_t *seq);

/** @brief Where a song is at a moment (bmds_seq_position()). */
typedef struct bmds_seq_position_t {
                uint32_t    size;       /**< Set by the caller: `sizeof(bmds_seq_position_t)`. */
                uint32_t    section;    /**< Its index. */
                const char *cue;        /**< Its cue (NULL: none). */
                uint32_t    bar;        /**< In the section, from 0. */
                uint32_t    beat;       /**< In the bar, from 0. */
                uint32_t    step;       /**< In the beat, from 0. */
                float       beat_phase; /**< 0..1 through the beat (a pulse: 1 − beat_phase). */
                float       bar_phase;  /**< 0..1 through the bar. */
                float       bpm;
                uint32_t    loops;   /**< Times the song went round (loop_section). */
                double      seconds; /**< Since the song's start. */
                int         ended;   /**< 1: past a song's end that doesn't loop. */
} bmds_seq_position_t;

/**
 * @brief Fills @p pos with where the song is heard at @p time_ns.
 * @return 0, or `-BMDS_EAGAIN` (not playing, or before its start).
 */
int bmds_seq_position(const bmds_seq_t *seq, int64_t time_ns, bmds_seq_position_t *pos);

/**
 * @brief Sets @p time_ns to when the start of beat @p beat of bar @p bar of section @p section
 *        is heard, the first time at or after @p after_ns (the loop counted): what
 *        bmds_seq_position() answers, the other way round, to put a sound, a line or a picture on
 *        a cue ahead of it (`bmds_game_play_at()`).
 * @return 0, or `-BMDS_EINVAL` (no such section, bar or beat), `-BMDS_EAGAIN` (not playing),
 *         `-BMDS_ENOENT` (passed, and not played again).
 */
int bmds_seq_time_of(const bmds_seq_t *seq, uint32_t section, uint32_t bar, uint32_t beat, int64_t after_ns,
                     int64_t *time_ns);

/** @brief A note heard (bmds_seq_hits()). */
typedef struct bmds_seq_hit_t {
                int64_t  time_ns; /**< When it is heard. */
                uint16_t track;
                uint8_t  note;
                uint8_t  velocity;
} bmds_seq_hit_t;

/**
 * @brief Copies the notes heard in (@p from_ns, @p to_ns] of the tracks in @p track_mask (a bit a
 *        track) into @p out, in time order, at most @p max: a frame passes the last frame's time and
 *        its own, and reacts to each once. Of the last 512 notes scheduled.
 * @return How many.
 */
uint32_t bmds_seq_hits(bmds_seq_t *seq, int64_t from_ns, int64_t to_ns, uint32_t track_mask, bmds_seq_hit_t *out,
                       uint32_t max);

/**
 * @brief The latest note of @p track heard at or before @p time_ns, for an envelope that decays
 *        (`velocity × e^−(time − hit.time) / τ`) with no state kept.
 * @return 0, or `-BMDS_ENOENT` (none among the last 512).
 */
int bmds_seq_last_hit(bmds_seq_t *seq, int64_t time_ns, uint32_t track, bmds_seq_hit_t *hit);

/**
 * @brief Moves the song's loudness to @p gain (0 to 1 of its own, as an amplitude) from when
 *        @p at_ns is heard, over @p over_ns: to duck the music under a line said
 *        (`bmds_game_play_at()` at the same time) and bring it back after. Each track's volume
 *        (controller 7, its own and its lanes') is sent scaled, in steps of 40 ms, among the notes
 *        and at their times; a synth moves the notes sounding with it.
 *
 *        A fade replaces those asked for before it that would be heard from @p at_ns on, and
 *        starts from where they left the song then. A time already sent is sent at once (late).
 *        The gain lasts through bmds_seq_stop() and bmds_seq_play() (its fades pending are dropped
 *        by a stop, the gain then their last). Any thread.
 * @param at_ns When it starts, heard (0: as soon as the song can be).
 * @return 0, or `-BMDS_EINVAL` (@p gain or @p over_ns out of range), `-BMDS_ENOSPC` (64 steps
 *         pending: try again once some are heard).
 * @nonblocking
 */
int bmds_seq_fade(bmds_seq_t *seq, float gain, int64_t at_ns, int64_t over_ns);

#ifdef __cplusplus
}
#endif

#endif // BMDS_SEQ_H
