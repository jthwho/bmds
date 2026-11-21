/**
 * @file      gpioline.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * gpioline: lines of /dev/gpiochip0 through Linux's GPIO v2 interface only (linux/gpio.h, the
 * SDK's ioctl() on gpio_ctl): requested, read, set, held, and their edges read from the line fd.
 *
 *   gpioline get N...        the lines as inputs, their values
 *   gpioline set N=V...      the lines as outputs at those values, read back
 *   gpioline hold N MS       N as an output for MS ms (another request of it is EBUSY meanwhile)
 *   gpioline bias N [COUNT]  N as an input watching both edges, its pull flipped COUNT times
 *                            (SET_CONFIG): an unwired pad follows its pull, so its edges come
 *                            without wires; each edge read is printed
 *   gpioline watch N MS      N's info watched for MS ms (GPIO_V2_GET_LINEINFO_WATCH_IOCTL): its
 *                            info, then each change read from the chip fd
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
#include <linux/gpio.h>

static int fail(const char *what) {
        fprintf(stderr, "gpioline: %s: %s\n", what, strerror(errno));
        return 1;
}

/* A request of @p count lines with @p flags for all; output values from @p values. */
static int request(int chip, const unsigned *offsets, unsigned count, uint64_t flags, uint64_t values) {
        struct gpio_v2_line_request r;
        memset(&r, 0, sizeof(r));
        for (unsigned i = 0; i < count; i++) r.offsets[i] = offsets[i];
        r.num_lines = count;
        snprintf(r.consumer, sizeof(r.consumer), "gpioline");
        r.config.flags = flags;
        if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
                r.config.num_attrs = 1;
                r.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
                r.config.attrs[0].attr.values = values;
                r.config.attrs[0].mask = (count == 64 ? ~0ull : (1ull << count) - 1);
        }
        if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &r) < 0) return -1;
        return r.fd;
}

/* A line's flags in words. */
static void printFlags(uint64_t f) {
        static const struct {
                        uint64_t    flag;
                        const char *word;
        } words[] = {
                {GPIO_V2_LINE_FLAG_USED, "used"},
                {GPIO_V2_LINE_FLAG_INPUT, "input"},
                {GPIO_V2_LINE_FLAG_OUTPUT, "output"},
                {GPIO_V2_LINE_FLAG_ACTIVE_LOW, "active-low"},
                {GPIO_V2_LINE_FLAG_EDGE_RISING, "rising"},
                {GPIO_V2_LINE_FLAG_EDGE_FALLING, "falling"},
                {GPIO_V2_LINE_FLAG_OPEN_DRAIN, "open-drain"},
                {GPIO_V2_LINE_FLAG_OPEN_SOURCE, "open-source"},
                {GPIO_V2_LINE_FLAG_BIAS_PULL_UP, "pull-up"},
                {GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN, "pull-down"},
                {GPIO_V2_LINE_FLAG_BIAS_DISABLED, "bias-disabled"},
        };
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
                if (f & words[i].flag) printf(" %s", words[i].word);
        return;
}

static void sleepMs(long ms) {
        struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
        nanosleep(&t, NULL);
}

int main(int argc, char **argv) {
        if (argc < 3) {
                fprintf(stderr, "usage: gpioline get N... | set N=V... | hold N MS | bias N [COUNT] | watch N MS\n");
                return 2;
        }
        const int chip = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
        if (chip < 0) return fail("/dev/gpiochip0");
        unsigned    offsets[GPIO_V2_LINES_MAX];
        unsigned    count = 0;
        uint64_t    values = 0;
        const char *verb = argv[1];
        if (strcmp(verb, "get") == 0 || strcmp(verb, "set") == 0) {
                const int set = strcmp(verb, "set") == 0;
                for (int i = 2; i < argc && count < GPIO_V2_LINES_MAX; i++) {
                        char *end = NULL;
                        offsets[count] = (unsigned)strtoul(argv[i], &end, 10);
                        if (set && end != NULL && *end == '=' && atoi(end + 1) != 0) values |= 1ull << count;
                        count++;
                }
                const int fd =
                        request(chip, offsets, count, set ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT, values);
                if (fd < 0) return fail("GPIO_V2_GET_LINE_IOCTL");
                struct gpio_v2_line_values v = {.bits = 0, .mask = (1ull << count) - 1};
                if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return fail("GPIO_V2_LINE_GET_VALUES_IOCTL");
                for (unsigned i = 0; i < count; i++)
                        printf("%u=%d%s", offsets[i], (int)((v.bits >> i) & 1), i + 1 == count ? "\n" : " ");
                if (set) {
                        /* And the other way, through SET_VALUES. */
                        struct gpio_v2_line_values s = {.bits = ~values, .mask = (1ull << count) - 1};
                        if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &s) < 0)
                                return fail("GPIO_V2_LINE_SET_VALUES_IOCTL");
                        if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
                                return fail("GPIO_V2_LINE_GET_VALUES_IOCTL");
                        printf("flipped:");
                        for (unsigned i = 0; i < count; i++) printf(" %u=%d", offsets[i], (int)((v.bits >> i) & 1));
                        printf("\n");
                }
                close(fd);
        } else if (strcmp(verb, "hold") == 0 && argc == 4) {
                offsets[0] = (unsigned)atoi(argv[2]);
                const int fd = request(chip, offsets, 1, GPIO_V2_LINE_FLAG_OUTPUT, 1);
                if (fd < 0) return fail("GPIO_V2_GET_LINE_IOCTL");
                printf("holding GPIO%u high for %s ms\n", offsets[0], argv[3]);
                fflush(stdout);
                sleepMs(atol(argv[3]));
                close(fd);
        } else if (strcmp(verb, "bias") == 0) {
                offsets[0] = (unsigned)atoi(argv[2]);
                const int      flips = argc > 3 ? atoi(argv[3]) : 4;
                const uint64_t in =
                        GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
                const int fd = request(chip, offsets, 1, in | GPIO_V2_LINE_FLAG_BIAS_PULL_UP, 0);
                if (fd < 0) return fail("GPIO_V2_GET_LINE_IOCTL");
                int seen = 0;
                for (int i = 0; i < flips; i++) {
                        struct gpio_v2_line_config c;
                        memset(&c, 0, sizeof(c));
                        c.flags = in | (i % 2 == 0 ? GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN : GPIO_V2_LINE_FLAG_BIAS_PULL_UP);
                        if (ioctl(fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &c) < 0)
                                return fail("GPIO_V2_LINE_SET_CONFIG_IOCTL");
                        /* Its edges, as they come: poll, then read whole records. */
                        struct pollfd p = {fd, POLLIN, 0};
                        while (poll(&p, 1, 100) > 0) {
                                struct gpio_v2_line_event e[4];
                                const ssize_t             n = read(fd, e, sizeof(e));
                                if (n < 0) return fail("read");
                                for (ssize_t k = 0; k < n / (ssize_t)sizeof(e[0]); k++, seen++)
                                        printf("%s GPIO%u at %llu.%06llu s, seqno %u, line %u\n",
                                               e[k].id == GPIO_V2_LINE_EVENT_RISING_EDGE ? "rising " : "falling",
                                               e[k].offset, (unsigned long long)(e[k].timestamp_ns / 1000000000ull),
                                               (unsigned long long)(e[k].timestamp_ns % 1000000000ull / 1000),
                                               e[k].seqno, e[k].line_seqno);
                        }
                }
                printf("%d edges for %d flips\n", seen, flips);
                close(fd);
        } else if (strcmp(verb, "watch") == 0 && argc == 4) {
                struct gpio_v2_line_info info;
                memset(&info, 0, sizeof(info));
                info.offset = (uint32_t)atoi(argv[2]);
                if (ioctl(chip, GPIO_V2_GET_LINEINFO_WATCH_IOCTL, &info) < 0)
                        return fail("GPIO_V2_GET_LINEINFO_WATCH_IOCTL");
                printf("GPIO%u \"%s\" consumer \"%s\":", info.offset, info.name, info.consumer);
                printFlags(info.flags);
                printf("\n");
                fflush(stdout);
                struct timespec start, now;
                clock_gettime(CLOCK_MONOTONIC, &start);
                const long ms = atol(argv[3]);
                for (;;) {
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        const long left =
                                ms - ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
                        if (left <= 0) break;
                        struct pollfd p = {chip, POLLIN, 0};
                        if (poll(&p, 1, (int)left) <= 0) continue;
                        struct gpio_v2_line_info_changed c;
                        if (read(chip, &c, sizeof(c)) != (ssize_t)sizeof(c)) return fail("read");
                        static const char *const types[] = {"?", "requested", "released", "reconfigured"};
                        printf("%s GPIO%u consumer \"%s\":", c.event_type <= 3 ? types[c.event_type] : "?",
                               c.info.offset, c.info.consumer);
                        printFlags(c.info.flags);
                        for (uint32_t a = 0; a < c.info.num_attrs; a++)
                                if (c.info.attrs[a].id == GPIO_V2_LINE_ATTR_ID_DEBOUNCE)
                                        printf(" debounce=%uus", c.info.attrs[a].debounce_period_us);
                        printf("\n");
                        fflush(stdout);
                }
                const uint32_t offset = info.offset;
                if (ioctl(chip, GPIO_GET_LINEINFO_UNWATCH_IOCTL, &offset) < 0)
                        return fail("GPIO_GET_LINEINFO_UNWATCH_IOCTL");
                if (ioctl(chip, GPIO_GET_LINEINFO_UNWATCH_IOCTL, &offset) == 0 || errno != EBUSY)
                        return fail("unwatching twice should be EBUSY");
        } else {
                fprintf(stderr, "gpioline: no such verb '%s'\n", verb);
                return 2;
        }
        close(chip);
        return 0;
}
