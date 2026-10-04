/**
 * @file      posix.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * picolibc's POSIX glue beyond stdio: files, directories, the current
 * directory, descriptors, time, sleeping, randomness. Each function checks
 * that the kernel has its slot (ENOSYS if not), so a program built with
 * this SDK still starts on an older kernel and can fall back.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

#define NS_PER_SEC 1000000000LL

/* A result from the table: >= 0, or -1 with errno set. */
static int result(int32_t r) {
        if (r < 0) {
                errno = -r;
                return -1;
        }
        return (int)r;
}

static int nosys(void) {
        errno = ENOSYS;
        return -1;
}

#define NEED(slot)                                                                                                     \
        do {                                                                                                           \
                if (!bmds_sys_has(slot)) return nosys();                                                               \
        } while (0)

/* --- Files ----------------------------------------------------------- */

int open(const char *path, int flags, ...) {
        NEED(BMDS_SYS_OPEN);
        /* Accepted and meaningless here: no exec, no controlling tty, every
         * write reaches the card when the file is closed or synced. */
        const int ignored = O_CLOEXEC | O_NOCTTY | O_SYNC;
        const int known = O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND | O_EXCL | O_DIRECTORY | ignored;
        if ((flags & ~known) != 0) {
                errno = EINVAL;
                return -1;
        }
        if (flags & O_DIRECTORY) {
                /* Directories aren't opened as fds: opendir() reads them. */
                struct stat st;
                if (stat(path, &st) < 0) return -1;
                errno = S_ISDIR(st.st_mode) ? EISDIR : ENOTDIR;
                return -1;
        }
        uint32_t f = 0;
        switch (flags & O_ACCMODE) {
                case O_RDONLY: f = BMDS_O_READ; break;
                case O_WRONLY: f = BMDS_O_WRITE; break;
                case O_RDWR: f = BMDS_O_READ | BMDS_O_WRITE; break;
                default: errno = EINVAL; return -1;
        }
        if (flags & O_CREAT) f |= BMDS_O_CREATE;
        if (flags & O_TRUNC) f |= BMDS_O_TRUNC;
        if (flags & O_APPEND) f |= BMDS_O_APPEND;
        if (flags & O_EXCL) f |= BMDS_O_EXCL;
        return result(bmds_open(path, f));
}

int creat(const char *path, mode_t mode) {
        return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

static void fill_stat(const bmds_stat_t *in, struct stat *st) {
        memset(st, 0, sizeof(*st));
        switch (in->type) {
                case BMDS_TYPE_DIRECTORY: st->st_mode = S_IFDIR | 0755; break;
                case BMDS_TYPE_FILE:
                        st->st_mode = S_IFREG | ((in->attributes & BMDS_ATTR_READONLY) ? 0444 : 0644);
                        break;
                default: st->st_mode = S_IFCHR | 0666; break;
        }
        st->st_nlink = 1;
        st->st_size = (off_t)in->file_size;
        st->st_blksize = 512;
        st->st_blocks = (blkcnt_t)((in->file_size + 511) / 512);
        st->st_mtim.tv_sec = (time_t)(in->modified_ns / NS_PER_SEC);
        st->st_mtim.tv_nsec = (long)(in->modified_ns % NS_PER_SEC);
        st->st_atim = st->st_mtim;
        st->st_ctim = st->st_mtim;
        return;
}

int stat(const char *path, struct stat *st) {
        NEED(BMDS_SYS_STAT);
        bmds_stat_t info = {.size = sizeof(info)};
        if (result(bmds_stat(path, &info)) < 0) return -1;
        fill_stat(&info, st);
        return 0;
}

int lstat(const char *path, struct stat *st) {
        return stat(path, st);
}

int fstat(int fd, struct stat *st) {
        NEED(BMDS_SYS_FSTAT);
        bmds_stat_t info = {.size = sizeof(info)};
        if (result(bmds_fstat(fd, &info)) < 0) return -1;
        fill_stat(&info, st);
        return 0;
}

int access(const char *path, int mode) {
        struct stat st;
        if (stat(path, &st) < 0) return -1;
        if ((mode & W_OK) && S_ISREG(st.st_mode) && (st.st_mode & 0200) == 0) {
                errno = EACCES;
                return -1;
        }
        return 0;
}

int mkdir(const char *path, mode_t mode) {
        (void)mode;
        NEED(BMDS_SYS_MKDIR);
        return result(bmds_mkdir(path));
}

int rmdir(const char *path) {
        NEED(BMDS_SYS_RMDIR);
        return result(bmds_rmdir(path));
}

int unlink(const char *path) {
        NEED(BMDS_SYS_UNLINK);
        return result(bmds_unlink(path));
}

int rename(const char *from, const char *to) {
        NEED(BMDS_SYS_RENAME);
        return result(bmds_rename(from, to));
}

static int set_mtime(const char *path, int64_t ns) {
        NEED(BMDS_SYS_SET_MTIME);
        return result(bmds_set_mtime(path, ns));
}

static int64_t now_ns(void) {
        int64_t ns = 0;
        if (bmds_sys_has(BMDS_SYS_CLOCK_GET)) (void)bmds_clock_get(BMDS_CLOCK_REALTIME, &ns);
        return ns;
}

int utime(const char *path, const struct utimbuf *times) {
        return set_mtime(path, times != NULL ? (int64_t)times->modtime * NS_PER_SEC : now_ns());
}

int utimes(const char *path, const struct timeval tv[2]) {
        return set_mtime(path,
                         tv != NULL ? (int64_t)tv[1].tv_sec * NS_PER_SEC + (int64_t)tv[1].tv_usec * 1000 : now_ns());
}

/* --- The current directory ---------------------------------------- */

char *getcwd(char *buf, size_t size) {
        if (!bmds_sys_has(BMDS_SYS_GETCWD)) {
                errno = ENOSYS;
                return NULL;
        }
        char *own = NULL;
        if (buf == NULL) { /* the glibc extension: allocate it */
                size = BMDS_PATH_MAX + 1;
                buf = own = malloc(size);
                if (buf == NULL) return NULL;
        }
        if (result(bmds_getcwd(buf, (uint32_t)size)) < 0) {
                free(own);
                return NULL;
        }
        return buf;
}

int chdir(const char *path) {
        NEED(BMDS_SYS_CHDIR);
        return result(bmds_chdir(path));
}

/* --- Descriptors ---------------------------------------------------- */

int dup(int fd) {
        NEED(BMDS_SYS_DUP);
        return result(bmds_dup(fd, 0));
}

int dup2(int fd, int to) {
        NEED(BMDS_SYS_DUP2);
        return result(bmds_dup2(fd, to));
}

int fcntl(int fd, int cmd, ...) {
        va_list args;
        va_start(args, cmd);
        const int arg = va_arg(args, int);
        va_end(args);
        bmds_stat_t info = {.size = sizeof(info)};
        switch (cmd) {
                case F_DUPFD:
                case F_DUPFD_CLOEXEC: NEED(BMDS_SYS_DUP); return result(bmds_dup(fd, arg));
                case F_GETFD:
                case F_SETFD:
                case F_GETFL:
                case F_SETFL:
                        NEED(BMDS_SYS_FSTAT);
                        if (result(bmds_fstat(fd, &info)) < 0) return -1;
                        if (cmd == F_GETFL) {
                                const uint32_t rw = info.open_flags & (BMDS_O_READ | BMDS_O_WRITE);
                                return rw == BMDS_O_WRITE ? O_WRONLY : rw == BMDS_O_READ ? O_RDONLY : O_RDWR;
                        }
                        if (cmd == F_SETFL && (arg & O_NONBLOCK)) {
                                errno = EINVAL;
                                return -1;
                        }
                        return 0;
                default: errno = EINVAL; return -1;
        }
}

/* --- Directories ---------------------------------------------------- */

/* picolibc's DIR is the head of what the implementation keeps. */
struct bmds_dir {
                DIR           dir;
                int32_t       handle;
                long          count;
                struct dirent entry;
};

DIR *opendir(const char *path) {
        if (!bmds_sys_has(BMDS_SYS_OPENDIR)) {
                errno = ENOSYS;
                return NULL;
        }
        struct bmds_dir *d = calloc(1, sizeof(*d));
        if (d == NULL) return NULL;
        d->handle = bmds_opendir(path);
        if (d->handle < 0) {
                errno = -d->handle;
                free(d);
                return NULL;
        }
        return &d->dir;
}

struct dirent *readdir(DIR *dir) {
        struct bmds_dir *d = (struct bmds_dir *)dir;
        bmds_stat_t      info = {.size = sizeof(info)};
        const int32_t    n = bmds_readdir(d->handle, &info, d->entry.d_name, sizeof(d->entry.d_name));
        if (n <= 0) {
                if (n < 0) errno = -n;
                return NULL;
        }
        d->entry.d_ino = (ino_t)++d->count;
        d->entry.d_type = info.type == BMDS_TYPE_DIRECTORY ? DT_DIR : info.type == BMDS_TYPE_FILE ? DT_REG : DT_CHR;
        return &d->entry;
}

int closedir(DIR *dir) {
        struct bmds_dir *d = (struct bmds_dir *)dir;
        const int        r = result(bmds_closedir(d->handle));
        free(d);
        return r;
}

/* --- Time --------------------------------------------------------- */

static int64_t start_ns; /* the monotonic clock when the program started */

void __bmds_posix_start(void) {
        if (bmds_sys_has(BMDS_SYS_CLOCK_GET)) (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &start_ns);
        return;
}

/* A program has the CPU to itself between system calls: its processor
 * time is the time since it started. */
#define BMDS_CLOCK_CPUTIME (-1)

static int32_t cpu_ns(int64_t *ns) {
        const int32_t r = bmds_clock_get(BMDS_CLOCK_MONOTONIC, ns);
        *ns -= start_ns;
        return r;
}

int clock_gettime(clockid_t clock, struct timespec *ts) {
        NEED(BMDS_SYS_CLOCK_GET);
        int64_t ns = 0;
        int32_t r;
        switch (clock) {
                case CLOCK_REALTIME: r = bmds_clock_get(BMDS_CLOCK_REALTIME, &ns); break;
                case CLOCK_MONOTONIC: r = bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns); break;
#ifdef CLOCK_PROCESS_CPUTIME_ID
                case CLOCK_PROCESS_CPUTIME_ID:
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
                case CLOCK_THREAD_CPUTIME_ID:
#endif
                case BMDS_CLOCK_CPUTIME: r = cpu_ns(&ns); break;
                default: errno = EINVAL; return -1;
        }
        if (result(r) < 0) return -1;
        ts->tv_sec = (time_t)(ns / NS_PER_SEC);
        ts->tv_nsec = (long)(ns % NS_PER_SEC);
        if (ts->tv_nsec < 0) { /* before 1970 */
                ts->tv_nsec += NS_PER_SEC;
                ts->tv_sec -= 1;
        }
        return 0;
}

int clock_getres(clockid_t clock, struct timespec *res) {
        (void)clock;
        if (res != NULL) {
                res->tv_sec = 0;
                res->tv_nsec = 1000; /* the kernel's clocks count microseconds */
        }
        return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
        (void)tz;
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return -1;
        if (tv != NULL) {
                tv->tv_sec = ts.tv_sec;
                tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
        }
        return 0;
}

clock_t times(struct tms *buf) {
        int64_t ns = 0;
        if (!bmds_sys_has(BMDS_SYS_CLOCK_GET) || result(cpu_ns(&ns)) < 0) return (clock_t)-1;
        const clock_t t = (clock_t)(ns / (NS_PER_SEC / CLOCKS_PER_SEC));
        if (buf != NULL) {
                buf->tms_utime = t;
                buf->tms_stime = 0;
                buf->tms_cutime = 0;
                buf->tms_cstime = 0;
        }
        return t;
}

/* --- Sleeping ------------------------------------------------------- */

int nanosleep(const struct timespec *req, struct timespec *rem) {
        NEED(BMDS_SYS_SLEEP);
        if (req->tv_nsec < 0 || req->tv_nsec >= NS_PER_SEC || req->tv_sec < 0) {
                errno = EINVAL;
                return -1;
        }
        const int64_t ns = (int64_t)req->tv_sec * NS_PER_SEC + req->tv_nsec;
        int64_t       before = 0, after = 0;
        if (rem != NULL) (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &before);
        const int32_t r = bmds_sleep(ns);
        if (r == -BMDS_EINTR && rem != NULL) {
                (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &after);
                int64_t left = ns - (after - before);
                if (left < 0) left = 0;
                rem->tv_sec = (time_t)(left / NS_PER_SEC);
                rem->tv_nsec = (long)(left % NS_PER_SEC);
        }
        return result(r);
}

int usleep(useconds_t us) {
        NEED(BMDS_SYS_SLEEP);
        return result(bmds_sleep((int64_t)us * 1000));
}

unsigned sleep(unsigned seconds) {
        struct timespec req = {(time_t)seconds, 0}, rem = {0, 0};
        if (nanosleep(&req, &rem) < 0) return (unsigned)rem.tv_sec + (rem.tv_nsec != 0 ? 1u : 0u);
        return 0;
}

/* --- Odds and ends ---------------------------------------------------- */

int getentropy(void *buf, size_t size) {
        NEED(BMDS_SYS_RANDOM);
        if (size > 256) {
                errno = EIO;
                return -1;
        }
        return result(bmds_random(buf, (uint32_t)size));
}

/* One thread, no asynchronous signals: there is nothing to mask. */
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
        (void)how;
        (void)set;
        if (old != NULL) *old = 0;
        return 0;
}
