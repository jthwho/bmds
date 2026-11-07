/**
 * @file      beat.c
 * @copyright Jason Howard. All rights reserved.
 *
 * libbmdsgame's sequencer (bmds/seq.h, devplan/sequencer.md): a song of four tracks played
 * through /dev/midi, and a screen in time with it. Each frame asks the sequencer about the moment
 * it will be seen (bmds_game_frame_ns()): the kick flashes the background (its last hit's
 * velocity, decaying), the clap lights the beat boxes, the hats tick the step row, and the text
 * says where the song is. Space stops and starts; Escape ends it.
 *
 *     beat [-F] [-p PORT] [-l LATENCY_MS]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>
#include <bmds/palette.h>
#include <bmds/seq.h>

#define WIDTH 320
#define HEIGHT 180
#define KEY_SPACE 0x0007002cu
#define KEY_ESCAPE 0x00070029u

enum {
        INK_BACK,
        INK_TEXT,
        INK_DIM,
        INK_BEAT,
        INK_CLAP,
        INK_HAT,
        INK_BAR,
        INKS
};
enum {
        T_KICK,
        T_CLAP,
        T_HATS,
        T_BASS
};

/* ---- The song: 128 BPM, sixteenths ------------------------------------------------------------ */

#define X(n, v)                                                                                                        \
        { n, v, 1, 0 }
#define A(n, v)                                                                                                        \
        { n, v, 1, BMDS_SEQ_ACCENT }
#define R                                                                                                              \
        { 0, 0, 0, 0 }

static const bmds_seq_step_t kick[16] = {X(36, 110), R, R, R, X(36, 110), R, R, R,
                                         X(36, 110), R, R, R, X(36, 110), R, R, X(36, 70)};
static const bmds_seq_step_t clap[16] = {R, R, R, R, X(39, 100), R, R, R, R, R, R, R, X(39, 100), R, R, R};
static const bmds_seq_step_t hats[16] = {X(42, 50), X(42, 35), A(46, 70), X(42, 35), X(42, 50), X(42, 35),
                                         A(46, 70), X(42, 35), X(42, 50), X(42, 35), A(46, 70), X(42, 35),
                                         X(42, 50), X(42, 35), A(46, 70), X(42, 40)};
static const bmds_seq_step_t bass[16] = {R, R, X(33, 100), R, R, R,         X(33, 100), X(45, 80),
                                         R, R, X(33, 100), R, R, X(36, 90), X(33, 100), R};
static const uint8_t sweep[16] = {30, 0xff, 38, 0xff, 46, 0xff, 54, 0xff, 62, 0xff, 70, 0xff, 78, 0xff, 86, 0xff};
static const bmds_seq_lane_t bass_lanes[1] = {{74, {0, 0, 0}, sweep}}; /* the filter, opening each bar */

#define PATTERN(steps, lanes, count)                                                                                   \
        { sizeof(bmds_seq_pattern_t), 16, steps, count, lanes }
static const bmds_seq_pattern_t kick_p[1] = {PATTERN(kick, NULL, 0)};
static const bmds_seq_pattern_t clap_p[1] = {PATTERN(clap, NULL, 0)};
static const bmds_seq_pattern_t hats_p[1] = {PATTERN(hats, NULL, 0)};
static const bmds_seq_pattern_t bass_p[1] = {PATTERN(bass, bass_lanes, 1)};

static const bmds_seq_track_t tracks[4] = {
        {sizeof(bmds_seq_track_t), "kick", 9, 0, 110, 64, 1, kick_p},
        {sizeof(bmds_seq_track_t), "clap", 9, 0, 110, 64, 1, clap_p},
        {sizeof(bmds_seq_track_t), "hats", 9, 0, 110, 64, 1, hats_p},
        {sizeof(bmds_seq_track_t), "bass", 0, 38, 100, 64, 1, bass_p}, /* Synth Bass 1 */
};
static const int8_t             intro[4] = {0, -1, 0, -1}, groove[4] = {0, 0, 0, 0};
static const bmds_seq_section_t sections[3] = {
        {sizeof(bmds_seq_section_t), "intro", 4, 0.0f, 0, intro},
        {sizeof(bmds_seq_section_t), "groove", 8, 0.0f, 0, groove},
        {sizeof(bmds_seq_section_t), "lift", 4, 0.0f, 5, groove}, /* up a fourth */
};
static const bmds_seq_song_t song = {sizeof(bmds_seq_song_t), "beat", 128.0f, 4, 4, 0.08f, 4, tracks, 3, sections, 1};

/* ---- The screen ------------------------------------------------------------------------------- */

typedef struct {
                bmds_seq_t *seq;
                int         playing;
                int64_t     last_ns;   /* the last frame's moment: the hats between it and this one */
                float       ticks[16]; /* the step row's glow, by the hats */
                uint32_t    palette[INKS];
} state_t;

static int64_t now_ns(void) {
        int64_t ns = 0;
        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

/* A hit's level at @p at: its velocity, falling to a third in @p tau seconds or so. */
static float envelope(bmds_seq_t *seq, int64_t at, uint32_t track, float tau) {
        bmds_seq_hit_t h;
        if (bmds_seq_last_hit(seq, at, track, &h) != 0) return 0.0f;
        return (float)h.velocity / 127.0f * expf(-(float)(at - h.time_ns) * 1e-9f / tau);
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        state_t *s = bmds_game_arg(game);
        if (e->type == BMDS_EVENT_CLOSE) bmds_game_quit(game);
        if (e->type != BMDS_EVENT_KEY || (e->flags & BMDS_KEY_DOWN) == 0) return;
        if (e->data[0] == KEY_ESCAPE) {
                bmds_game_quit(game);
        } else if (e->data[0] == KEY_SPACE) {
                if (s->playing)
                        bmds_seq_stop(s->seq);
                else
                        (void)bmds_seq_play(s->seq, 0);
                s->playing = !s->playing;
        }
        return;
}

static void on_tick(bmds_game_t *game) {
        (void)game; /* the loop draws only with a tick; all the work is the frame's */
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        state_t          *s = bmds_game_arg(game);
        const int64_t     lead = bmds_game_frame_ns(game);
        const int64_t     at = lead != 0 ? lead : now_ns(); /* when this frame will be seen */
        const bmds_rect_t whole = {0, 0, WIDTH, HEIGHT};
        bmds_game_repainted(game, &whole);

        bmds_seq_position_t pos;
        memset(&pos, 0, sizeof(pos));
        pos.size = sizeof(pos);
        const int known = bmds_seq_position(s->seq, at, &pos) == 0;

        /* The background: the kick's flash over a dark blue. */
        const float kick_level = known ? envelope(s->seq, at, T_KICK, 0.12f) : 0.0f;
        uint32_t    inks[INKS];
        memcpy(inks, s->palette, sizeof(inks));
        bmds_palette_mix_rgb(&inks[INK_BACK], &s->palette[INK_BACK], 1, 0x6040c0, (uint32_t)(255.0f * kick_level));
        const float clap_level = known ? envelope(s->seq, at, T_CLAP, 0.18f) : 0.0f;
        bmds_palette_mix_rgb(&inks[INK_CLAP], &s->palette[INK_BEAT], 1, 0xffffff, (uint32_t)(255.0f * clap_level));
        (void)bmds_game_palette(game, 0, INKS, inks);
        bmds_canvas_clear(c, INK_BACK);

        /* The hats since the last frame: each lights its step. */
        for (int i = 0; i < 16; ++i) s->ticks[i] *= 0.8f;
        if (known) {
                bmds_seq_hit_t hits[16];
                const uint32_t n = bmds_seq_hits(s->seq, s->last_ns, at, 1u << T_HATS, hits, 16);
                for (uint32_t i = 0; i < n; ++i) {
                        bmds_seq_position_t p;
                        memset(&p, 0, sizeof(p));
                        p.size = sizeof(p);
                        if (bmds_seq_position(s->seq, hits[i].time_ns, &p) == 0)
                                s->ticks[p.beat * 4 + p.step] = (float)hits[i].velocity / 127.0f;
                }
        }
        s->last_ns = at;

        /* The beats: four boxes, the current one lit (by the clap when it hits). */
        for (uint32_t b = 0; b < 4; ++b) {
                const int32_t x = 40 + (int32_t)b * 64;
                const int     now = known && pos.beat == b;
                bmds_canvas_fill(c, x, 50, 48, 48, now ? INK_CLAP : INK_DIM);
        }
        /* The steps: sixteen, the hats' glow and the step heard. */
        for (int i = 0; i < 16; ++i) {
                const int32_t x = 40 + i * 16;
                const int     now = known && (int)(pos.beat * 4 + pos.step) == i;
                const int32_t h = 4 + (int32_t)(16.0f * s->ticks[i]);
                bmds_canvas_fill(c, x, 124 - h, 12, h, now ? INK_TEXT : INK_HAT);
        }
        /* The bar's progress. */
        if (known) bmds_canvas_fill(c, 40, 132, (int32_t)(240.0f * pos.bar_phase), 3, INK_BAR);

        char line[96];
        if (known)
                snprintf(line, sizeof(line), "%s%s %u.%u.%u  %.0f BPM", pos.cue != NULL ? pos.cue : "",
                         pos.loops != 0 ? "*" : "", pos.bar + 1, pos.beat + 1, pos.step + 1, (double)pos.bpm);
        else
                snprintf(line, sizeof(line), "%s", s->playing ? "starting..." : "stopped: Space plays");
        bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 8, 8, line, INK_TEXT);
        snprintf(line, sizeof(line), "latency %.1f ms, frame seen in %.1f",
                 (double)bmds_seq_latency_ns(s->seq) / 1e6, lead != 0 ? (double)(lead - now_ns()) / 1e6 : 0.0);
        bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 8, 24, line, INK_HAT);
        bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 8, HEIGHT - 20, "Space: stop/play   Esc: quit", INK_HAT);
        bmds_canvas_damage_all(c);
        return;
}

int main(int argc, char **argv) {
        static state_t     s;
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = WIDTH;
        config.height = HEIGHT;
        config.format = BMDS_PIXEL_L8;
        config.flags = BMDS_SURFACE_WHOLE;
        config.title = "Beat";
        bmds_seq_config_t sc;
        memset(&sc, 0, sizeof(sc));
        sc.size = sizeof(sc);
        sc.latency_us = -1;
        int opt;
        while ((opt = getopt(argc, argv, "Fp:l:")) != -1) {
                if (opt == 'F') {
                        config.flags |= BMDS_SURFACE_FULL_SCREEN;
                } else if (opt == 'p') {
                        sc.port = optarg;
                } else if (opt == 'l') {
                        sc.latency_us = atoi(optarg) * 1000;
                } else {
                        fprintf(stderr, "usage: beat [-F] [-p PORT] [-l LATENCY_MS]\n");
                        return 2;
                }
        }
        s.palette[INK_BACK] = 0x080818;
        s.palette[INK_TEXT] = 0xf0f0f0;
        s.palette[INK_DIM] = 0x283048;
        s.palette[INK_BEAT] = 0xff6020;
        s.palette[INK_CLAP] = 0xff6020;
        s.palette[INK_HAT] = 0x40c0a0;
        s.palette[INK_BAR] = 0x8090ff;

        int err = bmds_seq_open(&sc, &song, &s.seq);
        if (err == 0) err = bmds_seq_play(s.seq, 0);
        if (err < 0) {
                fprintf(stderr, "beat: %s\n", strerror(-err));
                bmds_seq_close(s.seq);
                return 1;
        }
        s.playing = 1;
        printf("beat: latency %.1f ms\n", (double)bmds_seq_latency_ns(s.seq) / 1e6);
        config.arg = &s;
        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        config.options = BMDS_GAME_REPAINTS;
        err = bmds_game_run(&config);
        bmds_seq_close(s.seq);
        if (err < 0) {
                fprintf(stderr, "beat: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
