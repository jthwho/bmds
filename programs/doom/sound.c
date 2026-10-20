/**
 * @file      sound.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Doom's sound and music modules on libbmdsgame (devplan/games.md §19): a channel is a mixer
 * voice, a sound lump is loaded the first time it is played (kept up to a budget), and the music
 * is MIDI (MUS converted) played by the synth. Linked with doomgeneric (GPL-2) by dist/doom; this
 * file is ours.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>

#include "doom_bmds.h"
#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"
#include "w_wad.h"
#include "z_zone.h"

#define CHANNELS 16             /* Doom's snd_channels at most: the mixer's voices */
#define BUDGET_BYTES (4u << 20) /* sounds kept loaded, resampled to the device's rate */
#define MAX_LOADED 256          /* Doom II has 109 */
#define DMX_HEADER 8            /* format 3, rate, length */
#define DMX_PAD 16              /* each end of a DMX sound's samples */
#define WAV_HEADER 44

typedef struct {
                bmds_game_sound_t sound;
                uint32_t          bytes;
                uint32_t          used; /* the play it was last started by */
                sfxinfo_t        *sfx;
} loaded_t;

/* Settings i_sound.c keeps for its SDL module (i_sdlsound.c, not built): kept, unused. */
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static bmds_game_audio_t *g_audio;
static int                g_use_prefix;
static int                g_opened; /* tried: once */

static loaded_t *g_loaded[MAX_LOADED];
static uint32_t  g_loaded_count;
static uint32_t  g_loaded_bytes;
static uint32_t  g_plays;

static int32_t   g_voice[CHANNELS]; /* the channel's voice (kept after a stop: until it has faded) */
static loaded_t *g_playing[CHANNELS];

/* The game's sound device, opened by whichever module starts first. */
static bmds_game_audio_t *audio(void) {
        if (!g_opened) {
                g_opened = 1;
                const int err = bmds_game_audio_open(NULL, &g_audio);
                if (err != 0) {
                        fprintf(stderr, "doom: no sound (%d)\n", err);
                        g_audio = NULL;
                }
                for (int c = 0; c < CHANNELS; ++c) g_voice[c] = -1;
        }
        return g_audio;
}

/* ---- Effects */

static int is_playing(const loaded_t *l) {
        for (int c = 0; c < CHANNELS; ++c)
                if (g_playing[c] == l && g_voice[c] >= 0 && bmds_game_voice_is_playing(g_audio, g_voice[c])) return 1;
        return 0;
}

static void unload(uint32_t i) {
        loaded_t *l = g_loaded[i];
        for (int c = 0; c < CHANNELS; ++c)
                if (g_playing[c] == l) g_playing[c] = NULL;
        l->sfx->driver_data = NULL;
        g_loaded_bytes -= l->bytes;
        bmds_game_sound_free(&l->sound);
        free(l);
        g_loaded[i] = g_loaded[--g_loaded_count];
        return;
}

/* Room for @p bytes more: the least recently started sounds not playing freed first. */
static void make_room(uint32_t bytes) {
        while (g_loaded_count > 0 && (g_loaded_bytes + bytes > BUDGET_BYTES || g_loaded_count == MAX_LOADED)) {
                uint32_t oldest = UINT32_MAX;
                uint32_t which = 0;
                for (uint32_t i = 0; i < g_loaded_count; ++i)
                        if (g_loaded[i]->used < oldest && !is_playing(g_loaded[i])) {
                                oldest = g_loaded[i]->used;
                                which = i;
                        }
                if (oldest == UINT32_MAX) return; /* all playing: over the budget for now */
                unload(which);
        }
        return;
}

static void put16(uint8_t *p, uint32_t v) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        return;
}

static void put32(uint8_t *p, uint32_t v) {
        put16(p, v);
        put16(p + 2, v >> 16);
        return;
}

/* @p sfx's lump (DMX: 8-bit unsigned samples) loaded, as a WAV made in memory. */
static loaded_t *load(sfxinfo_t *sfx) {
        if (sfx->lumpnum < 0) return NULL;
        const uint8_t *lump = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
        const uint32_t size = (uint32_t)W_LumpLength(sfx->lumpnum);
        loaded_t      *l = NULL;
        uint8_t       *wav = NULL;
        if (size <= DMX_HEADER || lump[0] != 3 || lump[1] != 0) goto out;
        const uint32_t rate = (uint32_t)lump[2] | (uint32_t)lump[3] << 8;
        uint32_t       length =
                (uint32_t)lump[4] | (uint32_t)lump[5] << 8 | (uint32_t)lump[6] << 16 | (uint32_t)lump[7] << 24;
        if (length > size - DMX_HEADER) length = size - DMX_HEADER;
        const uint8_t *samples = lump + DMX_HEADER;
        /* The padding DMX plays isn't sound (Chocolate Doom skips it too). */
        if (length > 2 * DMX_PAD) {
                samples += DMX_PAD;
                length -= 2 * DMX_PAD;
        }
        if (rate == 0 || length == 0) goto out;

        wav = malloc(WAV_HEADER + length);
        l = calloc(1, sizeof(*l));
        if (wav == NULL || l == NULL) goto fail;
        memcpy(wav, "RIFF", 4);
        put32(wav + 4, 36 + length);
        memcpy(wav + 8, "WAVEfmt ", 8);
        put32(wav + 16, 16);
        put16(wav + 20, 1); /* PCM */
        put16(wav + 22, 1); /* mono */
        put32(wav + 24, rate);
        put32(wav + 28, rate);
        put16(wav + 32, 1);
        put16(wav + 34, 8);
        memcpy(wav + 36, "data", 4);
        put32(wav + 40, length);
        memcpy(wav + WAV_HEADER, samples, length);
        if (bmds_game_sound_load_memory(g_audio, wav, WAV_HEADER + length, &l->sound) != 0) goto fail;
        l->bytes = l->sound.frames * l->sound.channels * (uint32_t)sizeof(int16_t);
        l->sfx = sfx;
        make_room(l->bytes);
        if (g_loaded_count == MAX_LOADED) {
                bmds_game_sound_free(&l->sound);
                goto fail;
        }
        g_loaded[g_loaded_count++] = l;
        g_loaded_bytes += l->bytes;
        sfx->driver_data = l;
        goto out;
fail:
        free(l);
        l = NULL;
out:
        free(wav);
        W_ReleaseLumpNum(sfx->lumpnum);
        return l;
}

static boolean sound_init(boolean use_sfx_prefix) {
        g_use_prefix = use_sfx_prefix;
        return audio() != NULL;
}

static void sound_shutdown(void) {
        if (g_audio == NULL) return;
        for (int c = 0; c < CHANNELS; ++c)
                if (g_voice[c] >= 0) bmds_game_voice_stop(g_audio, g_voice[c]);
        bmds_game_audio_close(g_audio); /* every voice gone with it */
        g_audio = NULL;
        while (g_loaded_count > 0) unload(g_loaded_count - 1);
        return;
}

static int sound_lump(sfxinfo_t *sfx) {
        char name[16];
        if (sfx->link != NULL) sfx = sfx->link;
        snprintf(name, sizeof(name), g_use_prefix ? "ds%s" : "%s", sfx->name);
        return W_CheckNumForName(name);
}

static void sound_update(void) {
        return;
}

static float volume_of(int vol) {
        return vol <= 0 ? 0.0f : vol >= 127 ? 1.0f : (float)vol / 127.0f;
}

/* Doom's separation: 0 left, 128 the middle, 254 right. */
static float pan_of(int sep) {
        const float pan = ((float)sep - 127.0f) / 127.0f;
        return pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan;
}

static void sound_params(int channel, int vol, int sep) {
        if (g_audio == NULL || channel < 0 || channel >= CHANNELS || g_voice[channel] < 0) return;
        bmds_game_voice_set(g_audio, g_voice[channel], volume_of(vol), pan_of(sep));
        return;
}

static void sound_stop(int channel) {
        if (g_audio == NULL || channel < 0 || channel >= CHANNELS || g_voice[channel] < 0) return;
        bmds_game_voice_stop(g_audio, g_voice[channel]);
        return;
}

static int sound_start(sfxinfo_t *sfx, int channel, int vol, int sep) {
        if (g_audio == NULL || channel < 0 || channel >= CHANNELS) return -1;
        sound_stop(channel);
        loaded_t *l = sfx->driver_data != NULL ? sfx->driver_data : load(sfx);
        if (l == NULL) return -1;
        l->used = ++g_plays;
        const int32_t voice = bmds_game_play(g_audio, &l->sound, volume_of(vol), pan_of(sep), 0);
        if (voice < 0) return -1;
        g_voice[channel] = voice;
        g_playing[channel] = l;
        return channel;
}

static boolean sound_is_playing(int channel) {
        if (g_audio == NULL || channel < 0 || channel >= CHANNELS || g_voice[channel] < 0) return false;
        return bmds_game_voice_is_playing(g_audio, g_voice[channel]) ? true : false;
}

static snddevice_t sound_devices[] = {SNDDEVICE_SB,          SNDDEVICE_PAS,         SNDDEVICE_GUS,
                                      SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
        sound_devices, sizeof(sound_devices) / sizeof(sound_devices[0]),
        sound_init,    sound_shutdown,
        sound_lump,    sound_update,
        sound_params,  sound_start,
        sound_stop,    sound_is_playing,
        NULL, /* loaded when first played */
};

/* ---- Music */

typedef struct {
                void  *midi;
                size_t length;
} song_t;

static char g_music_path[64];
static int  g_music_volume = 127;
static int  g_music_paused;
static int  g_music_playing;

static void music_remove(void) {
        if (g_music_path[0] != '\0') unlink(g_music_path);
        return;
}

static uint32_t g_music_flags = BMDS_GAME_MUSIC_MIDI_OUT; /* doom/music: midi, else built-in (0) */

/* doom/music (devplan/audio.md §25): midi sends the music through /dev/midi, the system's synth
   or wherever system is routed; built-in plays it on a synth of Doom's own. */
static void read_music_setting(void) {
        bmds_sysconf_param_t p;
        memset(&p, 0, sizeof(p));
        p.size = sizeof(p);
        p.type = BMDS_SYSCONF_TYPE_ENUM;
        p.key = "doom/music";
        p.default_value = "midi";
        p.help = "Doom's music: midi (through /dev/midi: the system's synth, or wherever system goes), or built-in";
        p.choices = "midi|built-in";
        const int32_t id = bmds_sysconf_declare(&p);
        char          value[16] = "";
        if (id >= 0 && bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) >= 0 &&
            strcmp(value, "built-in") == 0)
                g_music_flags = 0;
        return;
}

static boolean music_init(void) {
        if (audio() == NULL) return false;
        if (g_music_path[0] != '\0') return true; /* early, before Doom asked */
        read_music_setting();
        const char *tmp = getenv("TMPDIR");
        snprintf(g_music_path, sizeof(g_music_path), "%s/doom-%d.mid", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp",
                 (int)getpid());
        atexit(music_remove);
        return true;
}

static void music_shutdown(void) {
        if (g_audio != NULL) bmds_game_music_stop(g_audio);
        g_music_playing = 0;
        music_remove();
        return;
}

static void music_apply_volume(void) {
        if (g_audio != NULL) bmds_game_music_volume(g_audio, g_music_paused ? 0.0f : volume_of(g_music_volume));
        return;
}

static void music_set_volume(int volume) {
        g_music_volume = volume;
        music_apply_volume();
        return;
}

/* Paused by muting, as Chocolate Doom's: the synth goes on. */
static void music_pause(void) {
        g_music_paused = 1;
        music_apply_volume();
        return;
}

static void music_resume(void) {
        g_music_paused = 0;
        music_apply_volume();
        return;
}

/* A song: MIDI as it is, MUS (id's) converted to MIDI. */
static void *music_register(void *data, int length) {
        song_t *s = calloc(1, sizeof(*s));
        if (s == NULL || length < 4) goto fail;
        if (memcmp(data, "MThd", 4) == 0) {
                s->midi = malloc((size_t)length);
                if (s->midi == NULL) goto fail;
                memcpy(s->midi, data, (size_t)length);
                s->length = (size_t)length;
                return s;
        }
        MEMFILE *in = mem_fopen_read(data, (size_t)length);
        MEMFILE *out = mem_fopen_write();
        if (in != NULL && out != NULL && !mus2mid(in, out)) { /* false: converted */
                void  *buf = NULL;
                size_t n = 0;
                mem_get_buf(out, &buf, &n);
                s->midi = malloc(n);
                if (s->midi != NULL) {
                        memcpy(s->midi, buf, n);
                        s->length = n;
                }
        }
        if (in != NULL) mem_fclose(in);
        if (out != NULL) mem_fclose(out);
        if (s->midi != NULL) return s;
fail:
        free(s);
        return NULL;
}

static void music_unregister(void *handle) {
        song_t *s = handle;
        if (s == NULL) return;
        free(s->midi);
        free(s);
        return;
}

static void music_stop(void) {
        if (g_audio != NULL) bmds_game_music_stop(g_audio);
        g_music_playing = 0;
        return;
}

/* @p length bytes of MIDI played through a file (the synth's decoder reads it whole as it opens,
   so it goes at once). */
static int play_midi(const void *midi, size_t length, float volume, uint32_t flags) {
        FILE *f = fopen(g_music_path, "wb");
        if (f == NULL) return -BMDS_EIO;
        const size_t written = fwrite(midi, 1, length, f);
        if (fclose(f) != 0 || written != length) {
                unlink(g_music_path);
                return -BMDS_EIO;
        }
        const int err = bmds_game_music_play(g_audio, g_music_path, volume, flags);
        unlink(g_music_path);
        return err;
}

static void music_play(void *handle, boolean looping) {
        const song_t *s = handle;
        if (g_audio == NULL || s == NULL) return;
        music_stop();
        const int err = play_midi(s->midi, s->length, g_music_paused ? 0.0f : volume_of(g_music_volume),
                                  (looping ? BMDS_GAME_PLAY_LOOP : 0) | g_music_flags);
        if (err != 0)
                fprintf(stderr, "doom: music didn't play (%d)\n", err);
        else
                g_music_playing = 1;
        return;
}

void doom_bmds_sound_early(void) {
        /* A MIDI file of one event (a channel's volume: the decoder refuses a song of none). */
        static const uint8_t empty[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0,    0, 0, 1, 0,    96,   'M',
                                        'T', 'r', 'k', 0,   0, 0, 8, 0, 0xb0, 7, 0, 0, 0xff, 0x2f, 0};
        if (!music_init()) return;
        if (g_music_flags != 0) return; /* through /dev/midi: the system's synth keeps its font loaded */
        const int err = play_midi(empty, sizeof(empty), 0.0f, 0);
        if (err != 0) fprintf(stderr, "doom: the synth's font didn't load (%d)\n", err);
        music_stop(); /* the font stays loaded (synth/keep-fonts) */
        return;
}

static boolean music_is_playing(void) {
        return g_music_playing ? true : false;
}

static snddevice_t music_devices[] = {SNDDEVICE_PAS,         SNDDEVICE_GUS,     SNDDEVICE_WAVEBLASTER,
                                      SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32};

music_module_t DG_music_module = {
        music_devices,
        sizeof(music_devices) / sizeof(music_devices[0]),
        music_init,
        music_shutdown,
        music_set_volume,
        music_pause,
        music_resume,
        music_register,
        music_unregister,
        music_play,
        music_stop,
        music_is_playing,
        NULL, /* the music plays on its own thread */
};
