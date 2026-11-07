/**
 * @file      demo.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1 (devplan/demo.md): what the show and its parts share. A part draws every pixel of
 * a 640×360 L8 frame; the palette is the show's (pal.h), whose top four entries are the overlay's.
 */

#ifndef DEMO1_DEMO_H
#define DEMO1_DEMO_H

#include <stdint.h>
#include <bmds/3d.h>
#include <bmds/game.h>
#include <bmds/seq.h>
#include "pal.h"
#include "song.h"

#define DEMO_WIDTH 640
#define DEMO_HEIGHT 360

typedef struct demo_t demo_t;

/** @brief The music at the moment a frame is seen (demo1.c fills it each frame; parts read it). */
typedef struct {
                int                 known; /* the song is playing and heard: the rest is valid */
                bmds_seq_position_t pos;
                float               kick, clap, hat, bass; /* each one's last note, its loudness decaying (0..1) */
                uint32_t            kicks, claps, hats;    /* notes heard since the last frame */
                float               pulse;                 /* 1 on the beat, falling to 0 at the next */
                float               bar_seconds;           /* a bar's length (at 140 BPM when not known) */
} sync_t;

/** @brief A part of the show. */
typedef struct {
                const char *name;
                /** @brief Once, before the show: tables, pictures. @return 0 or a negative error. */
                int (*init)(demo_t *demo);
                /** @brief On becoming the part shown: its palette (pal_set_base()), its time from 0 (the sync is
                 *         the frame's: a section's cue may choose a mood). */
                void (*enter)(demo_t *demo);
                /** @brief Draws the whole frame at @p t seconds into the part (@p dt since the last). */
                void (*frame)(demo_t *demo, bmds_canvas_t *canvas, float t, float dt);
                /** @brief An input event (the show's own keys taken first); may be NULL. */
                void (*event)(demo_t *demo, const bmds_surface_event_t *event);
} part_t;

struct demo_t {
                bmds_game_t *game;
                bmds_3d_t   *ctx; /* the parts' 3D: two cores, a 60° perspective at 16:9 */
                pal_t        pal;
                int32_t      pointer_x, pointer_y; /* the picture's pixels; -1: none */
                int          button;               /* the left button held */
                sync_t       sync;
};

/** @brief Entries in the sine table: a turn. */
#define DEMO_SINES 4096

/** @brief sin(i × 2π / DEMO_SINES); filled by demo_init_sines(). musl's sinf() works in doubles,
 *         which this core does in software: a few thousand cycles a call (docs/platforms/esp32p4.md). */
extern float demo_sines[DEMO_SINES];

void demo_init_sines(void);

/** @brief sin(@p a) by the table (radians, any sign): to 0.0016. */
static inline float dsin(float a) {
        const int32_t i = (int32_t)(a * (DEMO_SINES / 6.28318531f) + (a >= 0.0f ? 0.5f : -0.5f));
        return demo_sines[(uint32_t)i & (DEMO_SINES - 1)];
}

/** @brief cos(@p a) by the table. */
static inline float dcos(float a) {
        return dsin(a + 1.57079633f);
}

/** @brief The parts, in order (parts/ *.c). */
extern const part_t part_copper;
extern const part_t part_plasma;
extern const part_t part_stars;
extern const part_t part_tunnel;
extern const part_t part_fire;
extern const part_t part_balls;
extern const part_t part_solids;

#endif // DEMO1_DEMO_H
