/**
 * @file      voice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's speech and sounds (devplan/demo.md §3a): libbmdsgame's mixer, ahead (it needs no
 * deadline beside the drawing), and a thread that says lines into sounds before they are wanted
 * (bmds_game_sound_speak()), so the show plays each on its cue (bmds_game_play_at()). Slots are
 * the show's: one line each, said again when asked for another.
 */

#ifndef DEMO1_VOICE_H
#define DEMO1_VOICE_H

#include <stdint.h>
#include <bmds/game.h>
#include <bmds/seq.h>

/** @brief Lines kept at once. */
#define VOICE_SLOTS 52

/** @brief A slot's state. */
enum {
        VOICE_EMPTY,
        VOICE_QUEUED,
        VOICE_READY,
        VOICE_FAILED
};

/** @brief Opens the sound (ahead by @p latency_ms) and starts the speaker's thread. @return 0 or an error. */
int voice_open(uint32_t latency_ms);

/** @brief Stops everything and frees the lines (nothing open: nothing). */
void voice_close(void);

/** @brief Returns the mixer (NULL: none open). */
bmds_game_audio_t *voice_audio(void);

/**
 * @brief Asks for @p text said into slot @p slot (as @p how says; NULL: the system's voice), its
 *        last line dropped (stopped if it plays). Returns at once.
 */
void voice_say(int slot, const char *text, const bmds_game_speak_t *how);

/**
 * @brief Frees slot @p slot's line (one being made: given up), unless a voice still plays it
 *        (then voice_say() frees it later). The slot is then empty.
 */
void voice_drop(int slot);

/** @brief Returns slot @p slot's state (`VOICE_...`). */
int voice_state(int slot);

/** @brief Returns slot @p slot's line's length in seconds (0: not ready). */
float voice_seconds(int slot);

/**
 * @brief Plays slot @p slot's line, heard at @p at_ns (0: at once), the music ducked under it.
 * @return The mixer's voice, or a negative error (`-BMDS_EAGAIN`: not ready yet).
 */
int32_t voice_play(int slot, int64_t at_ns, float volume, float pan);

/**
 * @brief Plays slot @p slot's line after the last line played ends, @p gap seconds after (or at
 *        once: none playing), so lines never talk over each other.
 * @return Seconds from now until it ends, or < 0 (not ready).
 */
float voice_queue(int slot, float gap, float volume, float pan);

/** @brief When the last line played ends, heard (BMDS_CLOCK_MONOTONIC; 0: none yet). */
int64_t voice_said_end(void);

/** @brief Ducks @p seq's music under each line played from now on (NULL: no more). */
void voice_duck(bmds_seq_t *seq);

/** @brief Plays @p sound (the show's own) heard at @p at_ns (0: at once). */
int32_t voice_sound(const bmds_game_sound_t *sound, int64_t at_ns, float volume, float pan);

#endif // DEMO1_VOICE_H
