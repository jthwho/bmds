/**
 * @file      ptp_clock.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's PTP hardware clock interface as BMDS provides it (devplan/network.md §31e):
 * `/dev/ptp0`, an Ethernet device's 1588 clock, read with `clock_gettime(FD_TO_CLOCKID(fd))`
 * and these requests, which the SDK's `ioctl()` carries out on `ptp_ctl` (`bmds/sys.h`).
 * Written for BMDS; the layouts and numbers are Linux's (`include/uapi/linux/ptp_clock.h`),
 * which programs were built against.
 *
 * Supported: `PTP_CLOCK_GETCAPS`, `PTP_SYS_OFFSET` and `PTP_SYS_OFFSET_EXTENDED` (and their
 * `2` forms). `PTP_SYS_OFFSET_PRECISE` fails with `EOPNOTSUPP` (no cross-timestamping), as do
 * external time stamps, periodic outputs and pins. `clock_settime` and `clock_adjtime`
 * (`ADJ_SETOFFSET`, `ADJ_FREQUENCY`, `ADJ_NANO`) fail with `EBUSY` while the PTP service runs
 * on the clock.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_PTP_CLOCK_H
#define BMDS_LINUX_PTP_CLOCK_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A dynamic clock's id from its fd, and back (Linux's `CLOCKFD` convention). */
#define CLOCKFD 3
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | CLOCKFD)
#define CLOCKID_TO_FD(clk) ((unsigned int)~((clk) >> 3))

/** @brief `ptp_perout_request` flags, for reference: periodic outputs aren't provided. */
#define PTP_ENABLE_FEATURE (1 << 0)
#define PTP_RISING_EDGE (1 << 1)
#define PTP_FALLING_EDGE (1 << 2)

/** @brief A time: seconds and nanoseconds. */
struct ptp_clock_time {
                int64_t  sec;
                uint32_t nsec;
                uint32_t reserved;
};

/** @brief What the clock can do (`PTP_CLOCK_GETCAPS`). */
struct ptp_clock_caps {
                int max_adj;            /**< The largest frequency correction, parts per billion. */
                int n_alarm;            /**< 0. */
                int n_ext_ts;           /**< 0. */
                int n_per_out;          /**< 0. */
                int pps;                /**< 1 while a PPS output runs. */
                int n_pins;             /**< 0. */
                int cross_timestamping; /**< 0. */
                int adjust_phase;       /**< 0. */
                int max_phase_adj;      /**< 0. */
                int rsv[11];
};

/** @brief Readings, at most this many at once. */
#define PTP_MAX_SAMPLES 25

/** @brief `PTP_SYS_OFFSET`: system and clock times interleaved, a system time last. */
struct ptp_sys_offset {
                unsigned int          n_samples;
                unsigned int          rsv[3];
                struct ptp_clock_time ts[2 * PTP_MAX_SAMPLES + 1];
};

/** @brief `PTP_SYS_OFFSET_EXTENDED`: [before, clock, after] on @p clockid (realtime, monotonic or TAI). */
struct ptp_sys_offset_extended {
                unsigned int          n_samples;
                int                   clockid;
                unsigned int          rsv[2];
                struct ptp_clock_time ts[PTP_MAX_SAMPLES][3];
};

/** @brief `PTP_SYS_OFFSET_PRECISE`: not provided. */
struct ptp_sys_offset_precise {
                struct ptp_clock_time device;
                struct ptp_clock_time sys_realtime;
                struct ptp_clock_time sys_monoraw;
                unsigned int          rsv[4];
};

#define PTP_CLK_MAGIC '='

#define PTP_CLOCK_GETCAPS _IOR(PTP_CLK_MAGIC, 1, struct ptp_clock_caps)
#define PTP_SYS_OFFSET _IOW(PTP_CLK_MAGIC, 5, struct ptp_sys_offset)
#define PTP_SYS_OFFSET_PRECISE _IOWR(PTP_CLK_MAGIC, 8, struct ptp_sys_offset_precise)
#define PTP_SYS_OFFSET_EXTENDED _IOWR(PTP_CLK_MAGIC, 9, struct ptp_sys_offset_extended)
#define PTP_CLOCK_GETCAPS2 _IOR(PTP_CLK_MAGIC, 10, struct ptp_clock_caps)
#define PTP_SYS_OFFSET2 _IOW(PTP_CLK_MAGIC, 14, struct ptp_sys_offset)
#define PTP_SYS_OFFSET_PRECISE2 _IOWR(PTP_CLK_MAGIC, 17, struct ptp_sys_offset_precise)
#define PTP_SYS_OFFSET_EXTENDED2 _IOWR(PTP_CLK_MAGIC, 18, struct ptp_sys_offset_extended)

#ifdef __cplusplus
}
#endif

#endif
