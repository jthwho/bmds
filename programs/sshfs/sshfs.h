/**
 * @file      sshfs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * sshfs's core (devplan/netfs.md step 3): the requests of a filesystem served by this program
 * (fs_serve, bmds/sys.h) answered by an SFTP server, version 3 (draft-ietf-secsh-filexfer-02, as
 * OpenSSH speaks it). It reaches the server only through a pair of calls (main.c's: dbclient's
 * pipes; the host test's: our sftp-server's core), one request at a time. C99.
 */

#ifndef SSHFS_H
#define SSHFS_H

#include <stddef.h>
#include <stdint.h>
#include <bmds/sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest SFTP packet taken (OpenSSH's own limit). */
#define SSHFS_PACKET_MAX (256u * 1024u)

/** The longest remote path: the directory mounted, then a path in it. */
#define SSHFS_PATH_MAX (2u * BMDS_FS_PATH_MAX)

/** Files open at once, directories being listed at once. */
#define SSHFS_FILES 64
#define SSHFS_DIRS 4

/** How the core reaches the SFTP server. */
typedef struct sshfs_io {
                int (*send)(void *ctx, const void *data, size_t size); /**< All of it: 0, or -1. */
                int (*recv)(void *ctx, void *data, size_t size);       /**< Exactly @p size: 0, or -1. */
                void *ctx;
} sshfs_io_t;

/** A file open on the server: its SFTP handle. */
typedef struct sshfs_file {
                uint16_t used;
                uint16_t generation;
                uint32_t length;
                uint8_t  handle[256];
} sshfs_file_t;

/** A directory being listed: its SFTP handle, the entries given, those read but not yet given. */
typedef struct sshfs_dir {
                int      used;
                int      ended;
                char    *path; /* the request's, malloc'd */
                uint32_t handle_length;
                uint8_t  handle[256];
                uint64_t position; /* entries given */
                uint8_t *pending;  /* a NAME's entries, malloc'd */
                size_t   pending_length, pending_at;
                uint32_t pending_count; /* entries left in it */
                uint32_t last_used;
} sshfs_dir_t;

/** The core's state. Zero it, then `sshfs_start`. */
typedef struct sshfs {
                sshfs_io_t   io;
                char         root[SSHFS_PATH_MAX]; /* the directory mounted, absolute on the server */
                size_t       root_length;
                char         mount[BMDS_FS_PATH_MAX]; /* where (absolute): absolute links into root */
                size_t       mount_length;
                uint32_t     max_data; /* for bmds_fs_serve_t */
                int          posix_rename, statvfs;
                uint32_t     next_id;
                uint32_t     clock; /* the directories' last use */
                uint8_t     *in;    /* the last packet read (its type on) */
                size_t       in_length, in_capacity;
                uint8_t     *early; /* a reply read before the one waited for */
                size_t       early_length;
                uint8_t     *out;
                size_t       out_length, out_capacity;
                sshfs_file_t files[SSHFS_FILES];
                sshfs_dir_t  dirs[SSHFS_DIRS];
} sshfs_t;

/**
 * Says hello (INIT), learns the server's extensions and limits, and finds @p dir (relative to
 * the login directory; "" for it) on it.
 * @param mount Where it will be mounted, absolute: links pointing into @p dir are shown there.
 * @return 0, or -1 with @p error (of @p error_size bytes) saying why.
 */
int sshfs_start(sshfs_t *fs, const sshfs_io_t *io, const char *dir, const char *mount, char *error, size_t error_size);

/** The bytes a reply may need: `sshfs_answer`'s buffer. */
size_t sshfs_reply_max(const sshfs_t *fs);

/**
 * Answers the request record @p request (@p size bytes, as read from the server fd) into
 * @p reply (`sshfs_reply_max()` bytes).
 * @return The reply's bytes, or 0: the connection to the server broke.
 */
size_t sshfs_answer(sshfs_t *fs, const uint8_t *request, size_t size, uint8_t *reply);

/** Frees what the core holds (the server's handles are gone with the connection). */
void sshfs_finish(sshfs_t *fs);

#ifdef __cplusplus
}
#endif

#endif /* SSHFS_H */
