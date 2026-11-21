/**
 * @file      ioctl.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's ioctl request encoding (`include/uapi/asm-generic/ioctl.h`) as BMDS provides it, for
 * Linux's uapi headers: musl's <sys/ioctl.h> has the encoding (`_IOC`, `_IO`, `_IOR`, `_IOW`,
 * `_IOWR`, the same numbers); this adds Linux's field sizes and the macros that take a request
 * apart. Written for BMDS; the names are Linux's.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_IOCTL_H
#define BMDS_LINUX_IOCTL_H

#include <sys/ioctl.h>

#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#endif
#ifndef _IOC_TYPEBITS
#define _IOC_TYPEBITS 8
#endif
#ifndef _IOC_SIZEBITS
#define _IOC_SIZEBITS 14
#endif
#ifndef _IOC_DIRBITS
#define _IOC_DIRBITS 2
#endif

#define _IOC_NRMASK ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_DIR(nr) (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr) (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#define IOC_IN (_IOC_WRITE << _IOC_DIRSHIFT)
#define IOC_OUT (_IOC_READ << _IOC_DIRSHIFT)
#define IOC_INOUT ((_IOC_WRITE | _IOC_READ) << _IOC_DIRSHIFT)
#define IOCSIZE_MASK (_IOC_SIZEMASK << _IOC_SIZESHIFT)
#define IOCSIZE_SHIFT (_IOC_SIZESHIFT)

#endif /* BMDS_LINUX_IOCTL_H */
