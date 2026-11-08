/**
 * @file      voice.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1's speech (voice.h). The speaker's thread takes the lines asked for, the oldest
 * asked first, and says each into a sound; a line asked for again meanwhile is said again. A sound a
 * voice may still be reading is kept until the mixer has let it go. Each line played ducks the
 * music (bmds_seq_fade()), and a line queued waits for the one before it.
 */

#include "voice.h"
#include <pthread.h>
#include <string.h>

#define TEXT_MAX 400
#define DUCK 0.5f              /* the music under a line: -6 dB */
#define DUCK_IN_NS 120000000   /* down over this, before the line */
#define DUCK_HOLD_NS 300000000 /* after it, in case another follows */
#define DUCK_OUT_NS 600000000  /* back up over this */
#define RETIRED 8
#define SPEAKERS 3 /* voices kept open: the BIOS's and the duo's (the system has four) */
#define MISSING 4  /* voices found not installed, not asked for again */

typedef struct {
                int               state;
                uint32_t          generation; /* bumped by each voice_say() */
                uint32_t          order;      /* when asked: the oldest is made first */
                char              text[TEXT_MAX];
                bmds_game_speak_t how;
                int               has_how;
                bmds_game_sound_t sound;
                int32_t           playing; /* its last voice, or -1 */
} slot_t;

static struct {
                bmds_game_audio_t *audio;
                pthread_t          thread;
                pthread_mutex_t    lock;
                pthread_cond_t     wake;
                int                quit, started;
                int32_t            core;  /* the game's (voice_open()'s caller), or -1 */
                uint32_t           asked; /* voice_say()s in all */
                slot_t             slots[VOICE_SLOTS];
                /* Sounds dropped while a voice may still read them. */
                bmds_game_sound_t retired[RETIRED];
                int32_t           retired_voice[RETIRED];
                /* The speaker thread's: voices kept open, the oldest replaced. */
                struct {
                                bmds_game_speaker_t *speaker;
                                bmds_game_speak_t    how;
                                int                  has_how;
                                uint32_t             used;
                } speakers[SPEAKERS];
                uint32_t    uses;
                const char *missing[MISSING];
                /* The game's thread's. */
                bmds_seq_t *duck;     /* the music ducked under the lines (NULL: none) */
                int64_t     said_end; /* when the last line played ends, heard */
} v;

/* @p in as it's to be said, into @p out: the system's name as it's said ("Bum Doss"). */
static void respell(const char *in, char *out, size_t size) {
        static const char *const names[] = {"B M D S", "BMDS"};
        size_t                   o = 0;
        while (*in != '\0' && o + 1 < size) {
                size_t n = 0;
                for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && n == 0; ++i)
                        if (strncmp(in, names[i], strlen(names[i])) == 0) n = strlen(names[i]);
                if (n == 0) {
                        out[o++] = *in++;
                        continue;
                }
                const char *say = "Bum Doss";
                while (*say != '\0' && o + 1 < size) out[o++] = *say++;
                in += n;
        }
        out[o] = '\0';
        return;
}

static int same(const bmds_game_speak_t *a, const bmds_game_speak_t *b) {
        const int voices =
                a->voice == b->voice || (a->voice != NULL && b->voice != NULL && strcmp(a->voice, b->voice) == 0);
        return voices && a->speed == b->speed && a->pitch == b->pitch && a->volume == b->volume && a->flags == b->flags;
}

/* A speaker saying as @p how says (NULL: the system's way), kept open for the next line like it. */
static bmds_game_speaker_t *speaker_for(const bmds_game_speak_t *how, int *err) {
        int oldest = 0;
        for (int i = 0; i < SPEAKERS; ++i) {
                if (v.speakers[i].speaker != NULL && v.speakers[i].has_how == (how != NULL) &&
                    (how == NULL || same(&v.speakers[i].how, how))) {
                        v.speakers[i].used = ++v.uses;
                        return v.speakers[i].speaker;
                }
                if (v.speakers[i].used < v.speakers[oldest].used) oldest = i;
        }
        /* Opened first: one that can't be (a voice not installed) keeps the others. */
        bmds_game_speaker_t *made = NULL;
        *err = bmds_game_speaker_open(how, &made);
        if (*err != 0) return NULL;
        bmds_game_speaker_close(v.speakers[oldest].speaker);
        v.speakers[oldest].speaker = made;
        v.speakers[oldest].has_how = how != NULL;
        if (how != NULL) v.speakers[oldest].how = *how;
        v.speakers[oldest].used = ++v.uses;
        return v.speakers[oldest].speaker;
}

/* Frees what the mixer has let go of. The game's thread. */
static void reap(void) {
        for (int i = 0; i < RETIRED; ++i)
                if (v.retired[i].samples != NULL && !bmds_game_voice_is_playing(v.audio, v.retired_voice[i]))
                        bmds_game_sound_free(&v.retired[i]);
        return;
}

/* Drops @p sound, voice @p voice perhaps reading it. The game's thread. */
static void retire(bmds_game_sound_t *sound, int32_t voice) {
        if (sound->samples == NULL) return;
        if (voice < 0 || !bmds_game_voice_is_playing(v.audio, voice)) {
                bmds_game_sound_free(sound);
                return;
        }
        bmds_game_voice_stop(v.audio, voice);
        reap();
        for (int i = 0; i < RETIRED; ++i)
                if (v.retired[i].samples == NULL) {
                        v.retired[i] = *sound;
                        v.retired_voice[i] = voice;
                        memset(sound, 0, sizeof(*sound));
                        return;
                }
        /* No room: it is stopped and fades within a period; the oldest waits that out. */
        bmds_game_sound_free(sound);
        return;
}

static void *speaker(void *arg) {
        (void)arg;
        /* Background work: only on the time the drawing leaves (SCHED_IDLE, below normal threads). */
        bmds_sched_t idle;
        memset(&idle, 0, sizeof(idle));
        idle.size = sizeof(idle);
        idle.policy = BMDS_SCHED_IDLE;
        (void)bmds_sched(BMDS_SCHED_SET, 0, &idle);
        /* On the game's core (voice_open()'s): the drawing waits there for each frame, while the
           other holds the presenter and a synth (no idle at all beside the wavetable, 1-7 % beside
           FM; this thread got 10 % of it and fell behind for good). It can't move once going: its
           first floating point pins it. */
        if (v.core >= 0) {
                bmds_sched_t pin;
                memset(&pin, 0, sizeof(pin));
                pin.size = sizeof(pin);
                pin.cores = 1u << v.core;
                (void)bmds_sched(BMDS_SCHED_PIN, 0, &pin);
        }
        pthread_mutex_lock(&v.lock);
        while (!v.quit) {
                int slot = -1;
                for (int i = 0; i < VOICE_SLOTS; ++i)
                        if (v.slots[i].state == VOICE_QUEUED &&
                            (slot < 0 || (int32_t)(v.slots[i].order - v.slots[slot].order) < 0))
                                slot = i;
                if (slot < 0) {
                        pthread_cond_wait(&v.wake, &v.lock);
                        continue;
                }
                slot_t           *s = &v.slots[slot];
                const uint32_t    generation = s->generation;
                char              text[TEXT_MAX];
                bmds_game_speak_t how = s->how;
                const int         has_how = s->has_how;
                memcpy(text, s->text, sizeof(text));
                pthread_mutex_unlock(&v.lock);
                bmds_game_sound_t sound;
                int               err = 0;
                for (int i = 0; i < MISSING && has_how && how.voice != NULL; ++i)
                        if (v.missing[i] != NULL && strcmp(v.missing[i], how.voice) == 0) how.voice = NULL;
                bmds_game_speaker_t *speaker = speaker_for(has_how ? &how : NULL, &err);
                if (err == -BMDS_ENOENT && has_how && how.voice != NULL) {
                        /* That voice isn't installed (speech-voices): the system's, from now on. */
                        for (int i = 0; i < MISSING; ++i)
                                if (v.missing[i] == NULL) {
                                        v.missing[i] = how.voice; /* the show's strings: they last */
                                        break;
                                }
                        how.voice = NULL;
                        err = 0;
                        speaker = speaker_for(&how, &err);
                }
                memset(&sound, 0, sizeof(sound));
                if (speaker != NULL) err = bmds_game_speaker_say(speaker, v.audio, text, &sound);
                pthread_mutex_lock(&v.lock);
                if (s->generation != generation) {
                        bmds_game_sound_free(&sound); /* asked for another meanwhile */
                        continue;
                }
                {
                        int64_t dn = 0;
                        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &dn);
                }
                if (err == 0) {
                        s->sound = sound;
                        s->state = VOICE_READY;
                } else {
                        s->state = VOICE_FAILED;
                }
        }
        pthread_mutex_unlock(&v.lock);
        return NULL;
}

int voice_open(uint32_t latency_ms) {
        if (v.audio != NULL) return 0;
        bmds_game_audio_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.flags = BMDS_GAME_AUDIO_AHEAD;
        c.latency_ms = latency_ms;
        const int err = bmds_game_audio_open(&c, &v.audio);
        if (err < 0) return err;
        pthread_mutex_init(&v.lock, NULL);
        pthread_cond_init(&v.wake, NULL);
        for (int i = 0; i < VOICE_SLOTS; ++i) v.slots[i].playing = -1;
        v.quit = 0;
        bmds_sched_t here;
        memset(&here, 0, sizeof(here));
        here.size = sizeof(here);
        v.core = bmds_sched(BMDS_SCHED_GET, 0, &here) == 0 && here.core >= 0 && here.core <= 1 ? here.core : -1;
        v.started = pthread_create(&v.thread, NULL, speaker, NULL) == 0;
        return 0;
}

void voice_close(void) {
        if (v.audio == NULL) return;
        if (v.started) {
                pthread_mutex_lock(&v.lock);
                v.quit = 1;
                pthread_cond_signal(&v.wake);
                pthread_mutex_unlock(&v.lock);
                pthread_join(v.thread, NULL); /* a line being said finishes first */
        }
        for (int i = 0; i < SPEAKERS; ++i) bmds_game_speaker_close(v.speakers[i].speaker);
        bmds_game_audio_close(v.audio); /* every voice stopped: the sounds are free to go */
        v.audio = NULL;
        for (int i = 0; i < VOICE_SLOTS; ++i) bmds_game_sound_free(&v.slots[i].sound);
        for (int i = 0; i < RETIRED; ++i) bmds_game_sound_free(&v.retired[i]);
        pthread_cond_destroy(&v.wake);
        pthread_mutex_destroy(&v.lock);
        memset(&v, 0, sizeof(v));
        return;
}

bmds_game_audio_t *voice_audio(void) {
        return v.audio;
}

void voice_say(int slot, const char *text, const bmds_game_speak_t *how) {
        if (v.audio == NULL || !v.started || slot < 0 || slot >= VOICE_SLOTS || text == NULL) return;
        pthread_mutex_lock(&v.lock);
        slot_t *s = &v.slots[slot];
        retire(&s->sound, s->playing);
        s->playing = -1;
        ++s->generation;
        s->order = ++v.asked;
        respell(text, s->text, TEXT_MAX);
        s->has_how = how != NULL;
        if (how != NULL) s->how = *how;
        s->state = VOICE_QUEUED;
        pthread_cond_signal(&v.wake);
        pthread_mutex_unlock(&v.lock);
        return;
}

void voice_drop(int slot) {
        if (v.audio == NULL || slot < 0 || slot >= VOICE_SLOTS) return;
        pthread_mutex_lock(&v.lock);
        slot_t *s = &v.slots[slot];
        if (s->state == VOICE_QUEUED) {
                ++s->generation; /* the speaker drops what it was making */
                s->state = VOICE_EMPTY;
        } else if (s->playing < 0 || !bmds_game_voice_is_playing(v.audio, s->playing)) {
                bmds_game_sound_free(&s->sound);
                s->playing = -1;
                s->state = VOICE_EMPTY;
        }
        pthread_mutex_unlock(&v.lock);
        return;
}

int voice_state(int slot) {
        if (v.audio == NULL || slot < 0 || slot >= VOICE_SLOTS) return VOICE_FAILED;
        pthread_mutex_lock(&v.lock);
        const int state = v.slots[slot].state;
        pthread_mutex_unlock(&v.lock);
        return state;
}

float voice_seconds(int slot) {
        if (voice_state(slot) != VOICE_READY) return 0.0f;
        return (float)v.slots[slot].sound.frames / (float)bmds_game_audio_rate(v.audio);
}

/* Now, as the mixer would play a sound asked for now: heard its latency on. */
static int64_t heard_now(void) {
        int64_t now = 0;
        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
        return now + bmds_game_audio_latency_ns(v.audio);
}

int64_t voice_said_end(void) {
        return v.said_end;
}

void voice_duck(bmds_seq_t *seq) {
        v.duck = seq;
        return;
}

int32_t voice_play(int slot, int64_t at_ns, float volume, float pan) {
        if (voice_state(slot) != VOICE_READY) return -BMDS_EAGAIN;
        reap();
        slot_t       *s = &v.slots[slot]; /* ready: the speaker leaves it alone until voice_say() */
        const int32_t voice = bmds_game_play_at(v.audio, &s->sound, volume, pan, 0, at_ns);
        if (voice < 0) return voice;
        s->playing = voice;
        const int64_t at = at_ns != 0 ? at_ns : heard_now();
        const int64_t end = at + (int64_t)s->sound.frames * 1000000000 / bmds_game_audio_rate(v.audio);
        if (end > v.said_end) v.said_end = end;
        /* The music down under it, and up after (a line soon after keeps it down: its fade replaces
           this one's way back up). */
        if (v.duck != NULL) {
                (void)bmds_seq_fade(v.duck, DUCK, at - DUCK_IN_NS, DUCK_IN_NS);
                (void)bmds_seq_fade(v.duck, 1.0f, end + DUCK_HOLD_NS, DUCK_OUT_NS);
        }
        return voice;
}

float voice_queue(int slot, float gap, float volume, float pan) {
        if (voice_state(slot) != VOICE_READY) return -1.0f;
        const int64_t now = heard_now();
        int64_t       at = v.said_end + (int64_t)(gap * 1e9f);
        if (at < now) at = now;
        if (voice_play(slot, at, volume, pan) < 0) return -1.0f;
        return (float)(v.said_end - now) / 1e9f;
}

int32_t voice_sound(const bmds_game_sound_t *sound, int64_t at_ns, float volume, float pan) {
        if (v.audio == NULL) return -BMDS_ENODEV;
        return bmds_game_play_at(v.audio, sound, volume, pan, 0, at_ns);
}
