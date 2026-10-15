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
 * so it runs as fast as the music plays.
 */

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include "game_internal.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define DEFAULT_PERIOD 256
#define MAX_SOUND_SECONDS 600
#define MUSIC_FRAMES 1024 /* a music write */
#define MUSIC_RING_MS 250 /* its stream's queue */

struct bmds_game_audio_t {
                bmds_game_mixer_t mixer;
                int32_t           fd;
                uint32_t          rate;
                uint32_t          channels;
                pthread_t         thread;
                int               quit;
                int32_t           error; /* the mixer thread's, if it stopped */
                /* The music. */
                pthread_t thread_music;
                int       music_running;
                int       music_stop;
                int32_t   music_decoder;
                int32_t   music_out; /* its stream, or -1 */
                uint32_t  music_channels;
                uint32_t  music_loop;
                int32_t   music_gain; /* 0-32768 */
                int16_t   music_buf[MUSIC_FRAMES * 2];
};

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
                bmds_game_mixer_run(&a->mixer, c.out, c.frames, c.channels);
        }
        return NULL;
}

int bmds_game_audio_open(const bmds_game_audio_config_t *config, bmds_game_audio_t **audio) {
        if (audio == NULL || (config != NULL && config->size < sizeof(bmds_game_audio_config_t))) return -BMDS_EINVAL;
        *audio = NULL;
        bmds_game_audio_t *a = bmds_mem_alloc(sizeof(*a), BMDS_MEM_ALIGN, 0);
        if (a == NULL) return -BMDS_ENOMEM;
        memset(a, 0, sizeof(*a));
        bmds_game_mixer_init(&a->mixer);
        a->music_out = -1;
        a->music_decoder = -1;
        a->fd = bmds_open("/dev/dsp", BMDS_O_WRITE);
        if (a->fd < 0) {
                const int err = a->fd;
                bmds_mem_free(a);
                return err;
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
                if (pthread_create(&a->thread, NULL, mixer_thread, a) != 0) err = -BMDS_ENOMEM;
        }
        if (err != 0) {
                bmds_close(a->fd);
                bmds_mem_free(a);
                return err;
        }
        *audio = a;
        return 0;
}

void bmds_game_audio_close(bmds_game_audio_t *audio) {
        if (audio == NULL) return;
        bmds_game_music_stop(audio);
        STORE(audio->quit, 1);
        pthread_join(audio->thread, NULL);
        bmds_close(audio->fd);
        bmds_mem_free(audio);
        return;
}

uint32_t bmds_game_audio_rate(const bmds_game_audio_t *audio) {
        return audio->rate;
}

void bmds_game_audio_pause(bmds_game_audio_t *audio, int paused) {
        const bmds_game_command_t c = {BMDS_GAME_COMMAND_PAUSE, 0, 0, NULL, 0, 0, 0.0f, paused ? 1.0f : 0.0f, 0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
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
                                       0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
        return;
}

int32_t bmds_game_play(bmds_game_audio_t *audio, const bmds_game_sound_t *sound, float volume, float pan,
                       uint32_t flags) {
        return bmds_game_mixer_play(&audio->mixer, sound, volume, pan, flags);
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
                                       0};
        (void)bmds_game_mixer_push(&audio->mixer, &c);
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
                if (pthread_create(&audio->thread_music, NULL, music_thread, audio) != 0) err = -BMDS_ENOMEM;
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
        return;
}
