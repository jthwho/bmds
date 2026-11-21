/**
 * @file      pwm.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * pwm: the channels of /dev/pwmchip0 through Linux's PWM character device (linux/pwm.h, the SDK's
 * ioctl() on pwm_ctl), and their pins and timers through BMDS_PWM_INFO. Times are in ns.
 *
 *   pwm info                          each channel: its pin, holder, timer and resolution
 *   pwm get N                         N's waveform (PWM_IOCTL_GETWF)
 *   pwm round N PERIOD DUTY [OFFSET]  what setting it would give (PWM_IOCTL_ROUNDWF)
 *   pwm set N PERIOD DUTY [OFFSET]    set, rounded (PWM_IOCTL_SETROUNDEDWF), then read back; it
 *                                     stays after pwm ends, as Linux's does
 *   pwm exact N PERIOD DUTY [OFFSET]  set exactly (PWM_IOCTL_SETEXACTWF), or EDOM
 *   pwm hold N MS                     N held (PWM_IOCTL_REQUEST) for MS ms: another's request
 *                                     of it is EBUSY meanwhile
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>
#include <linux/pwm.h>

static int fail(const char *what) {
        fprintf(stderr, "pwm: %s: %s\n", what, strerror(errno));
        return 1;
}

static int usage(void) {
        fprintf(stderr, "usage: pwm info | get N | round N PERIOD DUTY [OFFSET] | set N PERIOD DUTY [OFFSET] |\n"
                        "           exact N PERIOD DUTY [OFFSET] | hold N MS   (times in ns)\n");
        return 2;
}

static void show(const struct pwmchip_waveform *w) {
        if (w->period_length_ns == 0) {
                printf("%u: off\n", w->hwpwm);
                return;
        }
        printf("%u: period %llu ns (%.3f Hz), duty %llu ns (%.2f%%), offset %llu ns\n", w->hwpwm,
               (unsigned long long)w->period_length_ns, 1e9 / (double)w->period_length_ns,
               (unsigned long long)w->duty_length_ns, 100.0 * (double)w->duty_length_ns / (double)w->period_length_ns,
               (unsigned long long)w->duty_offset_ns);
        return;
}

int main(int argc, char **argv) {
        if (argc < 2) return usage();
        const int chip = open("/dev/pwmchip0", O_RDWR);
        if (chip < 0) return fail("/dev/pwmchip0");
        const char *cmd = argv[1];
        if (strcmp(cmd, "info") == 0) {
                for (uint32_t n = 0;; ++n) {
                        bmds_pwm_info_t i = {.size = sizeof(i), .hwpwm = n};
                        const int32_t   r = bmds_pwm_ctl(chip, BMDS_PWM_INFO, &i);
                        if (r < 0) {
                                errno = -r;
                                return fail("info");
                        }
                        if (!(i.flags & BMDS_PWM_HAS_PIN))
                                printf("%u: no pin (io/pwm%u)\n", n, n);
                        else
                                printf("%u: GPIO%d, %s, %s\n", n, (int)i.pin, (i.flags & BMDS_PWM_ON) ? "on" : "off",
                                       (i.flags & BMDS_PWM_MINE)        ? "held here"
                                       : (i.flags & BMDS_PWM_REQUESTED) ? "held"
                                                                        : "free");
                        if (i.flags & BMDS_PWM_ON)
                                printf("   timer %d, %u bits, from a %u Hz clock\n", (int)i.timer, i.bits, i.source_hz);
                        if (n + 1 >= i.npwm) break;
                }
                return 0;
        }
        if (argc < 3) return usage();
        struct pwmchip_waveform w;
        memset(&w, 0, sizeof(w));
        w.hwpwm = (uint32_t)strtoul(argv[2], NULL, 0);
        if (strcmp(cmd, "get") == 0) {
                if (ioctl(chip, PWM_IOCTL_GETWF, &w) < 0) return fail("PWM_IOCTL_GETWF");
                show(&w);
                return 0;
        }
        if (strcmp(cmd, "hold") == 0) {
                if (argc < 4) return usage();
                if (ioctl(chip, PWM_IOCTL_REQUEST, (unsigned long)w.hwpwm) < 0) return fail("PWM_IOCTL_REQUEST");
                printf("%u held for %s ms\n", w.hwpwm, argv[3]);
                fflush(stdout);
                const long            ms = strtol(argv[3], NULL, 0);
                const struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
                nanosleep(&t, NULL);
                if (ioctl(chip, PWM_IOCTL_FREE, (unsigned long)w.hwpwm) < 0) return fail("PWM_IOCTL_FREE");
                return 0;
        }
        if (argc < 5) return usage();
        w.period_length_ns = strtoull(argv[3], NULL, 0);
        w.duty_length_ns = strtoull(argv[4], NULL, 0);
        w.duty_offset_ns = argc > 5 ? strtoull(argv[5], NULL, 0) : 0;
        if (strcmp(cmd, "round") == 0) {
                if (ioctl(chip, PWM_IOCTL_ROUNDWF, &w) < 0) return fail("PWM_IOCTL_ROUNDWF");
                show(&w);
                return 0;
        }
        const int exact = strcmp(cmd, "exact") == 0;
        if (!exact && strcmp(cmd, "set") != 0) return usage();
        if (ioctl(chip, exact ? PWM_IOCTL_SETEXACTWF : PWM_IOCTL_SETROUNDEDWF, &w) < 0)
                return fail(exact ? "PWM_IOCTL_SETEXACTWF" : "PWM_IOCTL_SETROUNDEDWF");
        if (ioctl(chip, PWM_IOCTL_GETWF, &w) < 0) return fail("PWM_IOCTL_GETWF");
        show(&w);
        return 0;
}
