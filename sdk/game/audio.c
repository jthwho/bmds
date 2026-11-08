/**
 * @file      audio.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's sound (devplan/games.md §16). Effects: a real-time client of /dev/dsp whose thread
 * runs the mixer (mix.c) a period at a time. Sounds: decoded whole by `audio_decode` and resampled
 * to the device's rate at load. Music: a thread that reads a decoder (the synth's, for MIDI) and
 * writes to an ordinary stream of its own, which the system resamples and mixes; its writes block,
 * so it runs as fast as the music plays. Both threads keep to the core the game's thread doesn't
 * (games.md §23).
 */

#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include "game_internal.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define DEFAULT_PERIOD 256
#define MAX_SOUND_SECONDS 600
#define MUSIC_FRAMES 1024 /* a music write */
#define MUSIC_RING_MS 250 /* its stream's queue */
#define AHEAD_DEFAULT_MS 100 /* BMDS_GAME_AUDIO_AHEAD without a latency */
#define AHEAD_MIN_MS 20
#define AHEAD_MAX_MS 1000
#define REANCHOR_NS 20000000 /* ahead: the heard time found again past this */
#define EASE_SHIFT 5         /* ahead: a period's 1/32 of the way to the measurement */
#define AHEAD_PERIODS 4      /* ahead: the device's periods a write */
#define AWAKE_NS 500000000ll /* ahead: silence written this long after the last sound, before sleeping */

struct bmds_game_audio_t {
                bmds_game_mixer_t mixer;
                int32_t           fd;
                uint32_t          rate;
                uint32_t          channels;
                pthread_t         thread;
                int               quit;
                int32_t           error; /* the mixer thread's, if it stopped */
                int32_t           core;  /* the audio threads', or -1 (anywhere) */
                uint32_t          ahead_ms;   /* BMDS_GAME_AUDIO_AHEAD's latency; 0: real time */
                uint32_t          period;     /* ahead: frames a write */
                uint32_t          ring;       /* ahead: the stream's ring, frames */
                int32_t           latency_us; /* a sound's play to its being heard (LOAD/STORE: 32 bits) */
                pthread_mutex_t   idle_lock;  /* ahead: the mixer's thread sleeps while nothing plays */
                pthread_cond_t    idle_wake;
                /* The music. */
                pthread_t thread_music;
                int       music_running;
                int       music_stop;
                int32_t   music_decoder;
                int32_t   music_out; /* its stream, or -1 */
                uint32_t  music_channels;
                uint32_t  music_loop;
                int32_t   music_gain; /* 0-32768 */
                struct bmds_game_midi_out *music_midi; /* BMDS_GAME_MUSIC_MIDI_OUT's player, or NULL */
                int16_t   music_buf[MUSIC_FRAMES * 2];
};

/* ---- Cores: the game's thread keeps to the one it runs on, the audio threads to the other, so the
   game, the mixer and the synth never pile onto one core while the other idles. A thread is pinned
   for good by its first floating point, so each audio thread asks first thing, in an entry that
   uses none (its body is another function: a prologue saving float registers would pin it). */

static int32_t game_core(void) {
        bmds_sched_t s;
        memset(&s, 0, sizeof(s));
        s.size = sizeof(s);
        if (bmds_sched(BMDS_SCHED_GET, 0, &s) < 0 || s.core < 0 || s.core > 1) return -1;
        if (s.cores != 3u) return s.cores == 1u ? 0 : s.cores == 2u ? 1 : -1; /* pinned already */
        s.cores = 1u << s.core;
        return bmds_sched(BMDS_SCHED_PIN, 0, &s) == 0 ? s.core : -1;
}

static void keep_to(int32_t core) {
        if (core < 0) return;
        bmds_sched_t s;
        memset(&s, 0, sizeof(s));
        s.size = sizeof(s);
        s.cores = 1u << core;
        (void)bmds_sched(BMDS_SCHED_PIN, 0, &s);
        return;
}

static void *mixer_thread(void *arg) __attribute__((noinline));
static void *ahead_thread(void *arg) __attribute__((noinline));
static void *music_thread(void *arg) __attribute__((noinline));

static void *mixer_entry(void *arg) {
        keep_to(((bmds_game_audio_t *)arg)->core);
        return mixer_thread(arg);
}

static void *music_entry(void *arg) {
        keep_to(((bmds_game_audio_t *)arg)->core);
        return music_thread(arg);
}

static void *mixer_thread(void *arg) {
        bmds_game_audio_t *a = arg;
        bmds_audio_cycle_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        for (;;) {
                const int     quit = LOAD(a->quit);
                const int32_t r = bmds_audio_cycle(a->fd, quit ? NULL : &c);
                if (quit) break;
                if (r == -BMDS_EINTR || r == -BMDS_EPIPE) continue; /* late, or stopped: joins again */
                if (r < 0) {
                        STORE(a->error, r);
                        break;
                }
                int64_t now = 0;
                (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
                if (c.play_ns > now) STORE(a->latency_us, (int32_t)((c.play_ns - now) / 1000));
                bmds_game_mixer_run_at(&a->mixer, c.out, c.frames, c.channels, c.play_ns, c.rate);
        }
        return NULL;
}

/* BMDS_GAME_AUDIO_AHEAD: an ordinary stream, a period at a time, its writes blocking; each period
   told when it is heard (devplan/audio.md §27's way: the mixer's next take after the ring, then two
   DMA buffers; a timeline eased towards that, found again after an underrun). */
static void *ahead_thread(void *arg) {
        /* The game's priority, not real time: the ring is its margin, and above the display's work
           it took the presenter's time for nothing (13 % of a core idle, at 7: demo.md step 6). */
        bmds_game_audio_t *a = arg;
        const uint32_t     frames = a->period * AHEAD_PERIODS;
        float             *out = bmds_mem_alloc(sizeof(float) * 2 * frames, BMDS_MEM_ALIGN, 0);
        if (out == NULL) {
                STORE(a->error, -BMDS_ENOMEM);
                return NULL;
        }
        const int64_t period_ns = (int64_t)frames * 1000000000 / a->rate;
        int64_t       heard = 0;
        uint32_t      underruns = 0;
        int           dry = 1;    /* the stream ran dry (or never started) */
        int64_t       quiet = -1; /* writes of silence since the last sound (-1: sounding) */
        while (!LOAD(a->quit)) {
                /* Nothing playing, nothing asked, for a while: asleep until a sound is (the stream
                   runs dry; the heard time is found again as it starts). Awake a little longer
                   first, so lines close together don't each start a dry stream. */
                if (bmds_game_mixer_idle(&a->mixer)) {
                        if (quiet < 0) quiet = 0;
                        if (quiet * period_ns >= AWAKE_NS) {
                                pthread_mutex_lock(&a->idle_lock);
                                while (!LOAD(a->quit) && bmds_game_mixer_idle(&a->mixer))
                                        pthread_cond_wait(&a->idle_wake, &a->idle_lock);
                                pthread_mutex_unlock(&a->idle_lock);
                                heard = 0;
                                dry = 1;
                                quiet = -1;
                                continue;
                        }
                        ++quiet;
                } else {
                        quiet = -1;
                }
                /* Starting dry: half the ring of silence first. The stream plays from its first
                   period, and this thread (no deadline: the game's priority) may not be back for
                   tens of ms; each gap was a click at a line's start (demo.md step 6). The timeline
                   is measured after it, so a time asked is still kept. */
                if (dry) {
                        dry = 0;
                        memset(out, 0, sizeof(float) * 2 * frames);
                        for (uint32_t lead = 0; lead + frames <= a->ring / 2 && !LOAD(a->quit); lead += frames)
                                if (bmds_write(a->fd, out, (uint32_t)(sizeof(float) * 2 * frames)) <= 0) break;
                }
                bmds_audio_status_t st;
                memset(&st, 0, sizeof(st));
                st.size = sizeof(st);
                int64_t now = 0;
                (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
                if (bmds_audio_status(a->fd, &st) == 0) {
                        int64_t next =
                                st.timestamp_ns != 0 && st.queued_frames != 0 ? st.timestamp_ns + period_ns : now;
                        if (next < now) next = now;
                        const int64_t measured =
                                next + (int64_t)st.queued_frames * 1000000000 / a->rate + 2 * period_ns;
                        const int64_t off = measured - heard;
                        if (heard == 0 || st.underruns != underruns || off > REANCHOR_NS || off < -REANCHOR_NS)
                                heard = measured;
                        else
                                heard += off / (1 << EASE_SHIFT);
                        underruns = st.underruns;
                }
                STORE(a->latency_us, heard > now ? (int32_t)((heard - now) / 1000) : 0);
                bmds_game_mixer_run_at(&a->mixer, out, frames, 2, heard, a->rate);
                const uint32_t bytes = (uint32_t)(sizeof(float) * 2 * frames);
                for (uint32_t done = 0; done < bytes && !LOAD(a->quit);) {
                        const int32_t w = bmds_write(a->fd, (const uint8_t *)out + done, bytes - done);
                        if (w <= 0) {
                                if (w == -BMDS_EINTR) continue;
                                STORE(a->error, w < 0 ? w : -BMDS_EIO);
                                bmds_mem_free(out);
                                return NULL;
                        }
                        done += (uint32_t)w;
                }
                heard += period_ns;
        }
        bmds_mem_free(out);
        return NULL;
}

static void *ahead_entry(void *arg) {
        keep_to(((bmds_game_audio_t *)arg)->core);
        return ahead_thread(arg);
}

/* The power of two at most @p frames, at least 512. */
static uint32_t ring_below(uint32_t frames) {
        uint32_t r = 512;
        while (r * 2 <= frames) r *= 2;
        return r;
}

/* BMDS_GAME_AUDIO_AHEAD's stream and thread on @p a's open fd. */
static int open_ahead(bmds_game_audio_t *a, uint32_t period_frames) {
        bmds_audio_status_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        int err = bmds_audio_status(a->fd, &st);
        if (err < 0) return err;
        a->rate = st.device_rate != 0 ? st.device_rate : 48000;
        a->channels = 2;
        a->period = period_frames != 0 ? period_frames : st.period_frames != 0 ? st.period_frames : DEFAULT_PERIOD;
        a->ring = ring_below((uint32_t)((uint64_t)a->ahead_ms * a->rate / 1000));
        if (a->ring < 2 * AHEAD_PERIODS * a->period) a->ring = ring_below(4 * AHEAD_PERIODS * a->period);
        pthread_mutex_init(&a->idle_lock, NULL);
        pthread_cond_init(&a->idle_wake, NULL);
        bmds_audio_config_t want;
        memset(&want, 0, sizeof(want));
        want.size = sizeof(want);
        want.format = BMDS_AUDIO_F32LE;
        want.channels = 2;
        want.rate = a->rate;
        want.ring_frames = a->ring;
        want.start_frames = a->period;                /* plays at once; the ring fills as the mixer outruns it */
        want.wake_frames = a->period * AHEAD_PERIODS; /* a write's worth: woken once for it */
        bmds_audio_config_t got;
        memset(&got, 0, sizeof(got));
        got.size = sizeof(got);
        err = bmds_audio_config(a->fd, &want, &got);
        if (err < 0) return err;
        a->latency_us = (int32_t)((int64_t)a->ring * 1000000 / a->rate);
        /* The game's core, beside its thread at its priority (time sliced: back within a tick or
           two). On the other, below the presenter and a synth rendering ahead there, it went
           without for the ring's whole 170 ms, the line playing (demo.md step 6). */
        a->core = game_core();
        if (pthread_create(&a->thread, NULL, ahead_entry, a) != 0) return -BMDS_ENOMEM;
        return 0;
}

int bmds_game_audio_open(const bmds_game_audio_config_t *config, bmds_game_audio_t **audio) {
        if (audio == NULL || (config != NULL && config->size < offsetof(bmds_game_audio_config_t, latency_ms)))
                return -BMDS_EINVAL;
        *audio = NULL;
        const uint32_t flags = config != NULL ? config->flags : 0;
        if ((flags & ~BMDS_GAME_AUDIO_AHEAD) != 0) return -BMDS_EINVAL;
        uint32_t ahead_ms = 0;
        if ((flags & BMDS_GAME_AUDIO_AHEAD) != 0) {
                ahead_ms = config->size >= sizeof(bmds_game_audio_config_t) && config->latency_ms != 0
                                   ? config->latency_ms
                                   : AHEAD_DEFAULT_MS;
                if (ahead_ms < AHEAD_MIN_MS) ahead_ms = AHEAD_MIN_MS;
                if (ahead_ms > AHEAD_MAX_MS) ahead_ms = AHEAD_MAX_MS;
        }
        bmds_game_audio_t *a = bmds_mem_alloc(sizeof(*a), BMDS_MEM_ALIGN, 0);
        if (a == NULL) return -BMDS_ENOMEM;
        memset(a, 0, sizeof(*a));
        bmds_game_mixer_init(&a->mixer);
        a->music_out = -1;
        a->music_decoder = -1;
        a->ahead_ms = ahead_ms;
        a->fd = bmds_open("/dev/dsp", BMDS_O_WRITE);
        if (a->fd < 0) {
                const int err = a->fd;
                bmds_mem_free(a);
                return err;
        }
        if (ahead_ms != 0) {
                const int err = open_ahead(a, config->period_frames);
                if (err != 0) {
                        bmds_close(a->fd);
                        bmds_mem_free(a);
                        return err;
                }
                *audio = a;
                return 0;
        }
        bmds_audio_config_t want;
        memset(&want, 0, sizeof(want));
        want.size = sizeof(want);
        want.format = BMDS_AUDIO_F32LE;
        want.channels = 2;
        want.wake_frames = config != NULL && config->period_frames != 0 ? config->period_frames : DEFAULT_PERIOD;
        want.flags = BMDS_AUDIO_REALTIME;
        bmds_audio_config_t got;
        memset(&got, 0, sizeof(got));
        got.size = sizeof(got);
        int err = bmds_audio_config(a->fd, &want, &got);
        if (err == 0) {
                a->rate = got.rate;
                a->channels = got.channels;
                const int32_t game = game_core(); /* two cores: the audio threads take the other */
                a->core = game < 0 ? -1 : 1 - game;
                if (pthread_create(&a->thread, NULL, mixer_entry, a) != 0) err = -BMDS_ENOMEM;
        }
        if (err != 0) {
                bmds_close(a->fd);
                bmds_mem_free(a);
                return err;
        }
        *audio = a;
        return 0;
}

/* Ahead: wakes the mixer's thread for what was just asked (nothing: real time). */
static void wake(bmds_game_audio_t *a) {
        if (a->ahead_ms == 0) return;
        pthread_mutex_lock(&a->idle_lock);
        pthread_cond_signal(&a->idle_wake);
        pthread_mutex_unlock(&a->idle_lock);
        return;
}

void bmds_game_audio_close(bmds_game_audio_t *audio) {
        if (audio == NULL) return;
        bmds_game_music_stop(audio);
        STORE(audio->quit, 1);
        wake(audio);
        pthread_join(audio->thread, NULL);
        if (audio->ahead_ms != 0) {
                pthread_cond_destroy(&audio->idle_wake);
                pthread_mutex_destroy(&audio->idle_lock);
        }
        bmds_close(audio->fd);
        bmds_mem_free(audio);
        return;
}

uint32_t bmds_game_audio_rate(const bmds_game_audio_t *audio) {
        return audio->rate;
}

int64_t bmds_game_audio_latency_ns(const bmds_game_audio_t *audio) {
        if (audio == NULL) return 0;
        const int64_t ns = (int64_t)LOAD(((bmds_game_audio_t *)audio)->latency_us) * 1000;
        return ns > 0 ? ns : (int64_t)DEFAULT_PERIOD * 3 * 1000000000 / (audio->rate != 0 ? audio->rate : 48000);
}

void bmds_game_audio_pause(bmds_game_audio_t *audio, int paused) {
        const bmds_game_command_t c = {BMDS_GAME_COMMAND_PAUSE, 0, 0, NULL, 0, 0, 0.0f, paused ? 1.0f : 0.0f, 0, 0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
        wake(audio);
        const int32_t out = LOAD(audio->music_out);
        if (out >= 0) (void)bmds_audio_control(out, paused ? BMDS_AUDIO_PAUSE : BMDS_AUDIO_RESUME, 0);
        return;
}

void bmds_game_audio_volume(bmds_game_audio_t *audio, float volume) {
        const bmds_game_command_t c = {BMDS_GAME_COMMAND_VOLUME,
                                       0,
                                       0,
                                       NULL,
                                       0,
                                       0,
                                       volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume,
                                       0.0f,
                                       0,
                                       0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
        wake(audio);
        return;
}

int32_t bmds_game_play(bmds_game_audio_t *audio, const bmds_game_sound_t *sound, float volume, float pan,
                       uint32_t flags) {
        const int32_t voice = bmds_game_mixer_play(&audio->mixer, sound, volume, pan, flags);
        wake(audio);
        return voice;
}

int32_t bmds_game_play_at(bmds_game_audio_t *audio, const bmds_game_sound_t *sound, float volume, float pan,
                          uint32_t flags, int64_t at_ns) {
        const int32_t voice = bmds_game_mixer_play_at(&audio->mixer, sound, volume, pan, flags, at_ns);
        wake(audio);
        return voice;
}

static void voice_command(bmds_game_audio_t *audio, int32_t voice, uint32_t type, float volume, float pan) {
        uint32_t      generation = 0;
        const int32_t slot = bmds_game_mixer_voice(&audio->mixer, voice, &generation);
        if (slot < 0) return;
        const bmds_game_command_t c = {type,
                                       (uint32_t)slot,
                                       generation,
                                       NULL,
                                       0,
                                       0,
                                       volume < 0.0f   ? 0.0f
                                       : volume > 1.0f ? 1.0f
                                                       : volume,
                                       pan < -1.0f  ? -1.0f
                                       : pan > 1.0f ? 1.0f
                                                    : pan,
                                       0,
                                       0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
        wake(audio);
        return;
}

void bmds_game_voice_set(bmds_game_audio_t *audio, int32_t voice, float volume, float pan) {
        voice_command(audio, voice, BMDS_GAME_COMMAND_SET, volume, pan);
        return;
}

void bmds_game_voice_stop(bmds_game_audio_t *audio, int32_t voice) {
        voice_command(audio, voice, BMDS_GAME_COMMAND_STOP, 0.0f, 0.0f);
        return;
}

int bmds_game_voice_is_playing(const bmds_game_audio_t *audio, int32_t voice) {
        uint32_t generation = 0;
        return bmds_game_mixer_voice(&audio->mixer, voice, &generation) >= 0;
}

/* ---- Sounds */

/* All of decoder @p dec's PCM (16-bit) into @p sound at @p rate. */
static int read_sound(int32_t dec, uint32_t rate, bmds_game_sound_t *sound) {
        bmds_decode_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        int err = bmds_audio_decode_info(dec, &info);
        if (err < 0) return err;
        if ((info.channels != 1 && info.channels != 2) || info.rate == 0) return -BMDS_ENOTSUP;
        const uint32_t frame_bytes = 2 * info.channels;
        const uint64_t most = (uint64_t)MAX_SOUND_SECONDS * info.rate;
        if (info.frames != BMDS_DECODE_UNKNOWN_FRAMES && info.frames > most) return -BMDS_ERANGE;
        uint32_t capacity =
                info.frames != BMDS_DECODE_UNKNOWN_FRAMES && info.frames > 0 ? (uint32_t)info.frames : info.rate;
        int16_t *pcm = bmds_mem_alloc(capacity * frame_bytes, BMDS_MEM_ALIGN, 0);
        if (pcm == NULL) return -BMDS_ENOMEM;
        uint32_t frames = 0;
        for (;;) {
                if (frames == capacity) {
                        /* Longer than said (or not said): half again. */
                        if (capacity >= most) {
                                err = -BMDS_ERANGE;
                                break;
                        }
                        const uint32_t more = capacity + capacity / 2 + 1;
                        int16_t       *grown = bmds_mem_realloc(pcm, more * frame_bytes, 0);
                        if (grown == NULL) {
                                err = -BMDS_ENOMEM;
                                break;
                        }
                        pcm = grown;
                        capacity = more;
                }
                const int32_t n = bmds_read(dec, (uint8_t *)pcm + (size_t)frames * frame_bytes,
                                            (capacity - frames) * frame_bytes);
                if (n < 0) {
                        err = n;
                        break;
                }
                if (n == 0) break;
                frames += (uint32_t)n / frame_bytes;
        }
        if (err == 0 && frames == 0) err = -BMDS_EINVAL;
        if (err != 0) {
                bmds_mem_free(pcm);
                return err;
        }
        if (info.rate != rate) {
                const uint32_t n = bmds_game_resampled(frames, info.rate, rate);
                int16_t       *out = bmds_mem_alloc(n * frame_bytes, BMDS_MEM_ALIGN, 0);
                if (out == NULL) {
                        bmds_mem_free(pcm);
                        return -BMDS_ENOMEM;
                }
                bmds_game_resample(pcm, frames, info.channels, info.rate, rate, out);
                bmds_mem_free(pcm);
                pcm = out;
                frames = n;
        } else if (frames < capacity) {
                int16_t *cut = bmds_mem_realloc(pcm, frames * frame_bytes, 0);
                if (cut != NULL) pcm = cut;
        }
        sound->samples = pcm;
        sound->frames = frames;
        sound->channels = info.channels;
        return 0;
}

static int load_sound(bmds_game_audio_t *audio, int32_t fd, const void *data, uint32_t length,
                      bmds_game_sound_t *sound) {
        bmds_decode_config_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.format = BMDS_AUDIO_S16LE;
        d.data = data;
        d.length = length;
        const int32_t dec = bmds_audio_decode(fd, &d);
        if (dec < 0) return dec;
        const int err = read_sound(dec, audio->rate, sound);
        bmds_close(dec);
        return err;
}

int bmds_game_sound_load(bmds_game_audio_t *audio, const char *path, bmds_game_sound_t *sound) {
        if (audio == NULL || path == NULL || sound == NULL) return -BMDS_EINVAL;
        memset(sound, 0, sizeof(*sound));
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return fd;
        const int err = load_sound(audio, fd, NULL, 0, sound);
        bmds_close(fd);
        return err;
}

int bmds_game_sound_load_memory(bmds_game_audio_t *audio, const void *data, uint32_t length, bmds_game_sound_t *sound) {
        if (audio == NULL || data == NULL || sound == NULL) return -BMDS_EINVAL;
        memset(sound, 0, sizeof(*sound));
        return load_sound(audio, -1, data, length, sound);
}

/* Speech's samples read into @p pcm (grown), @p frames so far: what fd @p fd has made. A read of 0
   is the end once the text's NUL is written (@p final), else only nothing made yet. */
static int read_speech(int32_t fd, int16_t **pcm, uint32_t *frames, uint32_t *capacity, int final, int *done) {
        for (;;) {
                if (*frames == *capacity) {
                        const uint32_t more = *capacity + *capacity / 2 + 4096;
                        if (more > (uint32_t)MAX_SOUND_SECONDS * 16000u) return -BMDS_ERANGE;
                        int16_t *grown = bmds_mem_realloc(*pcm, more * sizeof(int16_t), 0);
                        if (grown == NULL) return -BMDS_ENOMEM;
                        *pcm = grown;
                        *capacity = more;
                }
                const int32_t n = bmds_read(fd, *pcm + *frames, (*capacity - *frames) * (uint32_t)sizeof(int16_t));
                if (n < 0) return n;
                if (n == 0) {
                        *done = final;
                        return 0;
                }
                *frames += (uint32_t)n / sizeof(int16_t);
                if (*frames < *capacity) return 0; /* all it had: more text may be wanted */
        }
}

struct bmds_game_speaker_t {
                int32_t  fd;
                uint32_t rate; /* its samples' */
                int16_t *pcm;  /* a line's samples, kept to the next (grown, never shrunk) */
                uint32_t capacity;
};

int bmds_game_speaker_open(const bmds_game_speak_t *how, bmds_game_speaker_t **speaker) {
        if (speaker == NULL || (how != NULL && how->size < sizeof(*how))) return -BMDS_EINVAL;
        *speaker = NULL;
        if (how != NULL && (how->flags & ~BMDS_GAME_SPEAK_MARKUP) != 0) return -BMDS_EINVAL;
        if (!bmds_sys_has(BMDS_SYS_AUDIO_SPEAK)) return -BMDS_ENOSYS;
        bmds_speak_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.flags = BMDS_O_CLOEXEC;
        if (how != NULL) {
                c.voice = how->voice;
                c.speed = how->speed;
                c.pitch = how->pitch;
                c.volume = how->volume;
                if ((how->flags & BMDS_GAME_SPEAK_MARKUP) != 0) c.flags |= BMDS_SPEAK_MARKUP;
        }
        bmds_game_speaker_t *s = bmds_mem_alloc(sizeof(*s), BMDS_MEM_ALIGN, 0);
        if (s == NULL) return -BMDS_ENOMEM;
        memset(s, 0, sizeof(*s));
        s->fd = bmds_audio_speak(&c);
        if (s->fd < 0) {
                const int err = s->fd;
                bmds_mem_free(s);
                return err;
        }
        bmds_speak_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        s->rate = bmds_audio_speak_info(s->fd, &info) == 0 && info.rate != 0 ? info.rate : 16000;
        *speaker = s;
        return 0;
}

void bmds_game_speaker_close(bmds_game_speaker_t *speaker) {
        if (speaker == NULL) return;
        bmds_close(speaker->fd);
        bmds_mem_free(speaker->pcm);
        bmds_mem_free(speaker);
        return;
}

int bmds_game_speaker_say(bmds_game_speaker_t *speaker, bmds_game_audio_t *audio, const char *text,
                          bmds_game_sound_t *sound) {
        if (speaker == NULL || audio == NULL || text == NULL || sound == NULL) return -BMDS_EINVAL;
        memset(sound, 0, sizeof(*sound));
        uint32_t frames = 0;
        int      done = 0, err = 0;
        /* The text and its NUL (the last sentence said); what doesn't fit waits for a read. */
        const uint32_t length = (uint32_t)strlen(text) + 1;
        for (uint32_t put = 0; err == 0 && put < length;) {
                const int32_t w = bmds_write(speaker->fd, (const uint8_t *)text + put, length - put);
                if (w > 0)
                        put += (uint32_t)w;
                else if (w == -BMDS_EAGAIN || w == 0)
                        err = read_speech(speaker->fd, &speaker->pcm, &frames, &speaker->capacity, 0, &done);
                else
                        err = w;
        }
        while (err == 0 && !done) err = read_speech(speaker->fd, &speaker->pcm, &frames, &speaker->capacity, 1, &done);
        if (err == 0 && frames == 0) err = -BMDS_EINVAL;
        if (err != 0) return err;
        const uint32_t n = bmds_game_resampled(frames, speaker->rate, audio->rate);
        int16_t       *out = bmds_mem_alloc(n * sizeof(int16_t), BMDS_MEM_ALIGN, 0);
        if (out == NULL) return -BMDS_ENOMEM;
        bmds_game_resample(speaker->pcm, frames, 1, speaker->rate, audio->rate, out);
        sound->samples = out;
        sound->frames = n;
        sound->channels = 1;
        return 0;
}

int bmds_game_sound_speak(bmds_game_audio_t *audio, const char *text, const bmds_game_speak_t *how,
                          bmds_game_sound_t *sound) {
        if (audio == NULL || text == NULL || sound == NULL) return -BMDS_EINVAL;
        memset(sound, 0, sizeof(*sound));
        bmds_game_speaker_t *speaker = NULL;
        int                  err = bmds_game_speaker_open(how, &speaker);
        if (err == 0) err = bmds_game_speaker_say(speaker, audio, text, sound);
        bmds_game_speaker_close(speaker);
        return err;
}

void bmds_game_sound_free(bmds_game_sound_t *sound) {
        if (sound == NULL) return;
        bmds_mem_free((void *)sound->samples);
        memset(sound, 0, sizeof(*sound));
        return;
}

/* ---- Music */

static void *music_thread(void *arg) {
        bmds_game_audio_t *a = arg;
        const uint32_t     frame_bytes = 2 * a->music_channels;
        /* Real-time at the lowest priority: above the game and the display's work, so a busy frame
           doesn't starve the synth into gaps (games.md §20); refused, it plays as it is. */
        struct sched_param rt;
        memset(&rt, 0, sizeof(rt));
        rt.sched_priority = 1;
        (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt);
        while (!LOAD(a->music_stop)) {
                int32_t n = bmds_read(a->music_decoder, a->music_buf, MUSIC_FRAMES * frame_bytes);
                if (n == 0 && a->music_loop) {
                        if (bmds_seek(a->music_decoder, 0, BMDS_SEEK_SET, NULL) < 0) break;
                        n = bmds_read(a->music_decoder, a->music_buf, MUSIC_FRAMES * frame_bytes);
                }
                if (n <= 0) {
                        /* Its end: what is queued plays out (unless stopped meanwhile). */
                        if (n == 0) (void)bmds_audio_control(a->music_out, BMDS_AUDIO_DRAIN, 0);
                        break;
                }
                const int32_t gain = LOAD(a->music_gain);
                const int32_t count = n / 2;
                if (gain < 32768)
                        for (int32_t i = 0; i < count; ++i)
                                a->music_buf[i] = (int16_t)(((int32_t)a->music_buf[i] * gain) >> 15);
                for (int32_t done = 0; done < n && !LOAD(a->music_stop);) {
                        const int32_t w =
                                bmds_write(a->music_out, (uint8_t *)a->music_buf + done, (uint32_t)(n - done));
                        if (w <= 0) break;
                        done += w;
                }
        }
        return NULL;
}

void bmds_game_music_stop(bmds_game_audio_t *audio) {
        if (audio != NULL && audio->music_midi != NULL) {
                bmds_game_midi_out_stop(audio->music_midi);
                audio->music_midi = NULL;
        }
        if (audio == NULL || !audio->music_running) return;
        STORE(audio->music_stop, 1);
        (void)bmds_audio_control(audio->music_out, BMDS_AUDIO_DROP, 0); /* a write waiting returns */
        pthread_join(audio->thread_music, NULL);
        bmds_close(audio->music_out);
        bmds_close(audio->music_decoder);
        STORE(audio->music_out, -1);
        audio->music_decoder = -1;
        audio->music_running = 0;
        return;
}

int bmds_game_music_play(bmds_game_audio_t *audio, const char *path, float volume, uint32_t flags) {
        if (audio == NULL || path == NULL) return -BMDS_EINVAL;
        bmds_game_music_stop(audio);
        if ((flags & BMDS_GAME_MUSIC_MIDI_OUT) != 0 && bmds_game_is_midi_file(path)) {
                bmds_game_music_volume(audio, volume);
                int err = 0;
                audio->music_midi = bmds_game_midi_out_start(path, (flags & BMDS_GAME_PLAY_LOOP) != 0,
                                                             LOAD(audio->music_gain), &err);
                return audio->music_midi != NULL ? 0 : err;
        }
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return fd;
        bmds_decode_config_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.format = BMDS_AUDIO_S16LE;
        const int32_t dec = bmds_audio_decode(fd, &d);
        bmds_close(fd); /* the decoder keeps its own */
        if (dec < 0) return dec;
        bmds_decode_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        int err = bmds_audio_decode_info(dec, &info);
        if (err == 0 && info.channels != 1 && info.channels != 2) err = -BMDS_ENOTSUP;
        int32_t out = -1;
        if (err == 0) {
                out = bmds_open("/dev/dsp", BMDS_O_WRITE);
                if (out < 0) err = out;
        }
        if (err == 0) {
                bmds_audio_config_t c;
                memset(&c, 0, sizeof(c));
                c.size = sizeof(c);
                c.format = BMDS_AUDIO_S16LE;
                c.channels = info.channels;
                c.rate = info.rate;
                c.ring_frames = info.rate * MUSIC_RING_MS / 1000;
                err = bmds_audio_config(out, &c, NULL);
        }
        if (err == 0) {
                audio->music_decoder = dec;
                audio->music_channels = info.channels;
                audio->music_loop = (flags & BMDS_GAME_PLAY_LOOP) != 0;
                audio->music_stop = 0;
                bmds_game_music_volume(audio, volume);
                STORE(audio->music_out, out);
                if (pthread_create(&audio->thread_music, NULL, music_entry, audio) != 0) err = -BMDS_ENOMEM;
        }
        if (err != 0) {
                if (out >= 0) bmds_close(out);
                bmds_close(dec);
                STORE(audio->music_out, -1);
                audio->music_decoder = -1;
                return err;
        }
        audio->music_running = 1;
        return 0;
}

void bmds_game_music_volume(bmds_game_audio_t *audio, float volume) {
        const float v = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
        STORE(audio->music_gain, (int32_t)(v * 32768.0f));
        if (audio->music_midi != NULL) bmds_game_midi_out_gain(audio->music_midi, LOAD(audio->music_gain));
        return;
}
