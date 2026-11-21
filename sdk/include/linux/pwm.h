/**
 * @file      pwm.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's PWM character device as BMDS provides it (devplan/io.md §15.1): `/dev/pwmchip0`, the
 * channels `io/pwmN` gives pins, with these requests, which the SDK's `ioctl()` carries out on
 * `pwm_ctl` (`bmds/sys.h`). Written for BMDS; the layout and numbers are Linux's
 * (`include/uapi/linux/pwm.h`, Linux 6.13 on).
 *
 * `PWM_IOCTL_REQUEST` and `PWM_IOCTL_FREE` take the channel as the argument itself; the others a
 * `struct pwmchip_waveform`. Rounding is Linux's: each value down to the next the hardware has, up
 * only when there is none below; a period of 0 is off. A request on a channel no file holds takes
 * it for this file (`EBUSY` if another holds it); one without a pin is `ENODEV`.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_PWM_H
#define BMDS_LINUX_PWM_H

#include <linux/ioctl.h>
#include <linux/types.h>

/** @brief A channel's waveform: `duty_length_ns` of each period active, from `duty_offset_ns`. */
struct pwmchip_waveform {
                __u32 hwpwm;            /**< The channel. */
                __u32 __pad;            /**< Zero. */
                __u64 period_length_ns; /**< The repeating period; 0: off. */
                __u64 duty_length_ns;   /**< The active part of each period. */
                __u64 duty_offset_ns;   /**< Its start, from the period's. */
};

#define PWM_IOCTL_REQUEST _IO(0x75, 1)                                /**< Hold channel `arg` for this file. */
#define PWM_IOCTL_FREE _IO(0x75, 2)                                   /**< Let it go. */
#define PWM_IOCTL_ROUNDWF _IOWR(0x75, 3, struct pwmchip_waveform)     /**< Round it as setting it would. */
#define PWM_IOCTL_GETWF _IOWR(0x75, 4, struct pwmchip_waveform)       /**< The channel's waveform. */
#define PWM_IOCTL_SETROUNDEDWF _IOW(0x75, 5, struct pwmchip_waveform) /**< Round and set. */
#define PWM_IOCTL_SETEXACTWF _IOW(0x75, 6, struct pwmchip_waveform)   /**< Set exactly, or `EDOM`. */

#endif /* BMDS_LINUX_PWM_H */
