/**
 * @file      game_internal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's own declarations (devplan/games.md §13).
 */

#ifndef BMDS_GAME_INTERNAL_H
#define BMDS_GAME_INTERNAL_H

#include <bmds/game.h>

/** @brief Glyphs in the font: ASCII 0x20-0x7E, then Latin-1 0xA0-0xFF. */
#define BMDS_GAME_FONT_GLYPHS 191

extern const unsigned char bmds_game_font16[BMDS_GAME_FONT_GLYPHS][16];

/** @brief Adds @p rect, cut to the canvas's clip (what drawing there can change), to its damage. */
void bmds_game_damage_in_clip(bmds_canvas_t *canvas, const bmds_rect_t *rect);

/** @brief Adds @p rect to @p list (of @p count, at most @p max): merged with what it meets. */
void bmds_game_rects_add(bmds_rect_t *list, uint32_t *count, uint32_t max, const bmds_rect_t *rect);

/**
 * @brief Calls @p span for each piece of @p count rectangles (cut to @p c) outside all of
 *        @p excepts (at most `BMDS_GAME_REPAINTED_MAX`): bands of rows, the gaps across each.
 */
void bmds_game_spans_except(const bmds_canvas_t *c, const bmds_rect_t *rects, uint32_t count, const bmds_rect_t *except,
                            uint32_t excepts, void (*span)(void *arg, const bmds_rect_t *r), void *arg);

/**
 * @brief Copies @p count rectangles from @p from to @p to (same size and format) but none of the
 *        @p excepts (at most `BMDS_GAME_REPAINTED_MAX`).
 */
void bmds_game_copy_except(bmds_canvas_t *to, const bmds_canvas_t *from, const bmds_rect_t *rects, uint32_t count,
                           const bmds_rect_t *except, uint32_t excepts);

/** @brief The transparent RGB565 and BGR24 value of a loaded image: magenta. */
#define BMDS_GAME_KEY_RGB565 0xf81fu
#define BMDS_GAME_KEY_BGR24 0xff00ffu

/** @brief Turns BGRA32 pixels into a loaded image's (`bmds_game_load_t`). */
typedef struct bmds_game_converter_t {
                const bmds_game_load_t *how;
                uint32_t                bpp;
                uint32_t                key;      /* the transparent pixel value */
                uint32_t                last_rgb; /* L8: the last colour looked up, and its index */
                uint32_t                last_index;
                int                     have_last;
} bmds_game_converter_t;

/** @brief Readies @p converter for @p how. @return 0, or -1 (@p how can't be loaded). */
int bmds_game_converter_init(bmds_game_converter_t *converter, const bmds_game_load_t *how);

/**
 * @brief Converts @p count BGRA32 pixels at @p in to @p out (which may be @p in: written behind
 *        what is read).
 * @return The pixels made transparent.
 */
uint32_t bmds_game_convert(bmds_game_converter_t *converter, const uint8_t *in, uint8_t *out, uint32_t count);

/** @brief Commands the mixer keeps between its periods. */
#define BMDS_GAME_COMMANDS 64u

/** @brief `bmds_game_command_t::type`. */
enum {
        BMDS_GAME_COMMAND_PLAY,
        BMDS_GAME_COMMAND_SET,  /* a voice's gains */
        BMDS_GAME_COMMAND_STOP, /* faded */
        BMDS_GAME_COMMAND_VOLUME,
        BMDS_GAME_COMMAND_PAUSE
};

/** @brief What the game's thread asks the mixer's. */
typedef struct bmds_game_command_t {
                uint32_t       type;
                uint32_t       slot;
                uint32_t       generation; /* the voice's: a command for one that has ended is dropped */
                const int16_t *samples;
                uint32_t       frames;
                uint32_t       channels;
                float          volume; /* PLAY, SET, VOLUME */
                float          pan;    /* PLAY, SET; PAUSE: 1 paused */
                uint32_t       flags;
} bmds_game_command_t;

/** @brief A voice, the mixer thread's. */
typedef struct bmds_game_voice_t {
                const int16_t *samples;
                uint32_t       frames;
                uint32_t       channels;
                uint32_t       position;
                uint32_t       generation; /* 0: free */
                float          volume, pan;
                float          left, right; /* the gains now */
                int            loop;
                int            stopping; /* fading to nothing: then free */
} bmds_game_voice_t;

/**
 * @brief The effects' mixer: commands from one thread (the game's), periods mixed on another (the
 *        audio's). No system calls: tested on the host.
 */
typedef struct bmds_game_mixer_t {
                /* The mixer's. */
                bmds_game_voice_t voices[BMDS_GAME_VOICES];
                float             volume;
                int               paused;
                /* Both: the queue (head written by the game, tail by the mixer), what has ended. */
                bmds_game_command_t commands[BMDS_GAME_COMMANDS];
                uint32_t            head, tail;
                uint32_t            ended[BMDS_GAME_VOICES]; /* each slot's last generation that ended */
                /* The game's: what it gave each slot. */
                uint32_t given[BMDS_GAME_VOICES];
                uint32_t started[BMDS_GAME_VOICES]; /* in play order */
                uint8_t  looping[BMDS_GAME_VOICES];
                uint32_t plays;
} bmds_game_mixer_t;

/** @brief Readies @p mixer: no voices, the volume 1. */
void bmds_game_mixer_init(bmds_game_mixer_t *mixer);

/** @brief Queues @p command for the next period. @return 0, or -1 (full). */
int bmds_game_mixer_push(bmds_game_mixer_t *mixer, const bmds_game_command_t *command);

/** @brief `bmds_game_play()` on @p mixer (the game's thread). */
int32_t bmds_game_mixer_play(bmds_game_mixer_t *mixer, const bmds_game_sound_t *sound, float volume, float pan,
                             uint32_t flags);

/** @brief Returns @p voice's slot and generation if it may still play (else -1). */
int32_t bmds_game_mixer_voice(const bmds_game_mixer_t *mixer, int32_t voice, uint32_t *generation);

/** @brief The mixer's period: the commands queued, then @p frames × @p channels samples into @p out. */
void bmds_game_mixer_run(bmds_game_mixer_t *mixer, float *out, uint32_t frames, uint32_t channels);

/**
 * @brief Resamples @p frames × @p channels samples at @p from Hz to @p to Hz (linearly) into
 *        @p out, which holds `bmds_game_resampled()` frames.
 */
void bmds_game_resample(const int16_t *in, uint32_t frames, uint32_t channels, uint32_t from, uint32_t to,
                        int16_t *out);

/** @brief The frames @p frames at @p from Hz become at @p to Hz. */
uint32_t bmds_game_resampled(uint32_t frames, uint32_t from, uint32_t to);

#endif /* BMDS_GAME_INTERNAL_H */
