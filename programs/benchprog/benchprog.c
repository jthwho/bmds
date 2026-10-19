/**
 * @file      benchprog.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * benchprog: the system as programs see it, measured for `bench` (devplan/bench.md §4f, §12, §14):
 * system calls, starting a program, sleeps' lateness, TCP over loopback, the synth's load, a MIDI
 * file decoded, pictures encoded and decoded; a surface's presents, libbmdsgame's drawing, and a
 * software renderer's inner loops. `benchprog WHAT [KEY=VALUE...]`, run by `bench` as
 * share/bench/benchprog.bench declares; each run's figure a `@bench` line (bmds/benchlib.h).
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <bmds/benchlib.h>
#include <bmds/game.h>
#include <bmds/sys.h>

extern char **environ;

static bmds_bench_t bench;
static const char  *self = "benchprog";

static int fail(const char *what) {
        printf("%s: %s\n", what, strerror(errno));
        return 1;
}

/* ---- System calls ---- */

/* Three calls: getpid (the call alone: no lock), getppid (the process table looked up, under its
   lock), clock_gettime (the clock, through the kernel). */
static int syscall_sample(void *arg, double *out) {
        const int which = (int)(intptr_t)arg;
        enum {
                Calls = 20000
        };
        struct timespec t;
        const int64_t   t0 = bmds_bench_now_ns();
        for (int i = 0; i < Calls; ++i) {
                if (which == 0) {
                        (void)getpid();
                } else if (which == 1) {
                        (void)getppid();
                } else {
                        clock_gettime(CLOCK_MONOTONIC, &t);
                }
        }
        *out = (double)(bmds_bench_now_ns() - t0) / Calls;
        return 0;
}

static int run_syscall(void) {
        static const char *const names[] = {"getpid_ns", "getppid_ns", "clock_gettime_ns"};
        for (int k = 0; k < 3; ++k) {
                const int err = bmds_bench_measure(&bench, names[k], "ns", BMDS_BENCH_LOWER, syscall_sample,
                                                   (void *)(intptr_t)k);
                if (err != 0) return err;
        }
        return 0;
}

/* ---- Starting a program: this one, doing nothing, waited for ---- */

static int launch_sample(void *arg, double *out) {
        (void)arg;
        char *const   argv[] = {(char *)self, "noop", NULL};
        pid_t         pid;
        const int64_t t0 = bmds_bench_now_ns();
        if (posix_spawnp(&pid, self, NULL, NULL, argv, environ) != 0) return fail("posix_spawnp");
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return fail("waitpid");
        *out = (double)(bmds_bench_now_ns() - t0) / 1e6;
        return 0;
}

static int run_launch(void) {
        return bmds_bench_measure(&bench, "start_and_exit_ms", "ms", BMDS_BENCH_LOWER, launch_sample, NULL);
}

/* ---- Sleeps: how late a 1 ms sleep ends, at the normal priority and in real time ---- */

static int64_t sleeps_late_us(int n) {
        int64_t         sum = 0;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        for (int i = 0; i < n; ++i) {
                t.tv_nsec += 1000000L;
                while (t.tv_nsec >= 1000000000L) {
                        t.tv_nsec -= 1000000000L;
                        ++t.tv_sec;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                sum += ((int64_t)(now.tv_sec - t.tv_sec) * 1000000000 + (now.tv_nsec - t.tv_nsec)) / 1000;
        }
        return sum / n;
}

static int sleep_sample(void *arg, double *out) {
        (void)arg;
        *out = (double)sleeps_late_us(200);
        return 0;
}

static int run_sleep(void) {
        int err = bmds_bench_measure(&bench, "other_late_us", "us", BMDS_BENCH_LOWER, sleep_sample, NULL);
        if (err != 0) return err;
        struct sched_param sp = {.sched_priority = 50};
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) return fail("sched_setscheduler");
        err = bmds_bench_measure(&bench, "fifo_late_us", "us", BMDS_BENCH_LOWER, sleep_sample, NULL);
        sp.sched_priority = 0;
        (void)sched_setscheduler(0, SCHED_OTHER, &sp);
        return err;
}

/* ---- TCP over loopback: a megabyte a run, written by a thread, read here ---- */

enum {
        LoopbackBytes = 1024 * 1024,
        Chunk = 16384
};

static void *loopback_writer(void *arg) {
        const int   fd = (int)(intptr_t)arg;
        static char buf[Chunk];
        long        left = LoopbackBytes;
        while (left > 0) {
                const ssize_t n = write(fd, buf, left < Chunk ? (size_t)left : Chunk);
                if (n <= 0) break;
                left -= n;
        }
        close(fd);
        return NULL;
}

static int loopback_sample(void *arg, double *out) {
        const int          listener = (int)(intptr_t)arg;
        struct sockaddr_in a;
        socklen_t          al = sizeof(a);
        if (getsockname(listener, (struct sockaddr *)&a, &al) != 0) return fail("getsockname");
        const int c = socket(AF_INET, SOCK_STREAM, 0);
        if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof(a)) != 0) return fail("connect");
        const int s = accept(listener, NULL, NULL);
        if (s < 0) return fail("accept");
        pthread_t     writer;
        const int64_t t0 = bmds_bench_now_ns();
        if (pthread_create(&writer, NULL, loopback_writer, (void *)(intptr_t)c) != 0) return fail("pthread_create");
        static char buf[Chunk];
        long        got = 0;
        for (;;) {
                const ssize_t n = read(s, buf, sizeof(buf));
                if (n <= 0) break;
                got += n;
        }
        const int64_t ns = bmds_bench_now_ns() - t0;
        pthread_join(writer, NULL);
        close(s);
        if (got != LoopbackBytes) {
                printf("loopback: %ld bytes of %d arrived\n", got, LoopbackBytes);
                return 1;
        }
        *out = (double)got / (double)ns * 1e9 / (1024 * 1024);
        return 0;
}

static int run_loopback(void) {
        const int l = socket(AF_INET, SOCK_STREAM, 0);
        if (l < 0) return fail("socket");
        const int one = 1;
        (void)setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 2) != 0) return fail("listen");
        const int err =
                bmds_bench_measure(&bench, "tcp_mb_s", "MB/s", BMDS_BENCH_HIGHER, loopback_sample, (void *)(intptr_t)l);
        close(l);
        return err;
}

/* ---- The synth: notes held, the time each block of 64 frames takes to render ---- */

static bmds_synth_event_t synth_event(uint64_t frame, uint8_t a, uint8_t b, uint8_t c) {
        bmds_synth_event_t e = {.frame = frame, .length = 3, .data = {a, b, c}};
        return e;
}

struct synth_run {
                int      fd;
                int      notes;
                uint64_t frame;
};

/* One run: the notes started, a tenth of a second for the last ones to end, then a second of
   blocks timed (the first, which starts the notes, apart). The average block, in µs. */
static int synth_sample(void *arg, double *out) {
        struct synth_run    *r = arg;
        static const uint8_t programs[] = {0, 48, 24, 40};
        static float         block[64 * 2];
        for (int ch = 0; ch < 4; ++ch) {
                const bmds_synth_event_t off = synth_event(r->frame, (uint8_t)(0xb0 | ch), 123, 0);
                const bmds_synth_event_t p = {
                        .frame = r->frame, .length = 2, .data = {(uint8_t)(0xc0 | ch), programs[ch]}};
                if (write(r->fd, &off, sizeof(off)) != sizeof(off) || write(r->fd, &p, sizeof(p)) != sizeof(p))
                        return fail("synth write");
        }
        for (int i = 0; i < r->notes; ++i) {
                const bmds_synth_event_t on =
                        synth_event(r->frame + 4800, (uint8_t)(0x90 | (i % 4)), (uint8_t)(36 + i), 76);
                if (write(r->fd, &on, sizeof(on)) != sizeof(on)) return fail("synth write");
        }
        for (int i = 0; i < 75; ++i)
                if (read(r->fd, block, sizeof(block)) != sizeof(block)) return fail("synth read");
        int64_t sum = 0;
        for (int i = 0; i < 750; ++i) {
                const int64_t a = bmds_bench_now_ns();
                if (read(r->fd, block, sizeof(block)) != sizeof(block)) return fail("synth read");
                if (i > 0) sum += bmds_bench_now_ns() - a;
        }
        r->frame += 825 * 64;
        *out = (double)sum / 749 / 1000;
        return 0;
}

static int run_synth(void) {
        const char         *font = bmds_bench_param(&bench, "font", "");
        bmds_synth_config_t c = {
                .size = sizeof(c), .format = BMDS_AUDIO_F32LE, .max_voices = 128, .font = *font != '\0' ? font : NULL};
        const int fd = bmds_audio_synth(&c);
        if (fd < 0) {
                printf("synth: %s\n", strerror(-fd));
                return fd == -BMDS_ENOENT ? BMDS_BENCH_SKIPPED : 1;
        }
        struct synth_run r = {.fd = fd, .notes = 0, .frame = 0};
        static const int counts[] = {8, 16, 32, 48};
        int              err = 0;
        for (size_t k = 0; k < sizeof(counts) / sizeof(counts[0]) && err == 0; ++k) {
                char metric[32];
                snprintf(metric, sizeof(metric), "notes_%d_block_us", counts[k]);
                r.notes = counts[k];
                err = bmds_bench_measure(&bench, metric, "us", BMDS_BENCH_LOWER, synth_sample, &r);
        }
        close(fd);
        return err;
}

/* ---- A MIDI file decoded by the synth, whole ---- */

static int midi_sample(void *arg, double *out) {
        const char *path = arg;
        const int   file = open(path, O_RDONLY);
        if (file < 0) return fail(path);
        bmds_decode_config_t c = {.size = sizeof(c), .format = BMDS_AUDIO_S16LE};
        const int64_t        t0 = bmds_bench_now_ns();
        const int            pcm = bmds_audio_decode(file, &c);
        if (pcm < 0) {
                printf("decode: %s\n", strerror(-pcm));
                close(file);
                return 1;
        }
        static int16_t buf[4096 * 2];
        uint64_t       frames = 0;
        for (;;) {
                const ssize_t n = read(pcm, buf, sizeof(buf));
                if (n <= 0) break;
                frames += (uint64_t)n / 4;
        }
        const double seconds = (double)(bmds_bench_now_ns() - t0) / 1e9;
        close(pcm);
        close(file);
        *out = seconds > 0 ? (double)frames / 48000.0 / seconds : 0; /* times faster than it plays */
        return 0;
}

static int run_midi(void) {
        const char *path = bmds_bench_param(&bench, "file", "/sd0/p0/share/bench/scale.mid");
        struct stat st;
        if (stat(path, &st) != 0) {
                printf("%s: %s\n", path, strerror(errno));
                return BMDS_BENCH_SKIPPED;
        }
        return bmds_bench_measure(&bench, "times_realtime", "x", BMDS_BENCH_HIGHER, midi_sample, (void *)path);
}

/* ---- Pictures: a 1280x720 picture encoded as PNG and JPEG, then decoded ---- */

enum {
        ImageW = 1280,
        ImageH = 720
};

struct image_run {
                uint8_t *pixels; /* B, G, R */
                uint8_t *file;   /* the encoded picture */
                size_t   length;
                uint32_t codec;
                uint32_t flags; /* decoding's */
                uint8_t *out;
};

static int encode_sample(void *arg, double *out) {
        struct image_run *r = arg;
        static const char path[] = "/tmp/benchprog.img";
        const int         fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return fail(path);
        bmds_image_encode_t e;
        memset(&e, 0, sizeof(e));
        e.size = sizeof(e);
        e.codec = r->codec;
        e.format = BMDS_PIXEL_BGR24;
        e.width = ImageW;
        e.height = ImageH;
        e.pixels = r->pixels;
        e.quality = 90;
        e.sampling = BMDS_JPEG_YUV420;
        const int64_t t0 = bmds_bench_now_ns();
        const int32_t bytes = bmds_image_encode(fd, &e);
        *out = (double)(bmds_bench_now_ns() - t0) / 1e6;
        close(fd);
        if (bytes <= 0) {
                printf("encode: %s\n", strerror(-bytes));
                return 1;
        }
        /* Kept for decoding. */
        const int rfd = open(path, O_RDONLY);
        if (rfd < 0) return fail(path);
        r->length = (size_t)read(rfd, r->file, (size_t)bytes);
        close(rfd);
        unlink(path);
        return r->length == (size_t)bytes ? 0 : 1;
}

static int decode_sample(void *arg, double *out) {
        struct image_run   *r = arg;
        bmds_image_decode_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.format = BMDS_PIXEL_BGR24;
        d.flags = r->flags;
        d.out = r->out;
        d.out_size = ImageW * ImageH * 3;
        d.data = r->file;
        d.length = (uint32_t)r->length;
        const int64_t t0 = bmds_bench_now_ns();
        const int32_t err = bmds_image_decode(-1, &d, NULL);
        *out = (double)(bmds_bench_now_ns() - t0) / 1e6;
        if (err != 0) {
                printf("decode: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}

static int run_images(void) {
        struct image_run r;
        memset(&r, 0, sizeof(r));
        r.pixels = malloc(ImageW * ImageH * 3);
        r.file = malloc(ImageW * ImageH * 3 + 65536);
        /* Aligned as image_info's `align` asks for a no-copy decode (128: the L2 cache's line). */
        r.out = aligned_alloc(128, ImageW * ImageH * 3);
        if (r.pixels == NULL || r.file == NULL || r.out == NULL) return fail("malloc");
        for (int y = 0; y < ImageH; ++y) /* smooth with some noise: a photograph's kind of picture */
                for (int x = 0; x < ImageW; ++x) {
                        uint8_t *p = r.pixels + ((size_t)y * ImageW + x) * 3;
                        p[0] = (uint8_t)(x / 5 + (rand() & 7));
                        p[1] = (uint8_t)(y / 3 + (rand() & 7));
                        p[2] = (uint8_t)((x + y) / 8);
                }
        struct {
                        const char *encode, *decode;
                        uint32_t    codec, flags;
        } const cases[] = {
                {"png_encode_ms", "png_decode_ms", BMDS_IMAGE_PNG, 0},
                {"jpeg_encode_ms", "jpeg_decode_ms", BMDS_IMAGE_JPEG, 0},
                {NULL, "jpeg_software_decode_ms", BMDS_IMAGE_JPEG, BMDS_IMAGE_NO_HARDWARE},
        };
        int err = 0;
        for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]) && err == 0; ++k) {
                r.codec = cases[k].codec;
                r.flags = cases[k].flags;
                if (cases[k].encode != NULL)
                        err = bmds_bench_measure(&bench, cases[k].encode, "ms", BMDS_BENCH_LOWER, encode_sample, &r);
                if (err == 0)
                        err = bmds_bench_measure(&bench, cases[k].decode, "ms", BMDS_BENCH_LOWER, decode_sample, &r);
        }
        free(r.pixels);
        free(r.file);
        free(r.out);
        return err;
}

/* ---- A pipe: 4 MB a run from one thread to another, in 4 KB writes ---- */

enum {
        PipeBytes = 4 * 1024 * 1024,
        PipeChunk = 4096
};

static void *pipe_writer(void *arg) {
        const int   fd = (int)(intptr_t)arg;
        static char buf[PipeChunk];
        for (long left = PipeBytes; left > 0;) {
                const ssize_t n = write(fd, buf, PipeChunk);
                if (n <= 0) break;
                left -= n;
        }
        close(fd);
        return NULL;
}

static int pipe_sample(void *arg, double *out) {
        (void)arg;
        int p[2];
        if (pipe(p) != 0) return fail("pipe");
        pthread_t     writer;
        const int64_t t0 = bmds_bench_now_ns();
        if (pthread_create(&writer, NULL, pipe_writer, (void *)(intptr_t)p[1]) != 0) return fail("pthread_create");
        static char buf[PipeChunk];
        long        got = 0;
        for (;;) {
                const ssize_t n = read(p[0], buf, sizeof(buf));
                if (n <= 0) break;
                got += n;
        }
        const int64_t ns = bmds_bench_now_ns() - t0;
        pthread_join(writer, NULL);
        close(p[0]);
        if (got != PipeBytes) return 1;
        *out = (double)got / (double)ns * 1e9 / (1024 * 1024);
        return 0;
}

static int run_pipe(void) {
        return bmds_bench_measure(&bench, "mb_s", "MB/s", BMDS_BENCH_HIGHER, pipe_sample, NULL);
}

/* ---- Surfaces: a program's present, from its buffer to the screen (devplan/games.md §11) ---- */

/* The CPU time both cores were busy, from the counters (cpu.core0.busy_us, cpu.core1.busy_us): -1
   if they aren't counted. */
static int64_t busy_us(void) {
        static uint32_t ids[2];
        if (ids[0] == 0) {
                for (uint32_t i = 0;; ++i) {
                        bmds_counter_t c;
                        memset(&c, 0, sizeof(c));
                        c.size = sizeof(c);
                        if (bmds_stats_list(i, &c) != 0) break;
                        if (strcmp(c.name, "cpu.core0.busy_us") == 0) ids[0] = c.id;
                        if (strcmp(c.name, "cpu.core1.busy_us") == 0) ids[1] = c.id;
                }
                if (ids[0] == 0 || ids[1] == 0) return -1;
        }
        int64_t v[2];
        if (bmds_stats_read(ids, 2, v, NULL, 0) != 2) return -1;
        return v[0] + v[1];
}

static int64_t process_cpu_us(void) {
        struct timespec t;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
        return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

struct surface_case {
                const char *name;
                uint32_t    width, height, format, flags, aspect_x, aspect_y;
                int         small; /* damage: a 64x64 square a frame (else all of it) */
};

struct surface_run {
                const struct surface_case *c;
                int                        fd;
                void                      *pixels[3];
                uint32_t                   stride, width, height, bpp;
                int64_t                    seconds_us;
                uint32_t                   frame;
                double                     present_us, system_us;
};

/* One run: presents as fast as buffers come for `seconds`, each with a square moved; the frames
   shown a second, and on the side the average present's time and the rest of the system's CPU a
   frame shown. */
static int surface_sample(void *arg, double *out) {
        struct surface_run *r = arg;
        bmds_surface_info_t before = {.size = sizeof(before)}, after = {.size = sizeof(after)};
        if (bmds_surface_control(r->fd, BMDS_SURFACE_INFO, &before, sizeof(before)) != 0) return fail("info");
        const int64_t busy0 = busy_us(), cpu0 = process_cpu_us(), t0 = bmds_bench_now_ns();
        int64_t       in_present = 0;
        uint32_t      presents = 0;
        bmds_rect_t   last = {0, 0, 64, 64};
        while (bmds_bench_now_ns() - t0 < r->seconds_us * 1000) {
                const int32_t i = bmds_surface_acquire(r->fd, 200);
                if (i < 0) {
                        printf("acquire: %s\n", strerror(-i));
                        return 1;
                }
                /* A 64x64 square somewhere new: what changed. */
                const uint32_t x = (r->frame * 8) % (r->width - 64), y = (r->frame * 3) % (r->height - 64);
                ++r->frame;
                for (uint32_t row = 0; row < 64; ++row)
                        memset((uint8_t *)r->pixels[i] + (size_t)(y + row) * r->stride + (size_t)x * r->bpp,
                               (int)(r->frame & 0xff), 64 * r->bpp);
                bmds_rect_t damage[2] = {{(int32_t)x, (int32_t)y, 64, 64}, last};
                last = damage[0];
                bmds_surface_present_t p = {.size = sizeof(p), .index = (uint32_t)i};
                if (r->c->small) {
                        p.damage = damage;
                        p.damage_count = 2;
                }
                const int64_t a = bmds_bench_now_ns();
                const int32_t frame = bmds_surface_present(r->fd, &p);
                in_present += bmds_bench_now_ns() - a;
                if (frame < 0) {
                        printf("present: %s\n", strerror(-frame));
                        return 1;
                }
                ++presents;
        }
        const double  us = (double)(bmds_bench_now_ns() - t0) / 1000;
        const int64_t busy1 = busy_us(), cpu1 = process_cpu_us();
        if (bmds_surface_control(r->fd, BMDS_SURFACE_INFO, &after, sizeof(after)) != 0) return fail("info");
        const uint32_t shown = (after.presents - before.presents) - (after.dropped - before.dropped);
        *out = shown * 1e6 / us;
        r->present_us = presents > 0 ? (double)in_present / presents / 1000 : 0;
        r->system_us = busy0 >= 0 && shown > 0 ? (double)((busy1 - busy0) - (cpu1 - cpu0)) / shown : 0;
        return 0;
}

static int run_surface(void) {
        static const struct surface_case cases[] = {
                {"doom_l8_320x200_full", 320, 200, BMDS_PIXEL_L8, BMDS_SURFACE_FULL_SCREEN, 4, 3, 0},
                {"doom_l8_fast_full", 320, 200, BMDS_PIXEL_L8, BMDS_SURFACE_FULL_SCREEN | BMDS_SURFACE_FAST, 4, 3, 0},
                {"quake_l8_320x240_full", 320, 240, BMDS_PIXEL_L8, BMDS_SURFACE_FULL_SCREEN, 0, 0, 0},
                {"l8_320x200_window", 320, 200, BMDS_PIXEL_L8, 0, 4, 3, 0},
                {"blocks_565_640x360_full", 640, 360, BMDS_PIXEL_RGB565, BMDS_SURFACE_FULL_SCREEN, 0, 0, 0},
                {"bgr24_maximized_all", 0, 0, BMDS_PIXEL_BGR24, BMDS_SURFACE_MAXIMIZED, 0, 0, 0},
                {"bgr24_maximized_square", 0, 0, BMDS_PIXEL_BGR24, BMDS_SURFACE_MAXIMIZED, 0, 0, 1},
        };
        const char *only = bmds_bench_param(&bench, "case", "");
        const long  seconds_ms = bmds_bench_param_long(&bench, "ms", 1000);
        /* Paced (2 buffers): acquire() waits while the presenter still reads the older one, so
           the program keeps at most a frame ahead, as a game. Flat out (3): it presents as fast as
           buffers come, and the presents the presenter drops cost the system too (§19). */
        const uint32_t buffers = bmds_bench_param_long(&bench, "pace", 1) != 0 ? 2 : 3;
        if (!bmds_sys_has(BMDS_SYS_SURFACE_OPEN)) return BMDS_BENCH_SKIPPED;
        for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
                const struct surface_case *c = &cases[k];
                if (*only != '\0' && strstr(c->name, only) == NULL) continue;
                bmds_surface_config_t cfg = {.size = sizeof(cfg),
                                             .width = c->width,
                                             .height = c->height,
                                             .format = c->format,
                                             .buffers = buffers,
                                             .flags = c->flags,
                                             .aspect_x = c->aspect_x,
                                             .aspect_y = c->aspect_y,
                                             .title = "benchprog"};
                const int             fd = bmds_surface_open(&cfg);
                if (fd == -BMDS_ENOMEM && (c->flags & BMDS_SURFACE_FAST) != 0) {
                        printf("%s: skipped, no internal RAM for its buffers\n", c->name);
                        continue;
                }
                if (fd < 0) {
                        printf("%s: %s\n", c->name, strerror(-fd));
                        return fd == -BMDS_ENODEV ? BMDS_BENCH_SKIPPED : 1;
                }
                struct surface_run  r = {.c = c, .fd = fd, .seconds_us = seconds_ms * 1000};
                bmds_surface_info_t info = {.size = sizeof(info)};
                (void)bmds_surface_control(fd, BMDS_SURFACE_INFO, &info, sizeof(info));
                r.width = info.picture_width;
                r.height = info.picture_height;
                r.bpp = c->format == BMDS_PIXEL_L8 ? 1 : c->format == BMDS_PIXEL_RGB565 ? 2 : 3;
                for (uint32_t b = 0; b < buffers; ++b) {
                        bmds_surface_buffer_t buf = {.size = sizeof(buf), .index = b};
                        if (bmds_surface_buffer(fd, &buf) != 0) return fail("buffer");
                        r.pixels[b] = buf.pixels;
                        r.stride = buf.stride;
                        memset(buf.pixels, 0x20, (size_t)buf.stride * r.height);
                }
                if (c->format == BMDS_PIXEL_L8) {
                        static uint32_t grey[256];
                        for (uint32_t i = 0; i < 256; ++i) grey[i] = i * 0x010101u;
                        bmds_surface_palette_t pal = {.size = sizeof(pal), .first = 0, .count = 256, .rgb = grey};
                        (void)bmds_surface_control(fd, BMDS_SURFACE_PALETTE, &pal, sizeof(pal));
                }
                /* By hand rather than bmds_bench_measure(): each run gives three figures. */
                int err = 0;
                for (uint32_t i = 0; i < bench.warmup + bench.runs && err == 0; ++i) {
                        double fps = 0;
                        err = surface_sample(&r, &fps);
                        if (err != 0 || i < bench.warmup) continue;
                        char metric[64];
                        snprintf(metric, sizeof(metric), "%s_fps", c->name);
                        bmds_bench_result(metric, fps, "/s", BMDS_BENCH_HIGHER);
                        snprintf(metric, sizeof(metric), "%s_present_us", c->name);
                        bmds_bench_result(metric, r.present_us, "us", BMDS_BENCH_LOWER);
                        snprintf(metric, sizeof(metric), "%s_system_us", c->name);
                        bmds_bench_result(metric, r.system_us, "us", BMDS_BENCH_LOWER);
                }
                close(fd);
                if (err != 0) return err;
        }
        return 0;
}

/* ---- libbmdsgame's drawing, into a 640x360 canvas in PSRAM, in each format ---- */

enum {
        CanvasW = 640,
        CanvasH = 360,
        Sprite = 64
};

struct draw_run {
                bmds_canvas_t     canvas;
                bmds_game_image_t opaque, keyed;
                int               op;
};

static int draw_sample(void *arg, double *out) {
        struct draw_run *r = arg;
        bmds_canvas_t   *cv = &r->canvas;
        const uint32_t   px = bmds_canvas_pixel(cv, 0x406080);
        enum {
                Times = 200
        };
        double        pixels = 0;
        const int64_t t0 = bmds_bench_now_ns();
        for (int i = 0; i < Times; ++i) {
                const int32_t x = (i * 37) % (CanvasW - 2 * Sprite), y = (i * 23) % (CanvasH - 2 * Sprite);
                switch (r->op) {
                        case 0:
                                if (i % 20 == 0) {
                                        bmds_canvas_clear(cv, px);
                                        pixels += (double)CanvasW * CanvasH;
                                }
                                break;
                        case 1:
                                bmds_canvas_blit(cv, x, y, &r->opaque, NULL);
                                pixels += Sprite * Sprite;
                                break;
                        case 2:
                                bmds_canvas_blit(cv, x, y, &r->keyed, NULL);
                                pixels += Sprite * Sprite;
                                break;
                        case 3:
                                bmds_canvas_blit_scaled(cv, x, y, 2 * Sprite, 2 * Sprite, &r->keyed, NULL);
                                pixels += 4.0 * Sprite * Sprite;
                                break;
                        case 4:
                                bmds_canvas_blit_rotated(cv, x + Sprite, y + Sprite, BMDS_GAME_TURN / 12,
                                                         BMDS_GAME_SCALE, &r->keyed, NULL);
                                pixels += Sprite * Sprite;
                                break;
                        case 5:
                                bmds_canvas_fill_blend(cv, x, y, Sprite, Sprite, 0xffffff, BMDS_GAME_OPAQUE / 2);
                                pixels += Sprite * Sprite;
                                break;
                        default:
                                bmds_canvas_text(cv, x, y, "The quick brown fox jumps", px, 1);
                                pixels += 25;
                                break;
                }
                bmds_canvas_damage_clear(cv);
        }
        const double us = (double)(bmds_bench_now_ns() - t0) / 1000;
        *out = r->op == 6 ? pixels / us * 1000 : pixels / us; /* text: characters a ms; else Mpixel/s */
        return 0;
}

/* A 64x64 picture of @p format: rings, and with @p keyed its corners the key. */
static void make_sprite(bmds_game_image_t *image, uint32_t format, int keyed, uint8_t *pixels) {
        const uint32_t bpp = format == BMDS_PIXEL_L8 ? 1 : format == BMDS_PIXEL_RGB565 ? 2 : 3;
        for (int y = 0; y < Sprite; ++y)
                for (int x = 0; x < Sprite; ++x) {
                        const int      d = (x - 32) * (x - 32) + (y - 32) * (y - 32);
                        const uint32_t v = keyed && d > 31 * 31 ? 0 : (uint32_t)(1 + (d / 64) % 200);
                        memcpy(pixels + ((size_t)y * Sprite + x) * bpp, &v, bpp);
                }
        image->pixels = pixels;
        image->stride = Sprite * bpp;
        image->width = Sprite;
        image->height = Sprite;
        image->format = format;
        image->flags = keyed ? BMDS_GAME_IMAGE_KEYED : 0;
        image->key = 0;
        return;
}

static int run_draw(void) {
        static const struct {
                        const char *name;
                        uint32_t    format, bpp;
        } formats[] = {{"l8", BMDS_PIXEL_L8, 1}, {"rgb565", BMDS_PIXEL_RGB565, 2}, {"bgr24", BMDS_PIXEL_BGR24, 3}};
        static const char *const ops[] = {"fill", "blit", "keyed", "scaled", "rotated", "blend", "text"};
        static const char *const units[] = {"Mpx/s", "Mpx/s", "Mpx/s", "Mpx/s", "Mpx/s", "Mpx/s", "chars/ms"};
        uint8_t                 *pixels = malloc(CanvasW * CanvasH * 3);
        uint8_t                 *sprites = malloc(2 * Sprite * Sprite * 3);
        if (pixels == NULL || sprites == NULL) return fail("malloc");
        int err = 0;
        for (size_t f = 0; f < 3 && err == 0; ++f) {
                struct draw_run r;
                if (bmds_canvas_init(&r.canvas, pixels, CanvasW * formats[f].bpp, CanvasW, CanvasH,
                                     formats[f].format) != 0)
                        return fail("canvas");
                make_sprite(&r.opaque, formats[f].format, 0, sprites);
                make_sprite(&r.keyed, formats[f].format, 1, sprites + Sprite * Sprite * 3);
                for (int op = 0; op < 7 && err == 0; ++op) {
                        if (op == 5 && formats[f].format == BMDS_PIXEL_L8) continue; /* an index has no blend */
                        char metric[48];
                        snprintf(metric, sizeof(metric), "%s_%s", formats[f].name, ops[op]);
                        r.op = op;
                        err = bmds_bench_measure(&bench, metric, units[op], BMDS_BENCH_HIGHER, draw_sample, &r);
                }
        }
        free(pixels);
        free(sprites);
        return err;
}

/* ---- A software renderer's inner loops (Doom's R_DrawColumn and R_DrawSpan), 320x200 L8 ---- */

enum {
        ViewW = 320,
        ViewH = 200
};

struct render_run {
                uint8_t       *frame;    /* where the frame is drawn */
                const uint8_t *textures; /* 64 KB of 128-row wall columns, then 64x64 flats */
                const uint8_t *colormap; /* a light level's 256 entries */
                int            spans;
};

static int render_sample(void *arg, double *out) {
        const struct render_run *r = arg;
        const int64_t            t0 = bmds_bench_now_ns();
        if (!r->spans) {
                /* Every column: a wall's texture column scaled to the view's height. */
                for (int x = 0; x < ViewW; ++x) {
                        const uint8_t *source = r->textures + (size_t)(x * 37 & 511) * 128;
                        uint8_t       *dest = r->frame + x;
                        uint32_t       frac = (uint32_t)x << 12;
                        const uint32_t step = (128u << 16) / ViewH * 3 / 2;
                        for (int y = 0; y < ViewH; ++y) {
                                *dest = r->colormap[source[(frac >> 16) & 127]];
                                dest += ViewW;
                                frac += step;
                        }
                }
        } else {
                /* Every row: a floor's flat sampled across, as perspective steps it. */
                const uint8_t *flat = r->textures + 65536;
                for (int y = 0; y < ViewH; ++y) {
                        uint32_t       xfrac = (uint32_t)y << 14, yfrac = (uint32_t)y << 13;
                        const uint32_t xstep = 0x4000u + (uint32_t)y * 64, ystep = 0x1800u;
                        uint8_t       *dest = r->frame + (size_t)y * ViewW;
                        for (int x = 0; x < ViewW; ++x) {
                                const uint32_t spot = ((yfrac >> 10) & (63 * 64)) + ((xfrac >> 16) & 63);
                                dest[x] = r->colormap[flat[spot]];
                                xfrac += xstep;
                                yfrac += ystep;
                        }
                }
        }
        *out = (double)(bmds_bench_now_ns() - t0) / 1e6;
        return 0;
}

static int run_render(void) {
        /* The frame and the textures in PSRAM (malloc) or internal RAM (BMDS_MEM_FAST), as a port
           might place them. */
        uint8_t *frames[2] = {malloc(ViewW * ViewH), bmds_mem_alloc(ViewW * ViewH, 64, BMDS_MEM_FAST)};
        uint8_t *textures = malloc(65536 + 4096);
        uint8_t *colormap = bmds_mem_alloc(256, 64, BMDS_MEM_FAST);
        if (frames[0] == NULL || frames[1] == NULL || textures == NULL || colormap == NULL) {
                printf("render: no memory (internal RAM for the frame and colormap)\n");
                return 1;
        }
        for (int i = 0; i < 65536 + 4096; ++i) textures[i] = (uint8_t)(i * 7 + (i >> 9));
        for (int i = 0; i < 256; ++i) colormap[i] = (uint8_t)(i / 2 + 16);
        static const char *const names[2][2] = {{"columns_psram_ms", "columns_internal_ms"},
                                                {"spans_psram_ms", "spans_internal_ms"}};
        int                      err = 0;
        for (int spans = 0; spans < 2 && err == 0; ++spans)
                for (int f = 0; f < 2 && err == 0; ++f) {
                        struct render_run r = {
                                .frame = frames[f], .textures = textures, .colormap = colormap, .spans = spans};
                        err = bmds_bench_measure(&bench, names[spans][f], "ms", BMDS_BENCH_LOWER, render_sample, &r);
                }
        free(frames[0]);
        bmds_mem_free(frames[1]);
        free(textures);
        bmds_mem_free(colormap);
        return err;
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr,
                        "usage: benchprog syscall|launch|sleep|pipe|loopback|synth|midi|images|surface|draw|render "
                        "[KEY=VALUE...]\n");
                return 2;
        }
        self = argv[0];
        bmds_bench_init(&bench, argc - 1, argv + 1);
        const char *what = argv[1];
        if (strcmp(what, "noop") == 0) return 0;
        if (strcmp(what, "syscall") == 0) return run_syscall();
        if (strcmp(what, "launch") == 0) return run_launch();
        if (strcmp(what, "sleep") == 0) return run_sleep();
        if (strcmp(what, "loopback") == 0) return run_loopback();
        if (strcmp(what, "synth") == 0) return run_synth();
        if (strcmp(what, "midi") == 0) return run_midi();
        if (strcmp(what, "images") == 0) return run_images();
        if (strcmp(what, "pipe") == 0) return run_pipe();
        if (strcmp(what, "surface") == 0) return run_surface();
        if (strcmp(what, "draw") == 0) return run_draw();
        if (strcmp(what, "render") == 0) return run_render();
        fprintf(stderr, "benchprog: %s: no such benchmark\n", what);
        return 2;
}
