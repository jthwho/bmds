/**
 * @file      keytone.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * keytone [-p FRAMES] [-x] [-r RATE]: a keyboard played as a piano octave,
 * through a real-time audio client (devplan/audio.md §20). A thread loops in
 * bmds_audio_cycle(), making each period as the device asks for it; the
 * terminal gives key presses only, so each note is struck and decays.
 *
 * Keys: a w s e d f t g y h u j k (C to the next C), z and x an octave down
 * and up, 1-4 the waveform (sine, triangle, saw, square), q quits. Each
 * second it shows the period, the time from a key to its period leaving the
 * device, the cycle's time, and the late, skipped and throttled periods.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

/* Shared between the threads: GCC's atomics on 32-bit words (no <stdatomic.h> in the SDK). */
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define ADD(x, v) __atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)
#define TAKE(x) __atomic_exchange_n(&(x), 0, __ATOMIC_RELAXED)

#define VOICES 8
#define TABLE 2048     /* sine table entries */
#define EVENTS 32      /* key presses queued for the audio thread */
#define LEVEL 0.2f     /* a voice's peak */
#define ATTACK_MS 2    /* against a click */
#define DECAY_S 0.6f   /* to 1/e */
#define SILENT 0.0001f /* -80 dB: the voice is free again */

enum {
        SINE,
        TRIANGLE,
        SAW,
        SQUARE
};

struct voice {
                float phase; /* 0 to 1 */
                float step;  /* phase a frame */
                float env;
                int   attack; /* frames of attack left */
                int   on;
};

struct note {
                float   hz;
                int64_t key_ns; /* when the key was read */
};

static float                 sine[TABLE + 1];
static struct voice          voices[VOICES];
static struct note           events[EVENTS];
static unsigned              ev_head, ev_tail; /* single producer (main), single consumer (audio) */
static int                   waveform = SINE;
static volatile sig_atomic_t quit;
static int                   fd = -1;
static struct termios        saved;
static int                   raw_terminal;

/* Measured by the audio thread, read and reset by the main thread each second. */
static unsigned cycles, cycle_us_sum, cycle_us_max, period_frames, rate_hz, active_voices;
static unsigned late, skipped, throttled;
static int32_t  key_to_play_us = -1; /* the last key's (-1: none yet) */
static int32_t  made_to_play_us;

static int64_t now_ns(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void restore_terminal(void) {
        if (raw_terminal) tcsetattr(0, TCSANOW, &saved);
        raw_terminal = 0;
}

static void on_signal(int sig) {
        (void)sig;
        quit = 1;
}

/* One sample of a waveform at @p phase (0 to 1), with PolyBLEP for the saw's and square's steps. */
static float blep(float t, float dt) {
        if (t < dt) {
                t /= dt;
                return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
                t = (t - 1.0f) / dt;
                return t * t + t + t + 1.0f;
        }
        return 0.0f;
}

static float wave(int kind, float phase, float dt) {
        switch (kind) {
                case TRIANGLE: return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
                case SAW: return 2.0f * phase - 1.0f - blep(phase, dt);
                case SQUARE: {
                        float v = phase < 0.5f ? 1.0f : -1.0f;
                        v += blep(phase, dt);
                        float half = phase + 0.5f;
                        if (half >= 1.0f) half -= 1.0f;
                        return v - blep(half, dt);
                }
                default: {
                        const float x = phase * TABLE;
                        const int   i = (int)x;
                        return sine[i] + (sine[i + 1] - sine[i]) * (x - (float)i);
                }
        }
}

/* Starts a note on a free voice, or the quietest. */
static void strike(float hz, uint32_t rate) {
        int pick = 0;
        for (int i = 0; i < VOICES; ++i) {
                if (!voices[i].on) {
                        pick = i;
                        break;
                }
                if (voices[i].env < voices[pick].env) pick = i;
        }
        struct voice *v = &voices[pick];
        v->phase = 0.0f;
        v->step = hz / (float)rate;
        v->env = 0.0f;
        v->attack = (int)(rate * ATTACK_MS / 1000);
        v->on = 1;
}

/* The audio thread: one period per bmds_audio_cycle(). */
static void *audio_thread(void *arg) {
        (void)arg;
        bmds_audio_cycle_t c = {.size = sizeof(c)};
        int64_t            started = 0;
        for (;;) {
                if (started != 0) {
                        const unsigned us = (unsigned)((now_ns() - started) / 1000);
                        ADD(cycles, 1);
                        ADD(cycle_us_sum, us);
                        if (us > LOAD(cycle_us_max)) STORE(cycle_us_max, us);
                }
                const int32_t r = bmds_audio_cycle(fd, quit ? NULL : &c);
                if (quit) break;
                if (r == -BMDS_EINTR || r == -BMDS_EPIPE) {
                        started = 0;
                        continue;
                }
                if (r < 0) {
                        fprintf(stderr, "\r\nkeytone: audio_cycle: %s\r\n", strerror(-r));
                        quit = 1;
                        break;
                }
                started = now_ns();
                STORE(made_to_play_us, (int32_t)((c.play_ns - started) / 1000));
                STORE(period_frames, c.frames);
                STORE(rate_hz, c.rate);
                STORE(late, c.late);
                STORE(throttled, c.throttled);
                ADD(skipped, c.skipped);
                /* Keys pressed since the last period start now. */
                for (unsigned t = LOAD(ev_tail); t != LOAD(ev_head); ++t) {
                        const struct note *n = &events[t % EVENTS];
                        strike(n->hz, c.rate);
                        STORE(key_to_play_us, (int32_t)((c.play_ns - n->key_ns) / 1000));
                        STORE(ev_tail, t + 1);
                }
                const int   kind = LOAD(waveform);
                const float decay = expf(-1.0f / (DECAY_S * (float)c.rate));
                const float rise = 1.0f / (float)(c.rate * ATTACK_MS / 1000);
                unsigned    on = 0;
                for (uint32_t i = 0; i < c.frames * c.channels; ++i) c.out[i] = 0.0f;
                for (int k = 0; k < VOICES; ++k) {
                        struct voice *v = &voices[k];
                        if (!v->on) continue;
                        ++on;
                        for (uint32_t i = 0; i < c.frames; ++i) {
                                if (v->attack > 0) {
                                        v->env += rise;
                                        --v->attack;
                                } else {
                                        v->env *= decay;
                                }
                                const float s = LEVEL * v->env * wave(kind, v->phase, v->step);
                                for (uint32_t ch = 0; ch < c.channels; ++ch) c.out[i * c.channels + ch] += s;
                                v->phase += v->step;
                                if (v->phase >= 1.0f) v->phase -= 1.0f;
                        }
                        if (v->attack == 0 && v->env < SILENT) v->on = 0;
                }
                STORE(active_voices, on);
        }
        return NULL;
}

static int usage(void) {
        fprintf(stderr, "usage: keytone [-p FRAMES] [-x] [-r RATE]\n");
        return 2;
}

static void show_status(void) {
        const unsigned n = TAKE(cycles);
        const unsigned sum = TAKE(cycle_us_sum);
        const unsigned max = TAKE(cycle_us_max);
        const unsigned frames = LOAD(period_frames), rate = LOAD(rate_hz);
        if (rate == 0) return;
        const int32_t key = LOAD(key_to_play_us);
        const int32_t made = LOAD(made_to_play_us);
        char          keytext[24] = "-";
        if (key >= 0) snprintf(keytext, sizeof(keytext), "%d.%02d ms", (int)(key / 1000), (int)(key % 1000 / 10));
        printf("\r%u fr (%u us), made->out %d us, key->out %s, cycle %u/%u us, late %u, skipped %u, "
               "throttled %u, voices %u  ",
               frames, (unsigned)((uint64_t)frames * 1000000 / rate), (int)made, keytext, n ? sum / n : 0, max,
               LOAD(late), LOAD(skipped), LOAD(throttled), LOAD(active_voices));
        fflush(stdout);
}

int main(int argc, char **argv) {
        uint32_t period = 0, rate = 0;
        int      exclusive = 0;
        for (int i = 1; i < argc; ++i) {
                if (strcmp(argv[i], "-x") == 0) {
                        exclusive = 1;
                } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
                        period = (uint32_t)strtoul(argv[++i], NULL, 10);
                } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
                        rate = (uint32_t)strtoul(argv[++i], NULL, 10);
                } else {
                        return usage();
                }
        }
        if (rate != 0 && !exclusive) return usage();
        if (!bmds_sys_has(BMDS_SYS_AUDIO_CYCLE)) {
                fprintf(stderr, "keytone: this system has no real-time audio\n");
                return 1;
        }
        for (int i = 0; i <= TABLE; ++i) sine[i] = sinf(2.0f * (float)M_PI * (float)i / TABLE);
        fd = open("/dev/dsp", O_WRONLY);
        if (fd < 0) {
                fprintf(stderr, "keytone: /dev/dsp: %s\n", strerror(errno));
                return 1;
        }
        bmds_audio_config_t want = {.size = sizeof(want),
                                    .format = BMDS_AUDIO_F32LE,
                                    .channels = 1,
                                    .rate = rate,
                                    .wake_frames = period,
                                    .flags = BMDS_AUDIO_REALTIME | (exclusive ? BMDS_AUDIO_EXCLUSIVE : 0)};
        bmds_audio_config_t got = {.size = sizeof(got)};
        int32_t             r = bmds_audio_config(fd, &want, &got);
        if (r < 0) {
                fprintf(stderr, "keytone: %s: %s\n", exclusive ? "exclusive mode" : "real-time mode",
                        r == -BMDS_EBUSY ? "the device is busy" : strerror(-r));
                return 1;
        }
        printf("keytone: %u Hz, %u frames a period%s. Keys: a w s e d f t g y h u j k, z/x octave, "
               "1-4 waveform, q quits\n",
               (unsigned)got.rate, (unsigned)got.wake_frames, exclusive ? ", exclusive" : "");
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        if (tcgetattr(0, &saved) == 0) {
                struct termios t = saved;
                t.c_lflag &= ~(ICANON | ECHO);
                t.c_cc[VMIN] = 1;
                t.c_cc[VTIME] = 0;
                raw_terminal = tcsetattr(0, TCSANOW, &t) == 0;
        }
        pthread_t thread;
        if (pthread_create(&thread, NULL, audio_thread, NULL) != 0) {
                restore_terminal();
                fprintf(stderr, "keytone: no thread\n");
                return 1;
        }
        static const char keys[] = "awsedftgyhujk";
        int               octave = 0;
        while (!quit) {
                struct pollfd p = {.fd = 0, .events = POLLIN};
                const int     ready = poll(&p, 1, 1000);
                if (ready <= 0) {
                        show_status();
                        continue;
                }
                char ch;
                if (read(0, &ch, 1) != 1) break;
                const char *k = strchr(keys, ch);
                if (ch == 'q') break;
                if (ch == 'z' && octave > -3) --octave;
                if (ch == 'x' && octave < 3) ++octave;
                if (ch >= '1' && ch <= '4') STORE(waveform, ch - '1');
                if (k == NULL || ch == '\0') continue;
                /* C4 is 261.63 Hz; each key a semitone up. */
                const int      semis = (int)(k - keys) + 12 * octave;
                const unsigned head = LOAD(ev_head);
                if (head - LOAD(ev_tail) >= EVENTS) continue;
                events[head % EVENTS] = (struct note){261.6256f * powf(2.0f, (float)semis / 12.0f), now_ns()};
                STORE(ev_head, head + 1);
        }
        quit = 1;
        pthread_join(thread, NULL);
        restore_terminal();
        show_status();
        printf("\n");
        close(fd);
        return 0;
}
