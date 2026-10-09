/**
 * @file      tone.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * tone [-n] [FREQ [SECONDS [RATE]]]: a sine on /dev/dsp, set up through OSS's
 * ioctls (portable to any OSS system), or with -n through BMDS's own audio
 * calls (bmds/sys.h), which also tell what the device did.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>
#include <bmds/sys.h>

#define CHUNK 512   /* frames a write */
#define FADE_MS 5   /* in and out, against clicks */
#define LEVEL 0.25f /* -12 dBFS */

static int usage(void) {
        fprintf(stderr, "usage: tone [-n] [FREQ [SECONDS [RATE]]]\n");
        return 2;
}

/* OSS: 16-bit stereo at @p rate; the rate the device chose into *rate. */
static int setup_oss(int fd, int *rate) {
        int format = AFMT_S16_LE, channels = 2;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_S16_LE) return -1;
        if (ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0 || channels != 2) return -1;
        return ioctl(fd, SNDCTL_DSP_SPEED, rate);
}

/* Native: the same, in one call. */
static int setup_native(int fd, int *rate) {
        bmds_audio_config_t want = {
                .size = sizeof(want), .format = BMDS_AUDIO_S16LE, .channels = 2, .rate = (uint32_t)*rate};
        bmds_audio_config_t got = {.size = sizeof(got)};
        const int32_t       r = bmds_audio_config(fd, &want, &got);
        if (r < 0) {
                errno = -r;
                return -1;
        }
        printf("tone: ring %u frames, plays from %u, writable at %u free\n", (unsigned)got.ring_frames,
               (unsigned)got.start_frames, (unsigned)got.wake_frames);
        *rate = (int)got.rate;
        return 0;
}

int main(int argc, char **argv) {
        int native = 0, at = 1;
        if (at < argc && strcmp(argv[at], "-n") == 0) {
                native = 1;
                ++at;
        }
        const float freq = at < argc ? strtof(argv[at++], NULL) : 440.0f;
        const float seconds = at < argc ? strtof(argv[at++], NULL) : 2.0f;
        int         rate = at < argc ? atoi(argv[at++]) : 48000;
        if (at != argc || freq <= 0 || seconds <= 0 || rate <= 0) return usage();
        if (native && !bmds_sys_has(BMDS_SYS_AUDIO_CONFIG)) {
                fprintf(stderr, "tone: this kernel has no audio calls\n");
                return 1;
        }

        const int fd = open("/dev/dsp", O_WRONLY);
        if (fd < 0) {
                fprintf(stderr, "tone: /dev/dsp: %s\n", strerror(errno));
                return 1;
        }
        if ((native ? setup_native(fd, &rate) : setup_oss(fd, &rate)) < 0) {
                fprintf(stderr, "tone: setting up: %s\n", strerror(errno));
                close(fd);
                return 1;
        }
        if (freq >= rate / 2.0f) {
                fprintf(stderr, "tone: %g Hz is above half the rate (%d Hz)\n", (double)freq, rate);
                close(fd);
                return 1;
        }
        printf("tone: %g Hz for %g s at %d Hz (%s)\n", (double)freq, (double)seconds, rate, native ? "native" : "OSS");

        /* A phasor: one complex multiply a frame, renormalized each chunk. */
        const long  total = (long)(seconds * (float)rate);
        const long  fade = (long)rate * FADE_MS / 1000;
        const float step = 2.0f * 3.14159265f * freq / (float)rate;
        const float cs = cosf(step), sn = sinf(step);
        float       re = 1.0f, im = 0.0f;
        int16_t     buf[2 * CHUNK];
        for (long done = 0; done < total;) {
                const long n = total - done < CHUNK ? total - done : CHUNK;
                for (long i = 0; i < n; ++i, ++done) {
                        float gain = LEVEL;
                        if (done < fade) gain *= (float)done / (float)fade;
                        if (total - done < fade) gain *= (float)(total - done) / (float)fade;
                        const int16_t s = (int16_t)lrintf(im * gain * 32767.0f);
                        buf[2 * i] = buf[2 * i + 1] = s;
                        const float r2 = re * cs - im * sn;
                        im = re * sn + im * cs;
                        re = r2;
                }
                const float norm = 1.0f / sqrtf(re * re + im * im);
                re *= norm;
                im *= norm;
                const ssize_t bytes = (ssize_t)(n * 2 * (long)sizeof(int16_t));
                if (write(fd, buf, (size_t)bytes) != bytes) {
                        fprintf(stderr, "tone: write: %s\n", strerror(errno));
                        close(fd);
                        return 1;
                }
        }
        if (native) {
                /* Wait for it to play, then see what the device made of it. */
                (void)bmds_audio_control(fd, BMDS_AUDIO_DRAIN, 0);
                bmds_audio_status_t st = {.size = sizeof(st)};
                if (bmds_audio_status(fd, &st) == 0)
                        printf("tone: played %llu frames, %u underruns; dsp%u at %u Hz, %u late periods\n",
                               (unsigned long long)st.played_frames, (unsigned)st.underruns, (unsigned)st.device,
                               (unsigned)st.device_rate, (unsigned)st.late_periods);
        }
        close(fd); /* waits until what is queued has played */
        return 0;
}
