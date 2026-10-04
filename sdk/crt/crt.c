/**
 * @file      crt.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program startup after crt0.S, and picolibc's POSIX glue over the BMDS
 * system call table: fds 0-2 are the program's standard streams.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <bmds/sys.h>

const bmds_sys_t *bmds_sys;

extern char **environ;
extern void   __libc_init_array(void);
extern int    main(int argc, char **argv, char **envp);

void __bmds_start(const bmds_sys_t *sys, int32_t argc, char **argv, char **envp) __attribute__((noreturn));

void __bmds_start(const bmds_sys_t *sys, int32_t argc, char **argv, char **envp) {
        bmds_sys = sys;
        environ = envp;
        __libc_init_array();
        exit(main((int)argc, argv, envp));
}

/* A result from the table: >= 0, or -1 with errno set. */
static int result(int32_t r) {
        if (r < 0) {
                errno = -r;
                return -1;
        }
        return (int)r;
}

ssize_t read(int fd, void *buf, size_t count) {
        return result(bmds_read(fd, buf, (uint32_t)count));
}

ssize_t write(int fd, const void *buf, size_t count) {
        return result(bmds_write(fd, buf, (uint32_t)count));
}

int close(int fd) {
        return result(bmds_close(fd));
}

off_t lseek(int fd, off_t offset, int whence) {
        int64_t position = 0;
        if (result(bmds_seek(fd, (int64_t)offset, (uint32_t)whence, &position)) < 0) return (off_t)-1;
        return (off_t)position;
}

int isatty(int fd) {
        const int32_t r = bmds_isatty(fd);
        if (r <= 0) {
                errno = r < 0 ? -r : ENOTTY;
                return 0;
        }
        return 1;
}

void _exit(int status) {
        bmds_exit(status);
        for (;;) {}
}
