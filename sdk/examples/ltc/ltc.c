/**
 * @file      ltc.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ltc: SMPTE 12M linear timecode on BMDS's RMT ports (devplan/io.md §13.1).
 *
 *   ltc send DEVICE [FPS] [SECONDS]   frames from the time of day, biphase mark, written to a
 *                                     sending port (`continuous` for no gaps)
 *   ltc read DEVICE [FPS] [SECONDS]   frames decoded from a receiving port, each frame's first
 *                                     edge timed from the stream's headers
 *
 * A frame is 80 bits: BCD frames, seconds, minutes and hours with user bits between, then the
 * sync word 0011 1111 1111 1101. Biphase mark: every bit starts with a transition, and a 1 has
 * one more in its middle. The ticks of each edge are worked out from the start, so a frame rate
 * the resolution doesn't divide (30 at 1 MHz: 208 1/3 ticks a half bit) never drifts.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>

static int fail(const char *what) {
        fprintf(stderr, "ltc: %s: %s\n", what, strerror(errno));
        return 1;
}

static int64_t monotonicNs(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* ---- Frames */

typedef struct {
                unsigned hours, minutes, seconds, frames;
} timecode_t;

static void setBits(uint8_t *bits, unsigned first, unsigned count, unsigned value) {
        for (unsigned i = 0; i < count; i++) bits[first + i] = (uint8_t)((value >> i) & 1u);
}

static unsigned getBits(const uint8_t *bits, unsigned first, unsigned count) {
        unsigned v = 0;
        for (unsigned i = 0; i < count; i++) v |= (unsigned)bits[first + i] << i;
        return v;
}

static void frameBits(const timecode_t *tc, uint8_t bits[80]) {
        memset(bits, 0, 80);
        setBits(bits, 0, 4, tc->frames % 10);
        setBits(bits, 8, 2, tc->frames / 10);
        setBits(bits, 16, 4, tc->seconds % 10);
        setBits(bits, 24, 3, tc->seconds / 10);
        setBits(bits, 32, 4, tc->minutes % 10);
        setBits(bits, 40, 3, tc->minutes / 10);
        setBits(bits, 48, 4, tc->hours % 10);
        setBits(bits, 56, 2, tc->hours / 10);
        static const uint8_t sync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
        memcpy(bits + 64, sync, sizeof(sync));
}

static void nextFrame(timecode_t *tc, unsigned fps) {
        if (++tc->frames < fps) return;
        tc->frames = 0;
        if (++tc->seconds < 60) return;
        tc->seconds = 0;
        if (++tc->minutes < 60) return;
        tc->minutes = 0;
        tc->hours = (tc->hours + 1) % 24;
}

/* ---- Sending */

static int sendLtc(const char *device, unsigned fps, unsigned seconds) {
        const int fd = open(device, O_WRONLY);
        if (fd < 0) return fail(device);
        bmds_rmt_info_t info = {.size = sizeof(info)};
        if (bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info) < 0) return fail("BMDS_RMT_INFO");
        if (info.flags & BMDS_RMT_RECEIVE) {
                fprintf(stderr, "ltc: %s receives: send needs a sending port (dir=tx)\n", device);
                return 1;
        }
        const uint64_t halves = 160ull * fps; /* half bits a second */
        const uint64_t res = info.resolution_hz;
        time_t         now = time(NULL);
        struct tm      tm;
        gmtime_r(&now, &tm);
        timecode_t       tc = {(unsigned)tm.tm_hour, (unsigned)tm.tm_min, (unsigned)tm.tm_sec, 0};
        const timecode_t first = tc;
        uint64_t         half = 0;   /* half bits so far */
        uint64_t         tickAt = 0; /* the tick the next run starts at */
        /* The level of the next run: the first away from idle, so the first bit's edge is there. */
        unsigned       level = (info.flags & BMDS_RMT_IDLE_HIGH) ? 0u : 1u;
        uint32_t       pending = 0; /* a run waiting for its partner in a symbol */
        int            havePending = 0;
        uint32_t       out[256];
        size_t         n = 0;
        const unsigned frames = seconds * fps;
        for (unsigned f = 0; f < frames; f++, nextFrame(&tc, fps)) {
                uint8_t bits[80];
                frameBits(&tc, bits);
                for (unsigned b = 0; b < 80; b++) {
                        /* A 0: one run of the whole bit; a 1: two runs of half each. */
                        for (unsigned r = 0; r < (bits[b] ? 2u : 1u); r++) {
                                half += bits[b] ? 1 : 2;
                                const uint64_t end = half * res / halves;
                                const uint32_t ticks = (uint32_t)(end - tickAt);
                                tickAt = end;
                                const uint32_t run = (ticks & 0x7fffu) | (uint32_t)level << 15;
                                level ^= 1u;
                                if (!havePending) {
                                        pending = run;
                                        havePending = 1;
                                        continue;
                                }
                                out[n++] = pending | run << 16;
                                havePending = 0;
                                if (n == sizeof(out) / sizeof(out[0])) {
                                        if (write(fd, out, n * sizeof(out[0])) < 0) return fail("write");
                                        n = 0;
                                }
                        }
                }
                if (f == 0) {
                        if (write(fd, out, n * sizeof(out[0])) < 0) return fail("write");
                        n = 0;
                        bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info);
                        printf("sending %02u:%02u:%02u:%02u at %u fps on %s (channel %d), stream started at %lld ns\n",
                               first.hours, first.minutes, first.seconds, first.frames, fps, device, (int)info.channel,
                               (long long)info.start_ns);
                        fflush(stdout);
                }
        }
        if (havePending) out[n++] = pending | (uint32_t)(level ^ 1u) << 31 | 1u << 16;
        if (n > 0 && write(fd, out, n * sizeof(out[0])) < 0) return fail("write");
        bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info);
        printf("sent %u frames: %llu symbols, %llu ticks (%llu idle filler), %u underruns\n", frames,
               (unsigned long long)info.symbols, (unsigned long long)info.ticks, (unsigned long long)info.filler_ticks,
               (unsigned)info.underruns);
        close(fd);
        return 0;
}

/* ---- Reading */

static int readLtc(const char *device, unsigned fps, unsigned seconds) {
        const int fd = open(device, O_RDONLY);
        if (fd < 0) return fail(device);
        bmds_rmt_info_t info = {.size = sizeof(info)};
        if (bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info) < 0) return fail("BMDS_RMT_INFO");
        const double  cell = (double)info.resolution_hz / (80.0 * fps); /* ticks a bit */
        int64_t       anchorNs = 0;                                     /* the current run's first edge */
        uint64_t      tick = 0;                                         /* ticks since then */
        int           anchored = 0;
        int           half = 0; /* a half bit seen, waiting for its pair */
        uint8_t       bits[80];
        uint64_t      starts[80]; /* each bit's first tick, as a ring with bits */
        unsigned      have = 0;
        unsigned      frames = 0, shown = 0;
        int64_t       firstNs = 0, lastNs = 0;
        double        spread = 0;
        const int64_t until = monotonicNs() + (int64_t)seconds * 1000000000;
        uint32_t      words[256];
        uint32_t      headerWords = 0, headerLow = 0;
        while (monotonicNs() < until) {
                struct pollfd p = {fd, POLLIN, 0};
                if (poll(&p, 1, 200) <= 0) continue;
                const ssize_t got = read(fd, words, sizeof(words));
                if (got < 0) return fail("read");
                for (ssize_t w = 0; w < got / 4; w++) {
                        const uint32_t s = words[w];
                        if (headerWords > 0) { /* a header's time: low word, then high */
                                if (headerWords == 2) {
                                        headerLow = s;
                                } else {
                                        anchorNs = (int64_t)((uint64_t)s << 32 | headerLow);
                                        tick = 0;
                                        anchored = 1;
                                        half = 0;
                                        have = 0;
                                }
                                headerWords--;
                                continue;
                        }
                        if (s == BMDS_RMT_HEADER) {
                                headerWords = 2;
                                continue;
                        }
                        if (!anchored) continue;
                        for (int k = 0; k < 2; k++) {
                                const uint32_t d = k == 0 ? BMDS_RMT_DURATION0(s) : BMDS_RMT_DURATION1(s);
                                if (d == 0) { /* the run ended: the input went quiet */
                                        anchored = 0;
                                        break;
                                }
                                const uint64_t at = tick;
                                tick += d;
                                unsigned bit;
                                if (d > cell * 0.75) {
                                        bit = 0;
                                        half = 0;
                                } else if (!half) {
                                        half = 1;
                                        starts[have % 80] = at; /* a 1's first half: the bit starts here */
                                        continue;
                                } else {
                                        half = 0;
                                        bit = 1;
                                }
                                if (bit == 0) starts[have % 80] = at;
                                bits[have % 80] = (uint8_t)bit;
                                have++;
                                if (have < 80) continue;
                                /* The last 80 bits, oldest first: a frame if they end in the sync word. */
                                uint8_t f[80];
                                for (unsigned i = 0; i < 80; i++) f[i] = bits[(have + i) % 80];
                                if (getBits(f, 64, 16) != 0xbffcu) continue;
                                const int64_t frameNs = anchorNs + (int64_t)((double)starts[have % 80] * 1e9 /
                                                                             (double)info.resolution_hz);
                                const int64_t period = 1000000000ll / fps;
                                if (frames > 0) {
                                        const double off = (double)(frameNs - lastNs - period);
                                        if (off > spread || -off > spread) spread = off < 0 ? -off : off;
                                } else {
                                        firstNs = frameNs;
                                }
                                lastNs = frameNs;
                                frames++;
                                if (shown < 5 || frames % (fps * 5) == 0) {
                                        shown++;
                                        printf("%02u:%02u:%02u:%02u at %lld ns\n",
                                               getBits(f, 48, 4) + 10 * getBits(f, 56, 2),
                                               getBits(f, 32, 4) + 10 * getBits(f, 40, 3),
                                               getBits(f, 16, 4) + 10 * getBits(f, 24, 3),
                                               getBits(f, 0, 4) + 10 * getBits(f, 8, 2), (long long)frameNs);
                                        fflush(stdout);
                                }
                                have = 0;
                        }
                }
        }
        bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info);
        printf("read %u frames, the first at %lld ns; frame to frame within %.0f ns of 1/%u s; %llu symbols, %u "
               "overruns\n",
               frames, (long long)firstNs, spread, fps, (unsigned long long)info.symbols, (unsigned)info.overruns);
        close(fd);
        return 0;
}

int main(int argc, char **argv) {
        if (argc < 3 || argc > 5 || (strcmp(argv[1], "send") != 0 && strcmp(argv[1], "read") != 0)) {
                fprintf(stderr, "usage: ltc send|read DEVICE [FPS] [SECONDS]\n");
                return 2;
        }
        const unsigned fps = argc > 3 ? (unsigned)atoi(argv[3]) : 25;
        const unsigned seconds = argc > 4 ? (unsigned)atoi(argv[4]) : 5;
        if (fps != 24 && fps != 25 && fps != 30) {
                fprintf(stderr, "ltc: FPS is 24, 25 or 30\n");
                return 2;
        }
        return strcmp(argv[1], "send") == 0 ? sendLtc(argv[2], fps, seconds) : readLtc(argv[2], fps, seconds);
}
