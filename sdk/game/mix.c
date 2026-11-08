/**
 * @file      mix.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's effects mixer (devplan/games.md §16): the game's thread queues commands, the
 * audio thread applies them at a period's start and mixes its voices, each gain moving to its
 * target over the period (no clicks). Voices are numbered by slot and generation, so the game's
 * thread hands them out without waiting; the mixer says which ended. No system calls.
 */

#include <string.h>
#include "game_internal.h"

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define SLOT_BITS 4 /* BMDS_GAME_VOICES */
#define GENERATIONS 0x7ffffffu

void bmds_game_mixer_init(bmds_game_mixer_t *mixer) {
        memset(mixer, 0, sizeof(*mixer));
        mixer->volume = 1.0f;
        return;
}

int bmds_game_mixer_push(bmds_game_mixer_t *mixer, const bmds_game_command_t *command) {
        const uint32_t head = mixer->head;
        if (head - LOAD(mixer->tail) >= BMDS_GAME_COMMANDS) return -1;
        mixer->commands[head % BMDS_GAME_COMMANDS] = *command;
        STORE(mixer->head, head + 1);
        return 0;
}

static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
}

int32_t bmds_game_mixer_play(bmds_game_mixer_t *mixer, const bmds_game_sound_t *sound, float volume, float pan,
                             uint32_t flags) {
        return bmds_game_mixer_play_at(mixer, sound, volume, pan, flags, 0);
}

int32_t bmds_game_mixer_play_at(bmds_game_mixer_t *mixer, const bmds_game_sound_t *sound, float volume, float pan,
                                uint32_t flags, int64_t at_ns) {
        if (sound == NULL || sound->samples == NULL || sound->frames == 0 ||
            (sound->channels != 1 && sound->channels != 2))
                return -BMDS_EINVAL;
        /* A free slot (its last voice ended), else the oldest that doesn't loop. */
        int32_t slot = -1;
        for (uint32_t i = 0; i < BMDS_GAME_VOICES && slot < 0; ++i)
                if (LOAD(mixer->ended[i]) == mixer->given[i]) slot = (int32_t)i;
        for (uint32_t i = 0; i < BMDS_GAME_VOICES && slot < 0; ++i) {
                if (mixer->looping[i]) continue;
                slot = (int32_t)i;
                for (uint32_t j = i + 1; j < BMDS_GAME_VOICES; ++j)
                        if (!mixer->looping[j] &&
                            mixer->plays - mixer->started[j] > mixer->plays - mixer->started[slot])
                                slot = (int32_t)j;
        }
        if (slot < 0) return -BMDS_EBUSY;
        const uint32_t      generation = mixer->given[slot] % GENERATIONS + 1;
        bmds_game_command_t c = {BMDS_GAME_COMMAND_PLAY,
                                 (uint32_t)slot,
                                 generation,
                                 sound->samples,
                                 sound->frames,
                                 sound->channels,
                                 clamp(volume, 0.0f, 1.0f),
                                 clamp(pan, -1.0f, 1.0f),
                                 flags,
                                 at_ns};
        if (bmds_game_mixer_push(mixer, &c) != 0) return -BMDS_EAGAIN;
        mixer->given[slot] = generation;
        mixer->started[slot] = mixer->plays++;
        mixer->looping[slot] = (flags & BMDS_GAME_PLAY_LOOP) != 0;
        return (int32_t)(generation << SLOT_BITS | (uint32_t)slot);
}

int bmds_game_mixer_idle(const bmds_game_mixer_t *mixer) {
        if (LOAD(mixer->head) != mixer->tail) return 0;
        for (uint32_t i = 0; i < BMDS_GAME_VOICES; ++i)
                if (mixer->voices[i].generation != 0) return 0;
        return 1;
}

int32_t bmds_game_mixer_voice(const bmds_game_mixer_t *mixer, int32_t voice, uint32_t *generation) {
        if (voice < 0) return -1;
        const uint32_t slot = (uint32_t)voice & (BMDS_GAME_VOICES - 1), g = (uint32_t)voice >> SLOT_BITS;
        if (g == 0 || mixer->given[slot] != g || LOAD(mixer->ended[slot]) == g) return -1;
        *generation = g;
        return (int32_t)slot;
}

/* The gains a voice moves to: its volume and pan, the mixer's volume, nothing while stopping. */
static void targets(const bmds_game_mixer_t *m, const bmds_game_voice_t *v, float *left, float *right) {
        const float g = v->stopping ? 0.0f : v->volume * m->volume;
        *left = g * (v->pan > 0.0f ? 1.0f - v->pan : 1.0f);
        *right = g * (v->pan < 0.0f ? 1.0f + v->pan : 1.0f);
        return;
}

static void end_voice(bmds_game_mixer_t *m, uint32_t slot) {
        bmds_game_voice_t *v = &m->voices[slot];
        if (v->generation != 0) STORE(m->ended[slot], v->generation);
        v->generation = 0;
        return;
}

static void apply(bmds_game_mixer_t *m, const bmds_game_command_t *c) {
        bmds_game_voice_t *v = c->slot < BMDS_GAME_VOICES ? &m->voices[c->slot] : NULL;
        switch (c->type) {
                case BMDS_GAME_COMMAND_PLAY:
                        end_voice(m, c->slot); /* taken: the one there ends */
                        v->samples = c->samples;
                        v->frames = c->frames;
                        v->channels = c->channels;
                        v->position = 0;
                        v->generation = c->generation;
                        v->volume = c->volume;
                        v->pan = c->pan;
                        v->loop = (c->flags & BMDS_GAME_PLAY_LOOP) != 0;
                        v->stopping = 0;
                        v->delay = 0;
                        if (c->at_ns != 0 && m->heard_ns != 0 && c->at_ns > m->heard_ns) {
                                const int64_t d = (c->at_ns - m->heard_ns) * (int64_t)m->rate / 1000000000;
                                v->delay = d > 0x7fffffff ? 0x7fffffffu : (uint32_t)d;
                        }
                        targets(m, v, &v->left, &v->right); /* a sound's start is its own */
                        break;
                case BMDS_GAME_COMMAND_SET:
                        if (v->generation != c->generation) break;
                        v->volume = c->volume;
                        v->pan = c->pan;
                        break;
                case BMDS_GAME_COMMAND_STOP:
                        if (v->generation == c->generation) v->stopping = 1;
                        break;
                case BMDS_GAME_COMMAND_VOLUME: m->volume = c->volume; break;
                case BMDS_GAME_COMMAND_PAUSE: m->paused = c->pan != 0.0f; break;
                default: break;
        }
        return;
}

void bmds_game_mixer_run(bmds_game_mixer_t *mixer, float *out, uint32_t frames, uint32_t channels) {
        bmds_game_mixer_run_at(mixer, out, frames, channels, 0, 0);
        return;
}

void bmds_game_mixer_run_at(bmds_game_mixer_t *mixer, float *out, uint32_t frames, uint32_t channels, int64_t heard_ns,
                            uint32_t rate) {
        mixer->heard_ns = rate != 0 ? heard_ns : 0;
        mixer->rate = rate;
        const uint32_t head = LOAD(mixer->head);
        for (uint32_t t = mixer->tail; t != head; ++t) apply(mixer, &mixer->commands[t % BMDS_GAME_COMMANDS]);
        STORE(mixer->tail, head);
        memset(out, 0, sizeof(float) * frames * channels);
        if (mixer->paused || frames == 0) return;
        const float scale = 1.0f / 32768.0f;
        for (uint32_t s = 0; s < BMDS_GAME_VOICES; ++s) {
                bmds_game_voice_t *v = &mixer->voices[s];
                if (v->generation == 0) continue;
                /* Not started yet: waits; stopped meanwhile, it ends unheard. */
                if (v->delay != 0 && v->stopping) {
                        end_voice(mixer, s);
                        continue;
                }
                if (v->delay >= frames) {
                        v->delay -= frames;
                        continue;
                }
                const uint32_t first = v->delay;
                v->delay = 0;
                float to_left, to_right;
                targets(mixer, v, &to_left, &to_right);
                const float step = 1.0f / (float)(frames - first);
                const float dl = (to_left - v->left) * step, dr = (to_right - v->right) * step;
                float       l = v->left, r = v->right;
                uint32_t    p = v->position;
                for (uint32_t i = first; i < frames; ++i) {
                        if (p >= v->frames) {
                                if (!v->loop) break;
                                p = 0;
                        }
                        l += dl;
                        r += dr;
                        const int16_t *x = v->samples + (size_t)p * v->channels;
                        const float    a = (float)x[0] * scale, b = v->channels == 2 ? (float)x[1] * scale : a;
                        if (channels == 2) {
                                out[2 * i] += a * l;
                                out[2 * i + 1] += b * r;
                        } else {
                                out[i] += 0.5f * (a * l + b * r);
                        }
                        ++p;
                }
                v->position = p;
                v->left = to_left;
                v->right = to_right;
                if ((p >= v->frames && !v->loop) || v->stopping) end_voice(mixer, s); /* stopping: faded out */
        }
        return;
}

uint32_t bmds_game_resampled(uint32_t frames, uint32_t from, uint32_t to) {
        if (from == to || from == 0) return frames;
        return (uint32_t)(((uint64_t)frames * to + from - 1) / from);
}

void bmds_game_resample(const int16_t *in, uint32_t frames, uint32_t channels, uint32_t from, uint32_t to,
                        int16_t *out) {
        const uint32_t n = bmds_game_resampled(frames, from, to);
        if (from == to || from == 0) {
                memcpy(out, in, sizeof(int16_t) * frames * channels);
                return;
        }
        /* Each output frame's place in the input, in 1/65536ths. */
        const uint64_t step = ((uint64_t)from << 16) / to;
        for (uint32_t i = 0; i < n; ++i) {
                const uint64_t at = step * i;
                const uint32_t k = (uint32_t)(at >> 16), f = (uint32_t)(at & 0xffffu);
                const uint32_t k1 = k + 1 < frames ? k + 1 : frames - 1;
                for (uint32_t c = 0; c < channels; ++c) {
                        const int32_t a = in[(size_t)(k < frames ? k : frames - 1) * channels + c];
                        const int32_t b = in[(size_t)k1 * channels + c];
                        out[(size_t)i * channels + c] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * f) >> 16));
                }
        }
        return;
}
