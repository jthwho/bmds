/**
 * @file      sshfs.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * sshfs's core (sshfs.h): each request of the mount made into SFTP requests, their replies into
 * its reply. SFTP version 3 says little in its errors (FAILURE is most of POSIX's), so a failure
 * is looked into (an LSTAT) where the kernel needs to know which (EEXIST, ENOTEMPTY, ...).
 */

#include "sshfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/errno.h>

enum {
        FXP_INIT = 1,
        FXP_VERSION = 2,
        FXP_OPEN = 3,
        FXP_CLOSE = 4,
        FXP_READ = 5,
        FXP_WRITE = 6,
        FXP_LSTAT = 7,
        FXP_SETSTAT = 9,
        FXP_FSETSTAT = 10,
        FXP_OPENDIR = 11,
        FXP_READDIR = 12,
        FXP_REMOVE = 13,
        FXP_MKDIR = 14,
        FXP_RMDIR = 15,
        FXP_REALPATH = 16,
        FXP_STAT = 17,
        FXP_RENAME = 18,
        FXP_READLINK = 19,
        FXP_SYMLINK = 20,
        FXP_STATUS = 101,
        FXP_HANDLE = 102,
        FXP_DATA = 103,
        FXP_NAME = 104,
        FXP_ATTRS = 105,
        FXP_EXTENDED = 200,
        FXP_EXTENDED_REPLY = 201,
};

enum {
        FX_OK = 0,
        FX_EOF = 1,
        FX_NO_SUCH_FILE = 2,
        FX_PERMISSION_DENIED = 3,
        FX_FAILURE = 4,
        FX_OP_UNSUPPORTED = 8
};

#define ATTR_SIZE 0x1u
#define ATTR_UIDGID 0x2u
#define ATTR_PERMISSIONS 0x4u
#define ATTR_ACMODTIME 0x8u
#define ATTR_EXTENDED 0x80000000u

#define PF_READ 0x1u
#define PF_WRITE 0x2u
#define PF_CREAT 0x8u
#define PF_TRUNC 0x10u
#define PF_EXCL 0x20u

#define MODE_TYPE 0170000u
#define MODE_DIR 0040000u
#define MODE_LINK 0120000u
#define MODE_FILE 0100000u

/* The connection broke: the answer is none (sshfs_answer gives 0). */
#define BROKEN INT32_MIN

/* What an SFTP reply's attributes say. */
typedef struct attrs {
                uint32_t flags;
                uint64_t size;
                uint32_t permissions;
                uint32_t atime, mtime;
} attrs_t;

/* Reading a packet's fields. */
typedef struct reader {
                const uint8_t *p;
                size_t         left;
                int            bad;
} reader_t;

static uint32_t get_u32(reader_t *r) {
        if (r->left < 4) {
                r->bad = 1;
                return 0;
        }
        const uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
        r->p += 4;
        r->left -= 4;
        return v;
}

static uint64_t get_u64(reader_t *r) {
        const uint64_t hi = get_u32(r);
        return hi << 32 | get_u32(r);
}

static uint8_t get_u8(reader_t *r) {
        if (r->left < 1) {
                r->bad = 1;
                return 0;
        }
        r->left--;
        return *r->p++;
}

/* A string's bytes (not NUL-terminated) and length. */
static const uint8_t *get_str(reader_t *r, uint32_t *length) {
        *length = get_u32(r);
        if (r->bad || *length > r->left) {
                r->bad = 1;
                *length = 0;
                return NULL;
        }
        const uint8_t *s = r->p;
        r->p += *length;
        r->left -= *length;
        return s;
}

static void get_attrs(reader_t *r, attrs_t *a) {
        memset(a, 0, sizeof(*a));
        a->flags = get_u32(r);
        if (a->flags & ATTR_SIZE) a->size = get_u64(r);
        if (a->flags & ATTR_UIDGID) {
                (void)get_u32(r);
                (void)get_u32(r);
        }
        if (a->flags & ATTR_PERMISSIONS) a->permissions = get_u32(r);
        if (a->flags & ATTR_ACMODTIME) {
                a->atime = get_u32(r);
                a->mtime = get_u32(r);
        }
        if (a->flags & ATTR_EXTENDED) {
                const uint32_t n = get_u32(r);
                for (uint32_t i = 0; i < n && !r->bad; ++i) {
                        uint32_t len;
                        (void)get_str(r, &len);
                        (void)get_str(r, &len);
                }
        }
        return;
}

static uint32_t type_of(const attrs_t *a) {
        if ((a->flags & ATTR_PERMISSIONS) == 0) return BMDS_TYPE_FILE;
        switch (a->permissions & MODE_TYPE) {
                case MODE_DIR: return BMDS_TYPE_DIRECTORY;
                case MODE_LINK: return BMDS_TYPE_LINK;
                case MODE_FILE: return BMDS_TYPE_FILE;
                default: return BMDS_TYPE_DEVICE;
        }
}

/* ---- Writing requests */

static void put(sshfs_worker_t *w, const void *data, size_t size) {
        if (w->out_length + size > w->out_capacity) {
                w->out_length = w->out_capacity + 1; /* too long: never sent */
                return;
        }
        memcpy(w->out + w->out_length, data, size);
        w->out_length += size;
        return;
}

static void put_u32(sshfs_worker_t *w, uint32_t v) {
        const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        put(w, b, 4);
        return;
}

static void put_u64(sshfs_worker_t *w, uint64_t v) {
        put_u32(w, (uint32_t)(v >> 32));
        put_u32(w, (uint32_t)v);
        return;
}

static void put_str(sshfs_worker_t *w, const void *s, size_t length) {
        put_u32(w, (uint32_t)length);
        put(w, s, length);
        return;
}

static void put_text(sshfs_worker_t *w, const char *s) {
        put_str(w, s, strlen(s));
        return;
}

/* @p w's slot waiting for reply @p id, or NULL. Lock held. */
static sshfs_slot_t *slot_of(sshfs_worker_t *w, uint32_t id) {
        for (int i = 0; i < SSHFS_SLOTS; ++i)
                if (w->slots[i].waiting && w->slots[i].id == id) return &w->slots[i];
        return NULL;
}

static uint32_t id_at(const uint8_t *p) {
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A request of @p type begun: its id, a slot of @p w's waiting for its reply, read into @p into
 * (@p capacity bytes) if it fits there, else into the slot's own buffer. */
static uint32_t begin_into(sshfs_worker_t *w, uint8_t type, uint8_t *into, size_t capacity) {
        sshfs_t *const fs = w->fs;
        pthread_mutex_lock(&fs->lock);
        const uint32_t id = fs->next_id++;
        if (fs->next_id == 0) fs->next_id = 1;
        sshfs_slot_t *s = NULL;
        for (int i = 0; i < SSHFS_SLOTS && s == NULL; ++i)
                if (!w->slots[i].waiting) s = &w->slots[i];
        if (s == NULL) s = &w->slots[0]; /* never: each request's reply is taken before the next two */
        s->id = id;
        s->waiting = 1;
        s->ready = 0;
        s->into = into;
        s->into_capacity = capacity;
        pthread_mutex_unlock(&fs->lock);
        w->out_length = 0;
        put_u32(w, 0); /* the length, set by finish() */
        put(w, &type, 1);
        put_u32(w, id);
        return id;
}

static uint32_t begin(sshfs_worker_t *w, uint8_t type) {
        return begin_into(w, type, NULL, 0);
}

/* Sends the request begun, then @p data (@p size bytes: a WRITE's, sent from where it is), whole
 * (other workers' between, never inside): 0, or BROKEN. */
static int32_t finish_with(sshfs_worker_t *w, const uint8_t *data, uint32_t size) {
        sshfs_t *const fs = w->fs;
        if (w->out_length > w->out_capacity) {
                /* Never sent: no reply to wait for. */
                pthread_mutex_lock(&fs->lock);
                sshfs_slot_t *s = slot_of(w, id_at(w->out + 5));
                if (s != NULL) s->waiting = 0;
                pthread_mutex_unlock(&fs->lock);
                return -BMDS_ENAMETOOLONG;
        }
        const uint32_t n = (uint32_t)w->out_length - 4 + size;
        w->out[0] = (uint8_t)(n >> 24);
        w->out[1] = (uint8_t)(n >> 16);
        w->out[2] = (uint8_t)(n >> 8);
        w->out[3] = (uint8_t)n;
        pthread_mutex_lock(&fs->send_lock);
        int sent = fs->io.send(fs->io.ctx, w->out, w->out_length);
        if (sent == 0 && size != 0) sent = fs->io.send(fs->io.ctx, data, size);
        pthread_mutex_unlock(&fs->send_lock);
        return sent == 0 ? 0 : BROKEN;
}

/* Sends the request begun: 0, or BROKEN. */
static int32_t finish(sshfs_worker_t *w) {
        return finish_with(w, NULL, 0);
}

/* ---- Reading replies */

/* One packet read and handed to the worker waiting for it (@p only, if not null: before there are
 * workers); one nobody waits for is read and dropped. 0, or -1: the connection ended. */
static int deliver(sshfs_t *fs, sshfs_worker_t *only) {
        uint8_t h[9]; /* the length, the type, the id */
        if (fs->io.recv(fs->io.ctx, h, 4) != 0) return -1;
        const uint32_t n = id_at(h);
        if (n < 5 || n > SSHFS_PACKET_MAX + 1024) return -1;
        if (fs->io.recv(fs->io.ctx, h + 4, 5) != 0) return -1;
        const uint32_t id = id_at(h + 5);
        pthread_mutex_lock(&fs->lock);
        sshfs_worker_t *to = NULL;
        sshfs_slot_t   *s = NULL;
        if (only != NULL) {
                to = only;
                s = slot_of(only, id);
        }
        for (size_t i = 0; only == NULL && s == NULL && i < fs->worker_count; ++i) {
                to = fs->workers[i];
                s = slot_of(to, id);
        }
        if (s != NULL) s->at = s->into != NULL && n <= s->into_capacity ? s->into : s->data;
        if (s != NULL && s->at == s->data && n > s->capacity) {
                uint8_t *data = realloc(s->data, n);
                if (data == NULL) {
                        pthread_mutex_unlock(&fs->lock);
                        return -1;
                }
                s->data = s->at = data;
                s->capacity = n;
        }
        pthread_mutex_unlock(&fs->lock);
        if (s == NULL) {
                uint8_t discard[512];
                for (uint32_t left = n - 5; left > 0;) {
                        const uint32_t k = left < sizeof(discard) ? left : (uint32_t)sizeof(discard);
                        if (fs->io.recv(fs->io.ctx, discard, k) != 0) return -1;
                        left -= k;
                }
                return 0;
        }
        /* Its worker only waits meanwhile: the slot stays as it is. */
        memcpy(s->at, h + 4, 5);
        if (n > 5 && fs->io.recv(fs->io.ctx, s->at + 5, n - 5) != 0) return -1;
        pthread_mutex_lock(&fs->lock);
        s->length = n;
        s->ready = 1;
        pthread_cond_signal(&to->ready);
        pthread_mutex_unlock(&fs->lock);
        return 0;
}

/* Waits for reply @p id: its type, and @p r on what follows the id (valid until @p w's next
 * request but one). 0, or BROKEN. */
static int32_t take(sshfs_worker_t *w, uint32_t id, uint8_t *type, reader_t *r) {
        sshfs_t *const fs = w->fs;
        pthread_mutex_lock(&fs->lock);
        sshfs_slot_t *s = slot_of(w, id);
        while (s != NULL && !s->ready && !fs->broken) {
                if (fs->worker_count > 0) {
                        pthread_cond_wait(&w->ready, &fs->lock);
                        continue;
                }
                /* No workers yet (sshfs_start): read it here. */
                pthread_mutex_unlock(&fs->lock);
                const int got = deliver(fs, w);
                pthread_mutex_lock(&fs->lock);
                if (got != 0) fs->broken = 1;
        }
        const int ready = s != NULL && s->ready;
        if (s != NULL) {
                s->waiting = s->ready = 0;
                s->into = NULL;
        }
        pthread_mutex_unlock(&fs->lock);
        if (!ready) return BROKEN;
        *type = s->at[0];
        r->p = s->at + 5;
        r->left = s->length - 5;
        r->bad = 0;
        return 0;
}

static int32_t errno_of(uint32_t code) {
        switch (code) {
                case FX_OK: return 0;
                case FX_NO_SUCH_FILE: return -BMDS_ENOENT;
                case FX_PERMISSION_DENIED: return -BMDS_EACCES;
                case FX_OP_UNSUPPORTED: return -BMDS_ENOTSUP;
                default: return -BMDS_EIO; /* FAILURE: looked into where it matters */
        }
}

/* A reply that is a STATUS: its error (EOF as -BMDS_EIO: no op here expects it). */
static int32_t take_status(sshfs_worker_t *w, uint32_t id) {
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type != FXP_STATUS) return BROKEN;
        const uint32_t code = get_u32(&r);
        return r.bad ? BROKEN : errno_of(code);
}

/* Was the last STATUS taken a FAILURE (not one of the codes that say what)? */
static int32_t take_status_code(sshfs_worker_t *w, uint32_t id, uint32_t *code) {
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0 || type != FXP_STATUS) return BROKEN;
        *code = get_u32(&r);
        return r.bad ? BROKEN : errno_of(*code);
}

/* ---- Paths */

/* @p path (from the mount's root) on the server: the directory mounted's, then it. */
static int32_t remote(const sshfs_t *fs, const char *path, size_t length, char *out) {
        size_t n = fs->root_length;
        if (length == 0 || (length == 1 && path[0] == '/')) {
                memcpy(out, fs->root, n + 1);
                return 0;
        }
        if (n == 1) n = 0; /* "/" */
        if (n + length >= SSHFS_PATH_MAX) return -BMDS_ENAMETOOLONG;
        memcpy(out, fs->root, n);
        memcpy(out + n, path, length);
        out[n + length] = 0;
        return 0;
}

static const char *last_component(const char *path, size_t length, size_t *out) {
        size_t at = length;
        while (at > 0 && path[at - 1] != '/') --at;
        *out = length - at;
        return path + at;
}

/* Does @p s start with the @p n bytes of @p prefix as a whole path (it, or it and a '/')? */
static int under(const char *s, size_t length, const char *prefix, size_t n) {
        if (n == 1 && prefix[0] == '/') return length > 0 && s[0] == '/';
        return length >= n && memcmp(s, prefix, n) == 0 && (length == n || s[n] == '/');
}

/* A link's target shown here: an absolute one into the directory mounted, at the mount. */
static size_t target_here(const sshfs_t *fs, const char *t, size_t length, char *out, size_t capacity) {
        if (under(t, length, fs->root, fs->root_length)) {
                const size_t skip = fs->root_length == 1 ? 0 : fs->root_length;
                const size_t n = fs->mount_length + (length - skip);
                if (n > capacity) return 0;
                memcpy(out, fs->mount, fs->mount_length);
                memcpy(out + fs->mount_length, t + skip, length - skip);
                return n;
        }
        if (length > capacity) return 0;
        memcpy(out, t, length);
        return length;
}

/* And back: one into the mount, into the directory mounted. */
static size_t target_there(const sshfs_t *fs, const char *t, size_t length, char *out, size_t capacity) {
        if (under(t, length, fs->mount, fs->mount_length)) {
                const size_t rest = length - fs->mount_length;
                const size_t keep = fs->root_length == 1 && rest > 0 ? 0 : fs->root_length;
                if (keep + rest > capacity) return 0;
                memcpy(out, fs->root, keep);
                memcpy(out + keep, t + fs->mount_length, rest);
                return keep + rest;
        }
        if (length > capacity) return 0;
        memcpy(out, t, length);
        return length;
}

/* ---- Single requests */

static uint32_t send_path(sshfs_worker_t *w, uint8_t type, const char *path, int32_t *err) {
        const uint32_t id = begin(w, type);
        put_text(w, path);
        *err = finish(w);
        return id;
}

static int32_t take_attrs(sshfs_worker_t *w, uint32_t id, attrs_t *a) {
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) {
                const uint32_t code = get_u32(&r);
                return r.bad ? BROKEN : code == FX_OK ? -BMDS_EIO : errno_of(code);
        }
        if (type != FXP_ATTRS) return BROKEN;
        get_attrs(&r, a);
        return r.bad ? BROKEN : 0;
}

static int32_t lstat_of(sshfs_worker_t *w, const char *path, attrs_t *a) {
        int32_t        err;
        const uint32_t id = send_path(w, FXP_LSTAT, path, &err);
        return err != 0 ? err : take_attrs(w, id, a);
}

/* A HANDLE reply's handle, into @p f. */
static int32_t take_handle(sshfs_worker_t *w, uint32_t id, uint8_t *handle, uint32_t *length, uint32_t *code) {
        uint8_t  type;
        reader_t r;
        *code = FX_OK;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) {
                *code = get_u32(&r);
                return r.bad ? BROKEN : *code == FX_OK ? -BMDS_EIO : errno_of(*code);
        }
        uint32_t       n;
        const uint8_t *h = get_str(&r, &n);
        if (type != FXP_HANDLE || r.bad || n == 0 || n > 256) return BROKEN;
        memcpy(handle, h, n);
        *length = n;
        return 0;
}

static int32_t close_handle(sshfs_worker_t *w, const uint8_t *handle, uint32_t length) {
        const uint32_t id = begin(w, FXP_CLOSE);
        put_str(w, handle, length);
        const int32_t err = finish(w);
        return err != 0 ? err : take_status(w, id);
}

/* ---- Files */

/* File @p handle's SFTP handle copied into @p out (another worker may close it meanwhile); with
 * @p close, it is closed here. 0, or -1 (none such). */
static int file_copy(sshfs_t *fs, uint64_t handle, sshfs_file_t *out, int close) {
        const uint64_t slot = (handle & 0xFFFF) - 1;
        if (slot >= SSHFS_FILES) return -1;
        pthread_mutex_lock(&fs->lock);
        sshfs_file_t *f = &fs->files[slot];
        const int     ok = f->used == 1 && f->generation == (uint16_t)(handle >> 16);
        if (ok) {
                *out = *f;
                if (close) f->used = 0;
        }
        pthread_mutex_unlock(&fs->lock);
        return ok ? 0 : -1;
}

/* A free file, taken (being opened), or NULL. */
static sshfs_file_t *file_take(sshfs_t *fs) {
        sshfs_file_t *f = NULL;
        pthread_mutex_lock(&fs->lock);
        for (int i = 0; i < SSHFS_FILES && f == NULL; ++i)
                if (fs->files[i].used == 0) f = &fs->files[i];
        if (f != NULL) f->used = 2;
        pthread_mutex_unlock(&fs->lock);
        return f;
}

/* @p f opened (@p open) or given back; its handle for the kernel. */
static uint64_t file_done(sshfs_t *fs, sshfs_file_t *f, int open) {
        pthread_mutex_lock(&fs->lock);
        f->used = open ? 1 : 0;
        if (open) f->generation++;
        const uint64_t h = (uint64_t)f->generation << 16 | (uint64_t)(f - fs->files + 1);
        pthread_mutex_unlock(&fs->lock);
        return h;
}

/* OPEN into @p f (taken: no other worker touches it). */
static int32_t open_file(sshfs_worker_t *w, sshfs_file_t *f, const char *path, uint32_t flags) {
        const uint32_t pflags = ((flags & BMDS_FS_O_READ) ? PF_READ : 0) | ((flags & BMDS_FS_O_WRITE) ? PF_WRITE : 0) |
                                ((flags & BMDS_FS_O_CREATE) ? PF_CREAT : 0) |
                                ((flags & BMDS_FS_O_TRUNCATE) ? PF_TRUNC : 0) |
                                ((flags & BMDS_FS_O_EXCLUSIVE) ? PF_EXCL : 0);
        attrs_t  a;
        int32_t  lerr, err;
        uint32_t lid = 0, code;
        if (flags & BMDS_FS_O_CREATE) {
                /* Looked at first: the server would follow a link, or make what a dangling one names. */
                lerr = lstat_of(w, path, &a);
                if (lerr == BROKEN) return BROKEN;
                if (lerr == 0) {
                        if (type_of(&a) == BMDS_TYPE_LINK) return -BMDS_ELOOP;
                        if (type_of(&a) == BMDS_TYPE_DIRECTORY) return -BMDS_EISDIR;
                        if (flags & BMDS_FS_O_EXCLUSIVE) return -BMDS_EEXIST;
                } else if (lerr != -BMDS_ENOENT) {
                        return lerr;
                }
        } else {
                lid = send_path(w, FXP_LSTAT, path, &lerr); /* both at once: one round trip */
                if (lerr != 0) return lerr;
        }
        const uint32_t id = begin(w, FXP_OPEN);
        put_text(w, path);
        put_u32(w, pflags);
        put_u32(w, ATTR_PERMISSIONS);
        put_u32(w, 0644);
        err = finish(w);
        if (err != 0) return err;
        if ((flags & BMDS_FS_O_CREATE) == 0 && (lerr = take_attrs(w, lid, &a)) == BROKEN) return BROKEN;
        err = take_handle(w, id, f->handle, &f->length, &code);
        if (err == BROKEN) return BROKEN;
        const uint32_t type = lerr == 0 ? type_of(&a) : BMDS_TYPE_FILE;
        if (err == 0 && (type == BMDS_TYPE_LINK || type == BMDS_TYPE_DIRECTORY)) {
                if (close_handle(w, f->handle, f->length) == BROKEN) return BROKEN;
                return type == BMDS_TYPE_LINK ? -BMDS_ELOOP : -BMDS_EISDIR;
        }
        if (err != 0) {
                /* A link the server couldn't open (a loop, one dangling): still the kernel's to follow. */
                if (lerr == 0 && type == BMDS_TYPE_LINK) return -BMDS_ELOOP;
                if (lerr == 0 && type == BMDS_TYPE_DIRECTORY) return -BMDS_EISDIR;
                if (code == FX_FAILURE && lerr == 0 && (flags & BMDS_FS_O_EXCLUSIVE)) return -BMDS_EEXIST;
                return err;
        }
        return 0;
}

static int32_t op_open(sshfs_worker_t *w, const char *path, uint32_t flags, uint64_t *handle) {
        sshfs_file_t *f = file_take(w->fs);
        if (f == NULL) return -BMDS_EMFILE;
        const int32_t err = open_file(w, f, path, flags);
        *handle = file_done(w->fs, f, err == 0);
        if (err != 0) *handle = 0;
        return err;
}

/* READ into @p out, which has 9 bytes of room before it (the reply's header, written after). */
static int32_t op_read(sshfs_worker_t *w, const bmds_fs_request_t *q, uint8_t *out, uint32_t *count) {
        sshfs_t *const      fs = w->fs;
        sshfs_file_t        copy;
        const sshfs_file_t *f = &copy;
        if (file_copy(fs, q->handle, &copy, 0) != 0) return -BMDS_EBADF;
        const uint32_t want = q->length < fs->max_data ? q->length : fs->max_data;
        /* DATA's header (type, id, length: 9 bytes) just before @p out, so its data lands at it. */
        const uint32_t id = begin_into(w, FXP_READ, out - 9, (size_t)want + 9);
        put_str(w, f->handle, f->length);
        put_u64(w, q->offset);
        put_u32(w, want);
        int32_t err = finish(w);
        if (err != 0) return err;
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) {
                const uint32_t code = get_u32(&r);
                if (r.bad) return BROKEN;
                *count = 0;
                return code == FX_EOF ? 0 : code == FX_OK ? -BMDS_EIO : errno_of(code);
        }
        uint32_t       n;
        const uint8_t *data = get_str(&r, &n);
        if (type != FXP_DATA || r.bad || n > want) return BROKEN;
        if (data != out) memcpy(out, data, n); /* not read in place: too long, never */
        *count = n;
        return 0;
}

static int32_t op_write(sshfs_worker_t *w, const bmds_fs_request_t *q, const uint8_t *data, uint32_t *count) {
        sshfs_t *const      fs = w->fs;
        sshfs_file_t        copy;
        const sshfs_file_t *f = &copy;
        if (file_copy(fs, q->handle, &copy, 0) != 0) return -BMDS_EBADF;
        const uint32_t id = begin(w, FXP_WRITE);
        put_str(w, f->handle, f->length);
        put_u64(w, q->offset);
        put_u32(w, q->length); /* the data's length; the data itself sent after */
        const int32_t err = finish_with(w, data, q->length);
        if (err != 0) return err;
        const int32_t e = take_status(w, id);
        if (e == 0) *count = q->length;
        return e;
}

static int32_t op_truncate(sshfs_worker_t *w, const bmds_fs_request_t *q) {
        sshfs_t *const      fs = w->fs;
        sshfs_file_t        copy;
        const sshfs_file_t *f = &copy;
        if (file_copy(fs, q->handle, &copy, 0) != 0) return -BMDS_EBADF;
        const uint32_t id = begin(w, FXP_FSETSTAT);
        put_str(w, f->handle, f->length);
        put_u32(w, ATTR_SIZE);
        put_u64(w, q->offset);
        const int32_t err = finish(w);
        return err != 0 ? err : take_status(w, id);
}

static int32_t op_close(sshfs_worker_t *w, uint64_t handle) {
        sshfs_t *const fs = w->fs;
        sshfs_file_t   f;
        if (file_copy(fs, handle, &f, 1) != 0) return -BMDS_EBADF;
        return close_handle(w, f.handle, f.length);
}

/* ---- Directories */

/* @p d emptied (its SFTP handle closed if @p tell); whoever has it keeps it. */
static void dir_drop(sshfs_worker_t *w, sshfs_dir_t *d, int tell) {
        if (d->used && tell && d->handle_length != 0) (void)close_handle(w, d->handle, d->handle_length);
        free(d->path);
        free(d->pending);
        const int busy = d->busy;
        memset(d, 0, sizeof(*d));
        d->busy = busy;
        return;
}

/* @p d given back by the worker that had it. */
static void dir_release(sshfs_t *fs, sshfs_dir_t *d) {
        pthread_mutex_lock(&fs->lock);
        d->busy = 0;
        pthread_mutex_unlock(&fs->lock);
        return;
}

/* The directory listing @p path at entry @p offset, now @p w's (busy): one going on there, else
 * one free or the least used moved there. */
static int32_t dir_at(sshfs_worker_t *w, const char *path, size_t length, const char *rpath, uint64_t offset,
                      sshfs_dir_t **out) {
        sshfs_t *const fs = w->fs;
        sshfs_dir_t   *d = NULL, *oldest = NULL;
        int            found = 0;
        pthread_mutex_lock(&fs->lock);
        for (int i = 0; i < SSHFS_DIRS && !found; ++i) {
                sshfs_dir_t *c = &fs->dirs[i];
                if (c->busy) continue;
                if (c->used && c->position == offset && strlen(c->path) == length &&
                    memcmp(c->path, path, length) == 0) {
                        d = c;
                        found = 1;
                        break;
                }
                if (!c->used && d == NULL) d = c;
                if (oldest == NULL || c->last_used < oldest->last_used) oldest = c;
        }
        if (d == NULL) d = oldest;
        if (d != NULL) {
                d->busy = 1;
                d->last_used = ++fs->clock;
        }
        pthread_mutex_unlock(&fs->lock);
        if (d == NULL) return -BMDS_EAGAIN; /* never: a worker has one at most, and there are as many */
        if (found) {
                *out = d;
                return 0;
        }
        if (d->used) dir_drop(w, d, 1);
        d->path = malloc(length + 1);
        if (d->path == NULL) {
                dir_release(fs, d);
                return -BMDS_ENOMEM;
        }
        memcpy(d->path, path, length);
        d->path[length] = 0;
        d->used = 1;
        int32_t        err;
        const uint32_t id = send_path(w, FXP_OPENDIR, rpath, &err);
        uint32_t       code;
        if (err == 0) err = take_handle(w, id, d->handle, &d->handle_length, &code);
        if (err != 0) {
                dir_drop(w, d, 0);
                dir_release(fs, d);
                if (err == BROKEN) return BROKEN;
                /* FAILURE: most likely not a directory. */
                attrs_t a;
                if (code == FX_FAILURE && lstat_of(w, rpath, &a) == 0 && type_of(&a) != BMDS_TYPE_DIRECTORY)
                        return -BMDS_ENOTDIR;
                return err;
        }
        *out = d;
        return 0;
}

/* The next NAME's entries read into @p d (none: the end). */
static int32_t dir_fill(sshfs_worker_t *w, sshfs_dir_t *d) {
        const uint32_t id = begin(w, FXP_READDIR);
        put_str(w, d->handle, d->handle_length);
        int32_t err = finish(w);
        if (err != 0) return err;
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) {
                const uint32_t code = get_u32(&r);
                if (r.bad) return BROKEN;
                if (code == FX_EOF) {
                        d->ended = 1;
                        return 0;
                }
                return code == FX_OK ? -BMDS_EIO : errno_of(code);
        }
        const uint32_t n = get_u32(&r);
        if (type != FXP_NAME || r.bad) return BROKEN;
        uint8_t *p = realloc(d->pending, r.left != 0 ? r.left : 1);
        if (p == NULL) return -BMDS_ENOMEM;
        memcpy(p, r.p, r.left);
        d->pending = p;
        d->pending_length = r.left;
        d->pending_at = 0;
        d->pending_count = n;
        if (n == 0) d->ended = 1;
        return 0;
}

/* An entry of @p name with @p a's attributes at @p out (@p room bytes): its size, 0 if it doesn't fit. */
static size_t put_entry(uint8_t *out, size_t room, const uint8_t *name, uint32_t length, const attrs_t *a) {
        bmds_fs_entry_t e;
        memset(&e, 0, sizeof(e));
        e.size = (uint32_t)((sizeof(e) + length + 7) & ~(size_t)7);
        if (e.size > room) return 0;
        e.type = type_of(a);
        e.bytes = a->size;
        e.modified_ns = (int64_t)a->mtime * 1000000000;
        e.mode = a->permissions & 07777;
        e.name_len = length;
        memset(out, 0, e.size);
        memcpy(out, &e, sizeof(e));
        memcpy(out + sizeof(e), name, length);
        return e.size;
}

/* READDIR's entries from @p d (the worker's). */
static int32_t read_dir(sshfs_worker_t *w, sshfs_dir_t *d, const bmds_fs_request_t *q, uint8_t *out, size_t *n,
                        uint32_t *count) {
        int32_t err;
        *n = 0;
        *count = 0;
        for (;;) {
                if (d->pending_count == 0) {
                        /* None left: what there is now, else the next ones. */
                        if (d->ended || (*count > 0 && d->position >= q->offset)) break;
                        err = dir_fill(w, d);
                        if (err != 0) {
                                dir_drop(w, d, err != BROKEN);
                                return err;
                        }
                        continue;
                }
                reader_t       r = {d->pending + d->pending_at, d->pending_length - d->pending_at, 0};
                uint32_t       nlen, llen;
                attrs_t        a;
                const uint8_t *name = get_str(&r, &nlen);
                (void)get_str(&r, &llen);
                get_attrs(&r, &a);
                if (r.bad) {
                        dir_drop(w, d, 1);
                        return BROKEN;
                }
                const size_t next = d->pending_length - r.left;
                if ((nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.')) {
                        d->pending_at = next;
                        d->pending_count--;
                        continue;
                }
                if (d->position < q->offset) { /* a listing begun again: those given before */
                        d->pending_at = next;
                        d->pending_count--;
                        d->position++;
                        continue;
                }
                const size_t k = put_entry(out + *n, q->length - *n, name, nlen, &a);
                if (k == 0) {
                        if (*count == 0) return -BMDS_ENAMETOOLONG; /* not even one fits */
                        break;
                }
                *n += k;
                (*count)++;
                d->pending_at = next;
                d->pending_count--;
                d->position++;
        }
        if (*count == 0) dir_drop(w, d, 1); /* the end */
        return 0;
}

static int32_t op_readdir(sshfs_worker_t *w, const char *path, size_t length, const char *rpath,
                          const bmds_fs_request_t *q, uint8_t *out, size_t *n, uint32_t *count) {
        sshfs_dir_t  *d;
        const int32_t err = dir_at(w, path, length, rpath, q->offset, &d);
        if (err != 0) return err;
        const int32_t got = read_dir(w, d, q, out, n, count);
        dir_release(w->fs, d);
        return got;
}

/* ---- Names */

/* What a FAILURE of an op on @p path most likely was, by what is there now. */
static int32_t failure_of(sshfs_worker_t *w, uint8_t op, const char *path, int32_t err) {
        attrs_t       a;
        const int32_t l = lstat_of(w, path, &a);
        if (l == BROKEN) return BROKEN;
        const uint32_t type = l == 0 ? type_of(&a) : 0;
        switch (op) {
                case FXP_MKDIR:
                case FXP_SYMLINK: return l == 0 ? -BMDS_EEXIST : err;
                case FXP_RMDIR:
                        if (l != 0) return l;
                        return type != BMDS_TYPE_DIRECTORY ? -BMDS_ENOTDIR : -BMDS_ENOTEMPTY;
                case FXP_REMOVE: return type == BMDS_TYPE_DIRECTORY ? -BMDS_EISDIR : err;
                case FXP_READLINK: return l == 0 && type != BMDS_TYPE_LINK ? -BMDS_EINVAL : err;
                default: return err;
        }
}

static int32_t op_path(sshfs_worker_t *w, uint8_t op, const char *path) {
        const uint32_t id = begin(w, op);
        put_text(w, path);
        if (op == FXP_MKDIR) {
                put_u32(w, ATTR_PERMISSIONS);
                put_u32(w, 0755);
        }
        int32_t err = finish(w);
        if (err != 0) return err;
        uint32_t code = FX_OK;
        err = take_status_code(w, id, &code);
        return err != 0 && code == FX_FAILURE ? failure_of(w, op, path, err) : err;
}

static int32_t op_rename(sshfs_worker_t *w, const char *from, const char *to, uint32_t flags) {
        sshfs_t *const fs = w->fs;
        const int      replace = (flags & BMDS_FS_REPLACE) != 0;
        const uint32_t id = begin(w, replace && fs->posix_rename ? FXP_EXTENDED : FXP_RENAME);
        if (replace && fs->posix_rename) put_text(w, "posix-rename@openssh.com");
        put_text(w, from);
        put_text(w, to);
        int32_t err = finish(w);
        if (err != 0) return err;
        uint32_t code = FX_OK;
        err = take_status_code(w, id, &code);
        if (err == 0 || (code != FX_FAILURE && code != FX_NO_SUCH_FILE)) return err;
        /* Which failure (servers say ENOTDIR as NO_SUCH_FILE too): by what the two names are now. */
        attrs_t       a, b;
        const int32_t la = lstat_of(w, from, &a);
        if (la == BROKEN) return BROKEN;
        if (la != 0) return la;
        const int32_t lb = lstat_of(w, to, &b);
        if (lb == BROKEN) return BROKEN;
        if (lb != 0) return err;
        const int ad = type_of(&a) == BMDS_TYPE_DIRECTORY, bd = type_of(&b) == BMDS_TYPE_DIRECTORY;
        if (ad && !bd) return -BMDS_ENOTDIR;
        if (!ad && bd) return -BMDS_EISDIR;
        if (!replace) return -BMDS_EEXIST;
        if (ad) return -BMDS_ENOTEMPTY;
        if (!fs->posix_rename) {
                /* No replacing rename: the file there removed first (not atomic). */
                err = op_path(w, FXP_REMOVE, to);
                return err != 0 ? err : op_rename(w, from, to, 0);
        }
        return err;
}

static int32_t op_settime(sshfs_worker_t *w, const char *path, int64_t ns) {
        attrs_t a;
        int32_t err = lstat_of(w, path, &a);
        if (err != 0) return err;
        const uint32_t mtime = (uint32_t)(ns >= 0 ? ns / 1000000000 : 0);
        const uint32_t id = begin(w, FXP_SETSTAT);
        put_text(w, path);
        put_u32(w, ATTR_ACMODTIME);
        put_u32(w, (a.flags & ATTR_ACMODTIME) ? a.atime : mtime);
        put_u32(w, mtime);
        err = finish(w);
        return err != 0 ? err : take_status(w, id);
}

static int32_t op_statfs(sshfs_worker_t *w, bmds_fs_space_t *space) {
        sshfs_t *const fs = w->fs;
        memset(space, 0, sizeof(*space));
        if (!fs->statvfs) return 0;
        const uint32_t id = begin(w, FXP_EXTENDED);
        put_text(w, "statvfs@openssh.com");
        put_text(w, fs->root);
        int32_t err = finish(w);
        if (err != 0) return err;
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) return 0; /* not known: none said */
        if (type != FXP_EXTENDED_REPLY) return BROKEN;
        (void)get_u64(&r); /* bsize */
        const uint64_t frsize = get_u64(&r), blocks = get_u64(&r);
        (void)get_u64(&r); /* bfree */
        const uint64_t bavail = get_u64(&r);
        if (r.bad) return BROKEN;
        space->total = blocks * frsize;
        space->free = bavail * frsize;
        return 0;
}

static int32_t op_readlink(sshfs_worker_t *w, const char *path, uint8_t *out, uint32_t room, uint32_t *count) {
        sshfs_t *const fs = w->fs;
        int32_t        err;
        const uint32_t id = send_path(w, FXP_READLINK, path, &err);
        if (err != 0) return err;
        uint8_t  type;
        reader_t r;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type == FXP_STATUS) {
                const uint32_t code = get_u32(&r);
                if (r.bad) return BROKEN;
                err = code == FX_OK ? -BMDS_EIO : errno_of(code);
                return code == FX_FAILURE ? failure_of(w, FXP_READLINK, path, err) : err;
        }
        const uint32_t n = get_u32(&r);
        uint32_t       length;
        const uint8_t *target = get_str(&r, &length);
        if (type != FXP_NAME || r.bad || n < 1) return BROKEN;
        const size_t k = target_here(fs, (const char *)target, length, (char *)out, room);
        if (k == 0) return -BMDS_ENAMETOOLONG;
        *count = (uint32_t)k;
        return 0;
}

static int32_t op_symlink(sshfs_worker_t *w, const char *link, const char *target, size_t length) {
        sshfs_t *const fs = w->fs;
        char           there[SSHFS_PATH_MAX];
        const size_t   n = target_there(fs, target, length, there, sizeof(there));
        if (n == 0) return -BMDS_ENAMETOOLONG;
        /* OpenSSH's order, which its server and ours take: the target, then the link. */
        const uint32_t id = begin(w, FXP_SYMLINK);
        put_str(w, there, n);
        put_text(w, link);
        int32_t err = finish(w);
        if (err != 0) return err;
        uint32_t code = FX_OK;
        err = take_status_code(w, id, &code);
        return err != 0 && code == FX_FAILURE ? failure_of(w, FXP_SYMLINK, link, err) : err;
}

/* ---- The core's calls */

size_t sshfs_reply_max(const sshfs_t *fs) {
        size_t n = fs->max_data;
        if (n < 4096) n = 4096;
        return sizeof(bmds_fs_reply_t) + n + BMDS_FS_PATH_MAX;
}

size_t sshfs_answer(sshfs_worker_t *w, const uint8_t *request, size_t size, uint8_t *reply) {
        sshfs_t *const    fs = w->fs;
        bmds_fs_request_t q;
        bmds_fs_reply_t   y;
        memset(&y, 0, sizeof(y));
        uint8_t *const out = reply + sizeof(y);
        size_t         n = 0;
        int32_t        err = 0;
        if (size < sizeof(q)) return 0;
        memcpy(&q, request, sizeof(q));
        y.id = q.id;
        const char *path = (const char *)request + sizeof(q);
        const char *path2 = path + q.path_len;
        if (q.size != size ||
            (uint64_t)sizeof(q) + q.path_len + q.path2_len + (q.op == BMDS_FS_WRITE ? q.length : 0) > size ||
            q.path_len > BMDS_FS_PATH_MAX || q.path2_len > BMDS_FS_PATH_MAX) {
                err = -BMDS_EINVAL;
        }
        char rpath[SSHFS_PATH_MAX], rpath2[SSHFS_PATH_MAX];
        if (err == 0) err = remote(fs, path, q.path_len, rpath);
        const size_t room = sshfs_reply_max(fs) - sizeof(y);
        if (err == 0) {
                switch (q.op) {
                        case BMDS_FS_STAT: {
                                attrs_t a;
                                err = lstat_of(w, rpath, &a);
                                size_t      nlen;
                                const char *name = last_component(path, q.path_len, &nlen);
                                if (err == 0) n = put_entry(out, room, (const uint8_t *)name, (uint32_t)nlen, &a);
                                break;
                        }
                        case BMDS_FS_OPEN: err = op_open(w, rpath, q.flags, &y.handle); break;
                        case BMDS_FS_READ:
                                if (q.length > room) q.length = (uint32_t)room;
                                err = op_read(w, &q, out, &y.count);
                                n = y.count;
                                break;
                        case BMDS_FS_WRITE:
                                err = op_write(w, &q, (const uint8_t *)path2 + q.path2_len, &y.count);
                                break;
                        case BMDS_FS_CLOSE: err = op_close(w, q.handle); break;
                        case BMDS_FS_READDIR:
                                if (q.length > room) q.length = (uint32_t)room;
                                err = op_readdir(w, path, q.path_len, rpath, &q, out, &n, &y.count);
                                break;
                        case BMDS_FS_MKDIR: err = op_path(w, FXP_MKDIR, rpath); break;
                        case BMDS_FS_RMDIR: err = op_path(w, FXP_RMDIR, rpath); break;
                        case BMDS_FS_REMOVE: err = op_path(w, FXP_REMOVE, rpath); break;
                        case BMDS_FS_RENAME:
                                err = remote(fs, path2, q.path2_len, rpath2);
                                if (err == 0) err = op_rename(w, rpath, rpath2, q.flags);
                                break;
                        case BMDS_FS_SETTIME: err = op_settime(w, rpath, q.time_ns); break;
                        case BMDS_FS_STATFS: {
                                bmds_fs_space_t space;
                                err = op_statfs(w, &space);
                                if (err == 0) {
                                        memcpy(out, &space, sizeof(space));
                                        n = sizeof(space);
                                }
                                break;
                        }
                        case BMDS_FS_TRUNCATE: err = op_truncate(w, &q); break;
                        case BMDS_FS_READLINK: {
                                const uint32_t limit = q.length < room ? q.length : (uint32_t)room;
                                err = op_readlink(w, rpath, out, limit, &y.count);
                                n = y.count;
                                break;
                        }
                        case BMDS_FS_SYMLINK: err = op_symlink(w, rpath, path2, q.path2_len); break;
                        default: err = -BMDS_ENOSYS; break;
                }
        }
        if (err == BROKEN) return 0;
        if (err != 0) {
                y.error = err;
                y.count = 0;
                y.handle = 0;
                n = 0;
        }
        y.size = (uint32_t)(sizeof(y) + n);
        memcpy(reply, &y, sizeof(y));
        return y.size;
}

/* The VERSION's extensions; the limits, if it has them. */
static int32_t hello(sshfs_worker_t *w) {
        sshfs_t *const fs = w->fs;
        w->out_length = 0;
        put_u32(w, 5);
        const uint8_t init = FXP_INIT;
        put(w, &init, 1);
        put_u32(w, 3);
        if (fs->io.send(fs->io.ctx, w->out, w->out_length) != 0) return BROKEN;
        /* VERSION, which has no id: read here, into a slot's buffer. */
        sshfs_slot_t *s = &w->slots[0];
        uint8_t       h[4];
        if (fs->io.recv(fs->io.ctx, h, 4) != 0) return BROKEN;
        const uint32_t n = id_at(h);
        if (n < 5 || n > 65536) return BROKEN;
        uint8_t *data = realloc(s->data, n);
        if (data == NULL) return BROKEN;
        s->data = data;
        s->capacity = n;
        if (fs->io.recv(fs->io.ctx, s->data, n) != 0) return BROKEN;
        reader_t r = {s->data, n, 0};
        if (get_u8(&r) != FXP_VERSION || get_u32(&r) < 3 || r.bad) return BROKEN;
        int limits = 0;
        while (r.left > 0 && !r.bad) {
                uint32_t       nlen, dlen;
                const uint8_t *name = get_str(&r, &nlen);
                (void)get_str(&r, &dlen);
                if (r.bad) break;
#define IS(s) (nlen == sizeof(s) - 1 && memcmp(name, s, nlen) == 0)
                if (IS("posix-rename@openssh.com")) fs->posix_rename = 1;
                if (IS("statvfs@openssh.com")) fs->statvfs = 1;
                if (IS("limits@openssh.com")) limits = 1;
#undef IS
        }
        fs->max_data = 32768; /* what every server takes (the draft's least packet) */
        if (!limits) return 0;
        const uint32_t id = begin(w, FXP_EXTENDED);
        put_text(w, "limits@openssh.com");
        int32_t err = finish(w);
        if (err != 0) return err;
        uint8_t type;
        if (take(w, id, &type, &r) != 0) return BROKEN;
        if (type != FXP_EXTENDED_REPLY) return 0;
        (void)get_u64(&r); /* the packet's */
        const uint64_t rd = get_u64(&r), wr = get_u64(&r);
        if (r.bad) return 0;
        uint64_t m = BMDS_FS_DATA_MAX;
        if (rd != 0 && rd < m) m = rd;
        if (wr != 0 && wr < m) m = wr;
        m &= ~(uint64_t)4095;
        fs->max_data = m < BMDS_FS_DATA_MIN ? BMDS_FS_DATA_MIN : (uint32_t)m;
        return 0;
}

/* @p w's buffers freed. */
static void worker_free(sshfs_worker_t *w) {
        free(w->out);
        w->out = NULL;
        for (int i = 0; i < SSHFS_SLOTS; ++i) {
                free(w->slots[i].data);
                w->slots[i].data = NULL;
                w->slots[i].capacity = 0;
        }
        return;
}

/* @p w made @p fs's, its request buffer allocated (not yet a worker that answers). */
static int worker_make(sshfs_t *fs, sshfs_worker_t *w) {
        memset(w, 0, sizeof(*w));
        w->fs = fs;
        /* Two paths and the fields: a WRITE's data is sent from the request. */
        w->out_capacity = 2 * SSHFS_PATH_MAX + 1024;
        w->out = malloc(w->out_capacity);
        return w->out != NULL ? 0 : -1;
}

/* sshfs_start's work, on @p w (no workers yet: replies read as they are waited for). */
static int start(sshfs_worker_t *w, const char *dir, const char *mount, char *error, size_t error_size) {
        sshfs_t *const fs = w->fs;
        fs->mount_length = strlen(mount);
        while (fs->mount_length > 1 && mount[fs->mount_length - 1] == '/') fs->mount_length--;
        if (mount[0] != '/' || fs->mount_length >= sizeof(fs->mount)) {
                snprintf(error, error_size, "%s: not an absolute path", mount);
                return -1;
        }
        memcpy(fs->mount, mount, fs->mount_length);
        fs->mount[fs->mount_length] = 0;
        if (hello(w) != 0) {
                snprintf(error, error_size, "no SFTP server answered");
                return -1;
        }
        /* The directory, absolute on the server. */
        int32_t        err;
        const uint32_t id = send_path(w, FXP_REALPATH, dir[0] != 0 ? dir : ".", &err);
        uint8_t        type;
        reader_t       r;
        if (err != 0 || take(w, id, &type, &r) != 0) {
                snprintf(error, error_size, "the connection closed");
                return -1;
        }
        if (type == FXP_STATUS) {
                snprintf(error, error_size, "%s: %s", dir[0] != 0 ? dir : ".",
                         get_u32(&r) == FX_NO_SUCH_FILE ? "no such directory" : "not found on the server");
                return -1;
        }
        const uint32_t n = get_u32(&r);
        uint32_t       length;
        const uint8_t *name = get_str(&r, &length);
        if (type != FXP_NAME || r.bad || n < 1 || length == 0 || name[0] != '/' || length >= BMDS_FS_PATH_MAX) {
                snprintf(error, error_size, "the server's directory is not one this can mount");
                return -1;
        }
        while (length > 1 && name[length - 1] == '/') --length;
        memcpy(fs->root, name, length);
        fs->root[length] = 0;
        fs->root_length = length;
        attrs_t        a;
        const uint32_t sid = send_path(w, FXP_STAT, fs->root, &err);
        if (err != 0 || (err = take_attrs(w, sid, &a)) != 0 || type_of(&a) != BMDS_TYPE_DIRECTORY) {
                snprintf(error, error_size, "%s: %s", fs->root,
                         err == BROKEN ? "the connection closed"
                         : err != 0    ? "not found"
                                       : "not a directory");
                return -1;
        }
        return 0;
}

int sshfs_start(sshfs_t *fs, const sshfs_io_t *io, const char *dir, const char *mount, char *error, size_t error_size) {
        fs->io = *io;
        fs->next_id = 1;
        pthread_mutex_init(&fs->lock, NULL);
        pthread_mutex_init(&fs->send_lock, NULL);
        sshfs_worker_t w;
        if (worker_make(fs, &w) != 0) {
                snprintf(error, error_size, "out of memory");
                return -1;
        }
        const int r = start(&w, dir, mount, error, error_size);
        worker_free(&w);
        return r;
}

int sshfs_worker_init(sshfs_t *fs, sshfs_worker_t *w) {
        if (worker_make(fs, w) != 0) return -1;
        pthread_cond_init(&w->ready, NULL);
        pthread_mutex_lock(&fs->lock);
        const int room = fs->worker_count < SSHFS_WORKERS;
        if (room) fs->workers[fs->worker_count++] = w;
        pthread_mutex_unlock(&fs->lock);
        if (!room) {
                pthread_cond_destroy(&w->ready);
                worker_free(w);
                return -1;
        }
        return 0;
}

void sshfs_worker_finish(sshfs_worker_t *w) {
        sshfs_t *const fs = w->fs;
        pthread_mutex_lock(&fs->lock);
        for (size_t i = 0; i < fs->worker_count; ++i) {
                if (fs->workers[i] != w) continue;
                fs->workers[i] = fs->workers[--fs->worker_count];
                break;
        }
        pthread_mutex_unlock(&fs->lock);
        pthread_cond_destroy(&w->ready);
        worker_free(w);
        return;
}

void sshfs_read_replies(sshfs_t *fs) {
        while (deliver(fs, NULL) == 0) {}
        pthread_mutex_lock(&fs->lock);
        fs->broken = 1;
        for (size_t i = 0; i < fs->worker_count; ++i) pthread_cond_signal(&fs->workers[i]->ready);
        pthread_mutex_unlock(&fs->lock);
        return;
}

void sshfs_finish(sshfs_t *fs) {
        for (int i = 0; i < SSHFS_DIRS; ++i) dir_drop(NULL, &fs->dirs[i], 0);
        pthread_mutex_destroy(&fs->lock);
        pthread_mutex_destroy(&fs->send_lock);
        return;
}
