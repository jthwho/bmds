/**
 * @file      errno.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Error numbers of the BMDS userspace ABI. System calls return them
 * negated (a negative result is -BMDS_E...). The values are Linux's (as
 * musl's <errno.h>, so the SDK's layer under musl passes them through);
 * they are frozen here independently of any C library.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_ERRNO_H
#define BMDS_ERRNO_H

#define BMDS_EPERM 1         /**< Operation not permitted. */
#define BMDS_ENOENT 2        /**< No such file or directory. */
#define BMDS_ESRCH 3         /**< No such process. */
#define BMDS_EINTR 4         /**< Interrupted (a signal is due). */
#define BMDS_EIO 5           /**< Input/output error. */
#define BMDS_E2BIG 7         /**< Argument list too long. */
#define BMDS_ENOEXEC 8       /**< Exec format error. */
#define BMDS_EBADF 9         /**< Bad file descriptor. */
#define BMDS_ECHILD 10       /**< No child processes. */
#define BMDS_EAGAIN 11       /**< Try again (would block). */
#define BMDS_ENOMEM 12       /**< Out of memory. */
#define BMDS_EACCES 13       /**< Permission denied. */
#define BMDS_EFAULT 14       /**< Bad address. */
#define BMDS_EBUSY 16        /**< Device or resource busy. */
#define BMDS_EEXIST 17       /**< File exists. */
#define BMDS_EXDEV 18        /**< Cross-device link. */
#define BMDS_ENODEV 19       /**< No such device. */
#define BMDS_ENOTDIR 20      /**< Not a directory. */
#define BMDS_EISDIR 21       /**< Is a directory. */
#define BMDS_EINVAL 22       /**< Invalid argument (also: unknown flag bits). */
#define BMDS_ENFILE 23       /**< Too many open files in the system. */
#define BMDS_EMFILE 24       /**< Too many open files. */
#define BMDS_ENOTTY 25       /**< Not a terminal. */
#define BMDS_EFBIG 27        /**< File too large. */
#define BMDS_ENOSPC 28       /**< No space left on device. */
#define BMDS_ESPIPE 29       /**< Illegal seek (a pipe or terminal). */
#define BMDS_EROFS 30        /**< Read-only file system. */
#define BMDS_EPIPE 32        /**< Broken pipe. */
#define BMDS_ERANGE 34       /**< Result out of range. */
#define BMDS_ENAMETOOLONG 36 /**< File name too long. */
#define BMDS_ENOSYS 38       /**< Function not implemented (a slot this kernel lacks). */
#define BMDS_ENOTEMPTY 39    /**< Directory not empty. */
#define BMDS_ELOOP 40        /**< Too many levels of symbolic links. */
#define BMDS_EOVERFLOW 75    /**< Value too large. */
#define BMDS_EILSEQ 84       /**< Illegal byte sequence. */
#define BMDS_ENOTSOCK 88     /**< Not a socket. */
#define BMDS_EDESTADDRREQ 89 /**< A datagram socket without an address to send to. */
#define BMDS_EMSGSIZE 90     /**< Message too long. */
#define BMDS_EPROTOTYPE 91   /**< The protocol doesn't fit the socket type. */
#define BMDS_ENOPROTOOPT 92  /**< No such socket option. */
#define BMDS_EPROTONOSUPPORT 93 /**< Protocol not supported. */
#define BMDS_ENOTSUP 95         /**< Not supported (also EOPNOTSUPP). */
#define BMDS_EAFNOSUPPORT 97    /**< Address family not supported. */
#define BMDS_EADDRINUSE 98      /**< Address in use. */
#define BMDS_EADDRNOTAVAIL 99   /**< Address not available. */
#define BMDS_ENETDOWN 100       /**< Network is down. */
#define BMDS_ENETUNREACH 101    /**< Network is unreachable. */
#define BMDS_ECONNABORTED 103   /**< Connection aborted. */
#define BMDS_ECONNRESET 104     /**< Connection reset by peer. */
#define BMDS_ENOBUFS 105        /**< No buffer space. */
#define BMDS_EISCONN 106        /**< Already connected. */
#define BMDS_ENOTCONN 107       /**< Not connected. */
#define BMDS_ECONNREFUSED 111   /**< Connection refused. */
#define BMDS_EHOSTUNREACH 113   /**< No route to host. */
#define BMDS_EALREADY 114       /**< Operation already in progress. */
#define BMDS_EINPROGRESS 115    /**< Operation in progress. */
#define BMDS_ETIMEDOUT 110   /**< Timed out. */
#define BMDS_ENOMEDIUM 123   /**< No medium (the card is out). */
#define BMDS_ECANCELED 125   /**< Cancelled. */

#endif /* BMDS_ERRNO_H */
