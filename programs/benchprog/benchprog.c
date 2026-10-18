/**
 * @file      benchprog.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * benchprog: the system as programs see it, measured for `bench` (devplan/bench.md §4f, §12):
 * system calls, starting a program, sleeps' lateness, TCP over loopback, the synth's load, a MIDI
 * file decoded, pictures encoded and decoded. `benchprog WHAT [KEY=VALUE...]`, run by `bench` as
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
#include <bmds/sys.h>

extern char **environ;

static bmds_bench_t bench;
static const char  *self = "benchprog";

static int fail(const char *what) {
        printf("%s: %s\n", what, strerror(errno));
        return 1;
}

/* ---- System calls ---- */

static int syscall_sample(void *arg, double *out) {
        (void)arg;
        enum {
                Calls = 20000
        };
        const int64_t t0 = bmds_bench_now_ns();
        for (int i = 0; i < Calls; ++i) (void)getppid();
        *out = (double)(bmds_bench_now_ns() - t0) / Calls;
        return 0;
}

static int run_syscall(void) {
        return bmds_bench_measure(&bench, "getppid_ns", "ns", BMDS_BENCH_LOWER, syscall_sample, NULL);
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
        r.out = malloc(ImageW * ImageH * 3);
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

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr, "usage: benchprog syscall|launch|sleep|loopback|synth|midi|images [KEY=VALUE...]\n");
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
        fprintf(stderr, "benchprog: %s: no such benchmark\n", what);
        return 2;
}
