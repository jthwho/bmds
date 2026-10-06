/**
 * @file      linux.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Linux system calls musl makes, carried out on the BMDS system call
 * table (devplan/programs.md §12). musl's syscall_arch.h sends every call
 * here instead of to ecall; the result is what Linux would return: a value,
 * or a negated errno (BMDS's values are Linux's). Programs never see this
 * layer: they see musl's POSIX functions.
 *
 * What BMDS doesn't have is approximated, as noted at each call: one user
 * (root), no hard links, modes made up from the FAT attributes, O_NONBLOCK
 * per descriptor, signals delivered between calls (programs.md §15), no fork()
 * (vfork, and posix_spawn on it: programs.md §13). Sockets fail.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

/* Linux's futex operations (musl's public headers don't have them). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1 /* renameat2() flags, as Linux */
#define RENAME_EXCHANGE 2
#endif

/* --- Time ---------------------------------------------------------------- */

static int64_t now(uint32_t clock) {
        int64_t ns = 0;
        (void)bmds_clock_get(clock, &ns);
        return ns;
}

/* CPU time: the program's own time running, approximated by its age (the
 * program has its core to itself while it computes). */
static int64_t started_ns;

/* Linux's struct timespec for the time64 calls: two 64-bit words. musl's
 * own has a 32-bit tv_nsec and padding, so only the low half of the second
 * word is read. */
static int64_t ts_read(const void *ts) {
        const int64_t *t = ts;
        return t[0] * 1000000000 + (int32_t)t[1];
}

static void ts_write(void *ts, int64_t ns) {
        int64_t *t = ts;
        t[0] = ns / 1000000000;
        t[1] = ns % 1000000000;
        return;
}

static long clock_ns(long clock, int64_t *ns) {
        switch (clock) {
                case CLOCK_REALTIME:
                case CLOCK_REALTIME_COARSE: *ns = now(BMDS_CLOCK_REALTIME); return 0;
                case CLOCK_MONOTONIC:
                case CLOCK_MONOTONIC_RAW:
                case CLOCK_MONOTONIC_COARSE:
                case CLOCK_BOOTTIME: *ns = now(BMDS_CLOCK_MONOTONIC); return 0;
                case CLOCK_PROCESS_CPUTIME_ID:
                case CLOCK_THREAD_CPUTIME_ID: *ns = now(BMDS_CLOCK_MONOTONIC) - started_ns; return 0;
                default: return -EINVAL;
        }
}

static long sleep_ns(int64_t ns) {
        return ns > 0 ? (long)(bmds_sleep(ns)) : 0;
}

/* --- Paths and directories ------------------------------------------------ */

/*
 * A directory open as a file descriptor (open(O_DIRECTORY), as opendir()
 * does): a BMDS directory handle behind a number above every real fd.
 * Linux lists "." and ".." first; BMDS doesn't, so they are added. An entry
 * that didn't fit a getdents call waits in `held` for the next.
 */
#define DIR_FDS 8
#define DIR_FD0 0x4000

typedef struct {
                int32_t     handle; /* -1: free */
                char       *path;   /* as opened, for fchdir, rewinding and openat */
                uint32_t    pos;    /* entries returned (for "." and "..", and d_off) */
                char       *held;   /* an entry's name that didn't fit, or NULL */
                bmds_stat_t held_info;
} dir_t;

/* What Linux keeps per open file or per terminal and this layer keeps per
 * program (programs.md §16): O_NONBLOCK by descriptor number (a dup'd
 * descriptor and other programs don't share it), and the terminal's VMIN
 * and VTIME. A non-blocking read or write polls first and fails with
 * EAGAIN if it would wait; a raw terminal read with VMIN 0 polls for VTIME
 * tenths of a second and returns 0 if nothing came. */
typedef struct {
                uint32_t nonblock; /* a bit per descriptor (atomic) */
                uint8_t  vmin;
                uint8_t  vtime;
                uint8_t  timed; /* raw with VMIN 0: terminal reads poll first */
} io_modes_t;

/* musl's k_sigaction (src/internal/ksigaction.h, with SA_RESTORER as riscv32 has it). */
struct k_sigaction {
                void (*handler)(int);
                unsigned long flags;
                void (*restorer)(void);
                unsigned mask[2];
};

/*
 * What Linux keeps per process and this layer keeps in the program's memory:
 * the directory descriptors, O_NONBLOCK and the terminal's VMIN and VTIME,
 * the signal handlers, the umask. A vfork child works on a copy (see
 * __bmds_vfork_enter), so the parent's other threads never see its changes.
 * The process's own are shared by its threads: changes are made under
 * tables_lock (O_NONBLOCK's bits atomically).
 */
typedef struct {
                dir_t              dirs[DIR_FDS];
                io_modes_t         io_modes;
                struct k_sigaction actions[_NSIG];
                mode_t             umask;
} tables_t;

static tables_t process_tables = {
        .dirs = {[0 ... DIR_FDS - 1] = {.handle = -1}},
        .io_modes = {0, 1, 0, 0},
        .umask = 022,
};

/* musl's internal lock (src/thread/__lock.c): it costs nothing until a second thread starts. */
void __lock(volatile int *);
void __unlock(volatile int *);

static volatile int tables_lock[1];

/* vfork state, per thread: how deep the vfork children running on it are,
 * and for each the parent's registers (vfork.S) and the child's tables. */
#define VFORK_DEPTH 2

typedef struct {
                uint32_t regs[26];
                tables_t tables;
} vfork_saved_t;

_Thread_local int                   __bmds_vfork_depth;
static _Thread_local vfork_saved_t *vfork_saved[VFORK_DEPTH];

static int tp_ready; /* musl has set tp up: thread-local variables are there */

/* The tables the calling thread works on: the process's, or its vfork child's copy. */
static tables_t *tables(void) {
        return tp_ready && __bmds_vfork_depth > 0 ? &vfork_saved[__bmds_vfork_depth - 1]->tables : &process_tables;
}

static int dir_index(long fd) {
        const long i = fd - DIR_FD0;
        return i >= 0 && i < DIR_FDS && tables()->dirs[i].handle >= 0 ? (int)i : -1;
}

static char *copy_string(const char *s) {
        const size_t n = strlen(s) + 1;
        char        *p = bmds_mem_alloc((uint32_t)n, 0, 0);
        if (p != NULL) memcpy(p, s, n);
        return p;
}

/*
 * The path a call names, relative to @p dirfd as the *at() calls take it:
 * the path itself when absolute or with AT_FDCWD, else joined to a
 * directory fd's path in @p buf.
 */
static const char *at_path(long dirfd, const char *path, char *buf, size_t size) {
        if (path == NULL) return NULL;
        if (path[0] == '/' || dirfd == AT_FDCWD) return path;
        __lock(tables_lock);
        const int    i = dir_index(dirfd);
        const char  *dir = i >= 0 ? tables()->dirs[i].path : NULL;
        const size_t a = dir != NULL ? strlen(dir) : 0, b = strlen(path);
        const int    fits = dir != NULL && a + 1 + b + 1 <= size;
        if (fits) {
                memcpy(buf, dir, a);
                buf[a] = '/';
                memcpy(buf + a + 1, path, b + 1);
        }
        __unlock(tables_lock);
        return fits ? buf : NULL;
}

#define AT_PATH(dirfd, path)                                                                                           \
        char        at_buf_[BMDS_PATH_MAX + 1];                                                                        \
        const char *p = at_path((dirfd), (const char *)(path), at_buf_, sizeof(at_buf_));                              \
        if (p == NULL) return (path) == 0 ? -EFAULT : -EBADF

static long open_dir(const char *path) {
        char *copy = copy_string(path);
        if (copy == NULL) return -ENOMEM;
        const int32_t h = bmds_opendir(path);
        if (h < 0) {
                bmds_mem_free(copy);
                return (long)(h);
        }
        __lock(tables_lock);
        dir_t *dirs = tables()->dirs;
        int    i = 0;
        while (i < DIR_FDS && dirs[i].handle >= 0) ++i;
        if (i < DIR_FDS) {
                dirs[i].handle = h;
                dirs[i].path = copy;
                dirs[i].pos = 0;
                dirs[i].held = NULL;
        }
        __unlock(tables_lock);
        if (i == DIR_FDS) {
                (void)bmds_closedir(h);
                bmds_mem_free(copy);
                return -EMFILE;
        }
        return DIR_FD0 + i;
}

static long close_dir(int i) {
        __lock(tables_lock);
        dir_t        *d = &tables()->dirs[i];
        const int32_t handle = d->handle;
        char         *path = d->path, *held = d->held;
        d->handle = -1;
        __unlock(tables_lock);
        /* A vfork child has no directories of its own (they are per process):
         * the handle, and the memory the entry points to, are the parent's. */
        if (__bmds_vfork_depth > 0) return 0;
        const int32_t r = bmds_closedir(handle);
        bmds_mem_free(path);
        bmds_mem_free(held);
        return (long)(r);
}

/* Back to the first entry: BMDS directories don't rewind, so reopen. Under tables_lock. */
static long rewind_dir(dir_t *d) {
        const int32_t h = bmds_opendir(d->path);
        if (h < 0) return (long)(h);
        (void)bmds_closedir(d->handle);
        bmds_mem_free(d->held);
        d->handle = h;
        d->pos = 0;
        d->held = NULL;
        return 0;
}

static unsigned char d_type(uint32_t type) {
        return type == BMDS_TYPE_DIRECTORY ? DT_DIR
             : type == BMDS_TYPE_DEVICE    ? DT_CHR
             : type == BMDS_TYPE_LINK      ? DT_LNK
                                           : DT_REG;
}

/* One getdents64 record; 0 if it doesn't fit. */
static size_t put_dirent(char *buf, size_t room, const char *name, uint64_t ino, unsigned char type, uint32_t pos) {
        const size_t n = strlen(name);
        const size_t size = (offsetof(struct dirent, d_name) + n + 1 + 7) & ~(size_t)7;
        if (size > room) return 0;
        struct dirent *e = (struct dirent *)buf;
        e->d_ino = ino != 0 ? ino : 1;
        e->d_off = pos;
        e->d_reclen = (unsigned short)size;
        e->d_type = type;
        memcpy(e->d_name, name, n + 1);
        return size;
}

/* One call's worth of entries from directory @p d (under tables_lock). */
static long read_dir(dir_t *d, char *buf, size_t len) {
        size_t at = 0;
        for (;;) {
                char        name[BMDS_NAME_MAX + 1];
                bmds_stat_t st = {.size = sizeof(st)};
                const char *entry = name;
                if (d->pos < 2) {
                        entry = d->pos == 0 ? "." : "..";
                        st.type = BMDS_TYPE_DIRECTORY;
                } else if (d->held != NULL) {
                        entry = d->held;
                        st = d->held_info;
                } else {
                        const int32_t n = bmds_readdir(d->handle, &st, name, sizeof(name));
                        if (n < 0) return at > 0 ? (long)at : (long)(n);
                        if (n == 0) return (long)at;
                }
                const size_t put = put_dirent(buf + at, len - at, entry, st.file_id, d_type(st.type), d->pos + 1);
                if (put == 0) {
                        if (at == 0) return -EINVAL;
                        if (entry == name) {
                                d->held = copy_string(name);
                                d->held_info = st;
                                if (d->held == NULL) return -ENOMEM;
                        }
                        return (long)at;
                }
                if (entry == d->held) {
                        bmds_mem_free(d->held);
                        d->held = NULL;
                }
                at += put;
                ++d->pos;
        }
}

static long do_getdents(long fd, char *buf, size_t len) {
        __lock(tables_lock);
        const int  i = dir_index(fd);
        const long r = i >= 0 ? read_dir(&tables()->dirs[i], buf, len) : -EBADF;
        __unlock(tables_lock);
        return r;
}

/* --- Files ------------------------------------------------------------------ */

static uint32_t open_flags(long flags) {
        uint32_t f = 0;
        switch (flags & O_ACCMODE) {
                case O_RDONLY: f = BMDS_O_READ; break;
                case O_WRONLY: f = BMDS_O_WRITE; break;
                default: f = BMDS_O_READ | BMDS_O_WRITE; break;
        }
        if (f & BMDS_O_WRITE) {
                if (flags & O_CREAT) f |= BMDS_O_CREATE;
                if ((flags & O_EXCL) && (flags & O_CREAT)) f |= BMDS_O_EXCL;
                if (flags & O_TRUNC) f |= BMDS_O_TRUNC;
                if (flags & O_APPEND) f |= BMDS_O_APPEND;
        }
        if (flags & O_CLOEXEC) f |= BMDS_O_CLOEXEC;
        return f;
}

static void set_nonblock(long fd, int on) {
        if (fd < 0 || fd >= 32) return;
        uint32_t *bits = &tables()->io_modes.nonblock;
        if (on) {
                __atomic_fetch_or(bits, 1u << fd, __ATOMIC_RELAXED);
        } else {
                __atomic_fetch_and(bits, ~(1u << fd), __ATOMIC_RELAXED);
        }
        return;
}

static int is_nonblock(long fd) {
        return fd >= 0 && fd < 32 && (__atomic_load_n(&tables()->io_modes.nonblock, __ATOMIC_RELAXED) >> fd & 1u);
}

/* Before a read or write: 1 to go on; 0, a timed terminal read that got
 * nothing (the read returns 0); or -EAGAIN, -EINTR. */
static long io_gate(long fd, int writing) {
        const int         nonblock = is_nonblock(fd);
        const io_modes_t *modes = &tables()->io_modes;
        int32_t           ms = 0;
        if (!nonblock) {
                if (writing || !modes->timed || bmds_isatty((int32_t)fd) != 1) return 1;
                ms = modes->vtime * 100;
        }
        bmds_pollfd_t p = {.size = sizeof(p), .fd = (int32_t)fd, .events = writing ? BMDS_POLL_OUT : BMDS_POLL_IN};
        const int32_t r = bmds_io_poll(&p, 1, ms, NULL);
        if (r < 0) return (long)(r);
        if (r == 0) return nonblock ? -EAGAIN : 0;
        return 1;
}

static long gated_read(long fd, void *buf, size_t size) {
        const long g = io_gate(fd, 0);
        return g <= 0 ? g : (long)(bmds_read((int32_t)fd, buf, (uint32_t)size));
}

static long gated_write(long fd, const void *buf, size_t size) {
        const long g = io_gate(fd, 1);
        return g <= 0 ? g : (long)(bmds_write((int32_t)fd, buf, (uint32_t)size));
}

static long do_open(long dirfd, const char *path, long flags) {
        AT_PATH(dirfd, path);
        if (flags & O_NOFOLLOW) {
                bmds_stat_t st = {.size = sizeof(st)};
                if (bmds_lstat(p, &st) == 0 && st.type == BMDS_TYPE_LINK) return -ELOOP;
        }
        if (flags & O_DIRECTORY) return open_dir(p);
        const int32_t fd = bmds_open(p, open_flags(flags));
        /* open(".", O_RDONLY), for fchdir(), is a directory too. */
        if (fd == -BMDS_EISDIR && (flags & O_ACCMODE) == O_RDONLY) return open_dir(p);
        set_nonblock(fd, (flags & O_NONBLOCK) != 0);
        return (long)(fd);
}

static long do_close(long fd) {
        const int i = dir_index(fd);
        if (i >= 0) return close_dir(i);
        set_nonblock(fd, 0);
        return (long)(bmds_close((int32_t)fd));
}

static long io_vector(long fd, const struct iovec *v, long count, int writing) {
        const long g = io_gate(fd, writing);
        if (g <= 0) return g;
        long total = 0;
        for (long i = 0; i < count; ++i) {
                if (v[i].iov_len == 0) continue;
                const int32_t n = writing ? bmds_write((int32_t)fd, v[i].iov_base, (uint32_t)v[i].iov_len)
                                          : bmds_read((int32_t)fd, v[i].iov_base, (uint32_t)v[i].iov_len);
                if (n < 0) return total > 0 ? total : (long)(n);
                total += n;
                if ((size_t)n < v[i].iov_len) break;
        }
        return total;
}

/* pread/pwrite: at an offset, the fd's position kept. */
static long io_at(long fd, void *buf, size_t size, int64_t offset, int writing) {
        int64_t here, ignored;
        int32_t r = bmds_seek((int32_t)fd, 0, BMDS_SEEK_CUR, &here);
        if (r == 0) r = bmds_seek((int32_t)fd, offset, BMDS_SEEK_SET, &ignored);
        if (r < 0) return (long)(r);
        const int32_t n = writing ? bmds_write((int32_t)fd, buf, (uint32_t)size) : bmds_read((int32_t)fd, buf, (uint32_t)size);
        (void)bmds_seek((int32_t)fd, here, BMDS_SEEK_SET, &ignored);
        return (long)(n);
}

static long do_llseek(long fd, int64_t offset, int64_t *out, long whence) {
        if (dir_index(fd) >= 0) {
                if (offset != 0 || whence != SEEK_SET) return -EINVAL;
                __lock(tables_lock);
                const int  i = dir_index(fd);
                const long r = i >= 0 ? rewind_dir(&tables()->dirs[i]) : -EBADF;
                __unlock(tables_lock);
                if (r == 0) *out = 0;
                return r;
        }
        return (long)(bmds_seek((int32_t)fd, offset, (uint32_t)whence, out));
}

/* Starts like a program or a script (what the shell runs). */
static int runnable_content(const char *path) {
        char          head[4] = {0};
        const int32_t fd = bmds_open(path, BMDS_O_READ);
        if (fd < 0) return 0;
        const int32_t got = bmds_read(fd, head, sizeof(head));
        (void)bmds_close(fd);
        return (got >= 4 && memcmp(head, "\x7f" "ELF", 4) == 0) || (got >= 2 && head[0] == '#' && head[1] == '!');
}

/*
 * Whether a file gets execute bits, as the BMDS shell's `test -x`: .elf
 * and .sh by name; a name without an extension (as Unix programs and
 * scripts are named) by its first bytes; anything else not, without
 * reading it (`ls -l` stats every file).
 */
static int runnable(const char *path) {
        if (path == NULL) return 0;
        const char  *base = strrchr(path, '/');
        const char  *dot = strrchr(base != NULL ? base + 1 : path, '.');
        const size_t n = strlen(path);
        if ((n > 4 && strcmp(path + n - 4, ".elf") == 0) || (n > 3 && strcmp(path + n - 3, ".sh") == 0)) return 1;
        return dot == NULL && runnable_content(path);
}

/* A file id when the kernel has none (a device without one): the path hashed. */
static uint64_t path_hash(const char *p) {
        uint64_t h = 14695981039346656037ull;
        for (; *p != '\0'; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
        return h;
}

static void to_statx(const bmds_stat_t *st, const char *path, long fd, struct statx *x) {
        memset(x, 0, sizeof(*x));
        x->stx_mask = STATX_BASIC_STATS | STATX_BTIME;
        x->stx_blksize = 4096;
        x->stx_nlink = st->type == BMDS_TYPE_DIRECTORY ? 2 : 1;
        if (st->type == BMDS_TYPE_LINK) {
                x->stx_mode = S_IFLNK | 0777;
        } else if (st->type == BMDS_TYPE_DIRECTORY) {
                x->stx_mode = S_IFDIR | 0755;
        } else if (st->type == BMDS_TYPE_DEVICE) {
                x->stx_mode = S_IFCHR | 0666;
        } else {
                x->stx_mode = S_IFREG | ((st->attributes & BMDS_ATTR_READONLY) ? 0444 : 0644);
                if (runnable(path)) x->stx_mode |= 0111;
        }
        x->stx_ino = st->file_id != 0 ? st->file_id : path != NULL ? path_hash(path) : 0x8000000000000000ull | (uint64_t)fd;
        x->stx_dev_major = st->device != 0 ? st->device : 1;
        x->stx_size = st->file_size;
        x->stx_blocks = (st->file_size + 511) / 512;
        x->stx_mtime.tv_sec = st->modified_ns / 1000000000;
        x->stx_mtime.tv_nsec = (uint32_t)(st->modified_ns % 1000000000);
        x->stx_atime = x->stx_ctime = x->stx_btime = x->stx_mtime;
        return;
}

static long do_statx(long dirfd, const char *path, long flags, struct statx *out) {
        bmds_stat_t st = {.size = sizeof(st)};
        if (out == NULL) return -EFAULT;
        if ((flags & AT_EMPTY_PATH) && (path == NULL || path[0] == '\0')) {
                if (dir_index(dirfd) >= 0) {
                        /* Its path, as openat() would join it. */
                        char path_buf[BMDS_PATH_MAX + 1];
                        if (at_path(dirfd, ".", path_buf, sizeof(path_buf)) == NULL) return -EBADF;
                        path_buf[strlen(path_buf) - 2] = '\0'; /* without its "/." */
                        const int32_t r = bmds_stat(path_buf, &st);
                        if (r < 0) return (long)(r);
                        to_statx(&st, path_buf, dirfd, out);
                        return 0;
                }
                const int32_t r = bmds_fstat((int32_t)dirfd, &st);
                if (r < 0) return (long)(r);
                to_statx(&st, NULL, dirfd, out);
                return 0;
        }
        AT_PATH(dirfd, path);
        const int nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0; /* lstat: the link itself */
        const int32_t r = nofollow ? bmds_lstat(p, &st) : bmds_stat(p, &st);
        if (r < 0) return (long)(r);
        to_statx(&st, p, -1, out);
        return 0;
}

static long do_readlink(long dirfd, const char *path, char *buf, size_t size) {
        AT_PATH(dirfd, path);
        char          target[BMDS_PATH_MAX + 1];
        const int32_t n = bmds_readlink(p, target, sizeof(target));
        if (n < 0) return (long)(n);
        /* POSIX: no NUL, as much as fits. */
        const size_t len = (size_t)n < size ? (size_t)n : size;
        memcpy(buf, target, len);
        return (long)len;
}

static long do_symlink(const char *target, long dirfd, const char *path) {
        AT_PATH(dirfd, path);
        return (long)(bmds_symlink(target, p));
}

/* access(): F_OK exists; W_OK not read-only; X_OK as the shell's `test -x`
 * (a directory, a program, a #! script, a .sh file), by content. */
static long do_access(long dirfd, const char *path, long mode) {
        AT_PATH(dirfd, path);
        bmds_stat_t st = {.size = sizeof(st)};
        const int32_t r = bmds_stat(p, &st);
        if (r < 0) return (long)(r);
        if ((mode & W_OK) && st.type == BMDS_TYPE_FILE && (st.attributes & BMDS_ATTR_READONLY)) return -EACCES;
        if ((mode & X_OK) && st.type != BMDS_TYPE_DIRECTORY) {
                const size_t n = strlen(p);
                const int    sh = n > 3 && strcmp(p + n - 3, ".sh") == 0;
                if (st.type != BMDS_TYPE_FILE || !(sh || runnable_content(p))) return -EACCES;
        }
        return 0;
}

static long do_utimens(long dirfd, const char *path, const void *times) {
        if (path == NULL) return 0; /* futimens(): by fd, BMDS needs a name; ignored */
        AT_PATH(dirfd, path);
        int64_t ns = now(BMDS_CLOCK_REALTIME);
        if (times != NULL) {
                const int64_t *t = times; /* [atime sec, nsec, mtime sec, nsec] */
                const int32_t  nsec = (int32_t)t[3];
                if (nsec == UTIME_OMIT) return 0;
                if (nsec != UTIME_NOW) ns = t[2] * 1000000000 + nsec;
        }
        return (long)(bmds_set_mtime(p, ns));
}

static long do_ftruncate(long fd, int64_t size) {
        return (long)(bmds_ftruncate((int32_t)fd, size));
}

static long do_truncate(const char *path, int64_t size) {
        const int32_t fd = bmds_open(path, BMDS_O_WRITE);
        if (fd < 0) return (long)(fd);
        const long r = do_ftruncate(fd, size);
        (void)bmds_close(fd);
        return r;
}

static long do_fcntl(long fd, long cmd, long arg) {
        switch (cmd) {
                case F_DUPFD: {
                        const int32_t n = bmds_dup((int32_t)fd, (int32_t)arg);
                        set_nonblock(n, 0);
                        return (long)(n);
                }
                case F_DUPFD_CLOEXEC: {
                        const int32_t n = bmds_dup((int32_t)fd, (int32_t)arg);
                        if (n >= 0) (void)bmds_fd_flags(n, BMDS_FD_CLOEXEC);
                        set_nonblock(n, 0);
                        return (long)(n);
                }
                case F_GETFD:
                        /* Directory fds are the SDK's own: closed by an exec anyway. */
                        if (dir_index(fd) >= 0) return FD_CLOEXEC;
                        return (long)(bmds_fd_flags((int32_t)fd, -1));
                case F_SETFD: {
                        if (dir_index(fd) >= 0) return 0;
                        const int32_t r = bmds_fd_flags((int32_t)fd, (arg & FD_CLOEXEC) ? BMDS_FD_CLOEXEC : 0);
                        return r < 0 ? (long)(r) : 0;
                }
                case F_SETFL: {
                        if (dir_index(fd) >= 0) return 0;
                        bmds_stat_t   st = {.size = sizeof(st)};
                        const int32_t r = bmds_fstat((int32_t)fd, &st);
                        if (r < 0) return (long)(r);
                        set_nonblock(fd, (arg & O_NONBLOCK) != 0);
                        return 0;
                }
                case F_GETFL: {
                        if (dir_index(fd) >= 0) return O_RDONLY | O_DIRECTORY;
                        bmds_stat_t   st = {.size = sizeof(st)};
                        const int32_t r = bmds_fstat((int32_t)fd, &st);
                        if (r < 0) return (long)(r);
                        const uint32_t rw = st.open_flags & (BMDS_O_READ | BMDS_O_WRITE);
                        const long     mode = rw == BMDS_O_WRITE ? O_WRONLY : rw == BMDS_O_READ ? O_RDONLY : O_RDWR;
                        return mode | (is_nonblock(fd) ? O_NONBLOCK : 0);
                }
                case F_GETLK: ((struct flock *)arg)->l_type = F_UNLCK; return 0;
                case F_SETLK:
                case F_SETLKW: return 0; /* one process at a time owns a file anyway */
                default: return -EINVAL;
        }
}

/* --- The terminal ----------------------------------------------------------- */

/* termios over the console's modes: ICANON (line input), ECHO, ICRNL (raw
 * input), ISIG (Ctrl-C interrupts), OPOST|ONLCR (newlines as CR LF); VMIN
 * and VTIME are this layer's (io_modes). The other flags and control
 * characters are reported as Linux's defaults and can't be changed. */
static long tc_get(long fd, struct termios *t) {
        bmds_term_t   term = {.size = sizeof(term)};
        const int32_t r = bmds_term_get((int32_t)fd, &term);
        if (r < 0) return -ENOTTY;
        memset(t, 0, sizeof(*t));
        t->c_iflag = (term.flags & BMDS_TERM_ICRNL) ? ICRNL : 0;
        t->c_oflag = (term.flags & BMDS_TERM_ONLCR) ? OPOST | ONLCR : 0;
        t->c_cflag = CS8 | CREAD | B115200;
        t->c_lflag = ((term.flags & BMDS_TERM_LINE) ? ICANON : 0) |
                     ((term.flags & BMDS_TERM_ECHO) ? ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE : 0) |
                     ((term.flags & BMDS_TERM_SIGNALS) ? ISIG : 0) | IEXTEN;
        t->c_cc[VINTR] = 3;
        t->c_cc[VQUIT] = 28;
        t->c_cc[VERASE] = 127;
        t->c_cc[VKILL] = 21;
        t->c_cc[VEOF] = 4;
        t->c_cc[VSTART] = 17;
        t->c_cc[VSTOP] = 19;
        t->c_cc[VSUSP] = 26;
        t->c_cc[VMIN] = tables()->io_modes.vmin;
        t->c_cc[VTIME] = tables()->io_modes.vtime;
        t->__c_ispeed = t->__c_ospeed = B115200;
        return 0;
}

static long tc_set(long fd, const struct termios *t) {
        bmds_term_t   term = {.size = sizeof(term)};
        const int32_t r = bmds_term_get((int32_t)fd, &term);
        if (r < 0) return -ENOTTY;
        uint32_t flags = term.flags & (BMDS_TERM_COLORS | BMDS_TERM_LOGS);
        if (t->c_lflag & ICANON) flags |= BMDS_TERM_LINE;
        if (t->c_lflag & ECHO) flags |= BMDS_TERM_ECHO;
        if (t->c_lflag & ISIG) flags |= BMDS_TERM_SIGNALS;
        if (t->c_iflag & ICRNL) flags |= BMDS_TERM_ICRNL;
        if ((t->c_oflag & OPOST) && (t->c_oflag & ONLCR)) flags |= BMDS_TERM_ONLCR;
        const int32_t s = bmds_term_set((int32_t)fd, flags);
        if (s < 0) return (long)(s);
        io_modes_t *modes = &tables()->io_modes;
        modes->vmin = t->c_cc[VMIN];
        modes->vtime = t->c_cc[VTIME];
        modes->timed = !(t->c_lflag & ICANON) && t->c_cc[VMIN] == 0;
        return 0;
}

static long do_ioctl(long fd, unsigned long request, void *arg) {
        switch (request) {
                case TCGETS: return tc_get(fd, arg);
                case TCSETS:
                case TCSETSW:
                case TCSETSF: return tc_set(fd, arg);
                case TIOCGWINSZ: {
                        bmds_term_t term = {.size = sizeof(term)};
                        if (bmds_term_get((int32_t)fd, &term) < 0) return -ENOTTY;
                        struct winsize *w = arg;
                        w->ws_row = term.rows;
                        w->ws_col = term.columns;
                        w->ws_xpixel = w->ws_ypixel = 0;
                        return 0;
                }
                case TIOCGPGRP: {
                        const int32_t group = bmds_term_foreground((int32_t)fd, 0);
                        if (group < 0) return (long)(group);
                        *(pid_t *)arg = (pid_t)(group);
                        return 0;
                }
                case TIOCSPGRP: {
                        const pid_t group = *(const pid_t *)arg;
                        if (group <= 0) return -EINVAL;
                        const int32_t r = bmds_term_foreground((int32_t)fd, (int32_t)group);
                        return r < 0 ? (long)(r) : 0;
                }
                case TIOCGSID: {
                        /* The caller's session, if this is its terminal. */
                        const int32_t r = bmds_term_foreground((int32_t)fd, 0);
                        if (r < 0) return (long)(r);
                        *(pid_t *)arg = (pid_t)(bmds_proc_group(BMDS_GROUP_SESSION, 0, 0));
                        return 0;
                }
                case TCFLSH:
                case TCXONC:
                case TCSBRK:
                case TIOCSCTTY: return bmds_isatty((int32_t)fd) == 1 ? 0 : -ENOTTY;
                default: return -ENOTTY;
        }
}

/* poll, ppoll, select, pselect: io_poll, on a copy of the entries in the
 * kernel's form (programs.md §16). The timeout is rounded up to whole
 * milliseconds and isn't written back. With a mask, it is in effect while
 * the kernel waits and while the handlers of what interrupted it run. */
static int      deliver_due(void);
static uint32_t kernel_set(uint64_t set);

static long poll_entries(bmds_pollfd_t *k, unsigned long n, const void *timeout, const uint64_t *mask) {
        int32_t ms = -1;
        if (timeout != NULL) {
                const int64_t ns = ts_read(timeout);
                if (ns < 0) return -EINVAL;
                ms = ns >= (int64_t)INT32_MAX * 1000000 ? INT32_MAX : (int32_t)((ns + 999999) / 1000000);
        }
        if (mask == NULL) return (long)(bmds_io_poll(k, (uint32_t)n, ms, NULL));
        const uint32_t set = kernel_set(*mask);
        const int32_t  old = bmds_sig_mask(BMDS_SIG_BLOCK, 0);
        const int32_t  r = bmds_io_poll(k, (uint32_t)n, ms, &set);
        (void)deliver_due();
        (void)bmds_sig_mask(BMDS_SIG_SETMASK, (uint32_t)old);
        return (long)(r);
}

static long do_poll(struct pollfd *fds, unsigned long n, const void *timeout, const uint64_t *mask) {
        if (n > BMDS_POLL_MAX) return -EINVAL;
        bmds_pollfd_t k[n > 0 ? n : 1];
        for (unsigned long i = 0; i < n; ++i) {
                const short e = fds[i].events;
                k[i].size = sizeof(k[i]);
                k[i].fd = fds[i].fd;
                k[i].events = ((e & (POLLIN | POLLRDNORM)) ? BMDS_POLL_IN : 0) |
                              ((e & (POLLOUT | POLLWRNORM)) ? BMDS_POLL_OUT : 0) | ((e & POLLPRI) ? BMDS_POLL_PRI : 0);
        }
        const long r = poll_entries(k, n, timeout, mask);
        if (r < 0) return r;
        for (unsigned long i = 0; i < n; ++i) {
                const short    e = fds[i].events;
                const uint32_t got = k[i].revents;
                fds[i].revents = (short)(((got & BMDS_POLL_IN) ? e & (POLLIN | POLLRDNORM) : 0) |
                                         ((got & BMDS_POLL_OUT) ? e & (POLLOUT | POLLWRNORM) : 0) |
                                         ((got & BMDS_POLL_ERR) ? POLLERR : 0) | ((got & BMDS_POLL_HUP) ? POLLHUP : 0) |
                                         ((got & BMDS_POLL_NVAL) ? POLLNVAL : 0));
        }
        return r;
}

/* select's sets as poll entries: readable includes hung up and broken,
 * writable broken, as on Linux; nothing is ever exceptional. An fd in a set
 * that isn't open is EBADF. */
static long do_select(long n, fd_set *rd, fd_set *wr, fd_set *ex, const void *timeout, const long *data) {
        if (n < 0) return -EINVAL;
        if (n > FD_SETSIZE) n = FD_SETSIZE;
        bmds_pollfd_t k[BMDS_POLL_MAX];
        unsigned long count = 0;
        for (long fd = 0; fd < n; ++fd) {
                const uint32_t events = ((rd != NULL && FD_ISSET(fd, rd)) ? BMDS_POLL_IN : 0) |
                                        ((wr != NULL && FD_ISSET(fd, wr)) ? BMDS_POLL_OUT : 0) |
                                        ((ex != NULL && FD_ISSET(fd, ex)) ? BMDS_POLL_PRI : 0);
                if (events == 0) continue;
                if (count == BMDS_POLL_MAX) return -EBADF; /* more fds than a process can have open */
                k[count].size = sizeof(k[count]);
                k[count].fd = (int32_t)fd;
                k[count].events = events;
                ++count;
        }
        const uint64_t *mask = data != NULL && data[0] != 0 ? (const uint64_t *)data[0] : NULL;
        const long      r = poll_entries(k, count, timeout, mask);
        if (r < 0) return r;
        for (unsigned long i = 0; i < count; ++i) {
                if (k[i].revents & BMDS_POLL_NVAL) return -EBADF;
        }
        long ready = 0;
        for (unsigned long i = 0; i < count; ++i) {
                const int      fd = k[i].fd;
                const uint32_t got = k[i].revents;
                if (rd != NULL && FD_ISSET(fd, rd)) {
                        if (got & (BMDS_POLL_IN | BMDS_POLL_HUP | BMDS_POLL_ERR)) {
                                ++ready;
                        } else {
                                FD_CLR(fd, rd);
                        }
                }
                if (wr != NULL && FD_ISSET(fd, wr)) {
                        if (got & (BMDS_POLL_OUT | BMDS_POLL_ERR)) {
                                ++ready;
                        } else {
                                FD_CLR(fd, wr);
                        }
                }
                if (ex != NULL) FD_CLR(fd, ex);
        }
        return ready;
}

/* --- Memory ------------------------------------------------------------------- */

/*
 * Mappings, in whole pages as Linux gives them (musl's malloc keeps
 * metadata at the end of the last page). A file mapping is a copy (no
 * paging). A shared writable one keeps a descriptor of its own and is
 * written back (the parts that changed, up to the file's size then) at
 * msync, munmap, exec and exit: other programs see the changes from then
 * on, and a program killed by SIGKILL loses them. munmap frees a whole
 * mapping; a part of one stays until the program ends.
 */
typedef struct {
                void   *addr;
                size_t  len;
                int64_t offset; /* in the file */
                int32_t fd;     /* shared and writable: the mapping's own descriptor; else -1 */
} mapping_t;

static mapping_t   *maps;
static size_t       map_count, map_room;
static volatile int maps_lock[1];

static void map_write_back(const mapping_t *m) {
        if (m->fd < 0) return;
        bmds_stat_t st = {.size = sizeof(st)};
        if (bmds_fstat(m->fd, &st) < 0 || (int64_t)st.file_size <= m->offset) return;
        const uint64_t left = st.file_size - (uint64_t)m->offset;
        const size_t   n = left < m->len ? (size_t)left : m->len;
        const uint8_t *data = m->addr;
        uint8_t        chunk[512];
        int64_t        ignored;
        for (size_t at = 0; at < n; at += sizeof(chunk)) {
                const size_t  size = n - at < sizeof(chunk) ? n - at : sizeof(chunk);
                const int64_t pos = m->offset + (int64_t)at;
                if (bmds_seek(m->fd, pos, BMDS_SEEK_SET, &ignored) < 0) return;
                if (bmds_read(m->fd, chunk, (uint32_t)size) == (int32_t)size && memcmp(chunk, data + at, size) == 0)
                        continue;
                if (bmds_seek(m->fd, pos, BMDS_SEEK_SET, &ignored) < 0) return;
                if (bmds_write(m->fd, data + at, (uint32_t)size) != (int32_t)size) return;
        }
        return;
}

/* Before the program's memory goes (exit, exec): every shared mapping out.
 * Not from a vfork child: the mappings are its parent's. */
static void maps_write_back(void) {
        if (__bmds_vfork_depth > 0) return;
        __lock(maps_lock);
        for (size_t i = 0; i < map_count; ++i) map_write_back(&maps[i]);
        __unlock(maps_lock);
        return;
}

/* Room for one more mapping (under maps_lock): 0, or -ENOMEM. */
static long map_room_for_one(void) {
        if (map_count < map_room) return 0;
        const size_t room = map_room ? map_room * 2 : 32;
        mapping_t   *more = bmds_mem_realloc(maps, (uint32_t)(room * sizeof(*maps)), 0);
        if (more == NULL) return -ENOMEM;
        maps = more;
        map_room = room;
        return 0;
}

static long do_mmap(size_t len, long prot, long flags, long fd, long pgoff) {
        if (len == 0 || (flags & MAP_FIXED)) return -EINVAL;
        len = (len + 4095) & ~(size_t)4095;
        void *p = bmds_mem_alloc((uint32_t)len, 4096, 0);
        if (p == NULL) return -ENOMEM;
        memset(p, 0, len);
        int32_t own = -1;
        if (!(flags & MAP_ANONYMOUS)) {
                const long n = io_at(fd, p, len, (int64_t)pgoff * 4096, 0);
                /* Kept out of the low numbers the program expects to get. */
                if (n >= 0 && (flags & MAP_SHARED) && (prot & PROT_WRITE)) own = bmds_dup((int32_t)fd, 10);
                if (n < 0 || own < -1) {
                        bmds_mem_free(p);
                        return n < 0 ? n : (long)(own);
                }
                if (own >= 0) (void)bmds_fd_flags(own, BMDS_FD_CLOEXEC);
        }
        __lock(maps_lock);
        const long room = map_room_for_one();
        if (room == 0) {
                maps[map_count].addr = p;
                maps[map_count].len = len;
                maps[map_count].offset = (int64_t)pgoff * 4096;
                maps[map_count].fd = own;
                ++map_count;
        }
        __unlock(maps_lock);
        if (room < 0) {
                if (own >= 0) (void)bmds_close(own);
                bmds_mem_free(p);
                return room;
        }
        return (long)p;
}

/* Takes the mapping at @p addr (at least @p len long) off the list: 1 and its entry in @p out, or 0. */
static int take_mapping(void *addr, size_t len, mapping_t *out) {
        int found = 0;
        __lock(maps_lock);
        for (size_t i = 0; i < map_count; ++i) {
                if (maps[i].addr != addr || len < maps[i].len) continue; /* a part: kept until exit */
                *out = maps[i];
                maps[i] = maps[--map_count];
                found = 1;
                break;
        }
        __unlock(maps_lock);
        return found;
}

static long do_munmap(void *addr, size_t len) {
        mapping_t m;
        if (!take_mapping(addr, len, &m)) return 0;
        map_write_back(&m);
        if (m.fd >= 0) (void)bmds_close(m.fd);
        bmds_mem_free(addr);
        return 0;
}

static long do_msync(void *addr, size_t len) {
        const uint8_t *a = addr;
        __lock(maps_lock);
        for (size_t i = 0; i < map_count; ++i) {
                const uint8_t *m = maps[i].addr;
                if (a < m + maps[i].len && a + len > m) map_write_back(&maps[i]);
        }
        __unlock(maps_lock);
        return 0;
}

/* --- Signals --------------------------------------------------------------- */

/*
 * The kernel keeps each process's actions, mask and pending signals
 * (programs.md §15); this layer keeps the handlers and runs them. The
 * kernel sets a thread's sig_hint while a signal is due for it: after each call,
 * __bmds_syscall takes the due signals (bmds_sig_take) and runs their
 * handlers, then makes an interrupted call again if they all asked for
 * SA_RESTART. Realtime signals (32-64) are only recorded here: the kernel
 * never delivers them.
 */
/* Each thread's: the kernel sets it while a signal is due for the thread.
 * Thread-local, so only once musl has set tp up (tp_ready): before that, tp
 * is the kernel's and TLS isn't there. */
static _Thread_local volatile uint32_t sig_hint;

/* The kernel's part of a sigset: signals 1-31. */
static uint32_t kernel_set(uint64_t set) {
        return (uint32_t)set & 0x7fffffffu;
}

static long do_sigaction(long sig, const struct k_sigaction *act, struct k_sigaction *old) {
        if (sig < 1 || sig >= _NSIG) return -EINVAL;
        if (act != NULL && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
        struct k_sigaction next;
        if (act != NULL) next = *act;
        long r = 0;
        __lock(tables_lock);
        struct k_sigaction *a = &tables()->actions[sig];
        if (old != NULL) *old = *a;
        if (act != NULL) {
                if (sig < BMDS_SIG_COUNT) {
                        const uint32_t action = next.handler == SIG_DFL   ? BMDS_SIG_DEFAULT
                                                : next.handler == SIG_IGN ? BMDS_SIG_IGNORE
                                                                          : BMDS_SIG_CATCH;
                        r = bmds_sig_action((int32_t)sig, action);
                }
                if (r >= 0) *a = next;
        }
        __unlock(tables_lock);
        return r < 0 ? r : 0;
}

static long do_sigprocmask(long how, const uint64_t *set, uint64_t *old) {
        int32_t r;
        if (set == NULL) {
                r = bmds_sig_mask(BMDS_SIG_BLOCK, 0);
        } else {
                if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return -EINVAL;
                r = bmds_sig_mask((uint32_t)how, kernel_set(*set));
        }
        if (r < 0) return (long)(r);
        if (old != NULL) *old = (uint32_t)r;
        return 0;
}

static _Noreturn void signal_exit(int32_t sig);

/* Runs the handler of the taken signal @p info; returns whether it asked
 * for SA_RESTART. */
static int run_handler(const bmds_siginfo_t *info) {
        const int sig = info->signo;
        __lock(tables_lock);
        struct k_sigaction *a = &tables()->actions[sig];
        void (*handler)(int) = a->handler;
        const unsigned long flags = a->flags;
        const unsigned      mask[2] = {a->mask[0], a->mask[1]};
        if ((flags & SA_RESETHAND) && handler != SIG_DFL && handler != SIG_IGN) a->handler = SIG_DFL;
        __unlock(tables_lock);
        /* Changed here since the kernel took it: nothing to run. */
        if (handler == SIG_DFL || handler == SIG_IGN) return 1;
        uint32_t block = kernel_set((uint64_t)mask[1] << 32 | mask[0]);
        if (!(flags & SA_NODEFER)) block |= 1u << (sig - 1);
        const int32_t saved = bmds_sig_mask(BMDS_SIG_BLOCK, block);
        if (flags & SA_RESETHAND) (void)bmds_sig_action(sig, BMDS_SIG_DEFAULT);
        if (flags & SA_SIGINFO) {
                siginfo_t si;
                memset(&si, 0, sizeof(si));
                si.si_signo = sig;
                si.si_code = info->code;
                ((void (*)(int, siginfo_t *, void *))(void *)handler)(sig, &si, NULL);
        } else {
                handler(sig);
        }
        (void)bmds_sig_mask(BMDS_SIG_SETMASK, (uint32_t)saved);
        return (flags & SA_RESTART) != 0;
}

/* Takes and handles the due signals; returns whether every handler asked
 * for SA_RESTART (1 if none ran). */
static int deliver_due(void) {
        int restart = 1;
        while (sig_hint) {
                bmds_siginfo_t info = {.size = sizeof(info)};
                const int32_t  sig = bmds_sig_take(&info);
                if (sig <= 0) break;
                if (info.flags & BMDS_SIG_TERMINATE) signal_exit(sig);
                if (!run_handler(&info)) restart = 0;
        }
        return restart;
}

/* sigsuspend(): the mask becomes @p mask until a signal is due and handled. */
static long do_sigsuspend(uint32_t mask) {
        const int32_t old = bmds_sig_mask(BMDS_SIG_BLOCK, 0);
        (void)bmds_sig_suspend(mask);
        (void)deliver_due();
        (void)bmds_sig_mask(BMDS_SIG_SETMASK, (uint32_t)old);
        return -EINTR;
}

static long self(void) {
        return (long)(bmds_proc_id(BMDS_PROC_SELF));
}

static long do_kill(long pid, long sig) {
        if (sig < 0 || sig >= BMDS_SIG_COUNT) return -EINVAL; /* realtime signals aren't sent */
        return (long)(bmds_proc_kill((int32_t)pid, (int32_t)sig));
}

/* --- The rest ---------------------------------------------------------------- */

static long do_uname(struct utsname *u) {
        memset(u, 0, sizeof(*u));
        strcpy(u->sysname, "BMDS");
        strcpy(u->nodename, "bmds");
        u->release[0] = (char)('0' + BMDS_ABI_VERSION);
        strcpy(u->version, "ABI");
        strcpy(u->machine, "riscv32");
        return 0;
}

static long do_sysinfo(struct sysinfo *s) {
        memset(s, 0, sizeof(*s));
        bmds_mem_info_t m = {.size = sizeof(m)};
        (void)bmds_mem_info(0, &m);
        s->uptime = (long)(now(BMDS_CLOCK_MONOTONIC) / 1000000000);
        s->totalram = m.total_bytes;
        s->freeram = m.free_bytes;
        s->procs = 1;
        s->mem_unit = 1;
        return 0;
}

static long do_prlimit(long resource, struct rlimit *old) {
        if (old == NULL) return 0;
        old->rlim_cur = old->rlim_max = RLIM_INFINITY;
        if (resource == RLIMIT_NOFILE) old->rlim_cur = old->rlim_max = 32;
        return 0;
}

static long do_times(struct tms *t) {
        const int64_t cpu = now(BMDS_CLOCK_MONOTONIC) - started_ns;
        if (t != NULL) {
                memset(t, 0, sizeof(*t));
                t->tms_utime = (clock_t)(cpu / (1000000000 / 100));
        }
        return (long)(now(BMDS_CLOCK_MONOTONIC) / (1000000000 / 100));
}

/* --- Processes ----------------------------------------------------------------- */

/*
 * vfork (vfork.S saves the registers; programs.md §13). A vfork child runs
 * on its parent's thread, in its parent's memory, so what Linux keeps per
 * process but this layer keeps in that memory (tables_t: the signal
 * handlers, which musl's posix_spawn child resets, the umask, the
 * directory-fd table, O_NONBLOCK and VMIN/VTIME) is copied for the child,
 * which works on the copy while the parent's other threads go on with
 * theirs; the kernel keeps the child's actions and mask apart. The copy
 * lives on the heap while the child runs: programs that never vfork pay
 * nothing.
 */
_Noreturn void __bmds_vfork_resume(long pid, const uint32_t *regs);

/* From vfork.S: the room for the registers, with the child's copy of the
 * tables; NULL if vforks are nested too deep or there is no memory. A
 * room a failed vfork left is used again. */
uint32_t *__bmds_vfork_prepare(void) {
        if (__bmds_vfork_depth >= VFORK_DEPTH) return NULL;
        vfork_saved_t *saved = vfork_saved[__bmds_vfork_depth];
        if (saved == NULL) saved = bmds_mem_alloc(sizeof(*saved), 0, 0);
        if (saved == NULL) return NULL;
        vfork_saved[__bmds_vfork_depth] = saved;
        __lock(tables_lock);
        saved->tables = *tables();
        __unlock(tables_lock);
        return saved->regs;
}

/* From vfork.S, registers saved: 0 (now the child), or -errno. */
long __bmds_vfork_enter(void) {
        const int32_t r = bmds_proc_vfork();
        if (r < 0) return (long)(r);
        ++__bmds_vfork_depth;
        return 0;
}

/* The child has exec'd or exited: the parent goes on, vfork returning @p pid. */
static _Noreturn void vfork_return(long pid) {
        vfork_saved_t *saved = vfork_saved[--__bmds_vfork_depth];
        uint32_t       regs[26]; /* above sp until __bmds_vfork_resume has read them */
        memcpy(regs, saved->regs, sizeof(regs));
        vfork_saved[__bmds_vfork_depth] = NULL;
        bmds_mem_free(saved);
        __bmds_vfork_resume(pid, regs);
}

static long do_exit(long status) {
        if (__bmds_vfork_depth > 0) vfork_return((long)(bmds_proc_vfork_exit((int32_t)status)));
        maps_write_back();
        bmds_exit((int32_t)status);
        return 0;
}

/* --- Threads (devplan/processes.md §3.10) ------------------------------------------ */

/* SYS_exit: the calling thread ends (a kernel without threads: the program). */
static long do_exit_thread(long status) {
        if (__bmds_vfork_depth > 0 || !bmds_sys_has(BMDS_SYS_THREAD_EXIT)) return do_exit(status);
        bmds_thread_exit((int32_t)status, NULL);
        return 0;
}

/* musl's pthread_exit for a detached thread: its mapping (TLS, thread data)
 * goes as it ends. The kernel frees it once no program code runs on it: the
 * thread's stack is the kernel's. */
_Noreturn void __unmapself(void *base, size_t size) {
        mapping_t m;
        if (!take_mapping(base, size, &m)) base = NULL; /* not one of ours: left alone */
        bmds_thread_exit(0, base);
        for (;;) {}
}

/* __clone (vfork.S) for a thread: CLONE_VM|CLONE_THREAD|CLONE_SETTLS, as
 * musl's pthread_create asks, plus the stack size it passes (the kernel
 * allocates the stack; musl's @p stack is unused). The child runs fn(arg). */
long __bmds_thread_clone(int (*fn)(void *), void *stack, long flags, void *arg, pid_t *ptid, void *tls, pid_t *ctid,
                         size_t stack_size) {
        (void)stack;
        const long needed = CLONE_VM | CLONE_THREAD | CLONE_SIGHAND | CLONE_SETTLS;
        if ((flags & needed) != needed || !bmds_sys_has(BMDS_SYS_THREAD_START)) return -ENOSYS;
        /* The new thread's hint word: where its TLS puts this thread's. */
        const uintptr_t     hint_offset = (uintptr_t)&sig_hint - (uintptr_t)__builtin_thread_pointer();
        const bmds_thread_t t = {
                .size = sizeof(t),
                .entry = (int32_t(*)(void *))fn,
                .arg = arg,
                .tls = tls,
                .stack_size = (uint32_t)stack_size,
                .hint = (volatile uint32_t *)((uintptr_t)tls + hint_offset),
                .clear = (flags & CLONE_CHILD_CLEARTID) ? (volatile int32_t *)ctid : NULL,
                .tid = (flags & CLONE_PARENT_SETTID) ? (int32_t *)ptid : NULL,
        };
        return (long)(bmds_thread_start(&t));
}

/* Linux's futex on the kernel's: WAIT (a relative time64 timeout), WAKE,
 * REQUEUE and CMP_REQUEUE (the comparison made here, not atomically with the
 * move); the priority-inheritance and bitset operations aren't there. */
static long do_futex(volatile int *addr, long op, long val, const void *timeout, volatile int *addr2, long val3) {
        if (!bmds_sys_has(BMDS_SYS_FUTEX)) return (op & 0x7f) == FUTEX_WAIT ? -EAGAIN : 0; /* one thread */
        volatile int32_t *word = (volatile int32_t *)addr;
        switch (op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) {
                case FUTEX_WAIT: {
                        int64_t ns = -1; /* none: no timeout */
                        if (timeout != NULL) ns = ts_read(timeout) < 0 ? 0 : ts_read(timeout);
                        return (long)(bmds_futex(word, BMDS_FUTEX_WAIT, (int32_t)val, ns, NULL, 0));
                }
                case FUTEX_WAKE: return (long)(bmds_futex(word, BMDS_FUTEX_WAKE, (int32_t)val, 0, NULL, 0));
                case FUTEX_CMP_REQUEUE:
                        if (*addr != (int)val3) return -EAGAIN;
                        /* fall through */
                case FUTEX_REQUEUE:
                        /* The timeout argument is the count to move. */
                        return (long)(bmds_futex(word, BMDS_FUTEX_REQUEUE, (int32_t)val, 0, (volatile int32_t *)addr2,
                                                 (int32_t)(uintptr_t)timeout));
                default: return -ENOSYS;
        }
}

/* tkill/tgkill: a signal to one thread. musl's SIGCANCEL (33) only wakes
 * the thread from the call it is blocked in, to see its cancel flag; the
 * other real-time signals aren't sent. */
static long do_tkill(long tid, long sig) {
        if (!bmds_sys_has(BMDS_SYS_THREAD_KILL)) return do_kill(tid, sig);
        if (sig == 33) return (long)(bmds_thread_kill((int32_t)tid, 0, BMDS_THREAD_WAKE));
        if (sig < 0 || sig >= BMDS_SIG_COUNT) return -EINVAL;
        return (long)(bmds_thread_kill((int32_t)tid, (int32_t)sig, 0));
}

static long do_gettid(void) {
        return bmds_sys_has(BMDS_SYS_THREAD_START) ? (long)(bmds_proc_id(BMDS_PROC_THREAD)) : self();
}

/* musl calls it once tp is set up (__init_tp): the first thread's hint word is there now. */
static long do_set_tid_address(volatile int *word) {
        if (!tp_ready) {
                tp_ready = 1;
                if (bmds_sys_has(BMDS_SYS_SIG_SETUP)) (void)bmds_sig_setup(&sig_hint);
        }
        return bmds_sys_has(BMDS_SYS_THREAD_CLEAR) ? (long)(bmds_thread_clear((volatile int32_t *)word)) : self();
}

/* Ends the program (or the vfork child) as killed by @p sig. */
static _Noreturn void signal_exit(int32_t sig) {
        if (__bmds_vfork_depth > 0) vfork_return((long)(bmds_proc_exit_signal(sig)));
        maps_write_back();
        (void)bmds_proc_exit_signal(sig);
        for (;;) {}
}

/* A __clone child's function returned @p status. */
_Noreturn void __bmds_vfork_child_exit(long status) {
        (void)do_exit(status);
        for (;;) {}
}

static long do_execve(const char *path, char *const *argv, char *const *envp) {
        maps_write_back();
        const int32_t r = bmds_proc_exec(path, argv, envp);
        /* Only a vfork child gets here with an id: its parent goes on. */
        if (r >= 0) vfork_return((long)r);
        return (long)(r);
}

/* waitid(2); musl builds wait4 and waitpid on it. Process groups are the
 * whole family: P_PGID waits for any child. The resource usage (the kernel's
 * struct rusage: 18 longs) is zero: BMDS does not count CPU time. */
static long do_waitid(long type, long id, siginfo_t *info, long options, long *usage) {
        if (type != P_ALL && type != P_PID && type != P_PGID) return -EINVAL;
        int32_t       status = 0;
        const uint32_t flags = ((options & WNOHANG) ? BMDS_WAIT_NOHANG : 0u) |
                               ((options & WSTOPPED) ? BMDS_WAIT_UNTRACED : 0u) |
                               ((options & WCONTINUED) ? BMDS_WAIT_CONTINUED : 0u);
        const int32_t  r = bmds_proc_wait(type == P_PID ? (int32_t)id : -1, &status, flags);
        if (r < 0) return (long)(r);
        if (usage != NULL) memset(usage, 0, 18 * sizeof(long));
        if (info != NULL) {
                memset(info, 0, sizeof(*info));
                if (r > 0) {
                        info->si_signo = SIGCHLD;
                        info->si_pid = r;
                        if (status == 0xffff) {
                                info->si_code = CLD_CONTINUED;
                                info->si_status = SIGCONT;
                        } else if ((status & 0xff) == 0x7f) {
                                info->si_code = CLD_STOPPED;
                                info->si_status = (status >> 8) & 0xff;
                        } else if (status & 0x7f) {
                                info->si_code = CLD_KILLED;
                                info->si_status = status & 0x7f;
                        } else {
                                info->si_code = CLD_EXITED;
                                info->si_status = (status >> 8) & 0xff;
                        }
                }
        }
        return 0;
}

static long do_pipe2(int *fds, long flags) {
        if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
        int32_t       ends[2];
        const int32_t r = bmds_pipe(ends, (flags & O_CLOEXEC) ? BMDS_O_CLOEXEC : 0);
        if (r < 0) return (long)(r);
        fds[0] = ends[0];
        fds[1] = ends[1];
        set_nonblock(ends[0], (flags & O_NONBLOCK) != 0);
        set_nonblock(ends[1], (flags & O_NONBLOCK) != 0);
        return 0;
}

/* Called by the startup code before musl starts. */
void __bmds_linux_start(void) {
        started_ns = now(BMDS_CLOCK_MONOTONIC);
        if (bmds_sys_has(BMDS_SYS_SIG_SETUP)) {
                /* Ignored signals survive exec (nohup, a shell's trap ''). */
                for (int sig = 1; sig < BMDS_SIG_COUNT; ++sig) {
                        if (bmds_sig_action(sig, BMDS_SIG_QUERY) == (int32_t)BMDS_SIG_IGNORE)
                                process_tables.actions[sig].handler = SIG_IGN;
                }
        }
        return;
}

static long do_syscall(long n, long a, long b, long c, long d, long e, long f) {
        switch (n) {
                /* Files */
                case SYS_openat: return do_open(a, (const char *)b, c);
                case SYS_close: return do_close(a);
                case SYS_read: return gated_read(a, (void *)b, (size_t)c);
                case SYS_write: return gated_write(a, (const void *)b, (size_t)c);
                case SYS_readv: return io_vector(a, (const struct iovec *)b, c, 0);
                case SYS_writev: return io_vector(a, (const struct iovec *)b, c, 1);
                case SYS_pread64: return io_at(a, (void *)b, (size_t)c, (int64_t)((uint64_t)(uint32_t)e << 32 | (uint32_t)d), 0);
                case SYS_pwrite64: return io_at(a, (void *)b, (size_t)c, (int64_t)((uint64_t)(uint32_t)e << 32 | (uint32_t)d), 1);
                case SYS__llseek: return do_llseek(a, (int64_t)((uint64_t)(uint32_t)b << 32 | (uint32_t)c), (int64_t *)d, e);
                case SYS_statx: return do_statx(a, (const char *)b, c, (struct statx *)e);
                case SYS_getdents64: return do_getdents(a, (char *)b, (size_t)c);
                case SYS_fcntl64: return do_fcntl(a, b, c);
                case SYS_dup: {
                        if (dir_index(a) >= 0) return -EBADF;
                        const int32_t r = bmds_dup((int32_t)a, 0);
                        set_nonblock(r, 0);
                        return (long)(r);
                }
                case SYS_dup3: {
                        if (a == b || (c & ~O_CLOEXEC)) return -EINVAL;
                        const int32_t r = bmds_dup2((int32_t)a, (int32_t)b);
                        set_nonblock(r, 0);
                        if (r >= 0 && (c & O_CLOEXEC)) (void)bmds_fd_flags(r, BMDS_FD_CLOEXEC);
                        return (long)(r);
                }
                case SYS_pipe2: return do_pipe2((int *)a, b);
                case SYS_ioctl: return do_ioctl(a, (unsigned long)b, (void *)c);
                case SYS_ftruncate64: return do_ftruncate(a, (int64_t)((uint64_t)(uint32_t)c << 32 | (uint32_t)b));
                case SYS_truncate64: return do_truncate((const char *)a, (int64_t)((uint64_t)(uint32_t)c << 32 | (uint32_t)b));
                case SYS_fsync:
                case SYS_fdatasync:
                case SYS_sync: return 0; /* writes reach the card when the file closes */
                case SYS_faccessat:
                case SYS_faccessat2: return do_access(a, (const char *)b, c);
                case SYS_utimensat_time64: return do_utimens(a, (const char *)b, (const void *)c);
                case SYS_fchmod:
                case SYS_fchmodat:
                case SYS_fchown:
                case SYS_fchownat: return 0; /* no permissions on FAT: every user is root */
                case SYS_readlinkat: return do_readlink(a, (const char *)b, (char *)c, (size_t)d);
                case SYS_symlinkat: return do_symlink((const char *)a, b, (const char *)c);
                case SYS_linkat: /* no hard links on FAT */
                case SYS_mknodat: return -EPERM;
                case SYS_umask: return (long)__atomic_exchange_n(&tables()->umask, (mode_t)a & 0777, __ATOMIC_RELAXED);

                /* Directories */
                case SYS_getcwd: {
                        const int32_t r = bmds_getcwd((char *)a, (uint32_t)b);
                        return r < 0 ? (long)(r) : r + 1;
                }
                case SYS_chdir: return (long)(bmds_chdir((const char *)a));
                case SYS_fchdir: {
                        char path_buf[BMDS_PATH_MAX + 1];
                        if (dir_index(a) < 0 || at_path(a, ".", path_buf, sizeof(path_buf)) == NULL) return -ENOTDIR;
                        path_buf[strlen(path_buf) - 2] = '\0'; /* without its "/." */
                        return (long)(bmds_chdir(path_buf));
                }
                case SYS_mkdirat: {
                        AT_PATH(a, b);
                        return (long)(bmds_mkdir(p));
                }
                case SYS_unlinkat: {
                        AT_PATH(a, b);
                        return (long)((c & AT_REMOVEDIR) ? bmds_rmdir(p) : bmds_unlink(p));
                }
                case SYS_renameat2: {
                        char        from_buf[BMDS_PATH_MAX + 1];
                        const char *from = at_path(a, (const char *)b, from_buf, sizeof(from_buf));
                        AT_PATH(c, d);
                        if (from == NULL) return -EBADF;
                        if (e & RENAME_EXCHANGE) return -EINVAL;
                        if (e & RENAME_NOREPLACE) {
                                bmds_stat_t st = {.size = sizeof(st)};
                                if (bmds_stat(p, &st) == 0) return -EEXIST;
                        }
                        return (long)(bmds_rename(from, p));
                }

                /* Time */
                case SYS_clock_gettime64: {
                        int64_t    ns;
                        const long r = clock_ns(a, &ns);
                        if (r == 0) ts_write((void *)b, ns);
                        return r;
                }
                case SYS_clock_getres_time64:
                        if (b != 0) ts_write((void *)b, 1000);
                        return 0;
                case SYS_clock_settime64: return -EPERM;
                case SYS_clock_nanosleep_time64: {
                        int64_t base;
                        if (clock_ns(a, &base) != 0) return -EINVAL;
                        const int64_t req = ts_read((const void *)c);
                        const int64_t wait = (b & TIMER_ABSTIME) ? req - base : req;
                        const int64_t start = now(BMDS_CLOCK_MONOTONIC);
                        const long    r = sleep_ns(wait);
                        if (r == -EINTR && d != 0 && !(b & TIMER_ABSTIME)) {
                                const int64_t left = wait - (now(BMDS_CLOCK_MONOTONIC) - start);
                                ts_write((void *)d, left > 0 ? left : 0);
                        }
                        return r;
                }
                case SYS_times: return do_times((struct tms *)a);
                case SYS_setitimer: return -ENOSYS; /* alarm() needs signals from outside */

                /* Memory */
                case SYS_mmap2: return do_mmap((size_t)b, c, d, e, f);
                case SYS_munmap: return do_munmap((void *)a, (size_t)b);
                case SYS_msync: return do_msync((void *)a, (size_t)b);
                case SYS_brk: return 0; /* no brk: musl's malloc then maps */
                case SYS_mremap: return -ENOMEM; /* musl falls back to copying */
                case SYS_mprotect:
                case SYS_madvise:
                case SYS_membarrier: return 0;

                /* The program */
                case SYS_exit: return do_exit_thread(a);
                case SYS_exit_group: return do_exit(a);
                case SYS_execve: return do_execve((const char *)a, (char *const *)b, (char *const *)c);
                case SYS_waitid: return do_waitid(a, b, (siginfo_t *)c, d, (long *)e);
                case SYS_set_tid_address: return do_set_tid_address((volatile int *)a);
                case SYS_gettid: return do_gettid();
                case SYS_getpid: return self();
                case SYS_getppid: return (long)(bmds_proc_id(BMDS_PROC_PARENT));
                case SYS_getpgid: return (long)(bmds_proc_group(BMDS_GROUP_GET, (int32_t)a, 0));
                case SYS_getsid: return (long)(bmds_proc_group(BMDS_GROUP_SESSION, (int32_t)a, 0));
                case SYS_setpgid: return (long)(bmds_proc_group(BMDS_GROUP_SET, (int32_t)a, (int32_t)b));
                case SYS_setsid: return (long)(bmds_proc_group(BMDS_GROUP_NEW_SESSION, 0, 0));
                case SYS_getuid:
                case SYS_geteuid:
                case SYS_getgid:
                case SYS_getegid:
                case SYS_getgroups: return 0; /* root, in no groups */
                case SYS_setuid:
                case SYS_setgid:
                case SYS_setreuid:
                case SYS_setregid:
                case SYS_setresuid:
                case SYS_setresgid: return 0;
                case SYS_uname: return do_uname((struct utsname *)a);
                case SYS_sysinfo: return do_sysinfo((struct sysinfo *)a);
                case SYS_prlimit64: return do_prlimit(b, (struct rlimit *)d);
                case SYS_getrandom: {
                        const int32_t r = bmds_random((void *)a, (uint32_t)b);
                        return r < 0 ? (long)(r) : b;
                }
                case SYS_sched_yield: return 0;
                case SYS_futex: return do_futex((volatile int *)a, b, c, (const void *)d, (volatile int *)e, f);

                /* Signals */
                case SYS_rt_sigaction: return do_sigaction(a, (const struct k_sigaction *)b, (struct k_sigaction *)c);
                case SYS_rt_sigprocmask: return do_sigprocmask(a, (const uint64_t *)b, (uint64_t *)c);
                case SYS_rt_sigpending: *(uint64_t *)a = (uint32_t)bmds_sig_pending(); return 0;
                case SYS_rt_sigsuspend: return do_sigsuspend(kernel_set(*(const uint64_t *)a));
                case SYS_sigaltstack: return 0;
                case SYS_kill: return do_kill(a, b);
                case SYS_tkill: return do_tkill(a, b);
                case SYS_tgkill: return do_tkill(b, c);

                /* Waiting */
                case SYS_ppoll_time64:
                        return do_poll((struct pollfd *)a, (unsigned long)b, (const void *)c, (const uint64_t *)d);
                case SYS_pselect6_time64:
                        return do_select(a, (fd_set *)b, (fd_set *)c, (fd_set *)d, (const void *)e, (const long *)f);

                /* No sockets; no fork (SYS_clone: vfork.S handles CLONE_VFORK). */
                case SYS_socket: return -EAFNOSUPPORT;
                default: return -ENOSYS;
        }
}

/* The calls Linux makes again after a handler with SA_RESTART. */
static int restartable(long n) {
        switch (n) {
                case SYS_read:
                case SYS_readv:
                case SYS_write:
                case SYS_writev:
                case SYS_pread64:
                case SYS_pwrite64:
                case SYS_ioctl:
                case SYS_waitid:
                case SYS_openat: return 1;
                default: return 0;
        }
}

long __bmds_syscall(long n, long a, long b, long c, long d, long e, long f) {
        for (;;) {
                const long r = do_syscall(n, a, b, c, d, e, f);
                if (!tp_ready || !sig_hint) return r;
                /* A signal is due: its handler runs as the call returns. */
                const int restart = deliver_due();
                if (r != -EINTR || !restart || !restartable(n)) return r;
        }
}
