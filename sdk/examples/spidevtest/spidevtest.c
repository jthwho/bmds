/**
 * @file      spidevtest.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * spidevtest: an SPI device through Linux's spidev only (linux/spi/spidev.h, the SDK's ioctl() on
 * spi_ctl), in the shape of Linux's spidev_test: sets the mode, word size and speed, sends LEN
 * bytes (0, 1, 2...) in one SPI_IOC_MESSAGE as two transfers, reads what came back, and says
 * whether it matches (a looped-back port: io/spiN's loopback) and how long it took.
 *
 *   spidevtest DEVICE [SPEED] [MODE] [BITS] [LEN]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/spidev.h>

static int fail(const char *what, int fd) {
        fprintf(stderr, "spidevtest: %s: %s\n", what, strerror(errno));
        if (fd >= 0) close(fd);
        return 1;
}

int main(int argc, char **argv) {
        if (argc < 2 || argc > 6) {
                fprintf(stderr, "usage: spidevtest DEVICE [SPEED] [MODE] [BITS] [LEN]\n");
                return 2;
        }
        const int fd = open(argv[1], O_RDWR);
        if (fd < 0) return fail(argv[1], -1);
        uint32_t speed = 0, mode = 0;
        uint8_t  bits = 0;
        if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0 || ioctl(fd, SPI_IOC_RD_MODE32, &mode) < 0 ||
            ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0)
                return fail("reading the settings", fd);
        if (argc > 2) speed = (uint32_t)strtoul(argv[2], NULL, 0);
        if (argc > 3)
                mode = (mode & ~(uint32_t)SPI_MODE_X_MASK) | ((uint32_t)strtoul(argv[3], NULL, 0) & SPI_MODE_X_MASK);
        if (argc > 4) bits = (uint8_t)strtoul(argv[4], NULL, 0);
        const long len = argc > 5 ? strtol(argv[5], NULL, 0) : 32;
        if (len < 2 || len > 65536) {
                fprintf(stderr, "spidevtest: LEN is 2 to 65536\n");
                close(fd);
                return 2;
        }
        if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 || ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
            ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
                return fail("setting the mode, word size and speed", fd);
        uint8_t *tx = malloc((size_t)len), *rx = malloc((size_t)len);
        if (tx == NULL || rx == NULL) return fail("memory", fd);
        for (long i = 0; i < len; i++) tx[i] = (uint8_t)i;
        memset(rx, 0xee, (size_t)len);
        /* Two transfers, the chip select held between them: the first's words, then the rest. */
        const uint32_t          word = bits <= 8 ? 1 : bits / 8;
        const uint32_t          first = (uint32_t)(len / 2) / word * word;
        struct spi_ioc_transfer t[2];
        memset(t, 0, sizeof(t));
        t[0].tx_buf = (uintptr_t)tx;
        t[0].rx_buf = (uintptr_t)rx;
        t[0].len = first;
        t[1].tx_buf = (uintptr_t)(tx + first);
        t[1].rx_buf = (uintptr_t)(rx + first);
        t[1].len = (uint32_t)len - first;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        const int moved = ioctl(fd, SPI_IOC_MESSAGE(2), t);
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (moved < 0) return fail("SPI_IOC_MESSAGE", fd);
        const double us = (double)(b.tv_sec - a.tv_sec) * 1e6 + (double)(b.tv_nsec - a.tv_nsec) / 1e3;
        printf("%s: mode %u, %u bits, %u Hz: %d bytes in %.0f us (%.0f KB/s)\n", argv[1], (unsigned)(mode & 3),
               (unsigned)(bits ? bits : 8), (unsigned)speed, moved, us, (double)moved / us * 1e6 / 1024);
        for (long i = 0; i < len && i < 16; i++) printf("%02x%s", rx[i], i + 1 == len || i == 15 ? "\n" : " ");
        long differs = -1;
        for (long i = 0; i < len && differs < 0; i++)
                if (rx[i] != tx[i]) differs = i;
        if (differs < 0)
                printf("read back as sent\n");
        else
                printf("differs from what was sent at byte %ld (%02x, sent %02x)\n", differs, rx[differs], tx[differs]);
        free(tx);
        free(rx);
        close(fd);
        return 0;
}
