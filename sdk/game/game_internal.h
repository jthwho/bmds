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
                int64_t        at_ns; /* PLAY: when it is heard (BMDS_CLOCK_MONOTONIC); 0: at once */
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
                uint32_t       delay;    /* frames of silence before its start (bmds_game_play_at()) */
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
                int64_t           heard_ns; /* when this period's first frame is heard (0: unknown) */
                uint32_t          rate;     /* the period's rate */
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

/** @brief `bmds_game_play_at()` on @p mixer: heard at @p at_ns (0: at once). */
int32_t bmds_game_mixer_play_at(bmds_game_mixer_t *mixer, const bmds_game_sound_t *sound, float volume, float pan,
                                uint32_t flags, int64_t at_ns);

/** @brief The mixer's thread: true when no voice plays and no command waits (nothing to mix). */
int bmds_game_mixer_idle(const bmds_game_mixer_t *mixer);

/** @brief Returns @p voice's slot and generation if it may still play (else -1). */
int32_t bmds_game_mixer_voice(const bmds_game_mixer_t *mixer, int32_t voice, uint32_t *generation);

/** @brief The mixer's period: the commands queued, then @p frames × @p channels samples into @p out. */
void bmds_game_mixer_run(bmds_game_mixer_t *mixer, float *out, uint32_t frames, uint32_t channels);

/**
 * @brief bmds_game_mixer_run() for a period whose first frame is heard at @p heard_ns (0:
 *        unknown: sounds start at once) at @p rate: a sound played for later starts on its frame.
 */
void bmds_game_mixer_run_at(bmds_game_mixer_t *mixer, float *out, uint32_t frames, uint32_t channels, int64_t heard_ns,
                            uint32_t rate);

/**
 * @brief Resamples @p frames × @p channels samples at @p from Hz to @p to Hz (linearly) into
 *        @p out, which holds `bmds_game_resampled()` frames.
 */
void bmds_game_resample(const int16_t *in, uint32_t frames, uint32_t channels, uint32_t from, uint32_t to,
                        int16_t *out);

/** @brief The frames @p frames at @p from Hz become at @p to Hz. */
uint32_t bmds_game_resampled(uint32_t frames, uint32_t from, uint32_t to);

/* ---- MIDI files, sent through /dev/midi (game music with BMDS_GAME_MUSIC_MIDI_OUT, smf.c, midiout.c) */

/** @brief Tracks a Standard MIDI File may have, read at once. */
#define BMDS_GAME_SMF_TRACKS 64

/** @brief What `bmds_game_smf_next()` gives. */
#define BMDS_GAME_SMF_MESSAGE 1 /* a channel message, `data` and `length` (1 to 3) */
#define BMDS_GAME_SMF_SYSEX 2   /* `bytes` and `count`: a SysEx's (F0 and its data, its F7 included) */

typedef struct bmds_game_smf_track_t {
                const uint8_t *pos;
                const uint8_t *end;
                uint64_t       tick;   /* its next event's, absolute */
                uint8_t        status; /* running status */
                uint8_t        done;
} bmds_game_smf_track_t;

/** @brief A Standard MIDI File (format 0 or 1) walked in time order, its tempo map applied. */
typedef struct bmds_game_smf_t {
                const uint8_t        *data;
                size_t                size;
                uint32_t              count;    /* tracks */
                uint32_t              division; /* ticks a quarter note, or SMPTE's ticks a second */
                int                   smpte;    /* division is ticks a second (no tempo) */
                uint32_t              tempo;    /* microseconds a quarter note */
                uint64_t              tick;     /* the tempo map's place: this tick ... */
                int64_t               ns;       /* ... is this far in */
                bmds_game_smf_track_t tracks[BMDS_GAME_SMF_TRACKS];
} bmds_game_smf_t;

typedef struct bmds_game_smf_event_t {
                int64_t        ns;   /* from the song's start */
                int            kind; /* BMDS_GAME_SMF_... */
                uint8_t        data[3];
                uint8_t        length;
                const uint8_t *bytes; /* a SysEx's bytes (in sysex[]) */
                uint32_t       count;
                uint8_t        sysex[64]; /* F0 and its data (F7 escapes: the bytes alone); longer ones skipped */
} bmds_game_smf_event_t;

/** @brief Reads @p size bytes of a MIDI file's header and tracks. @return 0, or -BMDS_EINVAL. */
int bmds_game_smf_open(bmds_game_smf_t *smf, const void *data, size_t size);

/** @brief Back to the start. */
void bmds_game_smf_rewind(bmds_game_smf_t *smf);

/**
 * @brief The next event in time (tempo changes applied, other meta events skipped).
 * @return 1 (an event), 0 (the end), or -BMDS_EINVAL (the file ends mid-event: its end).
 */
int bmds_game_smf_next(bmds_game_smf_t *smf, bmds_game_smf_event_t *event);

/** @brief Returns true if @p path names a Standard MIDI File (`MThd`). */
int bmds_game_is_midi_file(const char *path);

/**
 * @brief Plays the MIDI file at @p path through /dev/midi (scheduled output), on a thread of
 *        its own, looping with @p loop, at @p gain (0 to 32768: its channel volumes scaled).
 * @return The player, or NULL with *@p err set (the open's, -BMDS_EINVAL, -BMDS_ENOMEM).
 */
struct bmds_game_midi_out *bmds_game_midi_out_start(const char *path, int loop, int32_t gain, int *err);

/** @brief Its volume, 0 to 32768 (sent at once as each channel's volume). */
void bmds_game_midi_out_gain(struct bmds_game_midi_out *out, int32_t gain);

/** @brief Stops: what is scheduled cancelled, every note off; freed. */
void bmds_game_midi_out_stop(struct bmds_game_midi_out *out);

#endif /* BMDS_GAME_INTERNAL_H */
