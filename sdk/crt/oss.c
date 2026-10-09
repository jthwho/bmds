/**
 * @file      oss.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * OSS's ioctls (`<sys/soundcard.h>`) on the native audio calls
 * (devplan/audio.md §18): `linux.c`'s `ioctl()` passes the 'P' and 'M'
 * requests here. Nothing of OSS reaches the kernel. Uses only the system
 * call table, so the host tests run it against a fake one.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/soundcard.h>
#include <bmds/sys.h>
#include "oss.h"

/* The formats OSS names (Linux's numbers): AFMT_ against BMDS_AUDIO_. */
static const struct {
                int      afmt;
                uint32_t format;
} formats[] = {
        {AFMT_MU_LAW, BMDS_AUDIO_MULAW}, {AFMT_A_LAW, BMDS_AUDIO_ALAW},   {AFMT_U8, BMDS_AUDIO_U8},
        {AFMT_S16_LE, BMDS_AUDIO_S16LE}, {AFMT_S16_BE, BMDS_AUDIO_S16BE}, {AFMT_S8, BMDS_AUDIO_S8},
};

#define FORMAT_COUNT (sizeof(formats) / sizeof(formats[0]))

/* Bytes in a sample of each BMDS_AUDIO_ format. */
static const uint8_t sample_bytes[] = {1, 1, 2, 2, 4, 3, 4, 4, 1, 1};

/* Per descriptor (the first 32, as O_NONBLOCK's bits): what GETOPTR last saw played, and
 * SETFRAGMENT's request (0: none), which sizes the ring again whenever the format changes. */
static uint64_t optr_played[32];
static int      fragment_request[32];

static int afmt_of(uint32_t format) {
        for (unsigned i = 0; i < FORMAT_COUNT; ++i)
                if (formats[i].format == format) return formats[i].afmt;
        return AFMT_QUERY; /* set natively: nothing OSS names */
}

static long frame_bytes(const bmds_audio_config_t *c) {
        return c->format < sizeof(sample_bytes) ? (long)sample_bytes[c->format] * (long)c->channels : 1;
}

static long get_config(long fd, bmds_audio_config_t *c) {
        memset(c, 0, sizeof(*c));
        c->size = sizeof(*c);
        return (long)bmds_audio_config((int32_t)fd, NULL, c);
}

/* The ring and wake mark for @p c's format: SETFRAGMENT's, in bytes (0xMMMMSSSS: M fragments,
 * 0x7fff as many as it likes, of 2^S bytes), else the kernel's defaults. */
static void size_ring(long fd, bmds_audio_config_t *c) {
        c->ring_frames = c->start_frames = c->wake_frames = 0;
        const int value = fd >= 0 && fd < 32 ? fragment_request[fd] : 0;
        if (value == 0) return;
        int shift = value & 0xffff;
        if (shift < 4) shift = 4;
        if (shift > 16) shift = 16;
        const uint32_t count = (uint32_t)(value >> 16) & 0xffffu;
        uint32_t       frag = (uint32_t)((1L << shift) / frame_bytes(c));
        if (frag == 0) frag = 1;
        c->wake_frames = frag;
        c->ring_frames = count == 0x7fffu || count < 2 ? 0 : count * frag;
        if (c->ring_frames > c->rate * 10u) c->ring_frames = c->rate * 10u;
        if (c->ring_frames != 0 && c->ring_frames < 2 * frag) c->ring_frames = 2 * frag;
        return;
}

/* Sets @p c (sized for its format), then reads back what was chosen into it. */
static long set_config(long fd, bmds_audio_config_t *c) {
        size_ring(fd, c);
        const bmds_audio_config_t want = *c;
        return (long)bmds_audio_config((int32_t)fd, &want, c);
}

static long control(long fd, uint32_t op) {
        return (long)bmds_audio_control((int32_t)fd, op, 0);
}

/* ---- The mixer: audio/dspN/volume and mute, OSS's percent linear in dB. */

static int32_t setting(uint32_t device, const char *name) {
        char key[32];
        snprintf(key, sizeof(key), "audio/dsp%u/%s", (unsigned)device, name);
        return bmds_sysconf_find(key);
}

static int read_percent(uint32_t device) {
        char          value[16];
        const int32_t mute = setting(device, "mute");
        if (mute >= 0 && bmds_sysconf_get(mute, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) >= 0 &&
            strcmp(value, "true") == 0)
                return 0;
        const int32_t volume = setting(device, "volume");
        if (volume < 0 || bmds_sysconf_get(volume, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value)) < 0) return 0;
        int db = 0;
        if (sscanf(value, "%d", &db) != 1) return 0;
        const int p = 100 + (db * 100 - 30) / 60; /* -60..0 dB to 0..100, to nearest */
        return p < 1 ? 1 : (p > 100 ? 100 : p);
}

static long write_percent(uint32_t device, int percent) {
        const int32_t mute = setting(device, "mute");
        const int32_t volume = setting(device, "volume");
        if (mute < 0 || volume < 0) return -ENODEV;
        if (percent <= 0) return (long)bmds_sysconf_set(mute, BMDS_SYSCONF_OP_SET, "true");
        if (percent > 100) percent = 100;
        char text[8];
        snprintf(text, sizeof(text), "%d", -(((100 - percent) * 6 + 5) / 10)); /* to nearest dB */
        int32_t r = bmds_sysconf_set(volume, BMDS_SYSCONF_OP_SET, text);
        if (r >= 0) r = bmds_sysconf_set(mute, BMDS_SYSCONF_OP_SET, "false");
        return (long)r;
}

static long mixer_ioctl(uint32_t device, unsigned long request, void *arg) {
        int *ip = arg;
        switch (request) {
                case SOUND_MIXER_READ_VOLUME:
                case SOUND_MIXER_READ_PCM: {
                        const int p = read_percent(device);
                        *ip = p | p << 8;
                        return 0;
                }
                case SOUND_MIXER_WRITE_VOLUME:
                case SOUND_MIXER_WRITE_PCM: {
                        /* One volume: the louder side. */
                        const int  left = *ip & 0xff, right = (*ip >> 8) & 0xff;
                        const long r = write_percent(device, left > right ? left : right);
                        if (r < 0) return r;
                        const int p = read_percent(device);
                        *ip = p | p << 8;
                        return 0;
                }
                case SOUND_MIXER_READ_DEVMASK:
                case SOUND_MIXER_READ_STEREODEVS: *ip = SOUND_MASK_VOLUME | SOUND_MASK_PCM; return 0;
                case SOUND_MIXER_READ_RECMASK:
                case SOUND_MIXER_READ_RECSRC:
                case SOUND_MIXER_READ_CAPS: *ip = 0; return 0;
                case SOUND_MIXER_INFO: {
                        mixer_info *info = arg;
                        memset(info, 0, sizeof(*info));
                        snprintf(info->id, sizeof(info->id), "dsp%u", (unsigned)device);
                        snprintf(info->name, sizeof(info->name), "BMDS dsp%u", (unsigned)device);
                        return 0;
                }
                default: return -EINVAL;
        }
}

/* ---- /dev/dsp */

static long set_rate(long fd, int *ip, const bmds_audio_status_t *st) {
        bmds_audio_config_t c;
        long                r = get_config(fd, &c);
        if (r < 0) return r;
        int rate = *ip;
        if (rate < (int)st->min_rate) rate = (int)st->min_rate;
        if (rate > (int)st->max_rate) rate = (int)st->max_rate;
        c.rate = (uint32_t)rate;
        r = set_config(fd, &c);
        if (r < 0) return r;
        *ip = (int)c.rate;
        return 0;
}

static long set_channels(long fd, int channels) {
        bmds_audio_config_t c;
        long                r = get_config(fd, &c);
        if (r < 0) return r;
        c.channels = channels <= 1 ? 1u : 2u; /* more: two */
        r = set_config(fd, &c);
        return r < 0 ? r : (long)c.channels;
}

static long set_format(long fd, int *ip) {
        bmds_audio_config_t c;
        long                r = get_config(fd, &c);
        if (r < 0) return r;
        if (*ip != AFMT_QUERY) {
                for (unsigned i = 0; i < FORMAT_COUNT; ++i) {
                        if (formats[i].afmt != *ip) continue;
                        c.format = formats[i].format;
                        r = set_config(fd, &c);
                        if (r < 0) return r;
                        break;
                }
        }
        *ip = afmt_of(c.format); /* not taken: the format stays, as OSS's drivers answer */
        return 0;
}

static long set_fragment(long fd, int value) {
        if (fd < 0 || fd >= 32) return 0; /* nowhere to keep it: the kernel's sizes */
        fragment_request[fd] = value;
        bmds_audio_config_t c;
        long                r = get_config(fd, &c);
        if (r >= 0) r = set_config(fd, &c);
        return r < 0 ? r : 0;
}

static long dsp_ioctl(long fd, unsigned long request, void *arg, const bmds_audio_status_t *st) {
        int                *ip = arg;
        bmds_audio_config_t c;
        long                r;
        switch (request) {
                case SNDCTL_DSP_RESET: return control(fd, BMDS_AUDIO_DROP);
                case SNDCTL_DSP_SYNC: return control(fd, BMDS_AUDIO_DRAIN);
                case SNDCTL_DSP_POST: return control(fd, BMDS_AUDIO_START);
                case SNDCTL_DSP_SPEED: return set_rate(fd, ip, st);
                case SNDCTL_DSP_STEREO:
                        r = set_channels(fd, *ip ? 2 : 1);
                        if (r < 0) return r;
                        *ip = (int)r - 1;
                        return 0;
                case SNDCTL_DSP_CHANNELS:
                        r = set_channels(fd, *ip);
                        if (r < 0) return r;
                        *ip = (int)r;
                        return 0;
                case SNDCTL_DSP_SETFMT: return set_format(fd, ip);
                case SNDCTL_DSP_GETFMTS: {
                        int mask = 0;
                        for (unsigned i = 0; i < FORMAT_COUNT; ++i)
                                if (st->formats & (1u << formats[i].format)) mask |= formats[i].afmt;
                        *ip = mask;
                        return 0;
                }
                case SNDCTL_DSP_SETFRAGMENT: return set_fragment(fd, *ip);
                case SNDCTL_DSP_SUBDIVIDE: return 0;
                case SNDCTL_DSP_GETBLKSIZE:
                        r = get_config(fd, &c);
                        if (r < 0) return r;
                        *ip = (int)(c.wake_frames * frame_bytes(&c));
                        return 0;
                case SNDCTL_DSP_GETOSPACE: {
                        r = get_config(fd, &c);
                        if (r < 0) return r;
                        const long      fb = frame_bytes(&c);
                        const long      frag = (long)c.wake_frames * fb;
                        const long      free = ((long)st->ring_frames - (long)st->queued_frames) * fb;
                        audio_buf_info *info = arg;
                        info->fragsize = (int)frag;
                        info->fragstotal = frag > 0 ? (int)((long)st->ring_frames * fb / frag) : 0;
                        info->fragments = frag > 0 ? (int)(free / frag) : 0;
                        info->bytes = (int)free;
                        return 0;
                }
                case SNDCTL_DSP_GETODELAY:
                        r = get_config(fd, &c);
                        if (r < 0) return r;
                        *ip = (int)((long)st->delay_frames * frame_bytes(&c));
                        return 0;
                case SNDCTL_DSP_GETOPTR: {
                        r = get_config(fd, &c);
                        if (r < 0) return r;
                        const long  fb = frame_bytes(&c);
                        count_info *info = arg;
                        info->bytes = (int)(st->played_frames * (uint64_t)fb);
                        info->ptr = st->ring_frames > 0 ? (int)(st->played_frames % st->ring_frames * (uint64_t)fb) : 0;
                        info->blocks = 0;
                        if (fd >= 0 && fd < 32 && c.wake_frames > 0) {
                                const uint64_t last = optr_played[fd];
                                info->blocks = st->played_frames > last
                                                       ? (int)(st->played_frames / c.wake_frames - last / c.wake_frames)
                                                       : 0;
                                optr_played[fd] = st->played_frames;
                        }
                        return 0;
                }
                case SNDCTL_DSP_GETCAPS: *ip = DSP_CAP_TRIGGER | DSP_CAP_MULTI; return 0;
                case SNDCTL_DSP_GETTRIGGER: *ip = st->state == BMDS_AUDIO_PAUSED ? 0 : PCM_ENABLE_OUTPUT; return 0;
                case SNDCTL_DSP_SETTRIGGER:
                        return control(fd, (*ip & PCM_ENABLE_OUTPUT) ? BMDS_AUDIO_RESUME : BMDS_AUDIO_PAUSE);
                default: return -EINVAL;
        }
}

long __bmds_oss_ioctl(long fd, unsigned long request, void *arg) {
        if (!bmds_sys_has(BMDS_SYS_AUDIO_STATUS)) return -ENOTTY; /* a kernel without audio calls */
        bmds_audio_status_t st = {.size = sizeof(st)};
        const int32_t       r = bmds_audio_status((int32_t)fd, &st);
        if (r < 0) return (long)(r);
        if (request == OSS_GETVERSION) {
                *(int *)arg = SOUND_VERSION;
                return 0;
        }
        if (st.state == BMDS_AUDIO_MIXER) return mixer_ioctl(st.device, request, arg);
        if ((request >> 8 & 0xff) != 'P') return -EINVAL;
        return dsp_ioctl(fd, request, arg, &st);
}

long __bmds_oss_room(long fd) {
        if (!bmds_sys_has(BMDS_SYS_AUDIO_STATUS)) return -1;
        bmds_audio_status_t st = {.size = sizeof(st)};
        if (bmds_audio_status((int32_t)fd, &st) < 0 || st.state == BMDS_AUDIO_MIXER) return -1;
        bmds_audio_config_t c;
        if (get_config(fd, &c) < 0) return -1;
        return ((long)st.ring_frames - (long)st.queued_frames) * frame_bytes(&c);
}

void __bmds_oss_closed(long fd) {
        if (fd >= 0 && fd < 32) {
                optr_played[fd] = 0;
                fragment_request[fd] = 0;
        }
        return;
}
