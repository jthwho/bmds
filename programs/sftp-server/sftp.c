/**
 * @file      sftp.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * SFTP version 3's requests and replies (sftp.h). Everything a client sends is checked here
 * before any of it is used: lengths against what is left, strings for NULs where they become
 * paths, handles against the table. Fuzzed (tests/fuzz/sftp.cpp).
 */

#include "sftp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Request and reply types. */
enum {
        FXP_INIT = 1,
        FXP_VERSION = 2,
        FXP_OPEN = 3,
        FXP_CLOSE = 4,
        FXP_READ = 5,
        FXP_WRITE = 6,
        FXP_LSTAT = 7,
        FXP_FSTAT = 8,
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

/* Status codes. */
enum {
        FX_OK = 0,
        FX_EOF = 1,
        FX_NO_SUCH_FILE = 2,
        FX_PERMISSION_DENIED = 3,
        FX_FAILURE = 4,
        FX_BAD_MESSAGE = 5,
        FX_OP_UNSUPPORTED = 8,
};

/* Attribute flags, and OPEN's. */
#define ATTR_SIZE 0x1u
#define ATTR_UIDGID 0x2u
#define ATTR_PERMISSIONS 0x4u
#define ATTR_ACMODTIME 0x8u
#define ATTR_EXTENDED 0x80000000u

#define OPEN_READ 0x1u
#define OPEN_WRITE 0x2u
#define OPEN_APPEND 0x4u
#define OPEN_CREAT 0x8u
#define OPEN_TRUNC 0x10u
#define OPEN_EXCL 0x20u

#define VERSION 3u
#define PATH_MAX_BYTES 1024u
#define HANDLE_BYTES 8u
#define NAME_ENTRY_RESERVE 1800u /* a READDIR entry at most: name, longname, attributes */

enum {
        HANDLE_FREE = 0,
        HANDLE_FILE = 1,
        HANDLE_DIR = 2
};

/* --- Reading a request ------------------------------------------------------------- */

typedef struct reader {
                const uint8_t *p;
                const uint8_t *end;
                int            ok;
} reader_t;

static uint32_t get_u32(reader_t *r) {
        if (!r->ok || r->end - r->p < 4) {
                r->ok = 0;
                return 0;
        }
        const uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
        r->p += 4;
        return v;
}

static uint64_t get_u64(reader_t *r) {
        const uint64_t high = get_u32(r);
        return high << 32 | get_u32(r);
}

/* A string: its bytes stay in the request. */
static const uint8_t *get_bytes(reader_t *r, uint32_t *length) {
        const uint32_t n = get_u32(r);
        if (!r->ok || (size_t)(r->end - r->p) < n) {
                r->ok = 0;
                *length = 0;
                return NULL;
        }
        const uint8_t *s = r->p;
        r->p += n;
        *length = n;
        return s;
}

/* A string as text, copied with its NUL; refused if it holds one or is too long. */
static int get_text(reader_t *r, char *out, size_t size) {
        uint32_t       n = 0;
        const uint8_t *s = get_bytes(r, &n);
        if (!r->ok) return 0;
        if (n >= size || memchr(s, 0, n) != NULL) return -1;
        memcpy(out, s, n);
        out[n] = '\0';
        return 1;
}

typedef struct attrs {
                uint32_t flags;
                uint64_t size;
                uint32_t permissions;
                uint32_t atime, mtime;
} attrs_t;

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
                const uint32_t count = get_u32(r);
                for (uint32_t i = 0; i < count && r->ok; ++i) {
                        uint32_t n;
                        (void)get_bytes(r, &n);
                        (void)get_bytes(r, &n);
                }
        }
        return;
}

/* --- Writing a reply ------------------------------------------------------------------ */

typedef struct writer {
                sftp_out_t *out;
                size_t      start; /* the packet's length field */
                int         ok;
} writer_t;

static void put_raw(writer_t *w, const void *data, size_t n) {
        if (!w->ok || w->out->capacity - w->out->length < n) {
                w->ok = 0;
                return;
        }
        memcpy(w->out->data + w->out->length, data, n);
        w->out->length += n;
        return;
}

static void put_u8(writer_t *w, uint8_t v) {
        put_raw(w, &v, 1);
        return;
}

static void put_u32(writer_t *w, uint32_t v) {
        const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        put_raw(w, b, 4);
        return;
}

static void put_u64(writer_t *w, uint64_t v) {
        put_u32(w, (uint32_t)(v >> 32));
        put_u32(w, (uint32_t)v);
        return;
}

static void put_bytes(writer_t *w, const void *data, uint32_t n) {
        put_u32(w, n);
        put_raw(w, data, n);
        return;
}

static void put_text(writer_t *w, const char *s) {
        put_bytes(w, s, (uint32_t)strlen(s));
        return;
}

static writer_t begin(sftp_out_t *out, uint8_t type) {
        writer_t w = {out, out->length, 1};
        put_u32(&w, 0);
        put_u8(&w, type);
        return w;
}

/* Fills in the length; a reply that didn't fit is taken back. */
static int finish(writer_t *w) {
        if (!w->ok) {
                w->out->length = w->start;
                return -1;
        }
        const size_t n = w->out->length - w->start - 4;
        uint8_t     *p = w->out->data + w->start;
        p[0] = (uint8_t)(n >> 24);
        p[1] = (uint8_t)(n >> 16);
        p[2] = (uint8_t)(n >> 8);
        p[3] = (uint8_t)n;
        return 0;
}

static void put_attrs(writer_t *w, const sftp_stat_t *st) {
        put_u32(w, ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME);
        put_u64(w, st->size);
        put_u32(w, st->uid);
        put_u32(w, st->gid);
        put_u32(w, st->mode);
        put_u32(w, st->atime);
        put_u32(w, st->mtime);
        return;
}

static void reply_status(sftp_out_t *out, uint32_t id, uint32_t code, const char *message) {
        writer_t w = begin(out, FXP_STATUS);
        put_u32(&w, id);
        put_u32(&w, code);
        put_text(&w, message);
        put_text(&w, "");
        (void)finish(&w);
        return;
}

/* A call's failure as a status. */
static void reply_errno(sftp_out_t *out, uint32_t id, int err) {
        switch (-err) {
                case ENOENT:
                case ENOTDIR: reply_status(out, id, FX_NO_SUCH_FILE, "No such file"); break;
                case EACCES:
                case EPERM:
                case EROFS: reply_status(out, id, FX_PERMISSION_DENIED, "Permission denied"); break;
                case ENOSYS:
                case EOPNOTSUPP: reply_status(out, id, FX_OP_UNSUPPORTED, "Operation unsupported"); break;
                case EEXIST: reply_status(out, id, FX_FAILURE, "File exists"); break;
                case ENOTEMPTY: reply_status(out, id, FX_FAILURE, "Directory not empty"); break;
                case EISDIR: reply_status(out, id, FX_FAILURE, "Is a directory"); break;
                case ENOSPC: reply_status(out, id, FX_FAILURE, "No space left"); break;
                default: reply_status(out, id, FX_FAILURE, "Failure"); break;
        }
        return;
}

static void reply_ok(sftp_out_t *out, uint32_t id, int result) {
        if (result < 0)
                reply_errno(out, id, result);
        else
                reply_status(out, id, FX_OK, "Success");
        return;
}

/* --- Paths ---------------------------------------------------------------------------- */

/* @p in made absolute against the current directory and normalized (".", "..", "//"), as text:
 * links aren't followed (BMDS's /home is one; its users see it as /home). */
static int absolute(sftp_server_t *s, const char *in, char *out, size_t size) {
        char joined[2 * PATH_MAX_BYTES + 2];
        if (in[0] == '/') {
                if (strlen(in) >= sizeof(joined)) return -ENAMETOOLONG;
                strcpy(joined, in);
        } else {
                char      cwd[PATH_MAX_BYTES + 1];
                const int r = s->fs->getcwd(s->ctx, cwd, sizeof(cwd));
                if (r < 0) return r;
                cwd[sizeof(cwd) - 1] = '\0';
                if (strlen(cwd) + 1 + strlen(in) >= sizeof(joined)) return -ENAMETOOLONG;
                strcpy(joined, cwd);
                strcat(joined, "/");
                strcat(joined, in);
        }
        /* Component by component into out. */
        size_t      n = 0;
        const char *p = joined;
        while (*p != '\0') {
                while (*p == '/') ++p;
                const char *start = p;
                while (*p != '\0' && *p != '/') ++p;
                const size_t len = (size_t)(p - start);
                if (len == 0 || (len == 1 && start[0] == '.')) continue;
                if (len == 2 && start[0] == '.' && start[1] == '.') {
                        while (n > 0 && out[n - 1] != '/') --n;
                        if (n > 0) --n; /* the slash; ".." never climbs above / */
                        continue;
                }
                if (n + 1 + len >= size || n + 1 + len > PATH_MAX_BYTES) return -ENAMETOOLONG;
                out[n++] = '/';
                memcpy(out + n, start, len);
                n += len;
        }
        if (n == 0) out[n++] = '/';
        out[n] = '\0';
        return 0;
}

/* A path argument: read, absolute, normalized. 1, 0 (bad request: replied), -1 (protocol). */
static int get_path(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out, char *path, size_t size) {
        char      raw[PATH_MAX_BYTES + 1];
        const int t = get_text(r, raw, sizeof(raw));
        if (t == 0) return -1;
        if (t < 0) {
                reply_status(out, id, FX_BAD_MESSAGE, "Bad path");
                return 0;
        }
        const int err = absolute(s, raw, path, size);
        if (err < 0) {
                reply_errno(out, id, err);
                return 0;
        }
        return 1;
}

/* --- Handles -------------------------------------------------------------------------- */

static void put_handle(writer_t *w, sftp_server_t *s, int index) {
        uint8_t        h[HANDLE_BYTES];
        const uint32_t g = s->handles[index].generation;
        h[0] = (uint8_t)(index >> 24);
        h[1] = (uint8_t)(index >> 16);
        h[2] = (uint8_t)(index >> 8);
        h[3] = (uint8_t)index;
        h[4] = (uint8_t)(g >> 24);
        h[5] = (uint8_t)(g >> 16);
        h[6] = (uint8_t)(g >> 8);
        h[7] = (uint8_t)g;
        put_bytes(w, h, HANDLE_BYTES);
        return;
}

/* A handle argument of @p kind: its index, -2 (bad: replied), or -1 (protocol). */
static int get_handle(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out, int kind) {
        uint32_t       n = 0;
        const uint8_t *h = get_bytes(r, &n);
        if (!r->ok) return -1;
        if (n == HANDLE_BYTES) {
                const uint32_t index = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
                const uint32_t g = (uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
                if (index < SFTP_MAX_HANDLES && s->handles[index].kind != HANDLE_FREE &&
                    s->handles[index].generation == g && (kind == 0 || s->handles[index].kind == kind))
                        return (int)index;
        }
        reply_status(out, id, FX_FAILURE, "Invalid handle");
        return -2;
}

static int new_handle(sftp_server_t *s, int kind, int file, const char *path) {
        for (int i = 0; i < SFTP_MAX_HANDLES; ++i) {
                if (s->handles[i].kind != HANDLE_FREE) continue;
                char *copy = malloc(strlen(path) + 1);
                if (copy == NULL) return -ENOMEM;
                strcpy(copy, path);
                s->handles[i].kind = kind;
                s->handles[i].id = file;
                s->handles[i].path = copy;
                s->handles[i].generation = ++s->generation;
                return i;
        }
        return -EMFILE;
}

static int close_handle(sftp_server_t *s, int index) {
        const int kind = s->handles[index].kind;
        const int id = s->handles[index].id;
        free(s->handles[index].path);
        s->handles[index].path = NULL;
        s->handles[index].kind = HANDLE_FREE;
        return kind == HANDLE_FILE ? s->fs->close(s->ctx, id) : s->fs->closedir(s->ctx, id);
}

/* --- Listings ------------------------------------------------------------------------- */

/* `ls -l`'s line for an entry (READDIR's longname; clients show it). */
static void longname(const char *name, const sftp_stat_t *st, char *out, size_t size) {
        char           mode[11] = "----------";
        const uint32_t m = st->mode;
        switch (m & 0170000u) {
                case 0040000u: mode[0] = 'd'; break;
                case 0120000u: mode[0] = 'l'; break;
                case 0020000u: mode[0] = 'c'; break;
                case 0060000u: mode[0] = 'b'; break;
                case 0010000u: mode[0] = 'p'; break;
                case 0140000u: mode[0] = 's'; break;
                default: break;
        }
        static const char rwx[] = "rwxrwxrwx";
        for (int i = 0; i < 9; ++i)
                if (m & (0400u >> i)) mode[1 + i] = rwx[i];
        char      when[16] = "";
        time_t    t = (time_t)st->mtime;
        struct tm tm;
        if (localtime_r(&t, &tm) != NULL) (void)strftime(when, sizeof(when), "%b %e %H:%M", &tm);
        (void)snprintf(out, size, "%s %4u %-8s %-8s %8llu %s %s", mode, (unsigned)(st->nlink ? st->nlink : 1), "root",
                       "root", (unsigned long long)st->size, when, name);
        return;
}

/* NAME with one entry (REALPATH, READLINK). */
static void reply_name(sftp_out_t *out, uint32_t id, const char *name) {
        writer_t w = begin(out, FXP_NAME);
        put_u32(&w, id);
        put_u32(&w, 1);
        put_text(&w, name);
        put_text(&w, name);
        put_u32(&w, 0); /* no attributes */
        if (finish(&w) < 0) reply_status(out, id, FX_FAILURE, "Name too long");
        return;
}

/* --- Requests ------------------------------------------------------------------------- */

static void do_init(sftp_server_t *s, sftp_out_t *out) {
        s->version = VERSION;
        writer_t w = begin(out, FXP_VERSION);
        put_u32(&w, VERSION);
        static const char *const extensions[][2] = {
                {"posix-rename@openssh.com", "1"}, {"fsync@openssh.com", "1"},   {"limits@openssh.com", "1"},
                {"expand-path@openssh.com", "1"},  {"statvfs@openssh.com", "2"},
        };
        for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
                put_text(&w, extensions[i][0]);
                put_text(&w, extensions[i][1]);
        }
        (void)finish(&w);
        return;
}

static int do_open(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        char      path[PATH_MAX_BYTES + 1];
        const int got = get_path(s, r, id, out, path, sizeof(path));
        if (got <= 0) return got;
        const uint32_t pflags = get_u32(r);
        attrs_t        a;
        get_attrs(r, &a);
        if (!r->ok) return -1;
        int flags = (pflags & OPEN_READ) && (pflags & OPEN_WRITE) ? O_RDWR
                    : (pflags & OPEN_WRITE)                       ? O_WRONLY
                                                                  : O_RDONLY;
        if (pflags & OPEN_APPEND) flags |= O_APPEND;
        if (pflags & OPEN_CREAT) flags |= O_CREAT;
        if (pflags & OPEN_TRUNC) flags |= O_TRUNC;
        if (pflags & OPEN_EXCL) flags |= O_EXCL;
        const uint32_t mode = (a.flags & ATTR_PERMISSIONS) ? (a.permissions & 07777u) : 0644u;
        const int      file = s->fs->open(s->ctx, path, flags, mode);
        if (file < 0) {
                reply_errno(out, id, file);
                return 1;
        }
        const int index = new_handle(s, HANDLE_FILE, file, path);
        if (index < 0) {
                (void)s->fs->close(s->ctx, file);
                reply_errno(out, id, index);
                return 1;
        }
        writer_t w = begin(out, FXP_HANDLE);
        put_u32(&w, id);
        put_handle(&w, s, index);
        (void)finish(&w);
        return 1;
}

static int do_read(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        const int index = get_handle(s, r, id, out, HANDLE_FILE);
        if (index < 0) return index == -1 ? -1 : 1;
        const uint64_t offset = get_u64(r);
        uint32_t       size = get_u32(r);
        if (!r->ok) return -1;
        if (size > SFTP_MAX_DATA) size = SFTP_MAX_DATA;
        /* Straight into the reply: header, then the data where its string goes. */
        writer_t w = begin(out, FXP_DATA);
        put_u32(&w, id);
        put_u32(&w, 0);
        if (!w.ok || out->capacity - out->length < size) {
                out->length = w.start;
                reply_status(out, id, FX_FAILURE, "Read too large");
                return 1;
        }
        const long n = s->fs->pread(s->ctx, s->handles[index].id, out->data + out->length, size, offset);
        if (n <= 0) {
                out->length = w.start;
                if (n == 0)
                        reply_status(out, id, FX_EOF, "End of file");
                else
                        reply_errno(out, id, (int)n);
                return 1;
        }
        uint8_t *len = out->data + out->length - 4;
        len[0] = (uint8_t)((uint32_t)n >> 24);
        len[1] = (uint8_t)((uint32_t)n >> 16);
        len[2] = (uint8_t)((uint32_t)n >> 8);
        len[3] = (uint8_t)n;
        out->length += (size_t)n;
        (void)finish(&w);
        return 1;
}

static int do_write(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        const int index = get_handle(s, r, id, out, HANDLE_FILE);
        if (index < 0) return index == -1 ? -1 : 1;
        uint64_t       offset = get_u64(r);
        uint32_t       n = 0;
        const uint8_t *data = get_bytes(r, &n);
        if (!r->ok) return -1;
        while (n > 0) {
                const long w = s->fs->pwrite(s->ctx, s->handles[index].id, data, n, offset);
                if (w <= 0) {
                        reply_errno(out, id, w == 0 ? -ENOSPC : (int)w);
                        return 1;
                }
                data += w;
                n -= (uint32_t)w;
                offset += (uint64_t)w;
        }
        reply_status(out, id, FX_OK, "Success");
        return 1;
}

static void reply_attrs(sftp_out_t *out, uint32_t id, int result, const sftp_stat_t *st) {
        if (result < 0) {
                reply_errno(out, id, result);
                return;
        }
        writer_t w = begin(out, FXP_ATTRS);
        put_u32(&w, id);
        put_attrs(&w, st);
        (void)finish(&w);
        return;
}

static int do_stat(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out, int follow) {
        char      path[PATH_MAX_BYTES + 1];
        const int got = get_path(s, r, id, out, path, sizeof(path));
        if (got <= 0) return got;
        sftp_stat_t st;
        memset(&st, 0, sizeof(st));
        reply_attrs(out, id, s->fs->stat(s->ctx, path, &st, follow), &st);
        return 1;
}

static int do_fstat(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        const int index = get_handle(s, r, id, out, HANDLE_FILE);
        if (index < 0) return index == -1 ? -1 : 1;
        sftp_stat_t st;
        memset(&st, 0, sizeof(st));
        reply_attrs(out, id, s->fs->fstat(s->ctx, s->handles[index].id, &st), &st);
        return 1;
}

/* SETSTAT's and FSETSTAT's changes: the size and the times (modes and owners are FAT's: kept). */
static int apply_attrs(sftp_server_t *s, const char *path, int file, const attrs_t *a) {
        int err = 0;
        if (a->flags & ATTR_SIZE)
                err = file >= 0 ? s->fs->ftruncate(s->ctx, file, a->size) : s->fs->truncate(s->ctx, path, a->size);
        if (err >= 0 && (a->flags & ATTR_ACMODTIME)) err = s->fs->utimes(s->ctx, path, a->atime, a->mtime);
        return err;
}

static int do_setstat(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        char      path[PATH_MAX_BYTES + 1];
        const int got = get_path(s, r, id, out, path, sizeof(path));
        if (got <= 0) return got;
        attrs_t a;
        get_attrs(r, &a);
        if (!r->ok) return -1;
        reply_ok(out, id, apply_attrs(s, path, -1, &a));
        return 1;
}

static int do_fsetstat(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        const int index = get_handle(s, r, id, out, HANDLE_FILE);
        if (index < 0) return index == -1 ? -1 : 1;
        attrs_t a;
        get_attrs(r, &a);
        if (!r->ok) return -1;
        reply_ok(out, id, apply_attrs(s, s->handles[index].path, s->handles[index].id, &a));
        return 1;
}

static int do_opendir(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        char      path[PATH_MAX_BYTES + 1];
        const int got = get_path(s, r, id, out, path, sizeof(path));
        if (got <= 0) return got;
        const int dir = s->fs->opendir(s->ctx, path);
        if (dir < 0) {
                reply_errno(out, id, dir);
                return 1;
        }
        const int index = new_handle(s, HANDLE_DIR, dir, path);
        if (index < 0) {
                (void)s->fs->closedir(s->ctx, dir);
                reply_errno(out, id, index);
                return 1;
        }
        writer_t w = begin(out, FXP_HANDLE);
        put_u32(&w, id);
        put_handle(&w, s, index);
        (void)finish(&w);
        return 1;
}

/* READDIR: as many entries as fit (at most 100), or EOF. */
static int do_readdir(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        const int index = get_handle(s, r, id, out, HANDLE_DIR);
        if (index < 0) return index == -1 ? -1 : 1;
        writer_t w = begin(out, FXP_NAME);
        put_u32(&w, id);
        const size_t count_at = out->length;
        put_u32(&w, 0);
        uint32_t             count = 0;
        static sftp_dirent_t entry; /* large: not on the stack */
        while (count < 100 && out->capacity - out->length > NAME_ENTRY_RESERVE) {
                memset(&entry, 0, sizeof(entry));
                const int got = s->fs->readdir(s->ctx, s->handles[index].id, &entry);
                if (got < 0 && count == 0) {
                        out->length = w.start;
                        reply_errno(out, id, got);
                        return 1;
                }
                if (got <= 0) break;
                entry.name[sizeof(entry.name) - 1] = '\0';
                if (!entry.have_st) {
                        char full[2 * PATH_MAX_BYTES + 2];
                        (void)snprintf(full, sizeof(full), "%s/%s", s->handles[index].path, entry.name);
                        if (s->fs->stat(s->ctx, full, &entry.st, 0) < 0) memset(&entry.st, 0, sizeof(entry.st));
                }
                char line[sizeof(entry.name) + 96];
                longname(entry.name, &entry.st, line, sizeof(line));
                put_text(&w, entry.name);
                put_text(&w, line);
                put_attrs(&w, &entry.st);
                ++count;
        }
        if (count == 0) {
                out->length = w.start;
                reply_status(out, id, FX_EOF, "End of directory");
                return 1;
        }
        uint8_t *c = out->data + count_at;
        c[0] = (uint8_t)(count >> 24);
        c[1] = (uint8_t)(count >> 16);
        c[2] = (uint8_t)(count >> 8);
        c[3] = (uint8_t)count;
        if (finish(&w) < 0) reply_status(out, id, FX_FAILURE, "Listing too large");
        return 1;
}

static int do_path_call(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out, int type) {
        char      path[PATH_MAX_BYTES + 1];
        const int got = get_path(s, r, id, out, path, sizeof(path));
        if (got <= 0) return got;
        switch (type) {
                case FXP_REMOVE: reply_ok(out, id, s->fs->remove(s->ctx, path)); break;
                case FXP_RMDIR: reply_ok(out, id, s->fs->rmdir(s->ctx, path)); break;
                case FXP_MKDIR: {
                        attrs_t a;
                        get_attrs(r, &a);
                        if (!r->ok) return -1;
                        const uint32_t mode = (a.flags & ATTR_PERMISSIONS) ? (a.permissions & 07777u) : 0755u;
                        reply_ok(out, id, s->fs->mkdir(s->ctx, path, mode));
                        break;
                }
                case FXP_REALPATH: reply_name(out, id, path); break;
                case FXP_READLINK: {
                        char      target[PATH_MAX_BYTES + 1];
                        const int n = s->fs->readlink(s->ctx, path, target, sizeof(target) - 1);
                        if (n < 0) {
                                reply_errno(out, id, n);
                                break;
                        }
                        target[n < (int)sizeof(target) ? n : (int)sizeof(target) - 1] = '\0';
                        reply_name(out, id, target);
                        break;
                }
                default: break;
        }
        return 1;
}

/* RENAME (fails if the new name exists, as version 3 says) and posix-rename (replaces). */
static int do_rename(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out, int replace) {
        char from[PATH_MAX_BYTES + 1], to[PATH_MAX_BYTES + 1];
        int  got = get_path(s, r, id, out, from, sizeof(from));
        if (got <= 0) return got;
        got = get_path(s, r, id, out, to, sizeof(to));
        if (got <= 0) return got;
        if (!replace) {
                sftp_stat_t st;
                if (s->fs->stat(s->ctx, to, &st, 0) >= 0) {
                        reply_status(out, id, FX_FAILURE, "File exists");
                        return 1;
                }
        }
        reply_ok(out, id, s->fs->rename(s->ctx, from, to, replace));
        return 1;
}

/* SYMLINK, in OpenSSH's order (the target first, then the link), as its clients send it. */
static int do_symlink(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        char      target[PATH_MAX_BYTES + 1], link[PATH_MAX_BYTES + 1];
        const int t = get_text(r, target, sizeof(target));
        if (t == 0) return -1;
        const int got = get_path(s, r, id, out, link, sizeof(link));
        if (got <= 0) return got;
        if (t < 0) {
                reply_status(out, id, FX_BAD_MESSAGE, "Bad target");
                return 1;
        }
        reply_ok(out, id, s->fs->symlink(s->ctx, target, link));
        return 1;
}

static int do_extended(sftp_server_t *s, reader_t *r, uint32_t id, sftp_out_t *out) {
        char      name[64];
        const int t = get_text(r, name, sizeof(name));
        if (t == 0) return -1;
        if (t > 0 && strcmp(name, "posix-rename@openssh.com") == 0) return do_rename(s, r, id, out, 1);
        if (t > 0 && strcmp(name, "fsync@openssh.com") == 0) {
                const int index = get_handle(s, r, id, out, HANDLE_FILE);
                if (index < 0) return index == -1 ? -1 : 1;
                reply_ok(out, id, s->fs->fsync(s->ctx, s->handles[index].id));
                return 1;
        }
        if (t > 0 && strcmp(name, "statvfs@openssh.com") == 0) {
                char      path[PATH_MAX_BYTES + 1];
                const int got = get_path(s, r, id, out, path, sizeof(path));
                if (got <= 0) return got;
                sftp_statvfs_t v;
                memset(&v, 0, sizeof(v));
                const int err = s->fs->statvfs(s->ctx, path, &v);
                if (err < 0) {
                        reply_errno(out, id, err);
                        return 1;
                }
                writer_t w = begin(out, FXP_EXTENDED_REPLY);
                put_u32(&w, id);
                const uint64_t fields[] = {v.bsize, v.frsize, v.blocks, v.bfree, v.bavail, v.files,
                                           v.ffree, v.favail, v.fsid,   v.flag,  v.namemax};
                for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) put_u64(&w, fields[i]);
                (void)finish(&w);
                return 1;
        }
        if (t > 0 && strcmp(name, "limits@openssh.com") == 0) {
                writer_t w = begin(out, FXP_EXTENDED_REPLY);
                put_u32(&w, id);
                put_u64(&w, SFTP_MAX_PACKET);
                put_u64(&w, SFTP_MAX_DATA);
                put_u64(&w, SFTP_MAX_DATA);
                put_u64(&w, SFTP_MAX_HANDLES);
                (void)finish(&w);
                return 1;
        }
        if (t > 0 && strcmp(name, "expand-path@openssh.com") == 0) {
                /* ~ and ~/... are the current directory's start (the server starts in the user's home);
                 * other paths as REALPATH. */
                char      raw[PATH_MAX_BYTES + 1], path[PATH_MAX_BYTES + 1];
                const int p = get_text(r, raw, sizeof(raw));
                if (p == 0) return -1;
                if (p < 0) {
                        reply_status(out, id, FX_BAD_MESSAGE, "Bad path");
                        return 1;
                }
                const char *rest = raw;
                if (raw[0] == '~' && (raw[1] == '\0' || raw[1] == '/')) rest = raw[1] == '/' ? raw + 2 : raw + 1;
                const int err = absolute(s, rest, path, sizeof(path));
                if (err < 0)
                        reply_errno(out, id, err);
                else
                        reply_name(out, id, path);
                return 1;
        }
        reply_status(out, id, FX_OP_UNSUPPORTED, "Unsupported extension");
        return 1;
}

/* --- The server ------------------------------------------------------------------------- */

void sftp_init(sftp_server_t *s, const sftp_fs_t *fs, void *ctx) {
        memset(s, 0, sizeof(*s));
        s->fs = fs;
        s->ctx = ctx;
        return;
}

int sftp_request(sftp_server_t *s, const uint8_t *packet, uint32_t length, sftp_out_t *out) {
        reader_t r = {packet, packet + length, 1};
        if (length < 1) return -1;
        const uint8_t type = packet[0];
        r.p++;
        if (type == FXP_INIT) {
                (void)get_u32(&r); /* the client's version: we speak 3 */
                if (!r.ok) return -1;
                do_init(s, out);
                return 0;
        }
        if (s->version == 0) return -1; /* INIT first */
        const uint32_t id = get_u32(&r);
        if (!r.ok) return -1;
        int result = 1;
        switch (type) {
                case FXP_OPEN: result = do_open(s, &r, id, out); break;
                case FXP_CLOSE: {
                        const int index = get_handle(s, &r, id, out, 0);
                        if (index == -1) return -1;
                        if (index >= 0) reply_ok(out, id, close_handle(s, index));
                        break;
                }
                case FXP_READ: result = do_read(s, &r, id, out); break;
                case FXP_WRITE: result = do_write(s, &r, id, out); break;
                case FXP_LSTAT: result = do_stat(s, &r, id, out, 0); break;
                case FXP_STAT: result = do_stat(s, &r, id, out, 1); break;
                case FXP_FSTAT: result = do_fstat(s, &r, id, out); break;
                case FXP_SETSTAT: result = do_setstat(s, &r, id, out); break;
                case FXP_FSETSTAT: result = do_fsetstat(s, &r, id, out); break;
                case FXP_OPENDIR: result = do_opendir(s, &r, id, out); break;
                case FXP_READDIR: result = do_readdir(s, &r, id, out); break;
                case FXP_REMOVE:
                case FXP_MKDIR:
                case FXP_RMDIR:
                case FXP_REALPATH:
                case FXP_READLINK: result = do_path_call(s, &r, id, out, type); break;
                case FXP_RENAME: result = do_rename(s, &r, id, out, 0); break;
                case FXP_SYMLINK: result = do_symlink(s, &r, id, out); break;
                case FXP_EXTENDED: result = do_extended(s, &r, id, out); break;
                default: reply_status(out, id, FX_OP_UNSUPPORTED, "Unsupported request"); break;
        }
        return result < 0 ? -1 : 0;
}

void sftp_finish(sftp_server_t *s) {
        for (int i = 0; i < SFTP_MAX_HANDLES; ++i)
                if (s->handles[i].kind != HANDLE_FREE) (void)close_handle(s, i);
        return;
}
