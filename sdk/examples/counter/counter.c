/**
 * @file      counter.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * counter: the counts of /dev/counter0 (io/pcntN), their events through Linux's counter character
 * device (linux/counter.h: watches, then read()), and what Linux keeps in sysfs through
 * counterdev_ctl (a count's value and time, its preset and threshold).
 *
 *   counter info                 each count: its function, pins, threshold
 *   counter read N [MS [TIMES]]  N's value; with MS, read again every MS ms with the rate between
 *   counter preset N VALUE       N set to VALUE
 *   counter watch N AT [MS]      N's threshold set to AT, its THRESHOLD events (with N's value)
 *                                read for MS ms (5000), then the threshold cleared
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>
#include <linux/counter.h>

static const char *const functions[] = {"increase",        "decrease",        "pulse-direction", "quadrature-x1-a",
                                        "quadrature-x1-b", "quadrature-x2-a", "quadrature-x2-b", "quadrature-x4"};

static int fail(const char *what, int err) {
        fprintf(stderr, "counter: %s: %s\n", what, strerror(err));
        return 1;
}

static int usage(void) {
        fprintf(stderr, "usage: counter info | read N [MS [TIMES]] | preset N VALUE | watch N AT [MS]\n");
        return 2;
}

static int ctl(int fd, uint32_t op, void *arg, const char *what) {
        const int32_t r = bmds_counterdev_ctl(fd, op, arg);
        if (r < 0) {
                fail(what, -r);
                return -1;
        }
        return 0;
}

static void sleep_ms(long ms) {
        const struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
        nanosleep(&t, NULL);
        return;
}

int main(int argc, char **argv) {
        if (argc < 2) return usage();
        const int dev = open("/dev/counter0", O_RDWR);
        if (dev < 0) return fail("/dev/counter0", errno);
        const char *cmd = argv[1];
        if (strcmp(cmd, "info") == 0) {
                for (uint32_t n = 0;; ++n) {
                        bmds_counterdev_info_t i = {.size = sizeof(i), .count = n};
                        if (ctl(dev, BMDS_COUNTERDEV_INFO, &i, "info") < 0) return 1;
                        if (!(i.flags & BMDS_COUNTERDEV_CONFIGURED)) {
                                printf("%u: not set up (io/pcnt%u)\n", n, n);
                        } else {
                                printf("%u: %s, A GPIO%d", n, i.function < 8 ? functions[i.function] : "?",
                                       (int)i.pin_a);
                                if (i.pin_b >= 0) printf(", B GPIO%d", (int)i.pin_b);
                                if (i.filter_ns) printf(", filter %u ns", i.filter_ns);
                                if (i.flags & BMDS_COUNTERDEV_MONITOR) printf(", monitor");
                                if (i.flags & BMDS_COUNTERDEV_HAS_THRESHOLD)
                                        printf(", threshold %lld", (long long)i.threshold);
                                printf("\n");
                        }
                        if (n + 1 >= i.ncounts) {
                                if (i.events_lost) printf("events lost: %llu\n", (unsigned long long)i.events_lost);
                                break;
                        }
                }
                return 0;
        }
        if (argc < 3) return usage();
        const uint32_t n = (uint32_t)strtoul(argv[2], NULL, 0);
        if (strcmp(cmd, "read") == 0) {
                bmds_counterdev_count_t c = {.size = sizeof(c), .count = n};
                if (ctl(dev, BMDS_COUNTERDEV_READ, &c, "read") < 0) return 1;
                printf("%u: %lld\n", n, (long long)c.value);
                if (argc < 4) return 0;
                const long ms = strtol(argv[3], NULL, 0);
                const long times = argc > 4 ? strtol(argv[4], NULL, 0) : 5;
                for (long k = 0; k < times; ++k) {
                        bmds_counterdev_count_t d = {.size = sizeof(d), .count = n};
                        sleep_ms(ms);
                        if (ctl(dev, BMDS_COUNTERDEV_READ, &d, "read") < 0) return 1;
                        const double s = (double)(d.time_ns - c.time_ns) / 1e9;
                        printf("%u: %lld, %+lld in %.6f s: %.3f a second\n", n, (long long)d.value,
                               (long long)(d.value - c.value), s, (double)(d.value - c.value) / s);
                        c = d;
                }
                return 0;
        }
        if (argc < 4) return usage();
        const long long v = strtoll(argv[3], NULL, 0);
        if (strcmp(cmd, "preset") == 0) {
                bmds_counterdev_count_t c = {.size = sizeof(c), .count = n, .value = v};
                return ctl(dev, BMDS_COUNTERDEV_PRESET, &c, "preset") < 0 ? 1 : 0;
        }
        if (strcmp(cmd, "watch") != 0) return usage();
        const long              ms = argc > 4 ? strtol(argv[4], NULL, 0) : 5000;
        bmds_counterdev_count_t t = {.size = sizeof(t), .count = n, .value = v, .flags = BMDS_COUNTERDEV_ARMED};
        if (ctl(dev, BMDS_COUNTERDEV_THRESHOLD, &t, "threshold") < 0) return 1;
        struct counter_watch w;
        memset(&w, 0, sizeof(w));
        w.component.type = COUNTER_COMPONENT_COUNT;
        w.component.scope = COUNTER_SCOPE_COUNT;
        w.component.parent = (uint8_t)n;
        w.event = COUNTER_EVENT_THRESHOLD;
        w.channel = (uint8_t)n;
        if (ioctl(dev, COUNTER_ADD_WATCH_IOCTL, &w) < 0) return fail("COUNTER_ADD_WATCH_IOCTL", errno);
        if (ioctl(dev, COUNTER_ENABLE_EVENTS_IOCTL) < 0) return fail("COUNTER_ENABLE_EVENTS_IOCTL", errno);
        printf("watching count %u for %lld, %ld ms\n", n, v, ms);
        fflush(stdout);
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        unsigned long seen = 0;
        long long     last = 0;
        for (;;) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                const long left =
                        ms - (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
                if (left <= 0) break;
                struct pollfd p = {.fd = dev, .events = POLLIN};
                if (poll(&p, 1, (int)left) <= 0) continue;
                struct counter_event e[16];
                const ssize_t        got = read(dev, e, sizeof(e));
                if (got < 0) return fail("read", errno);
                bmds_counterdev_count_t c = {.size = sizeof(c), .count = n};
                (void)bmds_counterdev_ctl(dev, BMDS_COUNTERDEV_READ, &c);
                for (size_t i = 0; i < (size_t)got / sizeof(e[0]); ++i) {
                        if (seen < 5 || seen % 1000 == 0)
                                printf("event %lu: count %u at %lld, %llu ns (+%lld ns); read %lld ns later: %+lld\n",
                                       seen, e[i].watch.channel, (long long)e[i].value,
                                       (unsigned long long)e[i].timestamp,
                                       seen ? (long long)e[i].timestamp - last : 0LL,
                                       (long long)(c.time_ns - (int64_t)e[i].timestamp),
                                       (long long)(c.value - (int64_t)e[i].value));
                        last = (long long)e[i].timestamp;
                        ++seen;
                }
        }
        printf("%lu events\n", seen);
        (void)ioctl(dev, COUNTER_DISABLE_EVENTS_IOCTL);
        t.flags = 0;
        (void)bmds_counterdev_ctl(dev, BMDS_COUNTERDEV_THRESHOLD, &t);
        return 0;
}
