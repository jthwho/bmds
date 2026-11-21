/**
 * @file      ltc.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ltc: SMPTE 12M linear timecode on BMDS's RMT ports (devplan/io.md §13.1).
 *
 *   ltc send DEVICE [FPS] [SECONDS] [-d SPIDEV] [-j TICKS]
 *                                     frames from the time of day, biphase mark, each behind a mark,
 *                                     written to a sending port (`continuous` for no gaps); each
 *                                     mark the wire reaches printed (bit 0's time), and with -d the
 *                                     next frame's digits shifted out to a seven-segment display
 *                                     (latched at the next mark by the port's `pulse=`); -j makes
 *                                     each even frame TICKS longer and each odd one TICKS shorter
 *   ltc runs DEVICE [SECONDS]         each run's first edge from a receiving port's headers (a
 *                                     pulse pin monitored: each pulse a run)
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

/* A seven-segment digit (a to g in bits 0 to 6), for a display of shift registers. */
static const uint8_t segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

/* HH:MM:SS:FF to a display: eight digits, hours first, shifted out over SPI. */
static void showFrame(int display, const timecode_t *tc) {
        const unsigned v[4] = {tc->hours, tc->minutes, tc->seconds, tc->frames};
        uint8_t        out[8];
        for (int i = 0; i < 4; i++) {
                out[2 * i] = segments[v[i] / 10];
                out[2 * i + 1] = segments[v[i] % 10];
        }
        if (write(display, out, sizeof(out)) < 0) fprintf(stderr, "ltc: the display: %s\n", strerror(errno));
}

static void printTimecode(const char *what, const timecode_t *tc, long long ns) {
        printf("%02u:%02u:%02u:%02u %s %lld ns\n", tc->hours, tc->minutes, tc->seconds, tc->frames, what, ns);
        fflush(stdout);
}

static int sendLtc(const char *device, unsigned fps, unsigned seconds, const char *displayPath, unsigned jiggle) {
        const int fd = open(device, O_RDWR);
        if (fd < 0) return fail(device);
        bmds_rmt_info_t info = {.size = sizeof(info)};
        if (bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info) < 0) return fail("BMDS_RMT_INFO");
        if (info.flags & BMDS_RMT_RECEIVE) {
                fprintf(stderr, "ltc: %s receives: send needs a sending port (dir=tx)\n", device);
                return 1;
        }
        /* Marks are numbered from the port's start: this program's first is the next. A record
           below it is an earlier program's, left unread; a stream still running (a continuous
           port another program fed) keeps its start, and these frames follow its filler. */
        const uint64_t base = info.marks_written;
        if (info.flags & BMDS_RMT_SENDING) printf("the stream goes on from %lld ns\n", (long long)info.start_ns);
        const int display = displayPath != NULL ? open(displayPath, O_WRONLY) : -1;
        if (displayPath != NULL && display < 0) return fail(displayPath);
        const uint64_t halves = 160ull * fps; /* half bits a second */
        const uint64_t res = info.resolution_hz;
        time_t         now = time(NULL);
        struct tm      tm;
        gmtime_r(&now, &tm);
        timecode_t       tc = {(unsigned)tm.tm_hour, (unsigned)tm.tm_min, (unsigned)tm.tm_sec, 0};
        const timecode_t first = tc;
        uint64_t         half = 0;   /* half bits so far */
        uint64_t         tickAt = 0; /* the tick the next run starts at */
        int64_t          moved = 0;  /* ticks added (or taken, below 0) so far: -j */
        /* The first run away from idle, so the first bit's edge is there. */
        unsigned       level = (info.flags & BMDS_RMT_IDLE_HIGH) ? 0u : 1u;
        uint32_t       out[160];
        unsigned       marksSeen = 0;
        const unsigned frames = seconds * fps;
        if (display >= 0) showFrame(display, &tc); /* frame 0's digits, latched at its mark */
        /* A stream's start is known only once it has started: 1 ms of idle first, so frame 0's
           mark (and its pulse) is timed before it is due, as every later one is. */
        {
                const uint32_t idle = (info.flags & BMDS_RMT_IDLE_HIGH) ? 1u : 0u;
                const uint32_t lead = (uint32_t)(res / 2000);
                const uint32_t symbol = (lead & 0x7fffu) | idle << 15 | (lead & 0x7fffu) << 16 | idle << 31;
                if (info.underruns == 0 && info.symbols == 0 && write(fd, &symbol, sizeof(symbol)) < 0)
                        return fail("write");
        }
        for (unsigned f = 0; f < frames; f++, nextFrame(&tc, fps)) {
                /* A mark at bit 0's edge, then the frame's symbols: its runs two to a symbol, the
                   last split over a symbol of its own if it has no partner (same level, no edge),
                   so the next frame's mark falls on a symbol too. */
                size_t n = 0;
                out[n++] = BMDS_RMT_MARK;
                uint8_t bits[80];
                frameBits(&tc, bits);
                uint32_t pending = 0;
                int      havePending = 0;
                for (unsigned b = 0; b < 80; b++) {
                        for (unsigned r = 0; r < (bits[b] ? 2u : 1u); r++) {
                                half += bits[b] ? 1 : 2;
                                const uint64_t end = half * res / halves;
                                uint32_t       ticks = (uint32_t)(end - tickAt);
                                tickAt = end;
                                if (jiggle != 0 && b == 79 && r + 1 == (bits[b] ? 2u : 1u)) {
                                        /* -j: this frame's last run longer, the next one's shorter. */
                                        const int64_t d = (f % 2 == 0) ? (int64_t)jiggle : -(int64_t)jiggle;
                                        ticks = (uint32_t)((int64_t)ticks + d);
                                        moved += d;
                                }
                                const uint32_t run = (ticks & 0x7fffu) | (uint32_t)level << 15;
                                level ^= 1u;
                                if (!havePending) {
                                        pending = run;
                                        havePending = 1;
                                        continue;
                                }
                                out[n++] = pending | run << 16;
                                havePending = 0;
                        }
                }
                if (havePending) {
                        const uint32_t ticks = pending & 0x7fffu, l = pending & 0x8000u;
                        out[n++] = (ticks / 2) | l | ((ticks - ticks / 2) | l) << 16;
                }
                if (write(fd, out, n * sizeof(out[0])) < 0) return fail("write");
                if (f == 0) {
                        bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info);
                        printf("sending %02u:%02u:%02u:%02u at %u fps on %s (channel %d), stream started at %lld "
                               "ns%s\n",
                               first.hours, first.minutes, first.seconds, first.frames, fps, device, (int)info.channel,
                               (long long)info.start_ns, info.pulse_pin >= 0 ? ", a pulse at each frame" : "");
                        fflush(stdout);
                }
                /* The marks the wire has reached: frame N's bit 0 went out then. Its display was
                   shifted after the mark before, and the pulse latched it; now the next one's. */
                struct pollfd p = {fd, POLLIN, 0};
                while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
                        bmds_rmt_mark_t m;
                        if (read(fd, &m, sizeof(m)) != (ssize_t)sizeof(m)) break;
                        if (m.number < base) continue; /* an earlier program's */
                        timecode_t at = first;
                        for (uint64_t k = base; k < m.number; k++) nextFrame(&at, fps);
                        if (marksSeen < 5 || at.frames == 0) printTimecode("mark at", &at, (long long)m.time_ns);
                        marksSeen++;
                        if (display >= 0) {
                                nextFrame(&at, fps);
                                showFrame(display, &at);
                        }
                }
        }
        bmds_rmt_ctl(fd, BMDS_RMT_INFO, &info);
        printf("sent %u frames: %llu symbols, %llu ticks (%llu idle filler, %lld moved by -j), %u underruns; %llu "
               "marks reached, %u lost\n",
               frames, (unsigned long long)info.symbols, (unsigned long long)info.ticks,
               (unsigned long long)info.filler_ticks, (long long)moved, (unsigned)info.underruns,
               (unsigned long long)info.marks, (unsigned)info.marks_lost);
        if (display >= 0) close(display);
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
                                if (shown < 5 ||
                                    getBits(f, 0, 4) + 10 * getBits(f, 8, 2) == 0) { /* the first, then each second */
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

/* Each run's first edge, as its header says (a pulse pin monitored: each pulse a run of its own). */
static int printRuns(const char *device, unsigned seconds) {
        const int fd = open(device, O_RDONLY);
        if (fd < 0) return fail(device);
        const int64_t until = monotonicNs() + (int64_t)seconds * 1000000000;
        uint32_t      words[64];
        unsigned      runs = 0;
        int           header = 0;
        uint32_t      low = 0;
        while (monotonicNs() < until) {
                struct pollfd p = {fd, POLLIN, 0};
                if (poll(&p, 1, 200) <= 0) continue;
                const ssize_t got = read(fd, words, sizeof(words));
                if (got < 0) return fail("read");
                for (ssize_t w = 0; w < got / 4; w++) {
                        if (header == 2) {
                                low = words[w];
                                header = 1;
                        } else if (header == 1) {
                                const int64_t t = (int64_t)((uint64_t)words[w] << 32 | low);
                                if (runs < 5 || runs % 25 == 0) printf("run %u at %lld ns\n", runs, (long long)t);
                                fflush(stdout);
                                runs++;
                                header = 0;
                        } else if (words[w] == BMDS_RMT_HEADER) {
                                header = 2;
                        }
                }
        }
        printf("%u runs\n", runs);
        close(fd);
        return 0;
}

int main(int argc, char **argv) {
        const char *display = NULL;
        unsigned    jiggle = 0;
        const char *args[5];
        int         count = 0;
        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
                        display = argv[++i];
                else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
                        jiggle = (unsigned)atoi(argv[++i]);
                else if (count < 5)
                        args[count++] = argv[i];
                else
                        count = 6;
        }
        if (count >= 2 && strcmp(args[0], "runs") == 0)
                return printRuns(args[1], count > 2 ? (unsigned)atoi(args[2]) : 5);
        if (count < 2 || count > 4 || (strcmp(args[0], "send") != 0 && strcmp(args[0], "read") != 0)) {
                fprintf(stderr, "usage: ltc send DEVICE [FPS] [SECONDS] [-d SPIDEV] [-j TICKS] | read DEVICE [FPS] "
                                "[SECONDS] | runs DEVICE [SECONDS]\n");
                return 2;
        }
        const unsigned fps = count > 2 ? (unsigned)atoi(args[2]) : 25;
        const unsigned seconds = count > 3 ? (unsigned)atoi(args[3]) : 5;
        if (fps != 24 && fps != 25 && fps != 30) {
                fprintf(stderr, "ltc: FPS is 24, 25 or 30\n");
                return 2;
        }
        return strcmp(args[0], "send") == 0 ? sendLtc(args[1], fps, seconds, display, jiggle)
                                            : readLtc(args[1], fps, seconds);
}
