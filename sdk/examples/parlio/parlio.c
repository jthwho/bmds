/**
 * @file      parlio.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * parlio: the PARLIO ports, /dev/parlioN (io/parlioN; a stream of samples each way).
 *
 *   parlio info N                 port N: its direction, width, clock and counts
 *   parlio rate N MS              a receiver's bytes against its clock, five times MS apart
 *   parlio loop TX RX BYTES [SEED] a pattern of BYTES sent on port TX while port RX reads, the two
 *                                 compared (RX wired to TX's pins and clock: monitor, clk-in=).
 *                                 The sender's clock runs on between transfers, the lines idle:
 *                                 the pattern never takes the idle value, and the receiver's idle
 *                                 samples are dropped before comparing (their runs: the gaps)
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>

static int fail(const char *what, int err) {
        fprintf(stderr, "parlio: %s: %s\n", what, strerror(err));
        return 1;
}

static int open_port(unsigned n, int flags) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/parlio%u", n);
        return open(path, flags);
}

static int info(int fd, bmds_parlio_info_t *i) {
        memset(i, 0, sizeof(*i));
        i->size = sizeof(*i);
        const int32_t r = bmds_parlio_ctl(fd, BMDS_PARLIO_INFO, i);
        return r < 0 ? fail("info", -r) : 0;
}

static double now_s(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
        if (argc >= 3 && strcmp(argv[1], "info") == 0) {
                const int fd = open_port((unsigned)strtoul(argv[2], NULL, 0), O_RDONLY);
                if (fd < 0) return fail("open", errno);
                bmds_parlio_info_t i;
                if (info(fd, &i) != 0) return 1;
                printf("parlio%u: %s, %u lines at %u Hz%s%s\n", i.port,
                       (i.flags & BMDS_PARLIO_RECEIVE) ? "receiver" : "sender", i.width, i.clock_hz,
                       (i.flags & BMDS_PARLIO_EXTERNAL_CLOCK) ? " (external clock)" : "",
                       (i.flags & BMDS_PARLIO_MONITOR) ? ", monitor" : "");
                printf("  %llu bytes, %llu transfers, %llu lost, ring %u, started %lld ns\n",
                       (unsigned long long)i.bytes, (unsigned long long)i.transfers, (unsigned long long)i.lost,
                       i.buffer, (long long)i.start_ns);
                return 0;
        }
        if (argc >= 4 && strcmp(argv[1], "rate") == 0) {
                // A receiver's count against its clock: bytes since its start, and what its rate
                // makes of the time since (a node the driver missed shows as a shortfall).
                const int fd = open_port((unsigned)strtoul(argv[2], NULL, 0), O_RDONLY);
                if (fd < 0) return fail("open", errno);
                const long ms = strtol(argv[3], NULL, 0);
                for (int k = 0; k < 5; ++k) {
                        bmds_parlio_info_t i;
                        if (info(fd, &i) != 0) return 1;
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        const double elapsed = (double)t.tv_sec + (double)t.tv_nsec / 1e9 - (double)i.start_ns / 1e9;
                        const double expect = elapsed * i.clock_hz * i.width / 8;
                        printf("%llu bytes in %.6f s: %.0f expected, %+.0f\n", (unsigned long long)i.bytes, elapsed,
                               expect, (double)i.bytes - expect);
                        const struct timespec d = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
                        nanosleep(&d, NULL);
                }
                return 0;
        }
        if (argc < 5 || strcmp(argv[1], "loop") != 0) {
                fprintf(stderr, "usage: parlio info N | rate N MS | loop TX RX BYTES [SEED]\n");
                return 2;
        }
        const size_t total = (size_t)strtoul(argv[4], NULL, 0);
        const int    tx = open_port((unsigned)strtoul(argv[2], NULL, 0), O_WRONLY | O_NONBLOCK);
        const int    rx = open_port((unsigned)strtoul(argv[3], NULL, 0), O_RDONLY | O_NONBLOCK);
        if (tx < 0 || rx < 0) return fail("open", errno);
        bmds_parlio_info_t ti, before;
        if (info(tx, &ti) != 0) return 1;
        const unsigned width = ti.width;
        const uint32_t mask = width == 16 ? 0xffffu : (1u << width) - 1u;
        const size_t   samples = total * 8 / width;
        // A pattern of samples that never takes the idle value: the receiver, on the sender's
        // clock, samples the idle lines between transfers too, and those are told apart by it.
        uint8_t *sent = calloc(total, 1), *got = malloc(total * 3);
        if (sent == NULL || got == NULL) return fail("memory", ENOMEM);
        uint32_t x = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0x12345678u;
        for (size_t i = 0; i < samples; ++i) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                uint32_t v = x & mask;
                if (v == ti.idle) v ^= 1u;
                if (width == 16) {
                        sent[2 * i] = (uint8_t)v;
                        sent[2 * i + 1] = (uint8_t)(v >> 8);
                } else {
                        sent[i * width / 8] |= (uint8_t)(v << (i * width % 8));
                }
        }
        // What the receiver held before: read away.
        uint8_t scrap[4096];
        while (read(rx, scrap, sizeof(scrap)) > 0) {}
        if (info(rx, &before) != 0) return 1; // what it lost before this test counted by now
        size_t       out = 0, in = 0;
        const double start = now_s();
        double       sentAt = start, quiet = 0, slowest = 0;
        for (;;) {
                struct pollfd p[2] = {{.fd = rx, .events = POLLIN}, {.fd = tx, .events = out < total ? POLLOUT : 0}};
                (void)poll(p, 2, 100);
                if (out < total && (p[1].revents & POLLOUT)) {
                        const double w0 = now_s();
                        // All that's left: a non-blocking write takes what fits and returns.
                        const ssize_t n = write(tx, sent + out, total - out);
                        if (now_s() - w0 > slowest) slowest = now_s() - w0;
                        if (n < 0 && errno != EAGAIN) return fail("write", errno);
                        if (n > 0) out += (size_t)n;
                        if (out == total) sentAt = now_s();
                }
                if (p[0].revents & POLLIN) {
                        const ssize_t n = read(rx, got + in, total * 3 - in);
                        if (n < 0 && errno != EAGAIN) return fail("read", errno);
                        if (n > 0) in += (size_t)n;
                }
                // Done once all is sent and a tenth of a second has passed after.
                if (out == total && quiet == 0) quiet = now_s() + 0.1;
                if ((quiet != 0 && now_s() > quiet) || in == total * 3 || now_s() - start > 30) break;
        }
        bmds_parlio_info_t after;
        if (info(rx, &after) != 0) return 1;
        // The received samples, idle runs dropped (their lengths kept: the gaps between transfers).
        const size_t got_samples = in * 8 / width;
        size_t       k = 0, matched = 0, run = 0, gaps = 0, gapMin = (size_t)-1, gapMax = 0, gapSum = 0;
        int          started = 0;
        for (size_t i = 0; i < got_samples && k < samples; ++i) {
                const uint32_t v = width == 16 ? (uint32_t)(got[2 * i] | got[2 * i + 1] << 8)
                                               : (got[i * width / 8] >> (i * width % 8)) & mask;
                if (v == ti.idle) {
                        ++run;
                        continue;
                }
                if (started && run > 0) {
                        ++gaps;
                        gapSum += run;
                        if (run < gapMin) gapMin = run;
                        if (run > gapMax) gapMax = run;
                }
                started = 1;
                run = 0;
                const uint32_t want = width == 16 ? (uint32_t)(sent[2 * k] | sent[2 * k + 1] << 8)
                                                  : (sent[k * width / 8] >> (k * width % 8)) & mask;
                if (v != want) {
                        printf("sample %zu (received %zu): %x, sent %x; idle %x\nreceived:", k, i, v, want, ti.idle);
                        for (size_t j = i >= 12 ? i - 12 : 0; j < i + 12 && j < got_samples; ++j)
                                printf(j == i ? " [%x]" : " %x",
                                       width == 16 ? (unsigned)(got[2 * j] | got[2 * j + 1] << 8)
                                                   : (unsigned)((got[j * width / 8] >> (j * width % 8)) & mask));
                        printf("\nsent:    ");
                        for (size_t j = k >= 6 ? k - 6 : 0; j < k + 12 && j < samples; ++j)
                                printf(j == k ? " [%x]" : " %x",
                                       width == 16 ? (unsigned)(sent[2 * j] | sent[2 * j + 1] << 8)
                                                   : (unsigned)((sent[j * width / 8] >> (j * width % 8)) & mask));
                        printf("\n");
                        // Where in the pattern the received run came from (stale data shows as an
                        // offset back: a sender's buffers or a receiver's ring).
                        size_t   run = 0;
                        uint32_t want16[16];
                        for (size_t j = i; j < got_samples && run < 16; ++j) {
                                const uint32_t w = width == 16 ? (uint32_t)(got[2 * j] | got[2 * j + 1] << 8)
                                                               : (got[j * width / 8] >> (j * width % 8)) & mask;
                                if (w != ti.idle) want16[run++] = w;
                        }
                        for (size_t at = 0; run == 16 && at + 16 <= samples; ++at) {
                                size_t m = 0;
                                while (m < 16) {
                                        const size_t   q = at + m;
                                        const uint32_t sv = width == 16
                                                                    ? (uint32_t)(sent[2 * q] | sent[2 * q + 1] << 8)
                                                                    : (sent[q * width / 8] >> (q * width % 8)) & mask;
                                        if (sv != want16[m]) break;
                                        ++m;
                                }
                                if (m == 16)
                                        printf("received from here on is sent sample %zu (%+lld)\n", at,
                                               (long long)at - (long long)k);
                        }
                        break;
                }
                ++k;
                ++matched;
        }
        printf("sent %zu samples (%zu bytes, %.2f MB/s queued), received %zu; %s", samples, out,
               (double)out / (sentAt - start) / 1e6, got_samples, matched == samples ? "every sample the same" : "");
        if (matched != samples) printf("the same for the first %zu", matched);
        printf("; the receiver lost %llu bytes\n", (unsigned long long)(after.lost - before.lost));
        printf("the slowest write: %.3f ms\n", slowest * 1e3);
        if (gaps > 0)
                printf("gaps between transfers: %zu, %zu to %zu samples (%.1f to %.1f us at %u Hz), %.1f on average\n",
                       gaps, gapMin, gapMax, gapMin * 1e6 / ti.clock_hz, gapMax * 1e6 / ti.clock_hz, ti.clock_hz,
                       (double)gapSum / (double)gaps);
        return matched == samples ? 0 : 1;
}
