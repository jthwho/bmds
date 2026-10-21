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
 * pipes; the host test's: our sftp-server's core). Workers answer requests at once, each on a
 * thread of its own, their SFTP requests in flight together on the one connection; a reader
 * hands each reply to the worker whose request it answers (devplan/netfs.md step 5). C99.
 */

#ifndef SSHFS_H
#define SSHFS_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <bmds/sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest SFTP packet taken (OpenSSH's own limit). */
#define SSHFS_PACKET_MAX (256u * 1024u)

/** The longest remote path: the directory mounted, then a path in it. */
#define SSHFS_PATH_MAX (2u * BMDS_FS_PATH_MAX)

/** Files open at once, directories being listed at once, workers at once. */
#define SSHFS_FILES 64
#define SSHFS_DIRS 4
#define SSHFS_WORKERS 4

/** Replies one worker waits for at once (an OPEN sends its LSTAT alongside). */
#define SSHFS_SLOTS 2

/** How the core reaches the SFTP server. */
typedef struct sshfs_io {
                int (*send)(void *ctx, const void *data, size_t size); /**< All of it: 0, or -1. */
                int (*recv)(void *ctx, void *data, size_t size);       /**< Exactly @p size: 0, or -1. */
                void *ctx;
} sshfs_io_t;

/** A file open on the server: its SFTP handle. */
typedef struct sshfs_file {
                uint16_t used; /* 0 free, 1 open, 2 being opened */
                uint16_t generation;
                uint32_t length;
                uint8_t  handle[256];
} sshfs_file_t;

/** A directory being listed: its SFTP handle, the entries given, those read but not yet given. */
typedef struct sshfs_dir {
                int      used;
                int      busy; /* a worker has it */
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

/** A reply a worker waits for. */
typedef struct sshfs_slot {
                uint32_t id;
                int      waiting; /* its request was sent (or is being) */
                int      ready;   /* it came: data, length */
                uint8_t *data;    /* a packet (its type on), malloc'd, kept for the next */
                size_t   length, capacity;
                uint8_t *into;          /* not null: the packet read there instead, if it fits (a READ's: its */
                size_t   into_capacity; /* data lands where its reply needs it) */
                uint8_t *at;            /* where the packet was read */
} sshfs_slot_t;

struct sshfs;

/** One thread's share: the request it builds, the replies it waits for. */
typedef struct sshfs_worker {
                struct sshfs  *fs;
                uint8_t       *out;
                size_t         out_length, out_capacity;
                sshfs_slot_t   slots[SSHFS_SLOTS];
                pthread_cond_t ready; /* a slot's reply came, or the connection broke */
} sshfs_worker_t;

/** The core's state. Zero it, then `sshfs_start`. */
typedef struct sshfs {
                sshfs_io_t      io;
                char            root[SSHFS_PATH_MAX]; /* the directory mounted, absolute on the server */
                size_t          root_length;
                char            mount[BMDS_FS_PATH_MAX]; /* where (absolute): absolute links into root */
                size_t          mount_length;
                uint32_t        max_data; /* for bmds_fs_serve_t */
                int             posix_rename, statvfs;
                pthread_mutex_t lock;      /* what follows, and the workers' slots */
                pthread_mutex_t send_lock; /* a packet sent whole */
                int             broken;    /* the connection ended */
                uint32_t        next_id;
                uint32_t        clock; /* the directories' last use */
                sshfs_worker_t *workers[SSHFS_WORKERS];
                size_t          worker_count; /* >0: replies come only through sshfs_read_replies() */
                sshfs_file_t    files[SSHFS_FILES];
                sshfs_dir_t     dirs[SSHFS_DIRS];
} sshfs_t;

/**
 * Says hello (INIT), learns the server's extensions and limits, and finds @p dir (relative to
 * the login directory; "" for it) on it: on the calling thread, before any worker.
 * @param mount Where it will be mounted, absolute: links pointing into @p dir are shown there.
 * @return 0, or -1 with @p error (of @p error_size bytes) saying why.
 */
int sshfs_start(sshfs_t *fs, const sshfs_io_t *io, const char *dir, const char *mount, char *error, size_t error_size);

/**
 * Makes @p w one of @p fs's workers (`SSHFS_WORKERS` at most), each answering requests on a
 * thread of its own. With the first, replies come only through `sshfs_read_replies()`, which
 * must run (on another thread) while any worker answers.
 * @return 0, or -1 (too many, or no memory).
 */
int sshfs_worker_init(sshfs_t *fs, sshfs_worker_t *w);

/** Frees @p w's buffers (it answers no more; the reader may still run). */
void sshfs_worker_finish(sshfs_worker_t *w);

/** The bytes a reply may need: `sshfs_answer`'s buffer. */
size_t sshfs_reply_max(const sshfs_t *fs);

/**
 * Answers the request record @p request (@p size bytes, as read from the server fd) into
 * @p reply (`sshfs_reply_max()` bytes), on @p w's thread.
 * @return The reply's bytes, or 0: the connection to the server broke.
 */
size_t sshfs_answer(sshfs_worker_t *w, const uint8_t *request, size_t size, uint8_t *reply);

/**
 * Reads the server's replies, handing each to the worker waiting for it, until the connection
 * ends (`io.recv` fails); then tells every worker waiting. On a thread of its own.
 */
void sshfs_read_replies(sshfs_t *fs);

/** Frees what the core holds (the server's handles are gone with the connection). */
void sshfs_finish(sshfs_t *fs);

#ifdef __cplusplus
}
#endif

#endif /* SSHFS_H */
