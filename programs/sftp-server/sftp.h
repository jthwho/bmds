/**
 * @file      sftp.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SFTP protocol, version 3 (draft-ietf-secsh-filexfer-02, as OpenSSH speaks it), as a core
 * that takes one request and makes its reply, reaching files only through a table of calls
 * (devplan/network.md §24): the program's are POSIX's (main.c), the fuzzer's are made up
 * (tests/fuzz/sftp.cpp). C99.
 */

#ifndef SFTP_H
#define SFTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest request taken, and the most a READ gives or a WRITE takes (`limits@openssh.com`). */
#define SFTP_MAX_PACKET (64u * 1024u + 1024u)
#define SFTP_MAX_DATA (64u * 1024u)

/** Open handles at once. */
#define SFTP_MAX_HANDLES 32

/** A file's attributes, as the calls give them. */
typedef struct sftp_stat {
                uint64_t size;
                uint32_t mode; /* POSIX st_mode: type and permissions */
                uint32_t uid, gid;
                uint32_t atime, mtime;
                uint32_t nlink;
} sftp_stat_t;

/** A directory entry, as `readdir` gives it. */
typedef struct sftp_dirent {
                char        name[766]; /* BMDS_NAME_MAX (765) and its NUL */
                sftp_stat_t st;
                int         have_st; /* st filled */
} sftp_dirent_t;

/** A filesystem's size, as `statvfs` gives it (`statvfs@openssh.com`, what `df` on sshfs shows). */
typedef struct sftp_statvfs {
                uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flag, namemax;
} sftp_statvfs_t;

/**
 * The calls the core makes. Each returns 0 or a result >= 0, or -errno (Linux's numbers).
 * Paths are as the client sent them, made absolute against the current directory.
 */
typedef struct sftp_fs {
                int (*open)(void *ctx, const char *path, int posix_flags, uint32_t mode); /* a file id */
                int (*close)(void *ctx, int file);
                long (*pread)(void *ctx, int file, void *buf, uint32_t size, uint64_t offset);
                long (*pwrite)(void *ctx, int file, const void *buf, uint32_t size, uint64_t offset);
                int (*fstat)(void *ctx, int file, sftp_stat_t *st);
                int (*ftruncate)(void *ctx, int file, uint64_t size);
                int (*fsync)(void *ctx, int file);
                int (*stat)(void *ctx, const char *path, sftp_stat_t *st, int follow);
                int (*truncate)(void *ctx, const char *path, uint64_t size);
                int (*utimes)(void *ctx, const char *path, uint32_t atime, uint32_t mtime);
                int (*opendir)(void *ctx, const char *path);              /* a directory id */
                int (*readdir)(void *ctx, int dir, sftp_dirent_t *entry); /* 1 an entry, 0 the end */
                int (*closedir)(void *ctx, int dir);
                int (*remove)(void *ctx, const char *path);
                int (*mkdir)(void *ctx, const char *path, uint32_t mode);
                int (*rmdir)(void *ctx, const char *path);
                int (*rename)(void *ctx, const char *from, const char *to, int replace);
                int (*readlink)(void *ctx, const char *path, char *buf, uint32_t size); /* its length */
                int (*symlink)(void *ctx, const char *target, const char *path);
                int (*getcwd)(void *ctx, char *buf, uint32_t size);
                int (*statvfs)(void *ctx, const char *path, sftp_statvfs_t *out);
} sftp_fs_t;

/** The server's state: its handles. Zero it, then `sftp_init`. */
typedef struct sftp_server {
                const sftp_fs_t *fs;
                void            *ctx;
                int              version; /* 0 until INIT */
                struct {
                                int      kind; /* 0 free, 1 file, 2 directory */
                                int      id;   /* the call's file or directory id */
                                uint32_t generation;
                                char    *path; /* a directory's (its entries' longnames need stat) */
                } handles[SFTP_MAX_HANDLES];
                uint32_t generation;
} sftp_server_t;

/** Where replies go: `data` of `capacity` bytes (at least `SFTP_MAX_PACKET`), `length` used. */
typedef struct sftp_out {
                uint8_t *data;
                size_t   capacity;
                size_t   length;
} sftp_out_t;

/** Starts a server on @p fs with @p ctx. */
void sftp_init(sftp_server_t *s, const sftp_fs_t *fs, void *ctx);

/**
 * Takes one request, @p length bytes at @p packet (what follows its length field), and appends
 * its reply (with its length field) to @p out.
 * @return 0, or -1: the client broke the protocol (end the session).
 */
int sftp_request(sftp_server_t *s, const uint8_t *packet, uint32_t length, sftp_out_t *out);

/** Closes every open handle (the session ended). */
void sftp_finish(sftp_server_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SFTP_H */
