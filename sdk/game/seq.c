/**
 * @file      seq.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's sequencer (bmds/seq.h, devplan/sequencer.md). The timeline: each section's start
 * and step length worked out once; a moment heard maps to a section, bar, beat and step by
 * arithmetic. The scheduler walks the steps in order, each track's pattern round, and emits a
 * step's events in time order with the note-offs due before it (scheduled MIDI never goes back in
 * time on an fd). The player: a real-time thread waking every 20 ms that emits what is heard
 * before now + the latency + the lookahead, each event sent its latency early.
 */

#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seq_internal.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define MAX_TRACKS 16u
#define MAX_OFFS 256u /* notes sounding at once, their offs pending */
#define HITS 512u     /* the ring of notes heard */
#define WAKE_NS 20000000ll
#define BATCH 32u                          /* events a write */
#define PLAYER_STACK BMDS_THREAD_STACK_MIN /* 3 KB used on the board (2026-11-06): the least there is */
#define DEFAULT_LOOKAHEAD_MS 80u
#define DEFAULT_LATENCY_US 15000
#define GAP_NS 1000000ll /* a note ends this much before its length, so the next can start clean */

typedef struct {
                int64_t  start_ns; /* from the song's start */
                int64_t  step_ns;
                uint32_t steps;
                float    bpm;
} section_time_t;

typedef struct {
                int64_t heard_ns;
                uint8_t channel, note;
} pending_off_t;

struct bmds_seq_t {
                const bmds_seq_song_t *song;
                uint32_t               spb, bpb;
                section_time_t        *times;
                int64_t                length_ns, loop_ns; /* the song's, and where its loop starts */
                /* Playing. */
                int64_t start_ns, latency_ns, lookahead_ns;
                int     playing;
                /* The scheduler's place. */
                uint32_t      section, step, loops;
                int64_t       origin_ns; /* when the scheduler's section began, heard */
                int           ended;
                pending_off_t offs[MAX_OFFS];
                uint32_t      off_count;
                /* The port and the thread. */
                int32_t   fd;
                bmds_midi_event_t out[BATCH]; /* a wake's events, written together */
                uint32_t          out_count;
                pthread_t thread;
                int       thread_running;
                int       stop; /* LOAD/STORE */
                /* The notes heard. */
                pthread_mutex_t lock;
                bmds_seq_hit_t  hits[HITS];
                uint32_t        hit_count; /* in all: the ring's next is hit_count % HITS */
};

static int64_t now_ns(void) {
        int64_t ns = 0;
        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

/* ---- Checking and the timeline ---------------------------------------------------------------- */

static int check(const bmds_seq_song_t *s) {
        if (s == NULL || s->size < sizeof(*s) || s->track_count > MAX_TRACKS || s->section_count == 0 ||
            s->sections == NULL || (s->track_count != 0 && s->tracks == NULL))
                return -BMDS_EINVAL;
        if (!(s->swing >= 0.0f && s->swing <= 0.5f)) return -BMDS_EINVAL;
        if (s->loop_section >= (int32_t)s->section_count) return -BMDS_EINVAL;
        for (uint32_t t = 0; t < s->track_count; ++t) {
                const bmds_seq_track_t *k = &s->tracks[t];
                if (k->size < sizeof(*k) || k->channel > 15 || k->program > 127 || k->volume > 127 || k->pan > 127 ||
                    (k->pattern_count != 0 && k->patterns == NULL))
                        return -BMDS_EINVAL;
                for (uint32_t p = 0; p < k->pattern_count; ++p) {
                        const bmds_seq_pattern_t *q = &k->patterns[p];
                        if (q->size < sizeof(*q) || q->steps == 0 || q->steps > 256 || q->notes == NULL ||
                            (q->lane_count != 0 && q->lanes == NULL))
                                return -BMDS_EINVAL;
                        for (uint32_t l = 0; l < q->lane_count; ++l)
                                if (q->lanes[l].values == NULL || q->lanes[l].controller > 119) return -BMDS_EINVAL;
                        for (uint32_t i = 0; i < q->steps; ++i)
                                if (q->notes[i].note > 127 || q->notes[i].velocity > 127) return -BMDS_EINVAL;
                }
        }
        for (uint32_t i = 0; i < s->section_count; ++i) {
                const bmds_seq_section_t *c = &s->sections[i];
                const float               bpm = c->bpm > 0.0f ? c->bpm : s->bpm;
                if (c->size < sizeof(*c) || c->bars == 0 || c->bars > 4096 || !(bpm >= 10.0f && bpm <= 400.0f) ||
                    (s->track_count != 0 && c->patterns == NULL))
                        return -BMDS_EINVAL;
                for (uint32_t t = 0; t < s->track_count; ++t)
                        if (c->patterns[t] >= (int32_t)s->tracks[t].pattern_count) return -BMDS_EINVAL;
        }
        return 0;
}

int bmds_seq_make(const bmds_seq_song_t *song, bmds_seq_t **out) {
        *out = NULL;
        const int e = check(song);
        if (e < 0) return e;
        bmds_seq_t *q = (bmds_seq_t *)calloc(1, sizeof(*q));
        if (q == NULL) return -BMDS_ENOMEM;
        q->times = (section_time_t *)calloc(song->section_count, sizeof(section_time_t));
        if (q->times == NULL) {
                free(q);
                return -BMDS_ENOMEM;
        }
        q->song = song;
        q->spb = song->steps_per_beat != 0 ? song->steps_per_beat : 4;
        q->bpb = song->beats_per_bar != 0 ? song->beats_per_bar : 4;
        int64_t at = 0;
        for (uint32_t i = 0; i < song->section_count; ++i) {
                const bmds_seq_section_t *c = &song->sections[i];
                section_time_t           *t = &q->times[i];
                t->bpm = c->bpm > 0.0f ? c->bpm : song->bpm;
                t->step_ns = (int64_t)(60.0e9 / (double)t->bpm / (double)q->spb + 0.5);
                t->steps = c->bars * q->bpb * q->spb;
                t->start_ns = at;
                at += t->step_ns * t->steps;
        }
        q->length_ns = at;
        q->loop_ns = song->loop_section >= 0 ? q->times[song->loop_section].start_ns : at;
        q->fd = -1;
        pthread_mutex_init(&q->lock, NULL);
        *out = q;
        return 0;
}

int bmds_seq_position(const bmds_seq_t *q, int64_t time_ns, bmds_seq_position_t *pos) {
        if (pos == NULL || pos->size < sizeof(*pos)) return -BMDS_EINVAL;
        if (!q->playing || time_ns < q->start_ns) return -BMDS_EAGAIN;
        int64_t  t = time_ns - q->start_ns;
        uint32_t loops = 0;
        int      ended = 0;
        if (t >= q->length_ns) {
                if (q->song->loop_section < 0) {
                        t = q->length_ns - 1;
                        ended = 1;
                } else {
                        const int64_t round = q->length_ns - q->loop_ns, past = t - q->length_ns;
                        loops = 1 + (uint32_t)(past / round);
                        t = q->loop_ns + past % round;
                }
        }
        uint32_t s = 0;
        while (s + 1 < q->song->section_count && q->times[s + 1].start_ns <= t) ++s;
        const section_time_t *st = &q->times[s];
        const int64_t         into = t - st->start_ns;
        const uint32_t        step = (uint32_t)(into / st->step_ns);
        const int64_t         beat_ns = st->step_ns * q->spb, bar_ns = beat_ns * q->bpb;
        memset((uint8_t *)pos + sizeof(uint32_t), 0, sizeof(*pos) - sizeof(uint32_t));
        pos->section = s;
        pos->cue = q->song->sections[s].cue;
        pos->bar = step / (q->spb * q->bpb);
        pos->beat = step / q->spb % q->bpb;
        pos->step = step % q->spb;
        pos->beat_phase = (float)(into % beat_ns) / (float)beat_ns;
        pos->bar_phase = (float)(into % bar_ns) / (float)bar_ns;
        pos->bpm = st->bpm;
        pos->loops = loops;
        pos->seconds = (double)(time_ns - q->start_ns) / 1e9;
        pos->ended = ended;
        return 0;
}

/* ---- The scheduler ---------------------------------------------------------------------------- */

void bmds_seq_start_at(bmds_seq_t *q, int64_t start_ns, int64_t latency_ns) {
        q->start_ns = start_ns;
        q->latency_ns = latency_ns;
        q->section = q->step = q->loops = 0;
        q->origin_ns = start_ns;
        q->ended = 0;
        q->off_count = 0;
        pthread_mutex_lock(&q->lock);
        q->hit_count = 0;
        pthread_mutex_unlock(&q->lock);
        q->playing = 1;
        return;
}

static void add_off(bmds_seq_t *q, int64_t heard_ns, uint8_t channel, uint8_t note) {
        if (q->off_count == MAX_OFFS) return; /* can't happen with 16 tracks of 256 steps; a note held then */
        uint32_t i = q->off_count++;
        while (i > 0 && q->offs[i - 1].heard_ns > heard_ns) {
                q->offs[i] = q->offs[i - 1];
                --i;
        }
        q->offs[i] = (pending_off_t){heard_ns, channel, note};
        return;
}

/* The offs heard at or before @p heard_ns, in order. */
static void emit_offs(bmds_seq_t *q, int64_t heard_ns, bmds_seq_emit_t emit, void *arg) {
        uint32_t n = 0;
        while (n < q->off_count && q->offs[n].heard_ns <= heard_ns) {
                const uint8_t off[3] = {(uint8_t)(0x80u | q->offs[n].channel), q->offs[n].note, 0};
                emit(arg, q->offs[n].heard_ns, off, 3);
                ++n;
        }
        if (n != 0) {
                memmove(q->offs, q->offs + n, (q->off_count - n) * sizeof(pending_off_t));
                q->off_count -= n;
        }
        return;
}

static void hit(bmds_seq_t *q, int64_t heard_ns, uint32_t track, uint8_t note, uint8_t velocity) {
        pthread_mutex_lock(&q->lock);
        q->hits[q->hit_count % HITS] = (bmds_seq_hit_t){heard_ns, (uint16_t)track, note, velocity};
        ++q->hit_count;
        pthread_mutex_unlock(&q->lock);
        return;
}

void bmds_seq_schedule(bmds_seq_t *q, int64_t horizon_ns, bmds_seq_emit_t emit, void *arg) {
        const bmds_seq_song_t *song = q->song;
        while (!q->ended) {
                const section_time_t     *st = &q->times[q->section];
                const bmds_seq_section_t *sec = &song->sections[q->section];
                const int64_t             swing = (q->step & 1u) != 0 ? (int64_t)((float)st->step_ns * song->swing) : 0;
                const int64_t             at = q->origin_ns + (int64_t)q->step * st->step_ns + swing;
                if (at >= horizon_ns) break;
                emit_offs(q, at, emit, arg);
                for (uint32_t t = 0; t < song->track_count; ++t) {
                        const int8_t p = sec->patterns[t];
                        if (p < 0) continue;
                        const bmds_seq_track_t   *k = &song->tracks[t];
                        const bmds_seq_pattern_t *pat = &k->patterns[p];
                        const uint32_t            i = q->step % pat->steps;
                        const uint8_t             ch = (uint8_t)k->channel;
                        for (uint32_t l = 0; l < pat->lane_count; ++l) {
                                const uint8_t v = pat->lanes[l].values[i];
                                if (v > 127) continue;
                                const uint8_t cc[3] = {(uint8_t)(0xb0u | ch), pat->lanes[l].controller, v};
                                emit(arg, at, cc, 3);
                        }
                        const bmds_seq_step_t *n = &pat->notes[i];
                        if (n->velocity == 0) continue;
                        int note = n->note + (ch == 9 ? 0 : sec->transpose);
                        if (note < 0 || note > 127) continue;
                        uint32_t velocity = n->velocity + ((n->flags & BMDS_SEQ_ACCENT) != 0 ? 24u : 0u);
                        if (velocity > 127) velocity = 127;
                        const uint8_t on[3] = {(uint8_t)(0x90u | ch), (uint8_t)note, (uint8_t)velocity};
                        emit(arg, at, on, 3);
                        const int64_t length = (int64_t)(n->length != 0 ? n->length : 1) * st->step_ns;
                        add_off(q, at + (length > 2 * GAP_NS ? length - GAP_NS : length / 2), ch, (uint8_t)note);
                        hit(q, at, t, (uint8_t)note, (uint8_t)velocity);
                }
                /* On: the next step, section, or round. */
                if (++q->step == st->steps) {
                        q->step = 0;
                        q->origin_ns += st->step_ns * st->steps;
                        if (++q->section == song->section_count) {
                                if (song->loop_section < 0) {
                                        q->ended = 1;
                                        emit_offs(q, INT64_MAX, emit, arg);
                                        break;
                                }
                                q->section = (uint32_t)song->loop_section;
                                ++q->loops;
                        }
                }
        }
        return;
}

/* ---- The hits --------------------------------------------------------------------------------- */

uint32_t bmds_seq_hits(bmds_seq_t *q, int64_t from_ns, int64_t to_ns, uint32_t track_mask, bmds_seq_hit_t *out,
                       uint32_t max) {
        uint32_t n = 0;
        pthread_mutex_lock(&q->lock);
        const uint32_t count = q->hit_count, first = count > HITS ? count - HITS : 0;
        for (uint32_t i = first; i < count && n < max; ++i) {
                const bmds_seq_hit_t *h = &q->hits[i % HITS];
                if (h->time_ns > from_ns && h->time_ns <= to_ns && (track_mask >> h->track & 1u) != 0) out[n++] = *h;
        }
        pthread_mutex_unlock(&q->lock);
        return n;
}

int bmds_seq_last_hit(bmds_seq_t *q, int64_t time_ns, uint32_t track, bmds_seq_hit_t *out) {
        int found = 0;
        pthread_mutex_lock(&q->lock);
        const uint32_t count = q->hit_count, first = count > HITS ? count - HITS : 0;
        for (uint32_t i = count; i > first; --i) {
                const bmds_seq_hit_t *h = &q->hits[(i - 1) % HITS];
                if (h->track == track && h->time_ns <= time_ns) {
                        *out = *h;
                        found = 1;
                        break;
                }
        }
        pthread_mutex_unlock(&q->lock);
        return found ? 0 : -BMDS_ENOENT;
}

/* ---- The player ------------------------------------------------------------------------------- */

/* The events gathered, to the port in as few writes as it takes; a full queue waited out. */
static void flush(bmds_seq_t *q) {
        uint32_t done = 0;
        while (done < q->out_count) {
                const int32_t r = bmds_write(q->fd, q->out + done, (q->out_count - done) * sizeof(bmds_midi_event_t));
                if (r > 0) {
                        done += (uint32_t)r / sizeof(bmds_midi_event_t);
                } else if (r == -BMDS_EAGAIN && !LOAD(q->stop)) {
                        (void)bmds_sleep(5000000);
                } else {
                        break; /* a port gone: what is left is dropped */
                }
        }
        q->out_count = 0;
        return;
}

/* An event for the port, sent its latency ahead of when it is heard (gathered; flush() writes). */
static void queue_event(void *arg, int64_t heard_ns, const uint8_t *bytes, uint32_t length) {
        bmds_seq_t *q = (bmds_seq_t *)arg;
        if (q->out_count == BATCH) flush(q);
        bmds_midi_event_t *e = &q->out[q->out_count++];
        memset(e, 0, sizeof(*e));
        e->time_ns = heard_ns - q->latency_ns;
        e->length = (uint8_t)length;
        memcpy(e->data, bytes, length);
        return;
}

/* An event now. */
static void send_event(void *arg, int64_t heard_ns, const uint8_t *bytes, uint32_t length) {
        queue_event(arg, heard_ns, bytes, length);
        flush((bmds_seq_t *)arg);
        return;
}

static void *player(void *arg) {
        bmds_seq_t        *q = (bmds_seq_t *)arg;
        struct sched_param rt = {.sched_priority = 40};
        (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt);
        while (!LOAD(q->stop) && !q->ended) {
                bmds_seq_schedule(q, now_ns() + q->latency_ns + q->lookahead_ns, queue_event, q);
                flush(q);
                (void)bmds_sleep(WAKE_NS);
        }
        return NULL;
}

/* A port by its file, or by its name (each /dev/midiN asked). */
static int32_t open_port(const char *port) {
        if (port == NULL) port = "/dev/midi";
        if (port[0] == '/') return bmds_open(port, BMDS_O_WRITE);
        for (uint32_t n = 0; n < 16; ++n) {
                char path[16];
                snprintf(path, sizeof(path), "/dev/midi%u", (unsigned)n);
                const int32_t fd = bmds_open(path, BMDS_O_WRITE);
                if (fd < 0) continue;
                bmds_midi_status_t st;
                memset(&st, 0, sizeof(st));
                st.size = sizeof(st);
                if (bmds_midi_status(fd, &st) == 0 && strncmp(st.name, port, sizeof(st.name)) == 0) return fd;
                bmds_close(fd);
        }
        return -BMDS_ENOENT;
}

int bmds_seq_open(const bmds_seq_config_t *config, const bmds_seq_song_t *song, bmds_seq_t **out) {
        if (out == NULL || (config != NULL && config->size < offsetof(bmds_seq_config_t, allow_latency_us)))
                return -BMDS_EINVAL;
        bmds_seq_t *q = NULL;
        int         e = bmds_seq_make(song, &q);
        if (e < 0) return e;
        q->fd = open_port(config != NULL ? config->port : NULL);
        if (q->fd < 0) {
                e = q->fd;
                q->fd = -1;
                bmds_seq_close(q);
                return e;
        }
        bmds_midi_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.flags = BMDS_MIDI_EVENTS;
        c.event_size = sizeof(bmds_midi_event_t);
        e = bmds_midi_config(q->fd, &c, NULL);
        if (e < 0) {
                bmds_seq_close(q);
                return e;
        }
        const int32_t lookahead = config != NULL && config->lookahead_ms != 0 ? (int32_t)config->lookahead_ms
                                                                              : (int32_t)DEFAULT_LOOKAHEAD_MS;
        q->lookahead_ns = (int64_t)lookahead * 1000000;
        /* Allowed first, so the latency asked is the one it settled on (an old kernel: EINVAL, real time). */
        if (config != NULL && config->size >= sizeof(*config) && config->allow_latency_us != 0)
                (void)bmds_midi_control(
                        q->fd, BMDS_MIDI_ALLOW_LATENCY,
                        config->allow_latency_us > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)config->allow_latency_us);
        int32_t latency = config != NULL ? config->latency_us : -1;
        if (latency < 0) latency = bmds_midi_control(q->fd, BMDS_MIDI_LATENCY, 0);
        q->latency_ns = (int64_t)(latency >= 0 ? latency : DEFAULT_LATENCY_US) * 1000;
        *out = q;
        return 0;
}

int64_t bmds_seq_latency_ns(const bmds_seq_t *q) {
        return q->latency_ns;
}

/* @p controller to @p value on every channel the song uses, now. */
static void all_channels(bmds_seq_t *q, uint8_t controller, uint8_t value) {
        uint32_t done = 0;
        for (uint32_t t = 0; t < q->song->track_count; ++t) {
                const uint32_t ch = q->song->tracks[t].channel;
                if ((done >> ch & 1u) != 0) continue;
                done |= 1u << ch;
                const uint8_t cc[3] = {(uint8_t)(0xb0u | ch), controller, value};
                send_event(q, q->latency_ns, cc, 3); /* time latency - latency: 0, now */
        }
        return;
}

int bmds_seq_play(bmds_seq_t *q, int64_t start_ns) {
        if (q->thread_running || q->fd < 0) return -BMDS_EINVAL;
        /* Each track's sound, now. */
        all_channels(q, 121, 0); /* controllers reset */
        for (uint32_t t = 0; t < q->song->track_count; ++t) {
                const bmds_seq_track_t *k = &q->song->tracks[t];
                const uint8_t           ch = (uint8_t)k->channel;
                const uint8_t           pc[2] = {(uint8_t)(0xc0u | ch), (uint8_t)k->program};
                const uint8_t           vol[3] = {(uint8_t)(0xb0u | ch), 7, (uint8_t)k->volume};
                const uint8_t           pan[3] = {(uint8_t)(0xb0u | ch), 10, (uint8_t)k->pan};
                if (ch != 9) send_event(q, q->latency_ns, pc, 2);
                send_event(q, q->latency_ns, vol, 3);
                send_event(q, q->latency_ns, pan, 3);
        }
        bmds_seq_start_at(q, start_ns != 0 ? start_ns : now_ns() + q->latency_ns + q->lookahead_ns, q->latency_ns);
        STORE(q->stop, 0);
        /* A small stack: a thread's default (32 KB) goes in internal RAM while there is room, which
         * full screen and the synths need too (devplan/sequencer.md §11). */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, PLAYER_STACK);
        const int made = pthread_create(&q->thread, &attr, player, q);
        pthread_attr_destroy(&attr);
        if (made != 0) {
                q->playing = 0;
                return -BMDS_ENOMEM;
        }
        q->thread_running = 1;
        return 0;
}

void bmds_seq_stop(bmds_seq_t *q) {
        if (q == NULL) return;
        if (q->thread_running) {
                STORE(q->stop, 1);
                pthread_join(q->thread, NULL);
                q->thread_running = 0;
        }
        if (q->fd >= 0) {
                (void)bmds_midi_control(q->fd, BMDS_MIDI_CANCEL, 0);
                all_channels(q, 123, 0); /* all notes off */
                all_channels(q, 120, 0); /* all sound off */
        }
        q->playing = 0;
        return;
}

void bmds_seq_close(bmds_seq_t *q) {
        if (q == NULL) return;
        bmds_seq_stop(q);
        if (q->fd >= 0) bmds_close(q->fd);
        pthread_mutex_destroy(&q->lock);
        free(q->times);
        free(q);
        return;
}
