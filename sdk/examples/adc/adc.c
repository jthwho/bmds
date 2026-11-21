/**
 * @file      adc.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * adc: the ADC's pins (io/adc0) through /dev/adc0 and adc_ctl.
 *
 *   adc info          the pins: unit, channel, attenuation, the board's divider
 *   adc read [PIN]    one-shot readings, calibrated: millivolts at the pin (and what a divider senses)
 *   adc stream MS     stream for MS ms (io/adc0's rate=): each pin's count, least, mean and most
 *                     mV, the conversions against the rate, those lost
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>

static int fail(const char *what, int err) {
        fprintf(stderr, "adc: %s: %s\n", what, strerror(err));
        return 1;
}

static int ctl(int fd, uint32_t op, void *arg, const char *what) {
        const int32_t r = bmds_adc_ctl(fd, op, arg);
        if (r < 0) {
                fail(what, -r);
                return -1;
        }
        return 0;
}

static double now_s(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr, "usage: adc info | read [PIN] | stream MS\n");
                return 2;
        }
        const int fd = open("/dev/adc0", O_RDONLY);
        if (fd < 0) return fail("/dev/adc0", errno);
        bmds_adc_info_t info = {.size = sizeof(info)};
        if (ctl(fd, BMDS_ADC_INFO, &info, "info") < 0) return 1;
        if (!(info.flags & BMDS_ADC_CONFIGURED)) {
                fprintf(stderr, "adc: no pins (io/adc0)\n");
                return 1;
        }
        bmds_adc_channel_t ch[16];
        for (uint32_t i = 0; i < info.channels && i < 16; ++i) {
                ch[i] = (bmds_adc_channel_t){.size = sizeof(ch[i]), .index = i};
                if (ctl(fd, BMDS_ADC_CHANNEL, &ch[i], "channel") < 0) return 1;
        }
        if (strcmp(argv[1], "info") == 0) {
                for (uint32_t i = 0; i < info.channels; ++i) {
                        printf("GPIO%d: ADC%u channel %u, %.1f dB", (int)ch[i].pin, ch[i].unit + 1, ch[i].channel,
                               ch[i].atten_cdb / 100.0);
                        if (ch[i].divider > 1) printf(", the board divides by %u", ch[i].divider);
                        printf("\n");
                }
                if (info.rate_hz) printf("streams at %u conversions a second\n", info.rate_hz);
                return 0;
        }
        if (strcmp(argv[1], "read") == 0) {
                const int only = argc > 2 ? atoi(argv[2]) : -1;
                for (uint32_t i = 0; i < info.channels; ++i) {
                        if (only >= 0 && ch[i].pin != only) continue;
                        bmds_adc_reading_t r = {.size = sizeof(r), .pin = ch[i].pin};
                        if (ctl(fd, BMDS_ADC_READ, &r, "read") < 0) return 1;
                        printf("GPIO%d: %u, %d mV", (int)r.pin, r.raw, (int)r.mv);
                        if (ch[i].divider > 1) printf(" (%.3f V sensed)", r.mv * ch[i].divider / 1000.0);
                        printf("\n");
                }
                return 0;
        }
        if (strcmp(argv[1], "stream") != 0 || argc < 3) return 2;
        if (info.rate_hz == 0) {
                fprintf(stderr, "adc: io/adc0 has no rate=: readings only\n");
                return 1;
        }
        const double ms = atof(argv[2]);
        uint64_t     count[64] = {0};
        int64_t      sum[64] = {0}, rawSum[64] = {0};
        int          lo[64], hi[64];
        for (int i = 0; i < 64; ++i) lo[i] = 1 << 30, hi[i] = -1;
        bmds_adc_sample_t buf[256];
        uint64_t          total = 0;
        const double      start = now_s();
        while (now_s() - start < ms / 1000.0) {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0) return fail("read", errno);
                for (size_t k = 0; k < (size_t)n / sizeof(buf[0]); ++k) {
                        const unsigned p = buf[k].pin & 63;
                        ++count[p];
                        sum[p] += buf[k].mv;
                        rawSum[p] += buf[k].raw;
                        if (buf[k].mv < lo[p]) lo[p] = buf[k].mv;
                        if (buf[k].mv > hi[p]) hi[p] = buf[k].mv;
                        ++total;
                }
        }
        bmds_adc_info_t end = {.size = sizeof(end)};
        if (ctl(fd, BMDS_ADC_INFO, &end, "info") < 0) return 1;
        const double elapsed = now_s() - start;
        for (uint32_t i = 0; i < info.channels; ++i) {
                const unsigned p = (unsigned)ch[i].pin & 63;
                if (count[p] == 0) {
                        printf("GPIO%d: nothing\n", (int)ch[i].pin);
                        continue;
                }
                printf("GPIO%d: %llu conversions, %d / %.1f / %d mV (raw %.1f on average)\n", (int)ch[i].pin,
                       (unsigned long long)count[p], lo[p], (double)sum[p] / (double)count[p], hi[p],
                       (double)rawSum[p] / (double)count[p]);
        }
        printf("%llu read in %.3f s (%.0f a second; rate %u); the stream counted %llu, %llu lost, a pool of %u bytes\n",
               (unsigned long long)total, elapsed, (double)total / elapsed, end.rate_hz,
               (unsigned long long)end.samples, (unsigned long long)end.lost, end.buffer);
        return 0;
}
