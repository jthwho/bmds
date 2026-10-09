/**
 * @file      oss.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * OSS's ioctls on the audio calls (oss.c), for linux.c. Internal to the SDK.
 */

#ifndef BMDS_CRT_OSS_H
#define BMDS_CRT_OSS_H

/* An OSS request ('P' or 'M') on @p fd: 0 or a negated Linux errno (-ENOTTY: not an audio fd). */
long __bmds_oss_ioctl(long fd, unsigned long request, void *arg);

/* The bytes an audio fd takes without waiting, or -1 if @p fd isn't one. */
long __bmds_oss_room(long fd);

/* @p fd was closed: forgets what it kept for it. */
void __bmds_oss_closed(long fd);

#endif /* BMDS_CRT_OSS_H */
