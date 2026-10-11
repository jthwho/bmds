/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * sftp-server: SFTP for the SSH server (devplan/network.md §24). Dropbear runs it as the
 * `sftp` subsystem, the client's requests on stdin and its replies on stdout; the protocol is
 * sftp.c's, the files musl's POSIX calls.
 */

#define _GNU_SOURCE /* futimens, utimensat */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "sftp.h"

/* The most directories open at once (the handles' share of them). */
#define MAX_DIRS 8

static DIR *dirs[MAX_DIRS];

static int fail(void) {
        return errno != 0 ? -errno : -EIO;
}

static void fill(sftp_stat_t *out, const struct stat *st) {
        out->size = (uint64_t)st->st_size;
        out->mode = (uint32_t)st->st_mode;
        out->uid = (uint32_t)st->st_uid;
        out->gid = (uint32_t)st->st_gid;
        out->atime = (uint32_t)st->st_atime;
        out->mtime = (uint32_t)st->st_mtime;
        out->nlink = (uint32_t)st->st_nlink;
        return;
}

static int fs_open(void *ctx, const char *path, int flags, uint32_t mode) {
        (void)ctx;
        const int fd = open(path, flags | O_CLOEXEC, (mode_t)mode);
        return fd >= 0 ? fd : fail();
}

static int fs_close(void *ctx, int file) {
        (void)ctx;
        return close(file) == 0 ? 0 : fail();
}

static long fs_pread(void *ctx, int file, void *buf, uint32_t size, uint64_t offset) {
        (void)ctx;
        const ssize_t n = pread(file, buf, size, (off_t)offset);
        return n >= 0 ? (long)n : fail();
}

static long fs_pwrite(void *ctx, int file, const void *buf, uint32_t size, uint64_t offset) {
        (void)ctx;
        const ssize_t n = pwrite(file, buf, size, (off_t)offset);
        return n >= 0 ? (long)n : fail();
}

static int fs_fstat(void *ctx, int file, sftp_stat_t *out) {
        (void)ctx;
        struct stat st;
        if (fstat(file, &st) != 0) return fail();
        fill(out, &st);
        return 0;
}

static int fs_ftruncate(void *ctx, int file, uint64_t size) {
        (void)ctx;
        return ftruncate(file, (off_t)size) == 0 ? 0 : fail();
}

static int fs_fsync(void *ctx, int file) {
        (void)ctx;
        return fsync(file) == 0 ? 0 : fail();
}

static int fs_stat(void *ctx, const char *path, sftp_stat_t *out, int follow) {
        (void)ctx;
        struct stat st;
        if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return fail();
        fill(out, &st);
        return 0;
}

static int fs_truncate(void *ctx, const char *path, uint64_t size) {
        (void)ctx;
        return truncate(path, (off_t)size) == 0 ? 0 : fail();
}

static int fs_utimes(void *ctx, const char *path, uint32_t atime, uint32_t mtime) {
        (void)ctx;
        const struct timespec times[2] = {{(time_t)atime, 0}, {(time_t)mtime, 0}};
        return utimensat(AT_FDCWD, path, times, 0) == 0 ? 0 : fail();
}

static int fs_opendir(void *ctx, const char *path) {
        (void)ctx;
        for (int i = 0; i < MAX_DIRS; ++i) {
                if (dirs[i] != NULL) continue;
                dirs[i] = opendir(path);
                return dirs[i] != NULL ? i : fail();
        }
        return -EMFILE;
}

static int fs_readdir(void *ctx, int dir, sftp_dirent_t *entry) {
        (void)ctx;
        if (dir < 0 || dir >= MAX_DIRS || dirs[dir] == NULL) return -EBADF;
        errno = 0;
        const struct dirent *d = readdir(dirs[dir]);
        if (d == NULL) return errno != 0 ? -errno : 0;
        const size_t n = strlen(d->d_name);
        if (n >= sizeof(entry->name)) return -ENAMETOOLONG;
        memcpy(entry->name, d->d_name, n + 1);
        entry->have_st = 0; /* the core stats it by name */
        return 1;
}

static int fs_closedir(void *ctx, int dir) {
        (void)ctx;
        if (dir < 0 || dir >= MAX_DIRS || dirs[dir] == NULL) return -EBADF;
        const int r = closedir(dirs[dir]);
        dirs[dir] = NULL;
        return r == 0 ? 0 : fail();
}

static int fs_remove(void *ctx, const char *path) {
        (void)ctx;
        return unlink(path) == 0 ? 0 : fail();
}

static int fs_mkdir(void *ctx, const char *path, uint32_t mode) {
        (void)ctx;
        return mkdir(path, (mode_t)mode) == 0 ? 0 : fail();
}

static int fs_rmdir(void *ctx, const char *path) {
        (void)ctx;
        return rmdir(path) == 0 ? 0 : fail();
}

static int fs_rename(void *ctx, const char *from, const char *to, int replace) {
        (void)ctx;
        if (rename(from, to) == 0) return 0;
        if (!replace || (errno != EEXIST && errno != EACCES)) return fail();
        /* A filesystem that won't rename over a file: what rename(2) promises, in two steps. */
        struct stat st;
        if (lstat(to, &st) != 0 || S_ISDIR(st.st_mode) || unlink(to) != 0) return fail();
        return rename(from, to) == 0 ? 0 : fail();
}

static int fs_readlink(void *ctx, const char *path, char *buf, uint32_t size) {
        (void)ctx;
        const ssize_t n = readlink(path, buf, size);
        return n >= 0 ? (int)n : fail();
}

static int fs_symlink(void *ctx, const char *target, const char *path) {
        (void)ctx;
        return symlink(target, path) == 0 ? 0 : fail();
}

static int fs_getcwd(void *ctx, char *buf, uint32_t size) {
        (void)ctx;
        return getcwd(buf, size) != NULL ? 0 : fail();
}

static int fs_statvfs(void *ctx, const char *path, sftp_statvfs_t *out) {
        (void)ctx;
        struct statvfs v;
        if (statvfs(path, &v) != 0) return fail();
        out->bsize = v.f_bsize;
        out->frsize = v.f_frsize;
        out->blocks = v.f_blocks;
        out->bfree = v.f_bfree;
        out->bavail = v.f_bavail;
        out->files = v.f_files;
        out->ffree = v.f_ffree;
        out->favail = v.f_favail;
        out->fsid = v.f_fsid;
        out->flag = v.f_flag;
        out->namemax = v.f_namemax;
        return 0;
}

static const sftp_fs_t posix_fs = {
        fs_open,  fs_close,    fs_pread,  fs_pwrite,   fs_fstat,   fs_ftruncate, fs_fsync,
        fs_stat,  fs_truncate, fs_utimes, fs_opendir,  fs_readdir, fs_closedir,  fs_remove,
        fs_mkdir, fs_rmdir,    fs_rename, fs_readlink, fs_symlink, fs_getcwd,    fs_statvfs,
};

/* All @p n bytes into @p buf from stdin: 1, or 0 at the end (or an error). */
static int read_all(uint8_t *buf, size_t n) {
        while (n > 0) {
                const ssize_t r = read(STDIN_FILENO, buf, n);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) return 0;
                buf += r;
                n -= (size_t)r;
        }
        return 1;
}

static int write_all(const uint8_t *buf, size_t n) {
        while (n > 0) {
                const ssize_t w = write(STDOUT_FILENO, buf, n);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) return 0;
                buf += w;
                n -= (size_t)w;
        }
        return 1;
}

int main(void) {
        static sftp_server_t server;
        uint8_t             *request = malloc(SFTP_MAX_PACKET);
        uint8_t             *reply = malloc(SFTP_MAX_PACKET + 64);
        if (request == NULL || reply == NULL) return 1;
        sftp_init(&server, &posix_fs, NULL);
        for (;;) {
                uint8_t head[4];
                if (!read_all(head, sizeof(head))) break;
                const uint32_t length =
                        (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
                if (length == 0 || length > SFTP_MAX_PACKET || !read_all(request, length)) break;
                sftp_out_t out = {reply, SFTP_MAX_PACKET + 64, 0};
                if (sftp_request(&server, request, length, &out) < 0) break;
                if (!write_all(out.data, out.length)) break;
        }
        sftp_finish(&server);
        return 0;
}
