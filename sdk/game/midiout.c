/**
 * @file      midiout.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A game's music as MIDI through /dev/midi (BMDS_GAME_MUSIC_MIDI_OUT,
 * devplan/audio.md §25): the system port, the wavetable synth by default or
 * wherever it is routed. A thread walks the file and writes each event a
 * little ahead with its time (scheduled output), so the kernel sends it then
 * and a synth places it on its sample; the thread's own wake-ups don't show.
 * The volume scales each channel's volume (controller 7) as the file sets it.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "game_internal.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define AHEAD_NS 40000000ll /* written this far ahead: a volume change waits no longer */
#define WAKE_NS 10000000ll  /* the thread looks again this often */
#define START_NS 50000000ll /* the first event this far after the start */
#define MAX_FILE (4u << 20) /* the largest file read */
#define CHANNELS 16

struct bmds_game_midi_out {
                pthread_t       thread;
                int32_t         fd;
                uint8_t        *file;
                bmds_game_smf_t smf;
                int             loop;
                int             stop;             /* LOAD/STORE */
                int32_t         gain;             /* 0-32768, LOAD/STORE */
                int32_t         gain_sent;        /* the thread's */
                uint8_t         volume[CHANNELS]; /* each channel's volume as the file set it */
};

static int64_t now_ns(void) {
        int64_t ns = 0;
        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

static uint8_t scaled(uint8_t volume, int32_t gain) {
        return (uint8_t)(((int32_t)volume * gain) >> 15);
}

/* Writes @p count bytes as records of up to 5 at @p when (0: now), waiting while the fd's
   queue is full. 0, or the write's error. */
static int send(struct bmds_game_midi_out *m, int64_t when, const uint8_t *bytes, uint32_t count) {
        while (count > 0) {
                bmds_midi_event_t e;
                memset(&e, 0, sizeof(e));
                e.time_ns = when;
                e.length = (uint8_t)(count < 5 ? count : 5);
                memcpy(e.data, bytes, e.length);
                const int32_t n = bmds_write(m->fd, &e, sizeof(e));
                if (n == -BMDS_EAGAIN) {
                        if (LOAD(m->stop)) return n;
                        (void)bmds_sleep(WAKE_NS);
                        continue;
                }
                if (n < 0) return n;
                bytes += e.length;
                count -= e.length;
        }
        return 0;
}

/* Every channel's volume, now, at gain @p gain. */
static void send_volumes(struct bmds_game_midi_out *m, int32_t gain) {
        for (uint8_t c = 0; c < CHANNELS; ++c) {
                const uint8_t cc[3] = {(uint8_t)(0xb0u | c), 7, scaled(m->volume[c], gain)};
                (void)send(m, 0, cc, 3);
        }
        return;
}

/* Controller @p controller to 0 on every channel, now (all notes off, all sound off, reset). */
static void send_all(struct bmds_game_midi_out *m, uint8_t controller) {
        for (uint8_t c = 0; c < CHANNELS; ++c) {
                const uint8_t cc[3] = {(uint8_t)(0xb0u | c), controller, 0};
                (void)send(m, 0, cc, 3);
        }
        return;
}

static void *midi_thread(void *arg) {
        struct bmds_game_midi_out *m = (struct bmds_game_midi_out *)arg;
        for (uint8_t c = 0; c < CHANNELS; ++c) m->volume[c] = 100; /* GM's default */
        send_all(m, 121); /* the controllers reset: another program may have left them */
        send_all(m, 123); /* and its notes */
        m->gain_sent = LOAD(m->gain);
        send_volumes(m, m->gain_sent);
        int64_t               start = now_ns() + START_NS;
        int64_t               last = 0; /* the latest event, from the start */
        bmds_game_smf_event_t e;
        int                   have = 0;
        while (!LOAD(m->stop)) {
                const int32_t gain = LOAD(m->gain);
                if (gain != m->gain_sent) {
                        m->gain_sent = gain;
                        send_volumes(m, gain);
                }
                if (!have) {
                        const int r = bmds_game_smf_next(&m->smf, &e);
                        if (r <= 0) {
                                if (!m->loop) break;
                                /* Again from the start, after the last event. */
                                start += last;
                                last = 0;
                                bmds_game_smf_rewind(&m->smf);
                                if (bmds_game_smf_next(&m->smf, &e) <= 0) break; /* nothing in it */
                        }
                        have = 1;
                }
                const int64_t when = start + e.ns;
                if (when > now_ns() + AHEAD_NS) {
                        (void)bmds_sleep(WAKE_NS);
                        continue;
                }
                last = e.ns;
                have = 0;
                if (e.kind == BMDS_GAME_SMF_MESSAGE) {
                        uint8_t d[3] = {e.data[0], e.data[1], e.data[2]};
                        if ((d[0] & 0xf0u) == 0xb0u && d[1] == 7) {
                                m->volume[d[0] & 0x0fu] = d[2];
                                d[2] = scaled(d[2], m->gain_sent);
                        }
                        if (send(m, when, d, e.length) < 0) break;
                } else if (send(m, when, e.bytes, e.count) < 0) {
                        break;
                }
        }
        return NULL;
}

int bmds_game_is_midi_file(const char *path) {
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return 0;
        char          head[4] = {0};
        const int32_t n = bmds_read(fd, head, sizeof(head));
        bmds_close(fd);
        return n == 4 && memcmp(head, "MThd", 4) == 0;
}

struct bmds_game_midi_out *bmds_game_midi_out_start(const char *path, int loop, int32_t gain, int *err) {
        struct bmds_game_midi_out *m = (struct bmds_game_midi_out *)calloc(1, sizeof(*m));
        if (m == NULL) {
                *err = -BMDS_ENOMEM;
                return NULL;
        }
        m->fd = -1;
        int           e = 0;
        const int32_t in = bmds_open(path, BMDS_O_READ);
        if (in < 0) e = in;
        int64_t size = 0;
        if (e == 0) {
                int64_t end = 0;
                if (bmds_seek(in, 0, BMDS_SEEK_END, &end) < 0 || end <= 0 || end > (int64_t)MAX_FILE ||
                    bmds_seek(in, 0, BMDS_SEEK_SET, NULL) < 0)
                        e = -BMDS_EINVAL;
                size = end;
        }
        if (e == 0) {
                m->file = (uint8_t *)malloc((size_t)size);
                if (m->file == NULL) e = -BMDS_ENOMEM;
        }
        for (int64_t got = 0; e == 0 && got < size;) {
                const int32_t n = bmds_read(in, m->file + got, (uint32_t)(size - got));
                if (n <= 0) e = n < 0 ? n : -BMDS_EINVAL;
                got += n > 0 ? n : 0;
        }
        if (in >= 0) bmds_close(in);
        if (e == 0) e = bmds_game_smf_open(&m->smf, m->file, (size_t)size);
        if (e == 0) {
                m->fd = bmds_open("/dev/midi", BMDS_O_WRITE);
                if (m->fd < 0) e = m->fd;
        }
        if (e == 0) {
                bmds_midi_config_t c;
                memset(&c, 0, sizeof(c));
                c.size = sizeof(c);
                c.flags = BMDS_MIDI_EVENTS;
                c.event_size = sizeof(bmds_midi_event_t);
                e = bmds_midi_config(m->fd, &c, NULL);
        }
        if (e == 0) {
                m->loop = loop;
                m->gain = gain;
                if (pthread_create(&m->thread, NULL, midi_thread, m) != 0) e = -BMDS_ENOMEM;
        }
        if (e != 0) {
                if (m->fd >= 0) bmds_close(m->fd);
                free(m->file);
                free(m);
                *err = e;
                return NULL;
        }
        return m;
}

void bmds_game_midi_out_gain(struct bmds_game_midi_out *out, int32_t gain) {
        if (out != NULL) STORE(out->gain, gain);
        return;
}

void bmds_game_midi_out_stop(struct bmds_game_midi_out *out) {
        if (out == NULL) return;
        STORE(out->stop, 1);
        pthread_join(out->thread, NULL);
        (void)bmds_midi_control(out->fd, BMDS_MIDI_CANCEL, 0); /* what was written ahead */
        send_all(out, 123);                                    /* every note off */
        send_all(out, 120);                                    /* and what still sounds */
        bmds_close(out->fd);
        free(out->file);
        free(out);
        return;
}
