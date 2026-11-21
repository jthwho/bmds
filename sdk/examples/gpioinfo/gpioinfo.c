/**
 * @file      gpioinfo.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * gpioinfo: every line of /dev/gpiochip0, as libgpiod's gpioinfo prints it, through Linux's
 * GPIO character device (linux/gpio.h, the SDK's ioctl() on gpio_ctl). With -b, BMDS's own
 * words for each pad instead (gpio_ctl's BMDS_GPIO_PINS): class, holder, use, route, level.
 * With -e N, the edges of pad N, watched by io/gpio (`14=in,edges`): BMDS_GPIO_EDGES.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <bmds/sys.h>
#include <linux/gpio.h>

static const char *bias(uint64_t flags) {
        if (flags & GPIO_V2_LINE_FLAG_BIAS_PULL_UP) return " bias=pull-up";
        if (flags & GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN) return " bias=pull-down";
        if (flags & GPIO_V2_LINE_FLAG_BIAS_DISABLED) return " bias=disabled";
        return "";
}

static const char *const Classes[] = {"free", "fixed", "system", "user"};

static int bmdsPins(int fd, unsigned lines) {
        static bmds_gpio_pin_t pins[64];
        bmds_gpio_pins_t p = {.size = sizeof(p), .first = 0, .count = 64, .pin_size = sizeof(pins[0]), .pins = pins};
        const int32_t    n = bmds_gpio_ctl(fd, BMDS_GPIO_PINS, &p);
        if (n < 0) {
                fprintf(stderr, "gpioinfo: BMDS_GPIO_PINS: %s\n", strerror((int)-n));
                return 1;
        }
        printf("%-7s %-3s %-7s %-9s %-24s %-24s %-10s %-8s %s\n", "PAD", "LVL", "CLASS", "OWNER", "USE", "ROUTE", "DIR",
               "PULL", "WHERE");
        for (int32_t i = 0; i < n && (unsigned)i < lines; i++) {
                const bmds_gpio_pin_t *g = &pins[i];
                printf("%-7s %-3c %-7s %-9s %-24s %-24s %-10s %-8s %s\n", g->name, g->level,
                       g->pin_class < 4 ? Classes[g->pin_class] : "?", g->owner[0] ? g->owner : "-",
                       g->use[0] ? g->use : "-", g->route, g->direction, g->pull, g->where[0] ? g->where : "-");
        }
        return 0;
}

static int bmdsEdges(int fd, unsigned pad) {
        static bmds_gpio_edge_t edges[64];
        bmds_gpio_edges_t       e = {.size = sizeof(e),
                                     .offset = pad,
                                     .after = 0,
                                     .count = 64,
                                     .edge_size = sizeof(edges[0]),
                                     .edges = edges};
        const int32_t           n = bmds_gpio_ctl(fd, BMDS_GPIO_EDGES, &e);
        if (n < 0) {
                fprintf(stderr, "gpioinfo: GPIO%u: %s\n", pad,
                        n == -BMDS_ENOENT ? "its edges aren't watched (io/gpio: edges)" : strerror((int)-n));
                return 1;
        }
        for (int32_t i = 0; i < n; i++)
                printf("%6u %14.6f s %s\n", (unsigned)edges[i].seq, (double)edges[i].time_ns / 1e9,
                       edges[i].rising ? "rising" : "falling");
        printf("%d edges, %u lost, next after %u\n", (int)n, (unsigned)e.lost, (unsigned)e.next);
        return 0;
}

int main(int argc, char **argv) {
        const int bmds = argc == 2 && strcmp(argv[1], "-b") == 0;
        const int edges = argc == 3 && strcmp(argv[1], "-e") == 0;
        if (argc > 1 && !bmds && !edges) {
                fprintf(stderr, "usage: gpioinfo [-b | -e PAD]\n");
                return 2;
        }
        const int fd = open("/dev/gpiochip0", O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "gpioinfo: /dev/gpiochip0: %s\n", strerror(errno));
                return 1;
        }
        struct gpiochip_info chip;
        memset(&chip, 0, sizeof(chip));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) < 0) {
                fprintf(stderr, "gpioinfo: GPIO_GET_CHIPINFO_IOCTL: %s\n", strerror(errno));
                close(fd);
                return 1;
        }
        int status = 0;
        if (bmds || edges) {
                status = bmds ? bmdsPins(fd, chip.lines) : bmdsEdges(fd, (unsigned)atoi(argv[2]));
                close(fd);
                return status;
        }
        printf("%s - %u lines:\n", chip.name, (unsigned)chip.lines);
        for (uint32_t i = 0; i < chip.lines; i++) {
                struct gpio_v2_line_info info;
                memset(&info, 0, sizeof(info));
                info.offset = i;
                if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0) {
                        fprintf(stderr, "gpioinfo: line %u: %s\n", (unsigned)i, strerror(errno));
                        status = 1;
                        continue;
                }
                printf("\tline %3u:\t\"%s\"\t%s%s%s%s\n", (unsigned)i, info.name,
                       info.flags & GPIO_V2_LINE_FLAG_OUTPUT ? "output" : "input",
                       info.flags & GPIO_V2_LINE_FLAG_USED ? " used" : "",
                       info.flags & GPIO_V2_LINE_FLAG_OPEN_DRAIN ? " drive=open-drain" : "", bias(info.flags));
                if (info.consumer[0]) printf("\t\t\tconsumer=\"%s\"\n", info.consumer);
        }
        close(fd);
        return status;
}
