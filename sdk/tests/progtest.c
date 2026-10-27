/**
 * @file      progtest.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program for testing the loader on the board:
 *   progtest status N     return N from main
 *   progtest exit N       exit(N) from deep inside
 *   progtest leak KB      allocate KB kilobytes (and a fast block) and never free them
 *   progtest env NAME     print $NAME from the environment
 *   progtest tty          print isatty() of fds 0-2
 *   progtest float        a little floating point (FPU state per task)
 *   progtest files DIR    files, directories, descriptors, cwd, a watched folder and their errors, in DIR
 *   progtest time         clocks, the time zone, sleeping
 *   progtest sleep MS     sleep MS milliseconds (for Ctrl-C)
 *   progtest term         the terminal on fds 0-2
 *   progtest lines        read lines (fgets) until end of input
 *   progtest raw          read keys raw until 'q'
 *   progtest mem          memory info
 *   progtest random       16 random bytes
 *   progtest cwd DIR      getcwd, chdir DIR, getcwd
 *   progtest spawn SELF DIR  processes: vfork, exec, posix_spawn, wait, pipes, in DIR
 *                         (SELF: this program's path)
 *   progtest fd N         exit 0 if fd N is open, 1 if not
 *   progtest ppid N       exit 0 if the parent's id is N
 *   progtest exec SELF    execve SELF status 42 (the program is replaced)
 *   progtest orphan SELF  start a child that outlives this program
 *   progtest tree SELF    start two `spin` children and wait (for Ctrl-C)
 *   progtest scripts SELF DIR  system, popen, scripts run by exec (/bin/sh, #!), in DIR
 *   progtest run PATH [ARG]  posix_spawn PATH ARG (stdout to pt-out.txt), print its status
 *   progtest signals SELF handlers, masks, kill, SIGCHLD, SIGPIPE, sigsuspend, SA_RESTART
 *   progtest catch        print "catch: ready", wait for SIGINT or SIGTERM, say which, exit 7
 *   progtest ignore       ignore SIGINT, then sleep (for Ctrl-C: the second one kills)
 *   progtest killer PID MS [FD]  after MS ms send SIGUSR1 to PID, then write "x" to FD
 *   progtest pipewrite    write to a pipe nobody reads (ends by SIGPIPE)
 *   progtest sigstate     exit 0 if SIGUSR1 is default and SIGUSR2 ignored (after exec)
 *   progtest winch        wait up to 5 s for SIGWINCH, print the new size to stderr
 *   progtest poll SELF    poll, ppoll, select, O_NONBLOCK, termios: pipes, timeouts, hang-ups, signals
 *   progtest later MS FD  after MS ms write "x" to FD
 *   progtest threads SELF pthreads: mutexes, condition variables, TLS, signals, spawn, cancel, exit, exec
 *   progtest threadspin N threads computing, sleeping and waiting (N > 0: for N seconds; for Ctrl-C, ps -T)
 *   progtest threadexit   a thread calls exit(9)
 *   progtest mainexit     the first thread ends (pthread_exit) before the other (200 ms)
 *   progtest threadexec SELF  a thread execs SELF status 42
 *   progtest audio        /dev/dsp: the audio calls, OSS's ioctls and their errors (plays silence)
 *   progtest audioexit    queue 500 ms of silence on /dev/dsp and exit without closing it
 *   progtest decode FLAC  the decoder calls on FLAC (tests/host/audio/tone.flac): info, tags, reads,
 *                         seeks, a buffer, and their errors
 *   progtest stats SELF   the counters' calls: listing, reading, health, history, problems, watching
 *                         (dropped at exit, kept across exec), and their errors
 *   progtest tasks        task_list: this task, a thread's, the kernel's, strides and errors
 *   progtest procs SELF   proc_info: this process, iterating, a zombie's time, time and start across exec
 *   progtest rusage SELF  getrusage, times, the CPU clocks and sysinfo; a child's time once waited for
 *   progtest system       system_info: the serial, the host name (uname's and gethostname's), the board
 *   progtest notify [served]  notify_open/send/close: posting, replacing, closing, errors; served: a
 *                         balloon's expiry read from the events fd (the panel running)
 *   progtest timers SELF  alarm, setitimer, getitimer: an interrupted read, counting down, repeating,
 *                         bad values; not inherited, kept across exec, SIGALRM's default
 *   progtest pty SELF     pseudo-terminals: openpty, ptsname, both directions, a child making the
 *                         program end its terminal (setsid, TIOCSCTTY) and the size it sees, the
 *                         end of output once it exits, /dev/pts/N gone after
 *   progtest ctty         print what a pty's child sees of its terminal (for pty)
 *   progtest syslog       syslog() and a hand-made AF_UNIX socket to /dev/log (logging.md §3.3):
 *                         their lines in `log show`, tagged progtest[PID] and progtest-raw
 *   progtest crypto       the AES and SHA engines (slot 112, network.md §26): FIPS 197's, SP
 *                         800-38A's and FIPS 180's vectors, chained calls, in place, the errors,
 *                         long (DMA) calls against one-block ones, and each one's speed
 *   progtest net          sockets through musl on loopback: TCP (blocking and not, half closes,
 *                         resets, timeouts, poll), UDP (cut datagrams), raw ICMP, the interface
 *                         ioctls, net_info and net_watch, the errors
 *   progtest netwatch [N] print the interfaces, then again after each of N changes (net_watch;
 *                         default 1)
 *   progtest sched SELF   scheduling (processes.md §3.12): policies set and read back on the
 *                         caller and on another thread, the errors, inheritance by threads and
 *                         processes, SCHED_RESET_ON_FORK, affinity and nproc, a real-time sleep's
 *                         lateness against the tick's, two busy real-time threads throttled
 *   progtest schedof      exit with the caller's policy (for sched)
 *   progtest speech [quiet] text to speech (speech.md §4): audio_speak's errors, a sentence's
 *                         samples, the info, NUL, markup, speed, odd reads, full writes, two fds;
 *                         bmds_say aloud unless quiet
 *   progtest netfs        a filesystem this program serves (fs_serve, netfs.md step 2): a tree in
 *                         memory at /net/progtest; the bad configs, a big file's gathered writes and
 *                         reads ahead, times, renames, then files' checks through it; unmounted when
 *                         the server's fd closes
 *   progtest rtnet [PEER] real-time sockets (network.md §30f): SO_PRIORITY and IP_TOS, the
 *                         timestamp options and their control messages on loopback, the error
 *                         queue's errors, BMDS_SO_FASTPATH's; with PEER (a UDP reflector on port
 *                         5004), hardware send and receive stamps through the fast path and not
 */

#define _GNU_SOURCE /* gettid() */
#include <dirent.h>
#include <stddef.h>
#include <math.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/ptp_clock.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timex.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/soundcard.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

#define CHECK(cond)                                                                                                    \
        do {                                                                                                           \
                if (!(cond)) {                                                                                         \
                        printf("FAILED line %d: %s (errno %d %s)\n", __LINE__, #cond, errno, strerror(errno));         \
                        return 1;                                                                                      \
                }                                                                                                      \
        } while (0)

static int files(const char *dir) {
        char path[256], other[256], buf[64];
        CHECK(chdir(dir) == 0);
        // A file: create, write, size, seek, read back.
        snprintf(path, sizeof(path), "pt-file.txt");
        (void)unlink(path);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 3);
        CHECK(write(fd, "hello, file\n", 12) == 12);
        struct stat st;
        CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 12);
        CHECK(fcntl(fd, F_GETFL) == O_WRONLY);
        CHECK(close(fd) == 0);
        CHECK(open(path, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
        fd = open(path, O_RDWR | O_APPEND);
        CHECK(fd >= 0 && write(fd, "more\n", 5) == 5);
        CHECK(lseek(fd, 7, SEEK_SET) == 7);
        CHECK(read(fd, buf, sizeof(buf)) == 10 && memcmp(buf, "file\nmore\n", 10) == 0);
        // dup and dup2 share the position.
        int d = dup(fd);
        CHECK(d > fd);
        CHECK(lseek(d, 0, SEEK_SET) == 0 && lseek(fd, 0, SEEK_CUR) == 0);
        CHECK(dup2(fd, 9) == 9 && read(9, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
        CHECK(close(9) == 0 && close(d) == 0 && close(fd) == 0);
        CHECK(close(fd) == -1 && errno == EBADF);
        // stdio over the same calls.
        FILE *f = fopen(path, "r");
        CHECK(f != NULL && fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "hello, file\n") == 0);
        CHECK(fclose(f) == 0);
        f = fopen(path, "a");
        CHECK(f != NULL && fprintf(f, "%d\n", 42) == 3 && fclose(f) == 0);
        CHECK(stat(path, &st) == 0 && st.st_size == 20);
        // Rename, directories, the errors.
        snprintf(other, sizeof(other), "pt-dir");
        CHECK(mkdir(other, 0755) == 0);
        CHECK(mkdir(other, 0755) == -1 && errno == EEXIST);
        CHECK(rename(path, "pt-dir/moved.txt") == 0);
        CHECK(stat(path, &st) == -1 && errno == ENOENT);
        CHECK(stat("pt-dir/moved.txt", &st) == 0 && st.st_size == 20);
        // A directory opens for reading (as on Linux: opendir does that), not for writing.
        int dfd = open("pt-dir", O_RDONLY);
        CHECK(dfd >= 0 && close(dfd) == 0);
        CHECK(open("pt-dir", O_WRONLY) == -1 && errno == EISDIR);
        CHECK(unlink("pt-dir") == -1 && errno == EISDIR);
        CHECK(rmdir("pt-dir") == -1 && errno == ENOTEMPTY);
        DIR *dp = opendir("pt-dir");
        CHECK(dp != NULL);
        struct dirent *e = readdir(dp);
        CHECK(e != NULL && strcmp(e->d_name, ".") == 0 && e->d_type == DT_DIR);
        e = readdir(dp);
        CHECK(e != NULL && strcmp(e->d_name, "..") == 0);
        e = readdir(dp);
        CHECK(e != NULL && strcmp(e->d_name, "moved.txt") == 0 && e->d_type == DT_REG);
        CHECK(readdir(dp) == NULL);
        CHECK(closedir(dp) == 0);
        CHECK(opendir("nonexistent") == NULL && errno == ENOENT);
        CHECK(chdir("pt-dir") == 0 && getcwd(buf, sizeof(buf)) != NULL);
        CHECK(strstr(buf, "/pt-dir") != NULL);
        CHECK(chdir("moved.txt") == -1 && errno == ENOTDIR);
        CHECK(chdir("..") == 0);
        CHECK(unlink("pt-dir/moved.txt") == 0 && rmdir("pt-dir") == 0);
        CHECK(getcwd(buf, 2) == NULL && errno == ERANGE);
        CHECK(open("/dev/null", O_WRONLY) >= 0);
        CHECK(stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode));
        // A folder watched (fs_watch): nothing until a change; each kind's bit; many changes one record.
        if (bmds_sys_has(BMDS_SYS_FS_WATCH)) {
                CHECK(mkdir("pt-watch", 0755) == 0);
                const int w = bmds_fs_watch("pt-watch", BMDS_O_CLOEXEC);
                CHECK(w >= 0);
                struct pollfd pw = {.fd = w, .events = POLLIN};
                uint32_t      bits = 0;
                CHECK(poll(&pw, 1, 0) == 0);
                fd = open("pt-watch/a", O_WRONLY | O_CREAT, 0644);
                CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
                CHECK(poll(&pw, 1, 1000) == 1 && (pw.revents & POLLIN) != 0);
                CHECK(read(w, &bits, sizeof(bits)) == 4 && bits == BMDS_FS_WATCH_CONTENTS);
                CHECK(poll(&pw, 1, 0) == 0);
                CHECK(rename("pt-watch/a", "pt-watch/b") == 0 && mkdir("pt-watch/c", 0755) == 0);
                CHECK(rmdir("pt-watch/c") == 0);
                CHECK(read(w, &bits, sizeof(bits)) == 4 && bits == BMDS_FS_WATCH_ENTRIES);
                CHECK(unlink("pt-watch/b") == 0 && rmdir("pt-watch") == 0); // the folder itself
                CHECK(read(w, &bits, sizeof(bits)) == 4 && (bits & BMDS_FS_WATCH_ENTRIES) != 0);
                CHECK(read(w, &bits, 3) == -1 && errno == EINVAL);
                CHECK(close(w) == 0);
                CHECK(bmds_fs_watch("pt-watch", 0) == -BMDS_ENOENT);
                CHECK(bmds_fs_watch("/dev/null", 0) == -BMDS_ENOTDIR);
                CHECK(bmds_fs_watch(".", 0x80000000u) == -BMDS_EINVAL);
        }
        // Too many directories open at once, then all closed at exit.
        DIR *many[9];
        int  opened = 0;
        while (opened < 9 && (many[opened] = opendir(".")) != NULL) ++opened;
        CHECK(opened == 8 && errno == EMFILE);
        printf("files: ok\n");
        return 0;
}

/* --- A filesystem served by this program (devplan/netfs.md step 2). */

#define NETFS_NODES 48
#define NETFS_PATH 128
#define NETFS_DATA 8192u
#define NETFS_SPACE (4u << 20)

typedef struct netfs_node_t {
                int     used;
                int     dir;
                char    path[NETFS_PATH];
                char   *data;
                size_t  len;
                size_t  cap;
                int64_t modified;
} netfs_node_t;

static netfs_node_t netfs_nodes[NETFS_NODES];
static int          netfs_fd = -1;
static volatile int netfs_quit;
static int          netfs_requests; /* __atomic: the server counts, the test reads */
static uint8_t     *netfs_in, *netfs_out;

static int64_t netfs_now(void) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int netfs_find(const char *p, size_t n) {
        for (int i = 0; i < NETFS_NODES; ++i)
                if (netfs_nodes[i].used && strlen(netfs_nodes[i].path) == n && memcmp(netfs_nodes[i].path, p, n) == 0)
                        return i;
        return -1;
}

static int netfs_add(const char *p, size_t n, int dir) {
        if (n >= NETFS_PATH) return -1;
        for (int i = 0; i < NETFS_NODES; ++i) {
                netfs_node_t *node = &netfs_nodes[i];
                if (node->used) continue;
                memset(node, 0, sizeof(*node));
                node->used = 1;
                node->dir = dir;
                memcpy(node->path, p, n);
                node->modified = netfs_now();
                return i;
        }
        return -1;
}

static void netfs_free(int i) {
        free(netfs_nodes[i].data);
        memset(&netfs_nodes[i], 0, sizeof(netfs_nodes[i]));
        return;
}

/* Is @p node directly in directory @p p (@p n bytes)? */
static int netfs_in_dir(const netfs_node_t *node, const char *p, size_t n) {
        if (strcmp(node->path, "/") == 0) return 0;
        size_t at = (size_t)(strrchr(node->path, '/') - node->path);
        if (at == 0) at = 1; /* in "/" */
        return at == n && memcmp(node->path, p, n) == 0;
}

static int netfs_has_children(int dir) {
        for (int i = 0; i < NETFS_NODES; ++i)
                if (netfs_nodes[i].used &&
                    netfs_in_dir(&netfs_nodes[i], netfs_nodes[dir].path, strlen(netfs_nodes[dir].path)))
                        return 1;
        return 0;
}

static int netfs_resize(netfs_node_t *node, size_t len) {
        if (len > node->cap) {
                const size_t cap = len > node->cap * 2 ? len : node->cap * 2;
                char        *d = realloc(node->data, cap);
                if (d == NULL) return -BMDS_ENOSPC;
                node->data = d;
                node->cap = cap;
        }
        if (len > node->len) memset(node->data + node->len, 0, len - node->len);
        node->len = len;
        node->modified = netfs_now();
        return 0;
}

static size_t netfs_entry(uint8_t *out, const netfs_node_t *node) {
        const char     *name = strcmp(node->path, "/") == 0 ? "" : strrchr(node->path, '/') + 1;
        bmds_fs_entry_t e = {0};
        e.type = node->dir ? BMDS_TYPE_DIRECTORY : BMDS_TYPE_FILE;
        e.bytes = node->len;
        e.modified_ns = node->modified;
        e.mode = node->dir ? 0755 : 0644;
        e.name_len = (uint32_t)strlen(name);
        e.size = (uint32_t)((sizeof(e) + e.name_len + 7) & ~(size_t)7);
        memset(out, 0, e.size);
        memcpy(out, &e, sizeof(e));
        memcpy(out + sizeof(e), name, e.name_len);
        return e.size;
}

/* RENAME: @p from to @p to, and everything under it. */
static int netfs_rename(int from, const char *to, size_t n, uint32_t flags) {
        const int there = netfs_find(to, n);
        if (there == from) return 0;
        if (there >= 0) {
                if ((flags & BMDS_FS_REPLACE) == 0) return -BMDS_EEXIST;
                if (netfs_nodes[there].dir && netfs_has_children(there)) return -BMDS_ENOTEMPTY;
                netfs_free(there);
        }
        char         old[NETFS_PATH];
        const size_t on = strlen(netfs_nodes[from].path);
        memcpy(old, netfs_nodes[from].path, on + 1);
        for (int i = 0; i < NETFS_NODES; ++i) {
                netfs_node_t *node = &netfs_nodes[i];
                if (!node->used || strncmp(node->path, old, on) != 0 || (node->path[on] != '/' && node->path[on] != 0))
                        continue;
                char         path[NETFS_PATH];
                const size_t rest = strlen(node->path + on);
                if (n + rest >= NETFS_PATH) return -BMDS_ENAMETOOLONG;
                memcpy(path, to, n);
                memcpy(path + n, node->path + on, rest + 1);
                memcpy(node->path, path, n + rest + 1);
        }
        return 0;
}

/* The reply to the request in @p in, made in @p out; its bytes. */
static size_t netfs_answer(const uint8_t *in, uint8_t *out) {
        bmds_fs_request_t r;
        memcpy(&r, in, sizeof(r));
        const char     *path = (const char *)in + sizeof(r);
        const char     *path2 = path + r.path_len;
        const uint8_t  *data = (const uint8_t *)path2 + r.path2_len;
        bmds_fs_reply_t y = {0};
        uint8_t        *pay = out + sizeof(y);
        size_t          n = 0;
        int             at = netfs_find(path, r.path_len);
        netfs_node_t   *h = r.handle >= 1 && r.handle <= NETFS_NODES && netfs_nodes[r.handle - 1].used
                                    ? &netfs_nodes[r.handle - 1]
                                    : NULL;
        y.id = r.id;
        switch (r.op) {
                case BMDS_FS_STAT:
                        if (at < 0)
                                y.error = -BMDS_ENOENT;
                        else
                                n = netfs_entry(pay, &netfs_nodes[at]);
                        break;
                case BMDS_FS_OPEN:
                        if (at < 0 && (r.flags & BMDS_FS_O_CREATE) == 0) {
                                y.error = -BMDS_ENOENT;
                        } else if (at >= 0 && (r.flags & BMDS_FS_O_EXCLUSIVE) != 0) {
                                y.error = -BMDS_EEXIST;
                        } else if (at >= 0 && netfs_nodes[at].dir) {
                                y.error = -BMDS_EISDIR;
                        } else {
                                if (at < 0) at = netfs_add(path, r.path_len, 0);
                                if (at < 0) {
                                        y.error = -BMDS_ENOSPC;
                                        break;
                                }
                                if ((r.flags & BMDS_FS_O_TRUNCATE) != 0) netfs_nodes[at].len = 0;
                                y.handle = (uint64_t)at + 1;
                        }
                        break;
                case BMDS_FS_READ:
                        if (h == NULL) {
                                y.error = -BMDS_EBADF;
                        } else if (r.offset < h->len) {
                                n = h->len - (size_t)r.offset < r.length ? h->len - (size_t)r.offset : r.length;
                                memcpy(pay, h->data + r.offset, n);
                        }
                        y.count = (uint32_t)n;
                        break;
                case BMDS_FS_WRITE:
                        if (h == NULL) {
                                y.error = -BMDS_EBADF;
                                break;
                        }
                        if (r.offset + r.length > h->len) y.error = netfs_resize(h, (size_t)r.offset + r.length);
                        if (y.error == 0) {
                                memcpy(h->data + r.offset, data, r.length);
                                h->modified = netfs_now();
                                y.count = r.length;
                        }
                        break;
                case BMDS_FS_TRUNCATE:
                        if (h == NULL) {
                                y.error = -BMDS_EBADF;
                        } else if (r.offset > NETFS_SPACE) {
                                y.error = -BMDS_EFBIG;
                        } else if (r.offset <= h->len) {
                                h->len = (size_t)r.offset;
                                h->modified = netfs_now();
                        } else {
                                y.error = netfs_resize(h, (size_t)r.offset);
                        }
                        break;
                case BMDS_FS_CLOSE: break;
                case BMDS_FS_READDIR: {
                        if (at < 0 || !netfs_nodes[at].dir) {
                                y.error = at < 0 ? -BMDS_ENOENT : -BMDS_ENOTDIR;
                                break;
                        }
                        uint64_t skip = r.offset;
                        for (int i = 0; i < NETFS_NODES; ++i) {
                                if (!netfs_nodes[i].used || !netfs_in_dir(&netfs_nodes[i], path, r.path_len)) continue;
                                if (skip > 0) {
                                        --skip;
                                        continue;
                                }
                                uint8_t      one[sizeof(bmds_fs_entry_t) + NETFS_PATH + 8];
                                const size_t k = netfs_entry(one, &netfs_nodes[i]);
                                if (n + k > r.length) break;
                                memcpy(pay + n, one, k);
                                n += k;
                                ++y.count;
                        }
                        break;
                }
                case BMDS_FS_MKDIR:
                        if (at >= 0)
                                y.error = -BMDS_EEXIST;
                        else if (netfs_add(path, r.path_len, 1) < 0)
                                y.error = -BMDS_ENOSPC;
                        break;
                case BMDS_FS_RMDIR:
                        if (at < 0)
                                y.error = -BMDS_ENOENT;
                        else if (!netfs_nodes[at].dir)
                                y.error = -BMDS_ENOTDIR;
                        else if (netfs_has_children(at))
                                y.error = -BMDS_ENOTEMPTY;
                        else
                                netfs_free(at);
                        break;
                case BMDS_FS_REMOVE:
                        if (at < 0)
                                y.error = -BMDS_ENOENT;
                        else if (netfs_nodes[at].dir)
                                y.error = -BMDS_EISDIR;
                        else
                                netfs_free(at);
                        break;
                case BMDS_FS_RENAME:
                        y.error = at < 0 ? -BMDS_ENOENT : netfs_rename(at, path2, r.path2_len, r.flags);
                        break;
                case BMDS_FS_SETTIME:
                        if (at < 0)
                                y.error = -BMDS_ENOENT;
                        else
                                netfs_nodes[at].modified = r.time_ns;
                        break;
                case BMDS_FS_STATFS: {
                        bmds_fs_space_t space = {NETFS_SPACE, NETFS_SPACE};
                        for (int i = 0; i < NETFS_NODES; ++i) space.free -= netfs_nodes[i].len;
                        memcpy(pay, &space, sizeof(space));
                        n = sizeof(space);
                        break;
                }
                default: y.error = -BMDS_ENOSYS; break;
        }
        y.size = (uint32_t)(sizeof(y) + n);
        memcpy(out, &y, sizeof(y));
        return y.size;
}

/* The server: a request read, its reply written, until told to stop (at the next request)
 * or the mount goes; its fd closed as it ends, which unmounts it. */
static void *netfs_server(void *arg) {
        (void)arg;
        for (;;) {
                const ssize_t n = read(netfs_fd, netfs_in, BMDS_FS_REQUEST_MAX(NETFS_DATA));
                if (n <= 0 || netfs_quit) break;
                __atomic_add_fetch(&netfs_requests, 1, __ATOMIC_RELAXED);
                const size_t m = netfs_answer(netfs_in, netfs_out);
                if (write(netfs_fd, netfs_out, m) != (ssize_t)m) break;
        }
        close(netfs_fd);
        return NULL;
}

static int netfs_count(void) {
        return __atomic_load_n(&netfs_requests, __ATOMIC_RELAXED);
}

static int netfs(void) {
        const char     *at = "/net/progtest";
        char            path[64], other[64];
        struct stat     st;
        bmds_fs_serve_t c = {.size = sizeof(c), .path = "/dev/pt", .type = "progtest", .max_data = NETFS_DATA};
        // The bad configs.
        CHECK(bmds_fs_serve(&c) == -BMDS_EINVAL);
        c.path = at;
        c.type = "a-name-too-long-for-it";
        CHECK(bmds_fs_serve(&c) == -BMDS_EINVAL);
        c.type = "";
        CHECK(bmds_fs_serve(&c) == -BMDS_EINVAL);
        c.type = "progtest";
        c.max_data = BMDS_FS_DATA_MIN - 1;
        CHECK(bmds_fs_serve(&c) == -BMDS_EINVAL);
        c.max_data = NETFS_DATA;
        c.flags = 0x80;
        CHECK(bmds_fs_serve(&c) == -BMDS_EINVAL);
        c.flags = BMDS_O_CLOEXEC;
        CHECK(bmds_fs_serve(NULL) == -BMDS_EFAULT);
        // Served: one mount there, the server a thread of this program.
        netfs_in = malloc(BMDS_FS_REQUEST_MAX(NETFS_DATA));
        netfs_out = malloc(sizeof(bmds_fs_reply_t) + NETFS_DATA + 4096);
        CHECK(netfs_in != NULL && netfs_out != NULL && netfs_add("/", 1, 1) == 0);
        netfs_fd = bmds_fs_serve(&c);
        CHECK(netfs_fd >= 3 && (fcntl(netfs_fd, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK(bmds_fs_serve(&c) == -BMDS_EEXIST);
        pthread_t server;
        CHECK(pthread_create(&server, NULL, netfs_server, NULL) == 0);
        bmds_fs_info_t info = {.size = sizeof(info)};
        CHECK(bmds_fs_info(at, &info) == 0 && strcmp(info.type, "progtest") == 0 && info.total_bytes == NETFS_SPACE);
        CHECK(stat(at, &st) == 0 && S_ISDIR(st.st_mode));
        // A big file: 100 000 bytes written 1 000 at a time, gathered into NETFS_DATA's; read back
        // 3 000 at a time from reads ahead.
        static uint8_t chunk[3000];
        snprintf(path, sizeof(path), "%s/big.bin", at);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 3);
        int before = netfs_count();
        for (int i = 0; i < 100; ++i) {
                for (int k = 0; k < 1000; ++k) chunk[k] = (uint8_t)((i * 1000 + k) * 7);
                CHECK(write(fd, chunk, 1000) == 1000);
        }
        const int writes = netfs_count() - before;
        CHECK(writes <= (int)(100000 / NETFS_DATA) + 1);
        CHECK(close(fd) == 0);
        const int big = netfs_find("/big.bin", 8);
        CHECK(big >= 0 && netfs_nodes[big].len == 100000);
        CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 100000);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 3);
        before = netfs_count();
        for (int at_ = 0; at_ < 100000;) {
                const ssize_t n = read(fd, chunk, sizeof(chunk));
                CHECK(n > 0);
                for (ssize_t k = 0; k < n; ++k) CHECK(chunk[k] == (uint8_t)((at_ + k) * 7));
                at_ += (int)n;
        }
        CHECK(read(fd, chunk, sizeof(chunk)) == 0);
        const int reads = netfs_count() - before;
        CHECK(reads <= (int)(100000 / NETFS_DATA) + 3);
        CHECK(lseek(fd, 0, SEEK_END) == 100000 && pread(fd, chunk, 10, 50000) == 10 &&
              chunk[3] == (uint8_t)(50003 * 7));
        CHECK(close(fd) == 0);
        // Its time set and read back; not renamed to another filesystem.
        struct timespec times[2] = {{0, UTIME_OMIT}, {1700000000, 0}};
        CHECK(utimensat(AT_FDCWD, path, times, 0) == 0 && stat(path, &st) == 0 && st.st_mtime == 1700000000);
        CHECK(rename(path, "/tmp/pt-netfs.bin") == -1 && errno == EXDEV);
        // A directory renamed takes what is in it.
        snprintf(other, sizeof(other), "%s/d", at);
        CHECK(mkdir(other, 0755) == 0);
        snprintf(other, sizeof(other), "%s/d/f", at);
        CHECK(rename(path, other) == 0 && stat(path, &st) == -1 && errno == ENOENT);
        snprintf(path, sizeof(path), "%s/d", at);
        snprintf(other, sizeof(other), "%s/e", at);
        CHECK(rename(path, other) == 0);
        snprintf(path, sizeof(path), "%s/e/f", at);
        CHECK(stat(path, &st) == 0 && st.st_size == 100000);
        snprintf(other, sizeof(other), "%s/d/f", at);
        CHECK(stat(other, &st) == -1 && errno == ENOENT);
        CHECK(unlink(path) == 0);
        snprintf(path, sizeof(path), "%s/e", at);
        CHECK(rmdir(path) == 0);
        // The files checks through it (they end in it with directories open).
        const int served = netfs_count();
        if (files(at) != 0) return 1;
        // The server ends at the request this makes; its fd closes and the mount goes.
        CHECK(chdir("/") == 0);
        netfs_quit = 1;
        snprintf(path, sizeof(path), "%s/wake", at);
        CHECK(stat(path, &st) == -1 && errno == ENOMEDIUM);
        CHECK(pthread_join(server, NULL) == 0);
        CHECK(stat(path, &st) == -1 && errno == ENOENT);
        CHECK(stat(at, &st) == -1 && errno == ENOENT);
        printf("netfs: ok (%d requests: %d writes and %d reads for 100 000 bytes, files %d)\n", netfs_count(), writes,
               reads, netfs_count() - served);
        return 0;
}

static int timing(void) {
        struct timespec rt, m1, m2;
        CHECK(clock_gettime(CLOCK_REALTIME, &rt) == 0);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &m1) == 0);
        const time_t t = time(NULL);
        CHECK(t >= rt.tv_sec && t <= rt.tv_sec + 1);
        struct timespec req = {0, 50 * 1000000};
        CHECK(nanosleep(&req, NULL) == 0);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &m2) == 0);
        const long ms = (long)((m2.tv_sec - m1.tv_sec) * 1000 + (m2.tv_nsec - m1.tv_nsec) / 1000000);
        if (ms < 50 || ms >= 70)
                printf("slept %ld ms: m1 %lld.%09ld m2 %lld.%09ld\n", ms, (long long)m1.tv_sec, m1.tv_nsec,
                       (long long)m2.tv_sec, m2.tv_nsec);
        CHECK(ms >= 50 && ms < 70);
        char       when[64];
        struct tm *tm = localtime(&t);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z", tm);
        const char *tz = getenv("TZ");
        printf("time: %s (TZ=%s), slept %ld ms, clock %ld\n", when, tz != NULL ? tz : "(unset)", ms, (long)clock());
        return 0;
}

static int term(void) {
        for (int fd = 0; fd < 3; ++fd) {
                bmds_term_t   t = {.size = sizeof(t)};
                const int32_t r = bmds_term_get(fd, &t);
                if (r < 0) {
                        printf("fd %d: %s\n", fd, strerror(-r));
                } else {
                        printf("fd %d: flags 0x%02x, %ux%u, %u colors\n", fd, (unsigned)t.flags, t.columns, t.rows,
                               (unsigned)t.colors);
                }
        }
        return 0;
}

static int lines(void) {
        char line[300];
        int  n = 0;
        while (fgets(line, sizeof(line), stdin) != NULL)
                printf("line %d: [%.*s]\n", ++n, (int)strcspn(line, "\n"), line);
        printf("end of input after %d lines%s\n", n, ferror(stdin) ? " (error)" : "");
        return 0;
}

static int raw(void) {
        bmds_term_t t = {.size = sizeof(t)};
        CHECK(bmds_term_get(0, &t) == 0);
        CHECK(bmds_term_set(0, t.flags & ~(BMDS_TERM_LINE | BMDS_TERM_SIGNALS | BMDS_TERM_ECHO | BMDS_TERM_ICRNL)) ==
              0);
        printf("raw keys, q ends:\n");
        fflush(stdout);
        for (;;) {
                unsigned char c;
                if (read(0, &c, 1) != 1) break;
                printf("0x%02x\n", c);
                if (c == 'q') break;
        }
        return 0;
}

static volatile sig_atomic_t caught;

/* Thread-locals, initialized and not: musl copies the template (described
 * to it by the startup code) into the thread's block. */
static __thread int  tls_counter = 5;
static __thread char tls_zeros[64];

static void on_signal(int sig) {
        caught = sig;
        return;
}

/* The POSIX layer (musl over BMDS): sizes, holes, file identity,
 * access, termios, signals raised by the program itself, uname. */
static int posix(const char *dir) {
        char        path[512];
        struct stat a, b;
        CHECK(tls_counter == 5 && tls_zeros[0] == 0 && tls_zeros[63] == 0);
        tls_counter += 2;
        tls_zeros[63] = 'x';
        CHECK(tls_counter == 7 && tls_zeros[63] == 'x');
        CHECK(chdir(dir) == 0);
        snprintf(path, sizeof(path), "pt-posix.txt");
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0 && write(fd, "abcdef", 6) == 6);
        // ftruncate: cut, then grown with zeros; the offset stays.
        CHECK(ftruncate(fd, 3) == 0 && fstat(fd, &a) == 0 && a.st_size == 3);
        CHECK(lseek(fd, 0, SEEK_CUR) == 6);
        CHECK(ftruncate(fd, 8) == 0 && fstat(fd, &a) == 0 && a.st_size == 8);
        char buf[16] = {0};
        CHECK(pread(fd, buf, sizeof(buf), 0) == 8 && memcmp(buf, "abc\0\0\0\0\0", 8) == 0);
        // Written past the end: the gap reads as zeros; seeking alone doesn't grow it.
        CHECK(lseek(fd, 100, SEEK_SET) == 100 && fstat(fd, &a) == 0 && a.st_size == 8);
        CHECK(write(fd, "Z", 1) == 1 && fstat(fd, &a) == 0 && a.st_size == 101);
        CHECK(pread(fd, buf, 4, 96) == 4 && memcmp(buf, "\0\0\0\0", 4) == 0);
        CHECK(ftruncate(fd, -1) == -1 && errno == EINVAL);
        // One file, one id: by name, by fd, in its directory; under another case too where the
        // filesystem folds case (FAT), else that is another name (tmpfs, littlefs).
        CHECK(stat(path, &a) == 0 && fstat(fd, &b) == 0);
        CHECK(a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_ino != 0);
        if (stat("PT-POSIX.TXT", &b) == 0) {
                CHECK(a.st_ino == b.st_ino);
        } else {
                CHECK(errno == ENOENT);
        }
        CHECK(stat(".", &b) == 0 && a.st_ino != b.st_ino && S_ISDIR(b.st_mode));
        DIR           *dp = opendir(".");
        struct dirent *e;
        int            found = 0;
        while (dp != NULL && (e = readdir(dp)) != NULL) found |= strcmp(e->d_name, path) == 0 && e->d_ino == a.st_ino;
        CHECK(dp != NULL && closedir(dp) == 0 && found);
        CHECK(close(fd) == 0 && truncate(path, 2) == 0 && stat(path, &a) == 0 && a.st_size == 2);
        // access: X_OK as the shell's test -x.
        CHECK(access(path, R_OK | W_OK) == 0 && access(path, X_OK) == -1 && errno == EACCES);
        CHECK(access(".", X_OK) == 0 && access("nonexistent", F_OK) == -1 && errno == ENOENT);
        // Symbolic links (emulated on FAT).
        char target[64] = {0};
        CHECK(stat(path, &a) == 0 && symlink(path, "pt-link") == 0);
        CHECK(lstat("pt-link", &b) == 0 && S_ISLNK(b.st_mode) && b.st_size == (off_t)strlen(path));
        CHECK(readlink("pt-link", target, sizeof(target)) == (ssize_t)strlen(path) && strcmp(target, path) == 0);
        CHECK(stat("pt-link", &b) == 0 && S_ISREG(b.st_mode) && b.st_ino == a.st_ino);
        CHECK(symlink("x", "pt-link") == -1 && errno == EEXIST);
        CHECK(symlink("pt-loop", "pt-loop") == 0 && open("pt-loop", O_RDONLY) == -1 && errno == ELOOP);
        CHECK(open("pt-link", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
        CHECK(unlink("pt-link") == 0 && unlink("pt-loop") == 0 && stat(path, &b) == 0);
        // rename() replaces: a file a file, a directory an empty one; never one with the other.
        int nf = open("pt-new", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(nf >= 0 && write(nf, "new", 3) == 3 && close(nf) == 0);
        CHECK(rename("pt-new", path) == 0 && stat(path, &a) == 0 && a.st_size == 3);
        CHECK(stat("pt-new", &b) == -1 && errno == ENOENT);
        CHECK(rename(path, path) == 0 && stat(path, &a) == 0 && a.st_size == 3);
        CHECK(mkdir("pt-dir", 0755) == 0 && mkdir("pt-empty", 0755) == 0);
        CHECK(rename(path, "pt-dir") == -1 && errno == EISDIR);
        CHECK(rename("pt-dir", path) == -1 && errno == ENOTDIR);
        CHECK(rename("pt-dir", "pt-empty") == 0 && stat("pt-dir", &b) == -1 && errno == ENOENT);
        CHECK(mkdir("pt-dir", 0755) == 0 && rename(path, "pt-empty/f") == 0);
        CHECK(rename("pt-dir", "pt-empty") == -1 && errno == ENOTEMPTY);
        CHECK(rename("pt-empty/f", path) == 0 && rmdir("pt-empty") == 0 && rmdir("pt-dir") == 0);
        CHECK(unlink(path) == 0);
        // termios on the console: raw and back (a pipe or file: ENOTTY).
        struct termios t, raw;
        if (tcgetattr(0, &t) == 0) {
                raw = t;
                cfmakeraw(&raw);
                CHECK(tcsetattr(0, TCSANOW, &raw) == 0 && tcgetattr(0, &raw) == 0 && !(raw.c_lflag & ICANON));
                CHECK(tcsetattr(0, TCSANOW, &t) == 0 && tcgetattr(0, &raw) == 0 && (raw.c_lflag & ICANON));
        } else {
                CHECK(errno == ENOTTY);
        }
        // Signals the program raises itself.
        CHECK(signal(SIGUSR1, on_signal) != SIG_ERR && raise(SIGUSR1) == 0 && caught == SIGUSR1);
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        caught = 0;
        CHECK(signal(SIGUSR2, on_signal) != SIG_ERR && sigprocmask(SIG_BLOCK, &set, &old) == 0);
        CHECK(raise(SIGUSR2) == 0 && caught == 0);
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0 && caught == SIGUSR2);
        CHECK(signal(SIGCHLD, SIG_DFL) != SIG_ERR && raise(SIGCHLD) == 0); // ignored by default
        struct utsname u;
        CHECK(uname(&u) == 0 && strcmp(u.sysname, "BMDS") == 0);
        printf("posix: ok (%s %s %s)\n", u.sysname, u.release, u.machine);
        return 0;
}

static int mem(void) {
        char *volatile p = malloc(100000); /* volatile: GCC drops a malloc nothing uses */
        bmds_mem_info_t psram = {.size = sizeof(psram)}, fast = {.size = sizeof(fast)};
        CHECK(bmds_mem_info(0, &psram) == 0 && bmds_mem_info(BMDS_MEM_FAST, &fast) == 0);
        CHECK(psram.program_bytes >= 100000 && psram.free_bytes <= psram.total_bytes);
        printf("psram %u/%u free (largest %u), internal %u/%u free, program %u bytes in %u blocks\n",
               (unsigned)psram.free_bytes, (unsigned)psram.total_bytes, (unsigned)psram.largest_free,
               (unsigned)fast.free_bytes, (unsigned)fast.total_bytes, (unsigned)psram.program_bytes,
               (unsigned)psram.program_blocks);
        free(p);
        /* munmap by a length short of its last page's end frees the whole mapping, as Linux's
           does (musl unmaps a zone file by its size). The first mapping makes the list. */
        void *m = mmap(NULL, 100, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED && munmap(m, 100) == 0);
        CHECK(bmds_mem_info(0, &psram) == 0);
        const uint32_t before = psram.program_bytes;
        m = mmap(NULL, 100, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(m != MAP_FAILED && munmap(m, 100) == 0);
        CHECK(bmds_mem_info(0, &psram) == 0 && psram.program_bytes == before);
        return 0;
}

extern char **environ;

/* Starts SELF with the arguments after it (NULL-terminated); its id. */
static pid_t start(const char *self, posix_spawn_file_actions_t *fa, char **env, ...) {
        char   *argv[8] = {"progtest"};
        int     n = 1;
        va_list ap;
        va_start(ap, env);
        for (char *a; n < 7 && (a = va_arg(ap, char *)) != NULL;) argv[n++] = a;
        va_end(ap);
        argv[n] = NULL;
        pid_t     pid = -1;
        const int r = posix_spawn(&pid, self, fa, NULL, argv, env != NULL ? env : environ);
        if (r != 0) {
                errno = r;
                return -1;
        }
        return pid;
}

/* The exit status of child @p pid, or -1. */
static int status_of(pid_t pid) {
        int st = 0;
        if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return -1;
        return WEXITSTATUS(st);
}

static volatile int shared_with_child;

/* Processes (programs.md §13). */
static int spawn(const char *self, const char *dir) {
        char buf[128], cwd[BMDS_PATH_MAX + 1], other[BMDS_PATH_MAX + 1];
        CHECK(chdir(dir) == 0);
        CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
        // No children yet.
        CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
        CHECK(getpid() > 1);

        // vfork + execve, and the exit status.
        pid_t pid = vfork();
        if (pid == 0) {
                char *argv[] = {"progtest", "status", "7", NULL};
                execve(self, argv, environ);
                _exit(99);
        }
        CHECK(pid > 0);
        CHECK(status_of(pid) == 7);

        // A vfork child that exits: its fds and directory were its own, its
        // memory the parent's.
        pid = vfork();
        if (pid == 0) {
                shared_with_child = 5;
                (void)dup2(2, 9);
                (void)chdir("/");
                (void)close(0);
                _exit(3);
        }
        CHECK(pid > 0);
        CHECK(shared_with_child == 5);
        CHECK(fcntl(9, F_GETFD) == -1 && errno == EBADF);
        CHECK(fcntl(0, F_GETFD) == 0);
        CHECK(getcwd(other, sizeof(other)) != NULL && strcmp(cwd, other) == 0);
        CHECK(status_of(pid) == 3);
        // exec failing in the child: the child goes on (and exits).
        pid = vfork();
        if (pid == 0) {
                char *argv[] = {"nope", NULL};
                execve("/nonexistent/program", argv, environ);
                _exit(errno == ENOENT ? 4 : 5);
        }
        CHECK(status_of(pid) == 4);

        // posix_spawn: file actions, the environment, the parent's id.
        posix_spawn_file_actions_t fa;
        CHECK(posix_spawn_file_actions_init(&fa) == 0);
        CHECK(posix_spawn_file_actions_addopen(&fa, 1, "pt-spawn.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0);
        char *env[] = {"PT_VAR=spawned", NULL};
        pid = start(self, &fa, env, "env", "PT_VAR", NULL);
        CHECK(pid > 0);
        CHECK(status_of(pid) == 0);
        CHECK(posix_spawn_file_actions_destroy(&fa) == 0);
        FILE *f = fopen("pt-spawn.txt", "r");
        CHECK(f != NULL && fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "PT_VAR=spawned\n") == 0);
        fclose(f);
        CHECK(unlink("pt-spawn.txt") == 0);
        snprintf(buf, sizeof(buf), "%d", (int)getpid());
        CHECK(status_of(start(self, NULL, NULL, "ppid", buf, NULL)) == 0);
        // A missing program is posix_spawn's error, not the child's status.
        pid_t none = -1;
        char *nargv[] = {"nope", NULL};
        CHECK(posix_spawn(&none, "/nonexistent/program", NULL, NULL, nargv, environ) == ENOENT);

        // A pipe: EOF once the writer has ended.
        int p[2];
        CHECK(pipe(p) == 0);
        CHECK(posix_spawn_file_actions_init(&fa) == 0);
        CHECK(posix_spawn_file_actions_adddup2(&fa, p[1], 1) == 0);
        CHECK(posix_spawn_file_actions_addclose(&fa, p[0]) == 0);
        pid = start(self, &fa, env, "env", "PT_VAR", NULL);
        CHECK(posix_spawn_file_actions_destroy(&fa) == 0);
        CHECK(close(p[1]) == 0);
        size_t got = 0;
        for (ssize_t n; (n = read(p[0], buf + got, sizeof(buf) - 1 - got)) > 0;) got += (size_t)n;
        buf[got] = '\0';
        CHECK(strcmp(buf, "PT_VAR=spawned\n") == 0);
        CHECK(close(p[0]) == 0);
        CHECK(status_of(pid) == 0);

        // Two children joined by a pipe; wait for any, then the other.
        CHECK(pipe(p) == 0);
        CHECK(posix_spawn_file_actions_init(&fa) == 0);
        CHECK(posix_spawn_file_actions_adddup2(&fa, p[1], 1) == 0);
        CHECK(posix_spawn_file_actions_addclose(&fa, p[0]) == 0);
        const pid_t writer = start(self, &fa, env, "env", "PT_VAR", NULL);
        CHECK(posix_spawn_file_actions_destroy(&fa) == 0);
        CHECK(posix_spawn_file_actions_init(&fa) == 0);
        CHECK(posix_spawn_file_actions_adddup2(&fa, p[0], 0) == 0);
        CHECK(posix_spawn_file_actions_addclose(&fa, p[1]) == 0);
        CHECK(posix_spawn_file_actions_addopen(&fa, 1, "pt-lines.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644) == 0);
        const pid_t reader = start(self, &fa, NULL, "lines", NULL);
        CHECK(posix_spawn_file_actions_destroy(&fa) == 0);
        CHECK(close(p[0]) == 0 && close(p[1]) == 0);
        int   st = 0;
        pid_t first = wait(&st);
        CHECK((first == writer || first == reader) && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        pid_t second = waitpid(-1, &st, 0);
        CHECK(second == (first == writer ? reader : writer) && WIFEXITED(st));
        f = fopen("pt-lines.txt", "r");
        CHECK(f != NULL && fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "line 1: [PT_VAR=spawned]\n") == 0);
        CHECK(fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "end of input after 1 lines\n") == 0);
        fclose(f);
        CHECK(unlink("pt-lines.txt") == 0);

        // WNOHANG while a child runs.
        pid = start(self, NULL, NULL, "sleep", "200", NULL);
        CHECK(pid > 0);
        CHECK(waitpid(pid, &st, WNOHANG) == 0);
        CHECK(status_of(pid) == 0);
        CHECK(waitpid(pid, &st, 0) == -1 && errno == ECHILD);

        // Close-on-exec: the flagged fd is gone in the child, the other not.
        const int keep = open("pt-keep.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int drop = open("pt-drop.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(keep >= 0 && drop >= 0);
        CHECK(fcntl(drop, F_GETFD) == FD_CLOEXEC && fcntl(keep, F_GETFD) == 0);
        snprintf(buf, sizeof(buf), "%d", keep);
        CHECK(status_of(start(self, NULL, NULL, "fd", buf, NULL)) == 0);
        snprintf(buf, sizeof(buf), "%d", drop);
        CHECK(status_of(start(self, NULL, NULL, "fd", buf, NULL)) == 1);
        CHECK(fcntl(keep, F_SETFD, FD_CLOEXEC) == 0 && fcntl(keep, F_GETFD) == FD_CLOEXEC);
        snprintf(buf, sizeof(buf), "%d", keep);
        CHECK(status_of(start(self, NULL, NULL, "fd", buf, NULL)) == 1);
        CHECK(close(keep) == 0 && close(drop) == 0);
        CHECK(unlink("pt-keep.txt") == 0 && unlink("pt-drop.txt") == 0);

        // A child that replaces itself: the status is the new program's.
        CHECK(status_of(start(self, NULL, NULL, "exec", self, NULL)) == 42);

        printf("spawn: all passed\n");
        return 0;
}

static int put_file(const char *path, const char *text) {
        FILE *f = fopen(path, "w");
        if (f == NULL) return -1;
        fputs(text, f);
        return fclose(f);
}

static int file_is(const char *path, const char *text) {
        char  buf[256];
        FILE *f = fopen(path, "r");
        if (f == NULL) return 0;
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        if (strcmp(buf, text) != 0) printf("%s: [%s]\n", path, buf);
        return strcmp(buf, text) == 0;
}

/* Runs @p path with @p arg, stdout to pt-out.txt; its exit status or -errno. */
static int run_to_file(const char *path, const char *arg) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "pt-out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char     *argv[] = {(char *)path, (char *)arg, NULL};
        pid_t     pid = -1;
        const int r = posix_spawn(&pid, path, &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        return r != 0 ? -r : status_of(pid);
}

/* /bin/sh and scripts (programs.md §13, step 8b). */
static int scripts(const char *self, const char *dir) {
        char buf[256];
        CHECK(chdir(dir) == 0);
        // system(): the shell runs the command, its status comes back.
        int st = system("echo hi > pt-sys.txt; exit 4");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 4);
        CHECK(file_is("pt-sys.txt", "hi\n"));
        CHECK(system(NULL) != 0);
        // popen() both ways.
        FILE *p = popen("echo one; echo two", "r");
        CHECK(p != NULL);
        size_t n = fread(buf, 1, sizeof(buf) - 1, p);
        buf[n] = '\0';
        CHECK(strcmp(buf, "one\ntwo\n") == 0);
        CHECK(pclose(p) == 0);
        p = popen("cat > pt-popen.txt", "w");
        CHECK(p != NULL && fputs("through popen\n", p) >= 0);
        CHECK(pclose(p) == 0);
        CHECK(file_is("pt-popen.txt", "through popen\n"));
        // Scripts by exec: #!/bin/sh, .sh without #!, a program as interpreter.
        CHECK(put_file("pt-s.sh", "#!/bin/sh\necho script $1\nexit 6\n") == 0);
        CHECK(run_to_file("pt-s.sh", "arg") == 6);
        CHECK(file_is("pt-out.txt", "script arg\n"));
        CHECK(put_file("pt-plain.sh", "echo plain $1\n") == 0);
        CHECK(run_to_file("pt-plain.sh", "x") == 0);
        CHECK(file_is("pt-out.txt", "plain x\n"));
        snprintf(buf, sizeof(buf), "#!%s env\n", self);
        CHECK(put_file("pt-interp", buf) == 0);
        CHECK(run_to_file("pt-interp", "z") == 0);
        CHECK(file_is("pt-out.txt", "pt-interp=(unset)\n"));
        CHECK(put_file("pt-crlf.sh", "#!/bin/sh -e\r\nfalse\necho not here\n") == 0);
        CHECK(run_to_file("pt-crlf.sh", "") == 1);
        // Not a program or script; an interpreter loop.
        CHECK(put_file("pt-text", "just text\n") == 0);
        CHECK(run_to_file("pt-text", "") == -ENOEXEC);
        CHECK(put_file("pt-loop", "#!pt-loop\n") == 0);
        CHECK(run_to_file("pt-loop", "") == -ELOOP);
        CHECK(run_to_file("pt-missing.sh", "") == -ENOENT);
        // execvp falls back to /bin/sh for a text file (POSIX).
        pid_t pid = vfork();
        if (pid == 0) {
                char *argv[] = {"pt-plain.sh", "vp", NULL};
                int   fd = open("pt-out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                dup2(fd, 1);
                close(fd);
                execvp("./pt-plain.sh", argv);
                _exit(99);
        }
        CHECK(status_of(pid) == 0);
        CHECK(file_is("pt-out.txt", "plain vp\n"));
        const char *made[] = {"pt-sys.txt", "pt-popen.txt", "pt-s.sh", "pt-plain.sh", "pt-interp",
                              "pt-crlf.sh", "pt-text",      "pt-loop", "pt-out.txt"};
        for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); ++i) CHECK(unlink(made[i]) == 0);
        printf("scripts: all passed\n");
        return 0;
}

static void deep(int depth, int status) {
        if (depth == 0) exit(status);
        deep(depth - 1, status);
}

static volatile sig_atomic_t got[NSIG];
static volatile int          last_code;

static void count_signal(int sig) {
        got[sig]++;
}

static void on_info(int sig, siginfo_t *si, void *context) {
        (void)context;
        got[sig]++;
        last_code = si->si_code;
}

static int catch_signal(int sig, void (*handler)(int), int flags) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sa.sa_flags = flags;
        return sigaction(sig, &sa, NULL);
}

/* How child @p pid ended: its exit status, or 1000 + the signal; -1 on error. */
static int ending_of(pid_t pid) {
        int st = 0;
        if (waitpid(pid, &st, 0) != pid) return -1;
        if (WIFSIGNALED(st)) return 1000 + WTERMSIG(st);
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Signals (programs.md §15). */
static int signals(const char *self) {
        char pidbuf[16], fdbuf[16];
        // raise() runs the handler before it returns; kill() of itself too, with siginfo.
        CHECK(catch_signal(SIGUSR1, count_signal, 0) == 0);
        CHECK(raise(SIGUSR1) == 0 && got[SIGUSR1] == 1);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_info;
        sa.sa_flags = SA_SIGINFO;
        CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);
        CHECK(kill(getpid(), SIGUSR2) == 0 && got[SIGUSR2] == 1 && last_code == SI_USER);
        // The mask holds a signal until it is unblocked.
        sigset_t set, old, pend;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        CHECK(raise(SIGUSR1) == 0 && got[SIGUSR1] == 1);
        CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1));
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0 && got[SIGUSR1] == 2);
        CHECK(sigpending(&pend) == 0 && !sigismember(&pend, SIGUSR1));
        // Ignored: dropped, a pending one too.
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0 && raise(SIGUSR1) == 0);
        CHECK(signal(SIGUSR1, SIG_IGN) == count_signal);
        CHECK(sigpending(&pend) == 0 && !sigismember(&pend, SIGUSR1));
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0 && got[SIGUSR1] == 2);
        // The job and errors.
        CHECK(kill(0, SIGUSR2) == 0 && got[SIGUSR2] == 2);
        CHECK(kill(-getpgrp(), SIGUSR2) == 0 && got[SIGUSR2] == 3);
        CHECK(kill(1, 0) == 0);                          /* pid 1, the serial shell, is there (one user: root) */
        CHECK(kill(1, SIGKILL) == -1 && errno == EPERM); /* but can't be killed */
        CHECK(kill(30000, 0) == -1 && errno == ESRCH);
        CHECK(kill(getpid(), 40) == -1 && errno == EINVAL);
        CHECK(sigaction(SIGKILL, &sa, NULL) == -1 && errno == EINVAL);
        // SIGCHLD when a child ends; waitpid still gets the child (no SA_RESTART).
        CHECK(catch_signal(SIGCHLD, count_signal, 0) == 0);
        pid_t c = start(self, NULL, NULL, "status", "3", NULL);
        CHECK(c > 0 && status_of(c) == 3 && got[SIGCHLD] == 1);
        CHECK(signal(SIGCHLD, SIG_DFL) == count_signal);
        // A zombie still accepts signals.
        c = start(self, NULL, NULL, "status", "0", NULL);
        CHECK(c > 0 && usleep(100000) == 0 && kill(c, 0) == 0 && status_of(c) == 0);
        // Children ended by SIGTERM: sleeping, computing; one that catches it.
        c = start(self, NULL, NULL, "sleep", "5000", NULL);
        CHECK(c > 0 && usleep(100000) == 0 && kill(c, SIGTERM) == 0 && ending_of(c) == 1000 + SIGTERM);
        c = start(self, NULL, NULL, "spin", NULL);
        CHECK(c > 0 && usleep(100000) == 0 && kill(c, SIGTERM) == 0 && ending_of(c) == 1000 + SIGTERM);
        c = start(self, NULL, NULL, "catch", NULL);
        CHECK(c > 0 && usleep(200000) == 0 && kill(c, SIGTERM) == 0 && ending_of(c) == 7);
        // SIGKILL can't be caught.
        c = start(self, NULL, NULL, "catch", NULL);
        CHECK(c > 0 && usleep(200000) == 0 && kill(c, SIGKILL) == 0 && ending_of(c) == 1000 + SIGKILL);
        // SIGPIPE ends a writer; ignored, the write fails with EPIPE.
        c = start(self, NULL, NULL, "pipewrite", NULL);
        CHECK(c > 0 && ending_of(c) == 1000 + SIGPIPE);
        int fds[2];
        CHECK(signal(SIGPIPE, SIG_IGN) == SIG_DFL && pipe(fds) == 0 && close(fds[0]) == 0);
        CHECK(write(fds[1], "x", 1) == -1 && errno == EPIPE && close(fds[1]) == 0);
        CHECK(signal(SIGPIPE, SIG_DFL) == SIG_IGN);
        // A vfork child that kills itself before exec; the parent goes on.
        shared_with_child = 0;
        c = vfork();
        if (c == 0) {
                shared_with_child = 1;
                kill(getpid(), SIGTERM);
                _exit(99); // not reached
        }
        CHECK(c > 0 && shared_with_child == 1 && ending_of(c) == 1000 + SIGTERM);
        // exec: handlers go back to default, ignored signals stay ignored.
        CHECK(catch_signal(SIGUSR1, count_signal, 0) == 0 && signal(SIGUSR2, SIG_IGN) != SIG_ERR);
        c = start(self, NULL, NULL, "sigstate", NULL);
        CHECK(c > 0 && status_of(c) == 0);
        CHECK(signal(SIGUSR2, SIG_DFL) == SIG_IGN);
        // A read interrupted by a handler: made again with SA_RESTART, EINTR without.
        snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
        char b = 0;
        for (int restart = 1; restart >= 0; --restart) {
                CHECK(catch_signal(SIGUSR1, count_signal, restart ? SA_RESTART : 0) == 0);
                CHECK(pipe(fds) == 0);
                snprintf(fdbuf, sizeof(fdbuf), "%d", fds[1]);
                const int before = got[SIGUSR1];
                c = start(self, NULL, NULL, "killer", pidbuf, "150", fdbuf, NULL);
                CHECK(c > 0 && close(fds[1]) == 0);
                const ssize_t n = read(fds[0], &b, 1);
                CHECK(got[SIGUSR1] == before + 1);
                if (restart) {
                        CHECK(n == 1 && b == 'x');
                } else {
                        CHECK(n == -1 && errno == EINTR);
                }
                CHECK(status_of(c) == 0 && close(fds[0]) == 0);
        }
        // sigsuspend: blocked until then, handled there, the mask put back.
        CHECK(catch_signal(SIGUSR1, count_signal, 0) == 0 && sigprocmask(SIG_BLOCK, &set, &old) == 0);
        const int before = got[SIGUSR1];
        c = start(self, NULL, NULL, "killer", pidbuf, "150", NULL);
        sigset_t none;
        sigemptyset(&none);
        CHECK(c > 0 && sigsuspend(&none) == -1 && errno == EINTR && got[SIGUSR1] == before + 1);
        CHECK(sigprocmask(SIG_SETMASK, &old, &pend) == 0 && sigismember(&pend, SIGUSR1));
        CHECK(status_of(c) == 0);
        printf("signals: all passed\n");
        return 0;
}

static long ms_since(const struct timespec *start) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (long)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

/* poll, ppoll, select (programs.md §16). */
static int polling(const char *self) {
        char            pidbuf[16], fdbuf[16], b = 0;
        struct timespec t0;
        int             fds[2];
        CHECK(pipe(fds) == 0);
        struct pollfd p[2] = {{.fd = fds[0], .events = POLLIN}, {.fd = 1, .events = POLLOUT}};
        // Not readable yet; the terminal is writable.
        CHECK(poll(p, 2, 0) == 1 && p[0].revents == 0 && p[1].revents == POLLOUT);
        // The timeout.
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(poll(p, 1, 100) == 0);
        long ms = ms_since(&t0);
        CHECK(ms >= 100 && ms < 150);
        // Readable when a child writes; hung up once every writer is gone.
        snprintf(fdbuf, sizeof(fdbuf), "%d", fds[1]);
        pid_t c = start(self, NULL, NULL, "later", "100", fdbuf, NULL);
        CHECK(c > 0 && close(fds[1]) == 0);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        const int n = poll(p, 1, -1);
        if (n != 1 || !(p[0].revents & POLLIN)) printf("poll %d revents %#x\n", n, p[0].revents);
        CHECK(n == 1 && (p[0].revents & POLLIN)); // and POLLHUP if the child has exited already
        ms = ms_since(&t0);
        CHECK(ms >= 90 && ms < 1000);
        CHECK(read(fds[0], &b, 1) == 1 && b == 'x' && status_of(c) == 0);
        CHECK(poll(p, 1, 0) == 1 && p[0].revents == (POLLIN | POLLHUP));
        CHECK(read(fds[0], &b, 1) == 0 && close(fds[0]) == 0);
        // A pipe without a reader: POLLERR, unasked.
        CHECK(pipe(fds) == 0 && close(fds[0]) == 0);
        p[0].fd = fds[1];
        p[0].events = POLLOUT;
        CHECK(poll(p, 1, 0) == 1 && p[0].revents == (POLLOUT | POLLERR) && close(fds[1]) == 0);
        // A closed fd: POLLNVAL; a negative one skipped.
        p[0].fd = 9;
        p[1].fd = -1;
        CHECK(poll(p, 2, 0) == 1 && p[0].revents == POLLNVAL && p[1].revents == 0);
        // ppoll with a mask: SIGUSR1 blocked but for the wait, its handler run
        // under that mask, then the old mask back.
        CHECK(catch_signal(SIGUSR1, count_signal, SA_RESTART) == 0);
        sigset_t set, old, none, now;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigemptyset(&none);
        CHECK(sigprocmask(SIG_BLOCK, &set, &old) == 0 && pipe(fds) == 0);
        snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
        const int before = got[SIGUSR1];
        c = start(self, NULL, NULL, "killer", pidbuf, "100", NULL);
        p[0].fd = fds[0];
        p[0].events = POLLIN;
        CHECK(c > 0 && ppoll(p, 1, NULL, &none) == -1 && errno == EINTR && got[SIGUSR1] == before + 1);
        CHECK(sigprocmask(SIG_SETMASK, &old, &now) == 0 && sigismember(&now, SIGUSR1));
        CHECK(status_of(c) == 0);
        // poll isn't restarted after a handler, even with SA_RESTART.
        c = start(self, NULL, NULL, "killer", pidbuf, "100", NULL);
        CHECK(c > 0 && poll(p, 1, 2000) == -1 && errno == EINTR && got[SIGUSR1] == before + 2);
        CHECK(status_of(c) == 0);
        // select: the pipe and the terminal; the timeout; a closed fd.
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fds[0], &rd);
        struct timeval tv = {0, 50000};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(select(fds[0] + 1, &rd, NULL, NULL, &tv) == 0 && !FD_ISSET(fds[0], &rd));
        ms = ms_since(&t0);
        CHECK(ms >= 50 && ms < 100);
        CHECK(write(fds[1], "y", 1) == 1);
        fd_set wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(fds[0], &rd);
        FD_SET(1, &wr);
        tv.tv_usec = 0;
        CHECK(select(fds[0] + 1, &rd, &wr, NULL, &tv) == 2 && FD_ISSET(fds[0], &rd) && FD_ISSET(1, &wr));
        FD_ZERO(&rd);
        FD_SET(9, &rd);
        CHECK(select(10, &rd, NULL, NULL, &tv) == -1 && errno == EBADF);
        CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
        // O_NONBLOCK: an empty pipe's read fails with EAGAIN; the flag is the
        // descriptor's (not a dup's).
        CHECK(pipe(fds) == 0 && fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
        CHECK((fcntl(fds[0], F_GETFL) & O_NONBLOCK) && read(fds[0], &b, 1) == -1 && errno == EAGAIN);
        const int d = dup(fds[0]);
        CHECK(d >= 0 && !(fcntl(d, F_GETFL) & O_NONBLOCK) && close(d) == 0);
        CHECK(write(fds[1], "z", 1) == 1 && read(fds[0], &b, 1) == 1 && b == 'z');
        CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
        // termios: ECHO and ICRNL both ways; VMIN 0 with VTIME 1: a read waits
        // 0.1 s and returns 0; O_NONBLOCK on the terminal.
        if (isatty(0)) {
                struct termios before_t, t, now_t;
                CHECK(tcgetattr(0, &before_t) == 0);
                CHECK((before_t.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG));
                CHECK((before_t.c_iflag & ICRNL) && before_t.c_cc[VMIN] == 1 && before_t.c_cc[VTIME] == 0);
                t = before_t;
                t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
                t.c_iflag &= ~(tcflag_t)ICRNL;
                t.c_cc[VMIN] = 0;
                t.c_cc[VTIME] = 1;
                CHECK(tcsetattr(0, TCSANOW, &t) == 0 && tcgetattr(0, &now_t) == 0);
                CHECK(!(now_t.c_lflag & (ICANON | ECHO)) && (now_t.c_lflag & ISIG) && !(now_t.c_iflag & ICRNL));
                CHECK(now_t.c_cc[VMIN] == 0 && now_t.c_cc[VTIME] == 1);
                bmds_term_t bt = {.size = sizeof(bt)};
                CHECK(bmds_term_get(0, &bt) == 0);
                CHECK(!(bt.flags & (BMDS_TERM_LINE | BMDS_TERM_ECHO | BMDS_TERM_ICRNL)) &&
                      (bt.flags & BMDS_TERM_SIGNALS));
                clock_gettime(CLOCK_MONOTONIC, &t0);
                CHECK(read(0, &b, 1) == 0);
                ms = ms_since(&t0);
                CHECK(ms >= 100 && ms < 150);
                CHECK(fcntl(0, F_SETFL, O_NONBLOCK) == 0 && read(0, &b, 1) == -1 && errno == EAGAIN);
                CHECK(fcntl(0, F_SETFL, 0) == 0 && tcsetattr(0, TCSANOW, &before_t) == 0);
                CHECK(bmds_term_get(0, &bt) == 0 && (bt.flags & (BMDS_TERM_LINE | BMDS_TERM_ECHO | BMDS_TERM_ICRNL)) ==
                                                            (BMDS_TERM_LINE | BMDS_TERM_ECHO | BMDS_TERM_ICRNL));
        }
        printf("poll: all passed\n");
        return 0;
}

/* --- Threads (processes.md §3.10) ------------------------------------------------ */

static pthread_mutex_t   count_lock = PTHREAD_MUTEX_INITIALIZER;
static long              count_total;
static _Thread_local int tls_value = 7;

/* Counts to 10000 under the lock; its TLS and errno are its own. */
static void *counter(void *arg) {
        tls_value = (int)(intptr_t)arg;
        errno = 0;
        (void)close(-1); /* EBADF: this thread's errno */
        for (int i = 0; i < 10000; ++i) {
                pthread_mutex_lock(&count_lock);
                ++count_total;
                pthread_mutex_unlock(&count_lock);
        }
        const int ok = tls_value == (int)(intptr_t)arg && errno == EBADF;
        return (void *)(intptr_t)(ok ? gettid() : 0);
}

static pthread_mutex_t ping_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ping_cond = PTHREAD_COND_INITIALIZER;
static int             ping_turn; /* 0: main's, 1: the other's */
static int             ping_rounds;

static void *ponger(void *arg) {
        (void)arg;
        for (int i = 0; i < 1000; ++i) {
                pthread_mutex_lock(&ping_lock);
                while (ping_turn != 1) pthread_cond_wait(&ping_cond, &ping_lock);
                ping_turn = 0;
                ++ping_rounds;
                pthread_cond_signal(&ping_cond);
                pthread_mutex_unlock(&ping_lock);
        }
        return NULL;
}

static volatile pthread_t handled_on; /* the thread a handler ran on */

static void note_thread(int sig) {
        got[sig]++;
        handled_on = pthread_self();
        return;
}

/* Waits for signals with SIGUSR2 unblocked (the main thread blocks it). */
static void *signal_taker(void *arg) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        sem_post((sem_t *)arg);
        for (int i = 0; i < 50 && got[SIGUSR2] == 0; ++i) usleep(10000);
        return NULL;
}

static void *sleeper(void *arg) {
        usleep((useconds_t)(intptr_t)arg * 1000);
        return (void *)42;
}

static void *spawner(void *arg) {
        const pid_t c = start((const char *)arg, NULL, NULL, "status", "5", NULL);
        return (void *)(intptr_t)(c > 0 ? status_of(c) : -1);
}

static void *blocked_reader(void *arg) {
        char b;
        (void)read((int)(intptr_t)arg, &b, 1); /* a cancellation point */
        return NULL;
}

static int deep_stack(int n) {
        volatile char pad[256];
        pad[0] = (char)n;
        return n == 0 ? pad[0] : deep_stack(n - 1) + 1;
}

static void *stack_user(void *arg) {
        return (void *)(intptr_t)deep_stack((int)(intptr_t)arg);
}

static pthread_barrier_t barrier;

static void *at_barrier(void *arg) {
        (void)arg;
        return (void *)(intptr_t)pthread_barrier_wait(&barrier);
}

static size_t program_bytes(void) {
        bmds_mem_info_t m = {.size = sizeof(m)};
        return bmds_mem_info(0, &m) == 0 ? m.program_bytes : 0;
}

static int threads(const char *self) {
        // Counting under a mutex; ids, TLS and errno apart; join's values.
        pthread_t t[4];
        for (int i = 0; i < 4; ++i) CHECK(pthread_create(&t[i], NULL, counter, (void *)(intptr_t)(100 + i)) == 0);
        pid_t ids[4];
        for (int i = 0; i < 4; ++i) {
                void *r = NULL;
                CHECK(pthread_join(t[i], &r) == 0);
                ids[i] = (pid_t)(intptr_t)r;
                CHECK(ids[i] > 0 && ids[i] != getpid());
                for (int j = 0; j < i; ++j) CHECK(ids[j] != ids[i]);
        }
        CHECK(count_total == 40000 && tls_value == 7 && gettid() == getpid());
        // A condition variable ping-pong.
        pthread_t p;
        CHECK(pthread_create(&p, NULL, ponger, NULL) == 0);
        for (int i = 0; i < 1000; ++i) {
                pthread_mutex_lock(&ping_lock);
                ping_turn = 1;
                pthread_cond_signal(&ping_cond);
                while (ping_turn != 0) pthread_cond_wait(&ping_cond, &ping_lock);
                pthread_mutex_unlock(&ping_lock);
        }
        CHECK(pthread_join(p, NULL) == 0 && ping_rounds == 1000);
        // Detached threads: their memory comes back once they have ended.
        const size_t   before = program_bytes();
        pthread_attr_t attr;
        CHECK(pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
        for (int i = 0; i < 3; ++i) CHECK(pthread_create(&p, &attr, sleeper, (void *)20) == 0);
        usleep(200000);
        CHECK(program_bytes() == before);
        // Stack sizes: small (raised to the minimum) and large.
        void *depth = NULL;
        CHECK(pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, 4096) == 0);
        CHECK(pthread_create(&p, &attr, stack_user, (void *)10) == 0 && pthread_join(p, &depth) == 0);
        CHECK((intptr_t)depth == 10);
        CHECK(pthread_attr_setstacksize(&attr, 128 * 1024) == 0);
        CHECK(pthread_create(&p, &attr, stack_user, (void *)300) == 0 && pthread_join(p, &depth) == 0);
        CHECK((intptr_t)depth == 300);
        // A barrier: one of three is the serial thread.
        CHECK(pthread_barrier_init(&barrier, NULL, 3) == 0);
        pthread_t b[2];
        for (int i = 0; i < 2; ++i) CHECK(pthread_create(&b[i], NULL, at_barrier, NULL) == 0);
        int serial = pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD;
        for (int i = 0; i < 2; ++i) {
                void *r = NULL;
                CHECK(pthread_join(b[i], &r) == 0);
                serial += (intptr_t)r == PTHREAD_BARRIER_SERIAL_THREAD;
        }
        CHECK(serial == 1 && pthread_barrier_destroy(&barrier) == 0);
        // pthread_kill: the handler runs on that thread.
        CHECK(catch_signal(SIGUSR1, note_thread, 0) == 0);
        CHECK(pthread_create(&p, NULL, sleeper, (void *)300) == 0);
        usleep(50000);
        const int before_usr1 = got[SIGUSR1];
        CHECK(pthread_kill(p, SIGUSR1) == 0);
        void *r = NULL;
        CHECK(pthread_join(p, &r) == 0 && (intptr_t)r == 42); /* its sleep ended early */
        CHECK(got[SIGUSR1] == before_usr1 + 1 && pthread_equal(handled_on, p));
        // A signal to the process goes to the thread that doesn't block it.
        CHECK(catch_signal(SIGUSR2, note_thread, 0) == 0);
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        CHECK(pthread_sigmask(SIG_BLOCK, &set, &old) == 0);
        sem_t ready;
        CHECK(sem_init(&ready, 0, 0) == 0);
        CHECK(pthread_create(&p, NULL, signal_taker, &ready) == 0 && sem_wait(&ready) == 0);
        CHECK(kill(getpid(), SIGUSR2) == 0 && pthread_join(p, NULL) == 0);
        CHECK(got[SIGUSR2] == 1 && pthread_equal(handled_on, p));
        CHECK(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0 && got[SIGUSR2] == 1);
        // posix_spawn from a thread.
        CHECK(pthread_create(&p, NULL, spawner, (void *)self) == 0 && pthread_join(p, &r) == 0);
        CHECK((intptr_t)r == 5);
        // A thread blocked in a read is cancelled.
        int fds[2];
        CHECK(pipe(fds) == 0);
        CHECK(pthread_create(&p, NULL, blocked_reader, (void *)(intptr_t)fds[0]) == 0);
        usleep(50000);
        CHECK(pthread_cancel(p) == 0 && pthread_join(p, &r) == 0 && r == PTHREAD_CANCELED);
        CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
        // Processes with threads: exit() from a thread; the first thread ending first; exec
        // from a thread; killed while their threads compute, sleep and wait.
        pid_t c = start(self, NULL, NULL, "threadexit", NULL);
        CHECK(c > 0 && status_of(c) == 9);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        c = start(self, NULL, NULL, "mainexit", NULL);
        CHECK(c > 0 && status_of(c) == 0 && ms_since(&t0) >= 200);
        c = start(self, NULL, NULL, "threadexec", self, NULL);
        CHECK(c > 0 && status_of(c) == 42);
        c = start(self, NULL, NULL, "threadspin", "0", NULL);
        CHECK(c > 0 && usleep(200000) == 0 && kill(c, SIGTERM) == 0 && ending_of(c) == 1000 + SIGTERM);
        c = start(self, NULL, NULL, "threadspin", "0", NULL);
        CHECK(c > 0 && usleep(200000) == 0 && kill(c, SIGKILL) == 0 && ending_of(c) == 1000 + SIGKILL);
        printf("threads: all passed\n");
        return 0;
}

/* Threads that compute, sleep and wait on a lock and a condition variable, for
 * Ctrl-C, Ctrl-Z, kill and ps -T; with N > 0, ends after N seconds. */
static void *spinning(void *arg) {
        (void)arg;
        for (volatile uint32_t n = 0;; ++n) {}
        return NULL;
}

static void *napping(void *arg) {
        (void)arg;
        for (;;) usleep(100000);
        return NULL;
}

static void *waiting(void *arg) {
        (void)arg;
        pthread_mutex_lock(&ping_lock);
        for (;;) pthread_cond_wait(&ping_cond, &ping_lock);
        return NULL;
}

static int threadspin(int seconds) {
        pthread_t t;
        if (pthread_create(&t, NULL, spinning, NULL) != 0 || pthread_create(&t, NULL, napping, NULL) != 0 ||
            pthread_create(&t, NULL, waiting, NULL) != 0)
                return 1;
        printf("threadspin: 4 threads\n");
        fflush(stdout);
        if (seconds > 0) {
                sleep((unsigned)seconds);
                return 0;
        }
        for (;;) pause();
}

static void *exit_nine(void *arg) {
        (void)arg;
        usleep(50000);
        exit(9);
}

static void *exec_self(void *arg) {
        char *eargv[] = {"progtest", "status", "42", NULL};
        usleep(50000);
        execve((const char *)arg, eargv, environ);
        return NULL;
}

static int64_t now_ms(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int audio_state(int fd) {
        bmds_audio_status_t st = {.size = sizeof(st)};
        return bmds_audio_status(fd, &st) == 0 ? (int)st.state : -1;
}

/* Writes @p frames frames of 16-bit stereo silence. */
static int write_silence(int fd, long frames) {
        static int16_t zeros[2 * 1024];
        while (frames > 0) {
                const long n = frames < 1024 ? frames : 1024;
                if (write(fd, zeros, (size_t)n * 4) != n * 4) return -1;
                frames -= n;
        }
        return 0;
}

/* /dev/dsp (devplan/audio.md §18): silence only, so nothing is heard. */
static int audio(void) {
        CHECK(bmds_sys_has(BMDS_SYS_AUDIO_CONFIG) && bmds_sys_has(BMDS_SYS_SOUND_PLAY));
        CHECK(open("/dev/dsp", O_RDONLY) < 0 && errno == ENOTSUP); /* no capture yet */
        const int fd = open("/dev/dsp", O_WRONLY);
        CHECK(fd >= 0);

        /* Opened at OSS's defaults. */
        bmds_audio_config_t c = {.size = sizeof(c)};
        CHECK(bmds_audio_config(fd, NULL, &c) == 0);
        CHECK(c.format == BMDS_AUDIO_U8 && c.channels == 1 && c.rate == 8000 && c.ring_frames == 1024);
        bmds_audio_status_t st = {.size = sizeof(st)};
        CHECK(bmds_audio_status(fd, &st) == 0);
        CHECK(st.state == BMDS_AUDIO_IDLE && st.formats == 0x3ff && st.max_channels == 2);
        CHECK(st.min_rate == 4000 && st.max_rate == 192000 && st.device_rate > 0 && st.period_frames > 0);

        /* Configured, and what was chosen read back. */
        bmds_audio_config_t want = {
                .size = sizeof(want), .format = BMDS_AUDIO_S16LE, .channels = 2, .rate = 48000, .ring_frames = 4800};
        CHECK(bmds_audio_config(fd, &want, &c) == 0);
        CHECK(c.format == BMDS_AUDIO_S16LE && c.channels == 2 && c.rate == 48000);
        CHECK(c.ring_frames == 8192 && c.start_frames == 4096 && c.wake_frames > 0);
        bmds_audio_config_t bad = want;
        bad.rate = 1000;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.channels = 3;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.flags = 1;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.format = 10;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.ring_frames = 48000 * 11;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad.size = 2;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        CHECK(bmds_audio_config(fd, NULL, &c) == 0 && c.rate == 48000); /* unchanged by the failures */

        /* Other fds. */
        CHECK(bmds_audio_status(1, &st) == -BMDS_ENOTTY);
        CHECK(bmds_audio_status(99, &st) == -BMDS_EBADF);
        CHECK(bmds_audio_control(1, BMDS_AUDIO_DROP, 0) == -BMDS_ENOTTY);
        CHECK(bmds_audio_control(fd, 99, 0) == -BMDS_EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_DRAIN, 1) == -BMDS_EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_GAIN, 1) == -BMDS_EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_GAIN, -9601) == -BMDS_EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_GAIN, -600) == 0);
        CHECK(bmds_audio_status(fd, &st) == 0 && st.gain_cdb == -600);
        CHECK(bmds_sound_play(4) == -BMDS_EINVAL);

        /* Below the start threshold it waits; START plays it; DRAIN plays it out. */
        CHECK(write_silence(fd, 1000) == 0);
        CHECK(bmds_audio_status(fd, &st) == 0);
        CHECK(st.state == BMDS_AUDIO_WAITING && st.queued_frames == 1000 && st.written_frames == 1000);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_START, 0) == 0);
        CHECK(audio_state(fd) == BMDS_AUDIO_PLAYING);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_DRAIN, 0) == 0);
        CHECK(bmds_audio_status(fd, &st) == 0);
        CHECK(st.state == BMDS_AUDIO_IDLE && st.played_frames == 1000 && st.timestamp_ns > 0);

        /* Paused, the ring keeps what it has; dropped, it is empty. */
        CHECK(write_silence(fd, 2000) == 0);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_PAUSE, 0) == 0);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_START, 0) == 0);
        usleep(50000);
        CHECK(bmds_audio_status(fd, &st) == 0);
        CHECK(st.state == BMDS_AUDIO_PAUSED && st.queued_frames == 2000 && st.played_frames == 1000);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_RESUME, 0) == 0);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_DROP, 0) == 0);
        CHECK(bmds_audio_status(fd, &st) == 0 && st.state == BMDS_AUDIO_IDLE && st.queued_frames == 0);

        /* Non-blocking: a write takes what fits, then EAGAIN; poll agrees. */
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_PAUSE, 0) == 0);
        CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
        static int16_t big[2 * 10000];
        CHECK(write(fd, big, sizeof(big)) == 8192 * 4);
        CHECK(write(fd, big, 4) < 0 && errno == EAGAIN);
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        CHECK(poll(&p, 1, 0) == 0);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_DROP, 0) == 0);
        CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
        CHECK(fcntl(fd, F_SETFL, 0) == 0);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_RESUME, 0) == 0);

        /* Six programs' streams a device; the seventh is busy. */
        int more[6];
        int opened = 0;
        for (; opened < 6; ++opened) {
                more[opened] = open("/dev/dsp0", O_WRONLY);
                if (more[opened] < 0) break;
        }
        CHECK(opened == 5 && errno == EBUSY);
        for (int i = 0; i < opened; ++i) CHECK(close(more[i]) == 0);

        /* OSS's ioctls on the same calls. */
        int v = AFMT_S16_LE;
        CHECK(ioctl(fd, SNDCTL_DSP_SETFMT, &v) == 0 && v == AFMT_S16_LE);
        v = 2;
        CHECK(ioctl(fd, SNDCTL_DSP_CHANNELS, &v) == 0 && v == 2);
        v = 44100;
        CHECK(ioctl(fd, SNDCTL_DSP_SPEED, &v) == 0 && v == 44100);
        v = 0x0004000c;
        CHECK(ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &v) == 0);
        CHECK(ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &v) == 0 && v == 4096);
        audio_buf_info info;
        CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0);
        CHECK(info.fragsize == 4096 && info.fragstotal == 4 && info.fragments == 4 && info.bytes == 16384);
        CHECK(write_silence(fd, 1024) == 0);
        CHECK(ioctl(fd, SNDCTL_DSP_GETODELAY, &v) == 0 && v == 4096); /* not started: only what is queued */
        CHECK(ioctl(fd, SNDCTL_DSP_SYNC) == 0);
        CHECK(ioctl(1, SNDCTL_DSP_SYNC) < 0 && errno == ENOTTY);
        const int mixer = open("/dev/mixer", O_RDONLY);
        CHECK(mixer >= 0);
        CHECK(ioctl(mixer, SOUND_MIXER_READ_VOLUME, &v) == 0 && (v & 0xff) <= 100 && (v & 0xff) == (v >> 8));
        CHECK(ioctl(mixer, SOUND_MIXER_READ_DEVMASK, &v) == 0 && v == (SOUND_MASK_VOLUME | SOUND_MASK_PCM));
        CHECK(read(mixer, &v, 1) < 0);
        CHECK(close(mixer) == 0);

        /* Closing waits until what is queued has played: the ring's 4096 frames, 93 ms at 44.1 kHz. */
        CHECK(write_silence(fd, 8820) == 0);
        const int64_t before = now_ms();
        CHECK(close(fd) == 0);
        const int64_t took = now_ms() - before;
        printf("audio: the close took %d ms\n", (int)took);
        CHECK(took >= 50);
        printf("audio: ok\n");
        return 0;
}

/* The decoder calls (slots 71-73) on @p path, tests/host/audio/tone.flac: a second of 44.1 kHz stereo. */
static int decode(const char *path) {
        CHECK(bmds_sys_has(BMDS_SYS_AUDIO_DECODE_TAG));
        const int file = open(path, O_RDONLY);
        CHECK(file >= 0);
        bmds_decode_config_t c = {.size = sizeof(c), .format = BMDS_AUDIO_S16LE};
        /* What is refused. */
        CHECK(bmds_audio_decode(file, NULL) == -BMDS_EFAULT);
        bmds_decode_config_t bad = c;
        bad.size = 0;
        CHECK(bmds_audio_decode(file, &bad) == -BMDS_EINVAL);
        bad = c;
        bad.format = BMDS_AUDIO_U8;
        CHECK(bmds_audio_decode(file, &bad) == -BMDS_EINVAL);
        bad = c;
        bad.flags = 1;
        CHECK(bmds_audio_decode(file, &bad) == -BMDS_EINVAL);
        CHECK(bmds_audio_decode(99, &c) == -BMDS_EBADF);
        const int dsp = open("/dev/dsp", O_WRONLY);
        CHECK(dsp >= 0 && bmds_audio_decode(dsp, &c) == -BMDS_EBADF);
        CHECK(close(dsp) == 0);
        bad = c;
        bad.length = 5;
        bad.data = "hello";
        CHECK(bmds_audio_decode(-1, &bad) == -BMDS_EINVAL);
        bad.data = NULL;
        CHECK(bmds_audio_decode(-1, &bad) == -BMDS_EFAULT);

        /* The file: what it is, its tags. */
        c.flags = BMDS_O_CLOEXEC;
        const int pcm = bmds_audio_decode(file, &c);
        CHECK(pcm > file);
        CHECK(fcntl(pcm, F_GETFD) == FD_CLOEXEC);
        CHECK(close(file) == 0); /* the decoded fd keeps it */
        bmds_decode_info_t info = {.size = sizeof(info)};
        CHECK(bmds_audio_decode_info(pcm, &info) == 0);
        CHECK(info.codec == BMDS_CODEC_FLAC && strcmp(info.name, "flac") == 0);
        CHECK(info.format == BMDS_AUDIO_S16LE && info.native_format == BMDS_AUDIO_S16LE && info.frame_bytes == 4);
        CHECK(info.channels == 2 && info.rate == 44100 && info.frames == 44100 && info.flags == 0);
        CHECK(info.source_bits == 16 && info.bitrate > 0 && info.tags >= 4);
        CHECK(bmds_audio_decode_info(1, &info) == -BMDS_ENOTTY);
        char key[16], value[16], small[3];
        int  title = 0;
        for (uint32_t i = 0; i < info.tags; ++i) {
                const int32_t n = bmds_audio_decode_tag(pcm, i, key, sizeof(key), value, sizeof(value));
                CHECK(n >= 0);
                if (strcmp(key, "TITLE") == 0) {
                        CHECK(n == 4 && strcmp(value, "Tone") == 0);
                        CHECK(bmds_audio_decode_tag(pcm, i, NULL, 0, small, sizeof(small)) == 4 &&
                              strcmp(small, "To") == 0);
                        title = 1;
                }
        }
        CHECK(title);
        CHECK(bmds_audio_decode_tag(pcm, info.tags, key, sizeof(key), value, sizeof(value)) == -BMDS_ENOENT);
        CHECK(bmds_audio_decode_tag(1, 0, key, sizeof(key), value, sizeof(value)) == -BMDS_ENOTTY);

        /* Reads, fstat, seeks; read-only. */
        struct stat st;
        CHECK(fstat(pcm, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 176400);
        static unsigned char whole[176400], part[4096];
        size_t               got = 0;
        for (;;) {
                const ssize_t n = read(pcm, whole + got, sizeof(whole) - got < 5000 ? sizeof(whole) - got : 5000);
                CHECK(n >= 0);
                if (n == 0) break;
                got += (size_t)n;
        }
        CHECK(got == sizeof(whole));
        CHECK(lseek(pcm, 1001, SEEK_SET) == 1001 && read(pcm, part, 3) == 3 && memcmp(part, whole + 1001, 3) == 0);
        CHECK(read(pcm, part, 4096) == 4096 && memcmp(part, whole + 1004, 4096) == 0);
        CHECK(lseek(pcm, 0, SEEK_END) == 176400 && read(pcm, part, 1) == 0);
        CHECK(write(pcm, part, 1) < 0 && errno == EBADF);

        /* A buffer, copied by the call. */
        unsigned char *copy = malloc(65536);
        const int      again = open(path, O_RDONLY);
        const ssize_t  size = read(again, copy, 65536);
        CHECK(size > 0 && size < 65536 && close(again) == 0);
        bmds_decode_config_t mem = {
                .size = sizeof(mem), .format = BMDS_DECODE_NATIVE, .length = (uint32_t)size, .data = copy};
        const int pcm2 = bmds_audio_decode(-1, &mem);
        CHECK(pcm2 >= 0);
        memset(copy, 0, (size_t)size);
        free(copy);
        CHECK(read(pcm2, part, 4096) == 4096 && memcmp(part, whole, 4096) == 0);
        CHECK(close(pcm2) == 0 && close(pcm) == 0);
        printf("decode: ok\n");
        return 0;
}

static int64_t mono_ns(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* Real-time clients (devplan/audio.md §20): flags and errors, the cycle's fields, a missed
   deadline counted and skipped over, a stalled thread throttled, exclusive mode. */
static int rt(void) {
        CHECK(bmds_sys_has(BMDS_SYS_AUDIO_CYCLE));
        const int fd = open("/dev/dsp", O_WRONLY);
        CHECK(fd >= 0);
        bmds_audio_cycle_t cy = {.size = sizeof(cy)};
        CHECK(bmds_audio_cycle(fd, &cy) == -BMDS_EINVAL); /* not real-time yet */
        CHECK(bmds_audio_cycle(1, &cy) == -BMDS_ENOTTY);
        CHECK(bmds_audio_cycle(99, &cy) == -BMDS_EBADF);
        bmds_audio_status_t st = {.size = sizeof(st)};
        CHECK(bmds_audio_status(fd, &st) == 0);
        const uint32_t      device_rate = st.device_rate;
        bmds_audio_config_t want = {.size = sizeof(want),
                                    .format = BMDS_AUDIO_F32LE,
                                    .channels = 1,
                                    .wake_frames = 64,
                                    .flags = BMDS_AUDIO_REALTIME};
        bmds_audio_config_t bad = want;
        bad.format = BMDS_AUDIO_S16LE;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.channels = 3;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.ring_frames = 1024;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.wake_frames = BMDS_AUDIO_MIN_PERIOD - 1;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad.wake_frames = BMDS_AUDIO_MAX_PERIOD + 1;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.flags = BMDS_AUDIO_EXCLUSIVE;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad.flags = 0x4;
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);
        bad = want;
        bad.rate = device_rate == 44100 ? 48000 : 44100; /* shared: the device's rate only */
        CHECK(bmds_audio_config(fd, &bad, NULL) == -BMDS_EINVAL);

        /* Real-time: the device runs at the period asked for. */
        bmds_audio_config_t got = {.size = sizeof(got)};
        CHECK(bmds_audio_config(fd, &want, &got) == 0);
        CHECK(got.format == BMDS_AUDIO_F32LE && got.channels == 1 && got.rate == device_rate);
        CHECK(got.wake_frames == 64 && got.flags == BMDS_AUDIO_REALTIME && got.ring_frames == 0);
        CHECK(bmds_audio_status(fd, &st) == 0);
        CHECK(st.state == BMDS_AUDIO_PLAYING && st.period_frames == 64);
        CHECK(write(fd, "x", 1) < 0 && errno == EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_DRAIN, 0) == -BMDS_EINVAL);
        CHECK(bmds_audio_control(fd, BMDS_AUDIO_GAIN, -600) == 0);

        /* 500 periods, each made in time: consecutive, with a deadline and a play time ahead. */
        uint64_t next = 0;
        int64_t  worst = 0;
        for (int i = 0; i < 500; ++i) {
                CHECK(bmds_audio_cycle(fd, &cy) == 0);
                const int64_t now = mono_ns();
                CHECK(cy.frames == 64 && cy.channels == 1 && cy.rate == device_rate && cy.out != NULL);
                CHECK(cy.in == NULL && cy.flags == 0);
                CHECK(i == 0 || cy.position == next);
                CHECK(cy.deadline_ns > now - 1000000 && cy.play_ns > cy.deadline_ns);
                if (cy.play_ns - now > worst) worst = cy.play_ns - now;
                next = cy.position + cy.frames;
                for (uint32_t f = 0; f < cy.frames; ++f) cy.out[f] = 0.0f;
        }
        CHECK(cy.late == 0 && cy.skipped == 0 && cy.throttled == 0);
        const int64_t period_ns = 64LL * 1000000000 / device_rate;
        CHECK(worst <= 2 * period_ns + 200000); /* two periods, give or take the wake */

        /* Out of the cycle for 20 ms: late, skipped over, and throttled meanwhile. */
        usleep(20000);
        CHECK(bmds_audio_cycle(fd, &cy) == 0);
        printf("rt: after the stall: late %u, skipped %u, throttled %u\n", (unsigned)cy.late, (unsigned)cy.skipped,
               (unsigned)cy.throttled);
        CHECK(cy.late >= 1 && cy.skipped >= 1 && cy.throttled >= 1);
        CHECK(cy.position > next);
        CHECK(bmds_audio_status(fd, &st) == 0 && st.late_periods == cy.late);
        const uint32_t late = cy.late;

        /* Exclusive mode isn't free while this fd is open. */
        const int other = open("/dev/dsp", O_WRONLY);
        CHECK(other >= 0);
        bmds_audio_config_t ex = {.size = sizeof(ex),
                                  .format = BMDS_AUDIO_F32LE,
                                  .channels = 1,
                                  .rate = 44100,
                                  .flags = BMDS_AUDIO_REALTIME | BMDS_AUDIO_EXCLUSIVE};
        CHECK(bmds_audio_config(other, &ex, NULL) == -BMDS_EBUSY);
        CHECK(close(other) == 0);
        CHECK(bmds_audio_cycle(fd, NULL) == 0); /* the period ended, no wait */
        CHECK(close(fd) == 0);

        /* Exclusive at 44.1 kHz: nothing else may open the device; sounds are dropped. */
        const int exfd = open("/dev/dsp", O_WRONLY);
        CHECK(exfd >= 0);
        CHECK(bmds_audio_config(exfd, &ex, &got) == 0);
        CHECK(got.rate == 44100 && got.flags == (BMDS_AUDIO_REALTIME | BMDS_AUDIO_EXCLUSIVE));
        CHECK(open("/dev/dsp", O_WRONLY) < 0 && errno == EBUSY);
        CHECK(bmds_sound_play(BMDS_SOUND_BELL) == 0);
        for (int i = 0; i < 100; ++i) {
                CHECK(bmds_audio_cycle(exfd, &cy) == 0);
                CHECK(cy.rate == 44100);
                for (uint32_t f = 0; f < cy.frames; ++f) cy.out[f] = 0.0f;
        }
        CHECK(cy.late == 0);
        CHECK(close(exfd) == 0);

        /* Shared again: a plain stream plays at the setting's rate. */
        const int fd2 = open("/dev/dsp", O_WRONLY);
        CHECK(fd2 >= 0);
        bmds_audio_config_t pcm = {.size = sizeof(pcm), .format = BMDS_AUDIO_S16LE, .channels = 2, .rate = 48000};
        CHECK(bmds_audio_config(fd2, &pcm, NULL) == 0);
        CHECK(write_silence(fd2, 4800) == 0);
        CHECK(bmds_audio_control(fd2, BMDS_AUDIO_DRAIN, 0) == 0);
        CHECK(bmds_audio_status(fd2, &st) == 0 && st.device_rate == device_rate && st.period_frames != 64);
        CHECK(close(fd2) == 0);
        printf("rt: ok, 500 periods of 64 frames on time, then %u late after a 20 ms stall\n", (unsigned)late);
        return 0;
}

/* MIDI (devplan/audio.md §21): virtual ports, bytes and events both ways, connections, a full
   ring counted, a port that goes away. No device needed. */
static int midi_port_open(uint32_t port, int flags) {
        char path[24];
        snprintf(path, sizeof(path), "/dev/midi%u", (unsigned)port);
        return open(path, flags);
}

static int midi(void) {
        CHECK(bmds_sys_has(BMDS_SYS_MIDI_CONTROL));
        signal(SIGPIPE, SIG_IGN); /* a write to a port that went: EPIPE, not the signal */
        const int v = open("/dev/midinew", O_RDWR);
        CHECK(v >= 0);
        bmds_midi_status_t st = {.size = sizeof(st)};
        CHECK(bmds_midi_status(v, &st) == 0 && st.port == 0xff);
        CHECK(bmds_midi_status(1, &st) == -BMDS_ENOTTY);
        CHECK(bmds_midi_status(99, &st) == -BMDS_EBADF);
        uint8_t buf[64];
        CHECK(read(v, buf, sizeof(buf)) == -1 && errno == EINVAL); /* no port yet */
        CHECK(write(v, buf, 1) == -1 && errno == EINVAL);
        bmds_midi_config_t c = {.size = sizeof(c)};
        bmds_midi_config_t got = {.size = sizeof(got)};
        c.flags = 0x100;
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL);
        c.flags = BMDS_MIDI_EVENTS;
        c.event_size = 8;
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL);
        c.flags = BMDS_MIDI_SOURCE;
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL);
        c.flags = BMDS_MIDI_CREATE;
        strcpy(c.name, "progtest");
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL); /* no side */
        c.flags = BMDS_MIDI_CREATE | BMDS_MIDI_SOURCE | BMDS_MIDI_DESTINATION;
        c.name[0] = '\0';
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL); /* no name */
        strcpy(c.name, "progtest");
        CHECK(bmds_midi_config(v, &c, &got) == 0);
        const uint32_t port = got.port;
        CHECK(port < 16 && strcmp(got.name, "progtest") == 0);
        CHECK(got.flags == (BMDS_MIDI_CREATE | BMDS_MIDI_SOURCE | BMDS_MIDI_DESTINATION) && got.event_size == 0);
        CHECK(bmds_midi_config(v, &c, NULL) == -BMDS_EINVAL); /* once */
        CHECK(bmds_midi_config(v, NULL, &got) == 0 && got.port == port);

        /* Its file exists while it does: r reads its source (what v writes), w writes to its
           destination (what v reads). */
        char path[24];
        snprintf(path, sizeof(path), "/dev/midi%u", (unsigned)port);
        struct stat sb;
        CHECK(stat(path, &sb) == 0 && S_ISCHR(sb.st_mode)); /* a device: nothing reads it to look */
        const int r = midi_port_open(port, O_RDONLY);
        const int w = midi_port_open(port, O_WRONLY);
        CHECK(r >= 0 && w >= 0);

        /* Bytes: running status written, whole messages read. */
        const uint8_t notes[] = {0x90, 0x3c, 0x64, 0x3e, 0x00};
        CHECK(write(v, notes, sizeof(notes)) == (ssize_t)sizeof(notes));
        const uint8_t want[] = {0x90, 0x3c, 0x64, 0x90, 0x3e, 0x00};
        CHECK(read(r, buf, sizeof(buf)) == (ssize_t)sizeof(want) && memcmp(buf, want, sizeof(want)) == 0);
        const uint8_t volume[] = {0xb0, 0x07, 0x7f};
        CHECK(write(w, volume, sizeof(volume)) == 3);
        CHECK(read(v, buf, 2) == 2 && read(v, buf + 2, sizeof(buf)) == 1 && memcmp(buf, volume, 3) == 0);

        /* poll: nothing waiting, then an event. */
        struct pollfd pf = {.fd = r, .events = POLLIN};
        CHECK(poll(&pf, 1, 0) == 0);
        const uint8_t program[] = {0xc0, 0x05};
        int64_t       t0 = mono_ns();
        CHECK(write(v, program, 2) == 2);
        int64_t t1 = mono_ns();
        CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN) != 0);

        /* Events: the record's port, length and time. */
        c.flags = BMDS_MIDI_EVENTS;
        c.event_size = sizeof(bmds_midi_event_t);
        CHECK(bmds_midi_config(r, &c, &got) == 0 && got.event_size == sizeof(bmds_midi_event_t));
        bmds_midi_event_t ev[4];
        CHECK(read(r, ev, 8) == -1 && errno == EINVAL); /* smaller than a record */
        CHECK(read(r, ev, sizeof(ev)) == (ssize_t)sizeof(ev[0]));
        CHECK(ev[0].port == port && ev[0].length == 2 && ev[0].data[0] == 0xc0 && ev[0].data[1] == 0x05);
        CHECK(ev[0].time_ns >= t0 && ev[0].time_ns <= t1);
        /* A bigger record (a later SDK's): the rest zeroed. */
        c.event_size = 24;
        CHECK(bmds_midi_config(r, &c, NULL) == 0);
        uint8_t big[48];
        memset(big, 0xaa, sizeof(big));
        CHECK(write(v, program, 2) == 2);
        CHECK(read(r, big, sizeof(big)) == 24 && big[9] == 2 && big[16] == 0 && big[23] == 0 && big[24] == 0xaa);
        c.event_size = sizeof(bmds_midi_event_t);
        CHECK(bmds_midi_config(r, &c, NULL) == 0);

        /* Events written: time 0 and 1 to 5 bytes, a whole number of records. */
        CHECK(bmds_midi_config(v, &c, NULL) == 0);
        bmds_midi_event_t out = {.length = 3, .data = {0x80, 0x3c, 0x00}};
        CHECK(write(v, &out, sizeof(out)) == (ssize_t)sizeof(out));
        CHECK(read(r, ev, sizeof(ev)) == (ssize_t)sizeof(ev[0]) && ev[0].data[0] == 0x80 && ev[0].length == 3);
        out.time_ns = 5; /* a time passed: sent now */
        CHECK(write(v, &out, sizeof(out)) == (ssize_t)sizeof(out));
        CHECK(read(r, ev, sizeof(ev)) == (ssize_t)sizeof(ev[0]) && ev[0].data[0] == 0x80);
        out.time_ns = mono_ns() + BMDS_MIDI_AHEAD_MAX_NS + 1000000000ll; /* too far ahead */
        CHECK(write(v, &out, sizeof(out)) == -1 && errno == EINVAL);
        out.time_ns = 0;
        out.length = 0;
        CHECK(write(v, &out, sizeof(out)) == -1 && errno == EINVAL);
        out.length = 3;
        CHECK(write(v, &out, 10) == -1 && errno == EINVAL);
        /* A SysEx in pieces. */
        bmds_midi_event_t sx[2] = {{.length = 5, .data = {0xf0, 0x7e, 0x01, 0x02, 0x03}},
                                   {.length = 2, .data = {0x04, 0xf7}}};
        CHECK(write(v, sx, sizeof(sx)) == (ssize_t)sizeof(sx));
        CHECK(read(r, ev, sizeof(ev)) == 2 * (ssize_t)sizeof(ev[0]));
        CHECK(ev[0].length == 5 && ev[0].data[0] == 0xf0 && ev[1].length == 2 && ev[1].data[1] == 0xf7);

        /* Scheduled output (devplan/audio.md §25): each sent at its time, carrying it, in order. */
        const int64_t     base = mono_ns() + 50000000; /* 50 ms on */
        bmds_midi_event_t s3[3] = {{.time_ns = base, .length = 3, .data = {0x90, 0x3c, 0x40}},
                                   {.time_ns = base + 20000000, .length = 3, .data = {0x90, 0x3e, 0x40}},
                                   {.time_ns = base + 40000000, .length = 3, .data = {0x80, 0x3c, 0x00}}};
        CHECK(write(v, s3, sizeof(s3)) == (ssize_t)sizeof(s3));
        CHECK(poll(&pf, 1, 0) == 0); /* nothing yet */
        int64_t worst = 0;
        for (int i = 0; i < 3; ++i) {
                CHECK(read(r, ev, sizeof(ev[0])) == (ssize_t)sizeof(ev[0])); /* waits for it */
                const int64_t at = mono_ns();
                CHECK(ev[0].time_ns == s3[i].time_ns && ev[0].data[1] == s3[i].data[1]);
                CHECK(at >= s3[i].time_ns && at - s3[i].time_ns < 5000000); /* within 5 ms */
                if (at - s3[i].time_ns > worst) worst = at - s3[i].time_ns;
        }
        /* Not before the last scheduled: refused. */
        bmds_midi_event_t later = {.time_ns = mono_ns() + 100000000, .length = 2, .data = {0xc0, 0x01}};
        CHECK(write(v, &later, sizeof(later)) == (ssize_t)sizeof(later));
        later.time_ns -= 50000000;
        CHECK(write(v, &later, sizeof(later)) == -1 && errno == EINVAL);
        /* Cancelled: never sent. */
        CHECK(bmds_midi_control(v, BMDS_MIDI_CANCEL, 1) == -BMDS_EINVAL);
        CHECK(bmds_midi_control(v, BMDS_MIDI_CANCEL, 0) == 0);
        usleep(150000);
        CHECK(poll(&pf, 1, 0) == 0);
        /* Full: about BMDS_MIDI_SCHEDULED_MAX held, then EAGAIN, and not writable until cancelled. */
        bmds_midi_event_t many = {.time_ns = mono_ns() + 10000000000ll, .length = 2, .data = {0xc0, 0x02}};
        int               taken = 0;
        while (write(v, &many, sizeof(many)) == (ssize_t)sizeof(many)) ++taken;
        CHECK(errno == EAGAIN && taken >= (int)BMDS_MIDI_SCHEDULED_MAX - 16 && taken <= (int)BMDS_MIDI_SCHEDULED_MAX);
        struct pollfd pw = {.fd = v, .events = POLLOUT};
        CHECK(poll(&pw, 1, 0) == 0);
        CHECK(bmds_midi_control(v, BMDS_MIDI_CANCEL, 0) == 0);
        CHECK(poll(&pw, 1, 0) == 1 && (pw.revents & POLLOUT) != 0);
        printf("midi: scheduled events sent at most %ld us late; %d held\n", (long)(worst / 1000), taken);

        /* A second port, destination only, connected from the first. */
        const int d = open("/dev/midinew", O_RDWR);
        CHECK(d >= 0);
        bmds_midi_config_t dc = {.size = sizeof(dc), .flags = BMDS_MIDI_CREATE | BMDS_MIDI_DESTINATION};
        strcpy(dc.name, "progtest sink");
        CHECK(bmds_midi_config(d, &dc, &got) == 0);
        const uint32_t sink = got.port;
        CHECK(midi_port_open(sink, O_RDONLY) == -1 && errno == ENOTSUP); /* no source to read */
        CHECK(write(d, buf, 1) == -1 && errno == EINVAL);                /* nor to write */
        CHECK(bmds_midi_control(v, BMDS_MIDI_CONNECT, (int32_t)sink) == 0);
        CHECK(bmds_midi_control(v, BMDS_MIDI_CONNECT, (int32_t)sink) == -BMDS_EEXIST);
        CHECK(bmds_midi_control(d, BMDS_MIDI_CONNECT, (int32_t)port) == -BMDS_EINVAL); /* no source */
        CHECK(bmds_midi_control(v, BMDS_MIDI_CONNECT, 16) == -BMDS_ENOENT);
        CHECK(bmds_midi_control(v, BMDS_MIDI_CONNECT, -1) == -BMDS_ENOENT);
        CHECK(bmds_midi_control(v, 99, 0) == -BMDS_EINVAL);
        CHECK(write(v, &out, sizeof(out)) == (ssize_t)sizeof(out));
        CHECK(read(d, buf, sizeof(buf)) == 3 && buf[0] == 0x80); /* d reads bytes */
        CHECK(read(r, ev, sizeof(ev)) == (ssize_t)sizeof(ev[0]));

        /* system (/dev/midi, devplan/audio.md §25): what any program writes, sent from its source to
           what it is connected to (wavetable too, by default: a note-on of velocity 0 is silent). */
        const int sys = open("/dev/midi", O_RDWR);
        CHECK(sys >= 0);
        CHECK(bmds_midi_status(sys, &st) == 0 && st.kind == BMDS_MIDI_SYSTEM && st.sides == BMDS_MIDI_SOURCE);
        CHECK(strcmp(st.name, "system") == 0);
        CHECK(bmds_midi_control(sys, BMDS_MIDI_CONNECT, (int32_t)sink) == 0);
        const uint8_t silent[3] = {0x91, 0x40, 0x00};
        CHECK(write(sys, silent, 3) == 3);
        CHECK(read(d, buf, sizeof(buf)) == 3 && buf[0] == 0x91);   /* to what it is connected to */
        CHECK(read(sys, buf, sizeof(buf)) == 3 && buf[0] == 0x91); /* and to its readers */
        CHECK(bmds_midi_control(sys, BMDS_MIDI_DISCONNECT, (int32_t)sink) == 0);
        CHECK(close(sys) == 0);

        /* A full ring: 300 events unread. Each reader's ring holds 256; the rest are counted. */
        for (int i = 0; i < 300; ++i) CHECK(write(v, &out, sizeof(out)) == (ssize_t)sizeof(out));
        CHECK(bmds_midi_status(r, &st) == 0 && st.fd_dropped == 44);
        CHECK(bmds_midi_status(d, &st) == 0 && st.fd_dropped == 44 && st.dropped == 44); /* its port's too */
        CHECK(st.events_out == 2 + 256 && st.sides == BMDS_MIDI_DESTINATION &&
              st.kind == BMDS_MIDI_VIRTUAL); /* + system's */
        CHECK(strcmp(st.name, "progtest sink") == 0);
        CHECK(bmds_midi_control(r, BMDS_MIDI_FLUSH, 0) == 0);
        CHECK(poll(&pf, 1, 0) == 0);
        CHECK(bmds_midi_status(v, &st) == 0 && st.port == port && st.flags == 0);
        CHECK(st.events_in >= 300 && st.last_ns > 0);
        CHECK(bmds_midi_control(v, BMDS_MIDI_DISCONNECT, (int32_t)sink) == 0);
        CHECK(bmds_midi_control(v, BMDS_MIDI_DISCONNECT, (int32_t)sink) == -BMDS_ENOENT);

        /* The port goes with v: the note it held (0x3e, scheduled above) let go first, then reads
           end, writes break, the file is gone. */
        CHECK(close(v) == 0);
        CHECK(read(r, ev, sizeof(ev)) == (ssize_t)sizeof(ev[0]) && ev[0].data[0] == 0x80 && ev[0].data[1] == 0x3e);
        CHECK(read(r, ev, sizeof(ev)) == 0);
        CHECK(bmds_midi_status(r, &st) == 0 && (st.flags & BMDS_MIDI_GONE) != 0);
        CHECK(write(w, volume, 3) == -1 && errno == EPIPE);
        CHECK(stat(path, &sb) == -1 && errno == ENOENT);
        CHECK(midi_port_open(port, O_RDONLY) == -1 && errno == ENOENT);
        CHECK(close(r) == 0 && close(w) == 0 && close(d) == 0);
        printf("midi: ok, ports %u and %u, 300 events with 44 dropped per reader\n", (unsigned)port, (unsigned)sink);
        return 0;
}

/* Text to speech (devplan/speech.md §4): the calls' errors; a sentence made into samples (about
   two seconds at 16 kHz, not silence); the info; text waiting for a NUL; markup read or obeyed; the
   speed; reads into odd sizes and addresses; writes taking what fits; two fds at once; bmds_say,
   aloud, unless ARG is "quiet". */
static int speak_open(uint32_t flags, const char *voice, uint32_t speed) {
        bmds_speak_config_t c = {.size = sizeof(c), .flags = flags, .voice = voice, .speed = speed};
        return bmds_audio_speak(&c);
}

/* Writes all of TEXT (LEN bytes) to speech fd FD, reading as it goes; the samples read. */
static long speak_all(int fd, const char *text, size_t len) {
        static int16_t out[4096];
        long           made = 0;
        size_t         at = 0;
        for (;;) {
                if (at < len) {
                        const ssize_t n = write(fd, text + at, len - at);
                        if (n < 0 && errno != EAGAIN) return -1;
                        if (n > 0) at += (size_t)n;
                }
                const ssize_t r = read(fd, out, sizeof(out));
                if (r < 0) return -1;
                made += r / 2;
                if (r == 0 && at == len) return made;
        }
}

static int speech(const char *arg) {
        CHECK(bmds_sys_has(BMDS_SYS_AUDIO_SPEAK_INFO));
        bmds_speak_config_t bad = {.size = sizeof(bad), .flags = 0x100};
        CHECK(bmds_audio_speak(&bad) == -BMDS_EINVAL);
        CHECK(speak_open(0, NULL, 10) == -BMDS_EINVAL);
        CHECK(speak_open(0, NULL, 501) == -BMDS_EINVAL);
        bad.flags = 0;
        bad.pitch = 201;
        CHECK(bmds_audio_speak(&bad) == -BMDS_EINVAL);
        bad.pitch = 0;
        bad.volume = 501;
        CHECK(bmds_audio_speak(&bad) == -BMDS_EINVAL);
        CHECK(speak_open(0, "xx-XX", 0) == -BMDS_ENOENT);
        CHECK(speak_open(0, "../en-US", 0) == -BMDS_EINVAL);
        CHECK(speak_open(0, "", 0) == -BMDS_EINVAL);
        CHECK(speak_open(0, "a-name-far-longer-than-any-voice-has", 0) == -BMDS_EINVAL);

        int fd = speak_open(BMDS_O_CLOEXEC, NULL, 0);
        CHECK(fd >= 0);
        bmds_speak_info_t info = {.size = sizeof(info)};
        CHECK(bmds_audio_speak_info(fd, &info) == 0);
        CHECK(info.rate == 16000 && info.channels == 1 && info.format == BMDS_AUDIO_S16LE && info.frames == 0);
        CHECK(info.speed == 100 && info.pitch == 100 && info.volume == 100 && info.flags == 0);
        CHECK(strcmp(info.voice, "en-US") == 0);
        CHECK(bmds_audio_speak_info(1, &info) == -BMDS_ENOTTY);

        /* A sentence. */
        static const char hello[] = "Hello, this is BMDS speaking."; /* its NUL written too */
        static int16_t    first[1024];
        CHECK(write(fd, hello, sizeof(hello)) == (ssize_t)sizeof(hello));
        CHECK(read(fd, first, sizeof(first)) == (ssize_t)sizeof(first));
        int  peak = 0;
        long made = 1024;
        for (;;) {
                const ssize_t r = read(fd, first, sizeof(first));
                CHECK(r >= 0 && r % 2 == 0);
                if (r == 0) break;
                for (ssize_t i = 0; i < r / 2; ++i) peak = abs(first[i]) > peak ? abs(first[i]) : peak;
                made += r / 2;
        }
        CHECK(made > 16000 && made < 64000);
        CHECK(peak > 1000);
        CHECK(bmds_audio_speak_info(fd, &info) == 0 && info.frames == (uint64_t)made);
        /* Said once. */
        CHECK(read(fd, first, sizeof(first)) == 0);

        /* The last text waits for a NUL; then it is said. */
        CHECK(write(fd, "Hello there", 11) == 11);
        CHECK(read(fd, first, sizeof(first)) == 0);
        CHECK(speak_all(fd, "", 1) > 4000);

        /* Odd reads: a byte is too few; three give a sample; an odd address works. */
        CHECK(write(fd, hello, sizeof(hello)) == (ssize_t)sizeof(hello));
        CHECK(read(fd, first, 1) == -1 && errno == EINVAL);
        CHECK(read(fd, first, 3) == 2);
        CHECK(read(fd, (char *)first + 1, 301) == 300);
        CHECK(speak_all(fd, "", 0) > 10000);
        CHECK(close(fd) == 0);

        /* Markup: read as text unless asked for; the speed as the tag's. */
        static const char tagged[] = "<speed level='300'>Fast words here.";
        static const char plain[] = "Fast words here.";
        fd = speak_open(0, "en-US", 0);
        const long plain_n = speak_all(fd, plain, sizeof(plain));
        const long read_n = speak_all(fd, tagged, sizeof(tagged));
        CHECK(close(fd) == 0);
        fd = speak_open(BMDS_SPEAK_MARKUP, NULL, 0);
        const long obeyed_n = speak_all(fd, tagged, sizeof(tagged));
        CHECK(bmds_audio_speak_info(fd, &info) == 0 && info.flags == BMDS_SPEAK_MARKUP);
        CHECK(close(fd) == 0);
        fd = speak_open(0, NULL, 300);
        const long fast_n = speak_all(fd, plain, sizeof(plain));
        CHECK(close(fd) == 0);
        CHECK(plain_n > 8000 && obeyed_n < plain_n * 2 / 3 && read_n > plain_n * 2);
        CHECK(fast_n == obeyed_n);

        /* A long text: a write takes what fits, then EAGAIN until read. */
        static char lots[16384];
        for (size_t i = 0; i < sizeof(lots); ++i) lots[i] = "Words and more words. "[i % 22];
        fd = speak_open(0, NULL, 0);
        const ssize_t took = write(fd, lots, sizeof(lots));
        CHECK(took > 0 && took < (ssize_t)sizeof(lots));
        CHECK(write(fd, lots + took, sizeof(lots) - (size_t)took) == -1 && errno == EAGAIN);
        CHECK(read(fd, first, sizeof(first)) == (ssize_t)sizeof(first));
        CHECK(close(fd) == 0); /* the rest dropped */

        /* Two at once, each as one alone. */
        const int a = speak_open(0, NULL, 0), b = speak_open(0, NULL, 0);
        CHECK(a >= 0 && b >= 0);
        CHECK(speak_all(a, plain, sizeof(plain)) == plain_n);
        CHECK(speak_all(b, plain, sizeof(plain)) == plain_n);
        CHECK(close(a) == 0 && close(b) == 0);

        if (strcmp(arg, "quiet") != 0) CHECK(bmds_say("Speech from a program, through the system call.") == 0);
        printf("speech: ok (%ld samples, plain %ld, read %ld, obeyed %ld)\n", made, plain_n, read_n, obeyed_n);
        return 0;
}

/* The synth (devplan/audio.md §22) on FONT (tests/host/synth/sine.sf2: one preset, a 440 Hz
   sine at its key A4): every bad config refused, events on their frames, the pitch, the queue,
   S16, a damaged font, two synths sharing the font. */
static int crossings_f(const float *x, size_t frames, int channels) {
        int n = 0;
        for (size_t i = 1; i < frames; ++i)
                if (x[(i - 1) * (size_t)channels] < 0 && x[i * (size_t)channels] >= 0) ++n;
        return n;
}

static bmds_synth_event_t synth_event(uint64_t frame, uint8_t a, uint8_t b, uint8_t c) {
        bmds_synth_event_t e = {.frame = frame, .length = 3, .data = {a, b, c}};
        return e;
}

static int synth(const char *font) {
        CHECK(bmds_sys_has(BMDS_SYS_AUDIO_SYNTH_INFO));
        bmds_synth_config_t c = {.size = sizeof(c), .format = BMDS_AUDIO_F32LE, .font = font};
        bmds_synth_config_t bad = c;
        bad.format = BMDS_AUDIO_S8;
        CHECK(bmds_audio_synth(&bad) == -BMDS_EINVAL);
        bad = c;
        bad.rate = 1000;
        CHECK(bmds_audio_synth(&bad) == -BMDS_EINVAL);
        bad = c;
        bad.channels = 3;
        CHECK(bmds_audio_synth(&bad) == -BMDS_EINVAL);
        bad = c;
        bad.max_voices = 257;
        CHECK(bmds_audio_synth(&bad) == -BMDS_EINVAL);
        bad = c;
        bad.flags = 0x100;
        CHECK(bmds_audio_synth(&bad) == -BMDS_EINVAL);
        bad = c;
        bad.font = "/tmp/no-such.sf2";
        CHECK(bmds_audio_synth(&bad) == -BMDS_ENOENT);
        /* A damaged font. */
        const int junk = open("/tmp/junk.sf2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(junk >= 0);
        static char garbage[4096];
        memset(garbage, 0x5a, sizeof(garbage));
        memcpy(garbage, "RIFF\x00\x10\x00\x00sfbk", 12);
        CHECK(write(junk, garbage, sizeof(garbage)) == (ssize_t)sizeof(garbage) && close(junk) == 0);
        bad.font = "/tmp/junk.sf2";
        CHECK(bmds_audio_synth(&bad) == -BMDS_EIO);
        CHECK(unlink("/tmp/junk.sf2") == 0);

        const int fd = bmds_audio_synth(&c);
        CHECK(fd >= 0);
        bmds_synth_info_t info = {.size = sizeof(info)};
        CHECK(bmds_audio_synth_info(fd, &info) == 0);
        CHECK(info.rate == 48000 && info.channels == 2 && info.format == BMDS_AUDIO_F32LE && info.frames == 0);
        CHECK(info.max_voices == 32 && info.presets == 1 && info.font_bytes > 4000 &&
              strstr(info.font, "sine.sf2") != NULL);
        CHECK(bmds_audio_synth_info(1, &info) == -BMDS_ENOTTY);

        /* A note at frame 100: silence before it, sound after. */
        bmds_synth_event_t e = synth_event(100, 0x90, 69, 127);
        CHECK(write(fd, &e, sizeof(e)) == (ssize_t)sizeof(e));
        CHECK(write(fd, &e, 10) == -1 && errno == EINVAL); /* whole records */
        static float out[48000 * 2];
        CHECK(read(fd, out, 256 * 8) == 256 * 8);
        for (int i = 0; i < 200; ++i) CHECK(out[i] == 0.0f);
        float peak = 0;
        for (int i = 200; i < 512; ++i) peak = fabsf(out[i]) > peak ? fabsf(out[i]) : peak;
        CHECK(peak > 0.01f);
        CHECK(read(fd, out, 7) == -1 && errno == EINVAL); /* less than a frame */
        /* A second of A4: 440 Hz. */
        CHECK(read(fd, out, sizeof(out)) == (ssize_t)sizeof(out));
        const int n = crossings_f(out, 48000, 2);
        CHECK(n >= 438 && n <= 442);
        CHECK(bmds_audio_synth_info(fd, &info) == 0 && info.frames == 48256 && info.voices == 1);
        CHECK(info.cpu_permille_max > 0);

        /* The queue: 1024 at most, then EAGAIN; a frame already played plays at once. */
        bmds_synth_event_t sense = {.frame = 1u << 30, .length = 1, .data = {0xfe}};
        for (int i = 0; i < 1024; ++i) CHECK(write(fd, &sense, sizeof(sense)) == (ssize_t)sizeof(sense));
        CHECK(write(fd, &sense, sizeof(sense)) == -1 && errno == EAGAIN);
        CHECK(bmds_audio_synth_info(fd, &info) == 0 && info.queued == 1024 && info.queue_full == 1);
        CHECK(close(fd) == 0);

        /* S16, mono, and a second synth sharing the font. */
        c.format = BMDS_AUDIO_S16LE;
        c.channels = 1;
        c.rate = 44100;
        const int a = bmds_audio_synth(&c);
        const int b = bmds_audio_synth(&c);
        CHECK(a >= 0 && b >= 0);
        e = synth_event(0, 0x90, 81, 127);
        CHECK(write(a, &e, sizeof(e)) == (ssize_t)sizeof(e));
        static int16_t pcm[44100];
        CHECK(read(a, pcm, sizeof(pcm)) == (ssize_t)sizeof(pcm));
        int     up = 0;
        int16_t top = 0;
        for (int i = 1; i < 44100; ++i) {
                if (pcm[i - 1] < 0 && pcm[i] >= 0) ++up;
                if (pcm[i] > top) top = pcm[i];
        }
        CHECK(up >= 876 && up <= 884 && top > 300);
        CHECK(read(b, pcm, 64) == 64); /* nothing played on b: silence */
        for (int i = 0; i < 32; ++i) CHECK(pcm[i] == 0);
        bmds_synth_info_t ia = {.size = sizeof(ia)}, ib = {.size = sizeof(ib)};
        CHECK(bmds_audio_synth_info(a, &ia) == 0 && bmds_audio_synth_info(b, &ib) == 0 &&
              ia.font_bytes == ib.font_bytes);
        CHECK(close(a) == 0 && close(b) == 0);
        printf("synth: ok, %u bytes of font, A4 at %d crossings a second\n", (unsigned)info.font_bytes, n);
        return 0;
}

/* The synth's cost with FONT (a GM font): N notes on piano, strings, guitar and violin, rendered
   in 64-frame reads for a second each; the CPU per period. */
/* A MIDI file as a decoder (devplan/audio.md §22): FILE (tests/host/synth/scale.mid, 5 s and the
   release's 2) played by the synth on synth/font, decoded in full, then sought into. */
static int mididecode(const char *path) {
        const int file = open(path, O_RDONLY);
        CHECK(file >= 0);
        bmds_decode_config_t c = {.size = sizeof(c), .format = BMDS_AUDIO_S16LE};
        const int64_t        t0 = mono_ns();
        const int            pcm = bmds_audio_decode(file, &c);
        CHECK(pcm >= 0);
        const int64_t      opened = mono_ns() - t0;
        bmds_decode_info_t info = {.size = sizeof(info)};
        CHECK(bmds_audio_decode_info(pcm, &info) == 0);
        CHECK(info.codec == BMDS_CODEC_MIDI && strcmp(info.name, "midi") == 0);
        CHECK(info.native_format == BMDS_AUDIO_F32LE && info.channels == 2 && info.rate == 48000);
        CHECK(info.frames >= 48000 * 6 && info.frames <= 48000 * 8);
        static int16_t buf[4096 * 2];
        uint64_t       frames = 0;
        int            peak = 0;
        int64_t        worst = 0;
        for (;;) {
                const int64_t a = mono_ns();
                const ssize_t n = read(pcm, buf, sizeof(buf));
                const int64_t d = mono_ns() - a;
                worst = d > worst ? d : worst;
                CHECK(n >= 0);
                if (n == 0) break;
                for (ssize_t i = 0; i < n / 2; ++i) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
                frames += (uint64_t)n / 4;
        }
        CHECK(frames == info.frames);
        CHECK(peak > 1000);
        /* Seek to 2 s: the third note on. */
        CHECK(lseek(pcm, 48000 * 4 * 2, SEEK_SET) == 48000 * 4 * 2);
        CHECK(read(pcm, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
        CHECK(close(pcm) == 0 && close(file) == 0);
        printf("mididecode: ok, %llu frames, peak %d\n", (unsigned long long)frames, peak); /* its speed: bench */
        (void)opened;
        (void)worst;
        return 0;
}

/* The system monitor's calls (devplan/sysmon.md §3.6). */
/* Keeps a core busy for MS milliseconds. */
static void burn_ms(int ms) {
        const int64_t end = mono_ns() + (int64_t)ms * 1000000;
        while (mono_ns() < end) {}
}

/* The id of the counter named NAME, or 0. */
static uint32_t counter_id(const char *name, bmds_counter_t *out) {
        for (uint32_t i = 0;; ++i) {
                out->size = sizeof(*out);
                if (bmds_stats_list(i, out) != 0) return 0;
                if (strcmp(out->name, name) == 0) return out->id;
        }
}

static int stats(const char *self) {
        CHECK(bmds_sys_has(BMDS_SYS_PROC_INFO));
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0);
        CHECK(st.count > 100 && st.generation != 0 && st.cores == 2 && st.interval_ms >= 100);
        CHECK((st.flags & BMDS_STATS_WATCHING) == 0);
        const uint32_t watchers = st.watchers;
        const int      counted = (st.flags & BMDS_STATS_CPU_COUNTED) != 0;

        /* Every counter listed: ids distinct from their neighbours', names dotted, rules sane. */
        bmds_counter_t c;
        uint32_t       n = 0, previous = 0;
        for (;; ++n) {
                c.size = sizeof(c);
                const int32_t r = bmds_stats_list(n, &c);
                if (r == -BMDS_ENOENT) break;
                CHECK(r == 0 && c.id != 0 && c.id != previous && strchr(c.name, '.') != NULL);
                CHECK(c.kind <= BMDS_COUNTER_INFO && c.unit <= BMDS_UNIT_PPB && c.rule <= BMDS_RULE_STATE);
                CHECK(c.generation == st.generation || n > 0); /* may change while listing */
                CHECK(c.help[0] != '\0');
                previous = c.id;
        }
        CHECK(n >= st.count - 8 && n <= st.count + 8);
        /* A known counter, as described. */
        const uint32_t busy = counter_id("cpu.core0.busy_us", &c);
        CHECK(busy != 0 && c.unit == BMDS_UNIT_MICROS && c.kind == BMDS_COUNTER_TOTAL && c.rule == BMDS_RULE_ABOVE);
        CHECK(c.warn == 900000 && c.crit == 980000 && c.hold == 5 && (c.flags & BMDS_COUNTER_HISTORY) != 0);
        const uint32_t late = counter_id("audio.dsp0.late", &c);
        CHECK(late != 0 && c.rule == BMDS_RULE_ZERO && c.health == BMDS_HEALTH_CRITICAL && c.hold == 60);
        const uint32_t state = counter_id("audio.dsp0.state", &c);
        CHECK(state != 0 && c.kind == BMDS_COUNTER_STATE && strcmp(c.labels, "stopped|running|faulted") == 0);
        /* An older, shorter struct: only what fits is filled. */
        bmds_counter_t old;
        memset(&old, 0x5a, sizeof(old));
        old.size = (uint32_t)offsetof(bmds_counter_t, name);
        CHECK(bmds_stats_list(0, &old) == 0 && old.id != 0 && (unsigned char)old.name[0] == 0x5a);
        /* Errors. */
        c.size = 0;
        CHECK(bmds_stats_list(0, &c) == -BMDS_EINVAL);
        CHECK(bmds_stats_list(0, NULL) == -BMDS_EFAULT);
        c.size = sizeof(c);
        CHECK(bmds_stats_list(100000, &c) == -BMDS_ENOENT);

        /* Reading: values at one moment, unjudged while nobody watches (if nobody did). */
        uint32_t ids[3] = {busy, late, 0x7fffff00u};
        int64_t  values[3], t0 = 0, t1 = 0;
        uint8_t  health[3];
        CHECK(bmds_stats_read(ids, 3, values, health, &t0) == 3);
        CHECK(values[2] == BMDS_COUNTER_UNAVAILABLE && health[2] == BMDS_HEALTH_UNJUDGED);
        CHECK(counted ? values[0] > 0 : values[0] == BMDS_COUNTER_UNAVAILABLE);
        if (watchers == 0) CHECK(health[0] == BMDS_HEALTH_UNJUDGED);
        const int64_t busy0 = values[0];
        burn_ms(200);
        CHECK(bmds_stats_read(ids, 1, values, NULL, &t1) == 1 && t1 - t0 >= 200000000);
        if (counted) CHECK(values[0] >= busy0); /* this core or the other: either may have run it */
        CHECK(bmds_stats_read(ids, 0, NULL, NULL, NULL) == 0);
        CHECK(bmds_stats_read(NULL, 1, values, NULL, NULL) == -BMDS_EFAULT);
        CHECK(bmds_stats_read(ids, 2000, values, NULL, NULL) == -BMDS_EINVAL);

        /* Watching: the sampler runs, health is judged, history is kept. */
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 1) == 0);
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 1) == 0); /* once per process */
        CHECK(bmds_stats_status(&st) == 0 && (st.flags & BMDS_STATS_WATCHING) != 0);
        CHECK(st.watchers == watchers + 1);
        usleep((useconds_t)(st.interval_ms * 2500));
        CHECK(bmds_stats_read(ids, 3, values, health, NULL) == 3);
        CHECK(health[0] <= BMDS_HEALTH_CRITICAL && health[1] <= BMDS_HEALTH_CRITICAL);
        int64_t       samples[200], newest = 0;
        const int32_t kept = bmds_stats_history(busy, samples, 200, &newest);
        CHECK(kept >= 2 && kept <= 120 && newest > 0 && newest <= mono_ns());
        CHECK(bmds_stats_history(busy, samples, 1, NULL) == 1);
        CHECK(bmds_stats_history(late, samples, 200, NULL) == 0); /* not kept */
        CHECK(bmds_stats_history(busy, NULL, 4, NULL) == -BMDS_EFAULT);
        CHECK(bmds_stats_status(&st) == 0 && st.last_sample_ns > 0);

        /* Problems: whatever the log holds, in the order of its changes. */
        bmds_problem_t problems[16];
        problems[0].size = sizeof(problems[0]);
        int32_t got = bmds_stats_problems(0, problems, 16);
        CHECK(got >= 0 && got <= 16);
        for (int i = 0; i < got; ++i) {
                CHECK(problems[i].size == sizeof(problems[0]) && problems[i].name[0] != '\0');
                CHECK(problems[i].health == BMDS_HEALTH_WARNING || problems[i].health == BMDS_HEALTH_CRITICAL);
                CHECK(problems[i].changed <= st.last_change &&
                      (i == 0 || problems[i].changed > problems[i - 1].changed));
                CHECK(problems[i].start_ns <= problems[i].end_ns);
        }
        CHECK(bmds_stats_problems(st.last_change, problems, 16) == 0);
        CHECK(bmds_stats_problems(0, problems, 0) == 0);
        problems[0].size = 0;
        CHECK(bmds_stats_problems(0, problems, 16) == -BMDS_EINVAL);
        CHECK(bmds_stats_problems(0, NULL, 16) == -BMDS_EFAULT);
        CHECK(bmds_stats_control(BMDS_STATS_SEEN, st.last_change) == 0);
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 2) == -BMDS_EINVAL);
        CHECK(bmds_stats_control(BMDS_STATS_CLEAR, 1) == -BMDS_EINVAL);
        CHECK(bmds_stats_control(99, 0) == -BMDS_EINVAL);
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 0) == 0);
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 0) == 0);
        CHECK(bmds_stats_status(&st) == 0 && (st.flags & BMDS_STATS_WATCHING) == 0 && st.watchers == watchers);

        /* A watch ends with its process, and goes on across exec. */
        CHECK(status_of(start(self, NULL, NULL, "statswatch", NULL)) == 0);
        CHECK(bmds_stats_status(&st) == 0 && st.watchers == watchers);
        CHECK(status_of(start(self, NULL, NULL, "statswatch", self, NULL)) == 0);
        CHECK(bmds_stats_status(&st) == 0 && st.watchers == watchers);
        printf("stats: all passed (%u counters, CPU time %s)\n", (unsigned)n, counted ? "counted" : "not counted");
        return 0;
}

/* Watches and ends without stopping; with SELF, execs SELF statswatching first. */
static int stats_watch(const char *self) {
        CHECK(bmds_stats_control(BMDS_STATS_WATCH, 1) == 0);
        if (self != NULL) {
                char *eargv[] = {"progtest", "statswatching", NULL};
                execve(self, eargv, environ);
                return 1;
        }
        return 0;
}

static int stats_watching(void) {
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0 && (st.flags & BMDS_STATS_WATCHING) != 0);
        return 0;
}

static void *task_thread(void *arg) {
        (void)arg;
        usleep(500000);
        return NULL;
}

static int tasks(void) {
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0);
        const int counted = (st.flags & BMDS_STATS_CPU_COUNTED) != 0;
        pthread_t t;
        CHECK(pthread_create(&t, NULL, task_thread, NULL) == 0);
        usleep(50000);
        burn_ms(100);
        static bmds_task_info_t list[96];
        list[0].size = sizeof(list[0]);
        const int32_t n = bmds_task_list(list, 96);
        CHECK(n >= 12 && n <= 96);
        int self = 0, thread = 0, idle = 0, timers = 0;
        for (int i = 0; i < n; ++i) {
                const bmds_task_info_t *e = &list[i];
                CHECK(e->size == sizeof(*e) && e->name[0] != '\0' && e->state <= BMDS_TASK_DELETED);
                CHECK(e->stack_free > 0 && e->number != 0 && e->core >= -1 && e->core <= 1);
                if (e->pid == getpid() && (e->flags & BMDS_TASK_MAIN) != 0) {
                        ++self;
                        CHECK(e->tid == getpid() && e->state == BMDS_TASK_RUNNING &&
                              strncmp(e->name, "progtest", 8) == 0);
                        CHECK(counted ? e->cpu_us >= 80000 : e->cpu_us == 0);
                } else if (e->pid == getpid()) {
                        ++thread;
                        CHECK(e->tid != getpid() && e->tid > 0);
                }
                if (strncmp(e->name, "IDLE", 4) == 0) {
                        ++idle;
                        CHECK(e->pid == 0 && e->tid == 0 && e->priority == 0 && e->core == e->name[4] - '0');
                }
                if (strcmp(e->name, "Tmr Svc") == 0) ++timers;
        }
        CHECK(self == 1 && thread == 1 && idle == 2 && timers == 1);
        /* Fewer than there are: the total still. A wider stride: each entry at it. */
        list[0].size = sizeof(list[0]);
        CHECK(bmds_task_list(list, 2) == n || bmds_task_list(list, 2) >= n - 2);
        CHECK(bmds_task_list(NULL, 0) >= n - 2);
        static uint8_t    wide[8][sizeof(bmds_task_info_t) + 16];
        bmds_task_info_t *w = (bmds_task_info_t *)wide[0];
        w->size = sizeof(wide[0]);
        CHECK(bmds_task_list(w, 8) >= 8);
        CHECK(((bmds_task_info_t *)wide[7])->size == sizeof(wide[0]) && ((bmds_task_info_t *)wide[7])->name[0] != '\0');
        list[0].size = 0;
        CHECK(bmds_task_list(list, 4) == -BMDS_EINVAL);
        CHECK(bmds_task_list(NULL, 4) == -BMDS_EFAULT);
        CHECK(pthread_join(t, NULL) == 0);
        printf("tasks: all passed (%d tasks)\n", (int)n);
        return 0;
}

static int procs(const char *self) {
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0);
        const int counted = (st.flags & BMDS_STATS_CPU_COUNTED) != 0;
        burn_ms(100);
        bmds_proc_info_t p = {.size = sizeof(p)};
        CHECK(bmds_proc_info(getpid(), 0, &p) == getpid());
        CHECK(p.pid == getpid() && p.parent == getppid() && p.state == BMDS_PROC_RUNNING && p.threads == 1);
        CHECK(strstr(p.command, "progtest") != NULL && strstr(p.command, " procs") != NULL && p.memory > 0);
        CHECK(p.start_ns > 0 && p.start_ns < mono_ns());
        CHECK(counted ? p.cpu_us >= 80000 : p.cpu_us == 0);
        /* Every process, in pid order: pid 1 and this one among them. */
        int32_t pid = 0, last = 0, seen_init = 0, seen_self = 0;
        while ((pid = bmds_proc_info(pid, BMDS_PROC_INFO_NEXT, &p)) > 0) {
                CHECK(pid > last && p.pid == pid);
                seen_init += pid == 1;
                seen_self += pid == getpid();
                last = pid;
        }
        CHECK(pid == -BMDS_ESRCH && seen_init == 1 && seen_self == 1);
        CHECK(bmds_proc_info(30000, 0, &p) == -BMDS_ESRCH);
        CHECK(bmds_proc_info(getpid(), 2, &p) == -BMDS_EINVAL);
        p.size = 0;
        CHECK(bmds_proc_info(getpid(), 0, &p) == -BMDS_EINVAL);
        p.size = sizeof(p);

        /* A zombie keeps its time and status until waited for. */
        const pid_t child = start(self, NULL, NULL, "burn", "300", NULL);
        CHECK(child > 0);
        for (int i = 0; i < 100; ++i) {
                CHECK(bmds_proc_info(child, 0, &p) == child);
                if (p.state == BMDS_PROC_ZOMBIE) break;
                usleep(20000);
        }
        CHECK(p.state == BMDS_PROC_ZOMBIE && p.status == (5 << 8) && p.parent == getpid());
        CHECK(counted ? p.cpu_us >= 250000 : p.cpu_us == 0);
        CHECK(status_of(child) == 5);
        CHECK(bmds_proc_info(child, 0, &p) == -BMDS_ESRCH);

        /* Across exec: the time so far and the start stay the process's. */
        CHECK(status_of(start(self, NULL, NULL, "procexec", self, NULL)) == 0);
        printf("procs: all passed\n");
        return 0;
}

/* Linux's view of the process's time (devplan/sysmon.md §3.5, step 6): a burn counts, a sleep
   doesn't; times() and CLOCK_PROCESS_CPUTIME_ID agree with getrusage(); sysinfo() counts the
   processes and the memory. */
static int rusage_test(const char *self) {
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0);
        const int counted = (st.flags & BMDS_STATS_CPU_COUNTED) != 0;
#define RU_US(r) ((int64_t)(r).ru_utime.tv_sec * 1000000 + (r).ru_utime.tv_usec)
#define TS_NS(t) ((int64_t)(t).tv_sec * 1000000000 + (t).tv_nsec)
        struct rusage   r0, r1, r2;
        struct tms      t0, t1;
        struct timespec c0, c1;
        CHECK(getrusage(RUSAGE_SELF, &r0) == 0);
        CHECK(times(&t0) != (clock_t)-1);
        CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0) == 0);
        burn_ms(300);
        CHECK(getrusage(RUSAGE_SELF, &r1) == 0);
        CHECK(times(&t1) != (clock_t)-1);
        CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1) == 0);
        const int64_t burned = RU_US(r1) - RU_US(r0);
        printf("rusage: burned %lld us, times %ld ticks, clock %lld us, maxrss %ld KB\n", (long long)burned,
               (long)(t1.tms_utime - t0.tms_utime), (long long)((TS_NS(c1) - TS_NS(c0)) / 1000), r1.ru_maxrss);
        if (counted) {
                CHECK(burned >= 250000 && burned <= 400000);
                CHECK(t1.tms_utime - t0.tms_utime >= 25 && t1.tms_utime - t0.tms_utime <= 40);
                CHECK(TS_NS(c1) - TS_NS(c0) >= 250000000 && TS_NS(c1) - TS_NS(c0) <= 400000000);
        }
        CHECK(r1.ru_stime.tv_sec == 0 && r1.ru_stime.tv_usec == 0 && t1.tms_stime == 0);
        CHECK(r1.ru_maxrss >= 0);
        usleep(300000);
        CHECK(getrusage(RUSAGE_SELF, &r2) == 0);
        if (counted) CHECK(RU_US(r2) - RU_US(r1) < 50000); /* asleep: not running */
        /* A child's time is the parent's children's once waited for (not before). */
        struct rusage rc, rc2;
        struct tms    tc;
        CHECK(getrusage(RUSAGE_CHILDREN, &rc) == 0);
        const pid_t child = start(self, NULL, NULL, "burn", "300", NULL);
        CHECK(child > 0);
        bmds_proc_info_t p = {.size = sizeof(p)};
        for (int i = 0; i < 100; ++i) {
                CHECK(bmds_proc_info(child, 0, &p) == child);
                if (p.state == BMDS_PROC_ZOMBIE) break;
                usleep(20000);
        }
        CHECK(getrusage(RUSAGE_CHILDREN, &rc2) == 0 && RU_US(rc2) == RU_US(rc)); /* a zombie: not yet */
        CHECK(status_of(child) == 5);
        CHECK(getrusage(RUSAGE_CHILDREN, &rc2) == 0);
        CHECK(times(&tc) != (clock_t)-1);
        printf("rusage: a child's 300 ms burn, waited for: children %lld us, %ld ticks\n",
               (long long)(RU_US(rc2) - RU_US(rc)), (long)tc.tms_cutime);
        if (counted) {
                CHECK(RU_US(rc2) - RU_US(rc) >= 250000 && RU_US(rc2) - RU_US(rc) <= 400000);
                CHECK(tc.tms_cutime >= 25);
        }
        /* An older program's smaller struct still works (the fields that fit). */
        bmds_proc_info_t old = {.size = offsetof(bmds_proc_info_t, children_cpu_us)};
        old.children_cpu_us = 12345;
        CHECK(bmds_proc_info(getpid(), 0, &old) == getpid() && old.children_cpu_us == 12345);
        errno = 0;
        CHECK(getrusage(99, &rc) == -1 && errno == EINVAL);
        struct sysinfo si;
        CHECK(sysinfo(&si) == 0);
        CHECK(si.totalram > 0 && si.freeram > 0 && si.freeram <= si.totalram && si.mem_unit == 1);
        CHECK(si.procs >= 2 && si.uptime > 0); /* init and this one at least */
        printf("rusage: %u processes, %lu of %lu bytes free; all passed\n", (unsigned)si.procs,
               (unsigned long)si.freeram, (unsigned long)si.totalram);
#undef RU_US
#undef TS_NS
        return 0;
}

/* The system's serial number and name (devplan/notifications.md §2): system_info, and uname and
   gethostname agreeing with it. */
static int system_test(void) {
        CHECK(bmds_sys_has(BMDS_SYS_SYSTEM_INFO));
        bmds_system_info_t si;
        memset(&si, 0, sizeof(si));
        si.size = sizeof(si);
        CHECK(bmds_system_info(&si) == 0);
        printf("system: serial %s, host %s, board %s, BMDS %s, %u cores at %u MHz, rev %u\n", si.serial, si.hostname,
               si.board, si.version, (unsigned)si.cores, (unsigned)si.cpu_mhz, (unsigned)si.chip_revision);
        CHECK(si.serial[0] != '\0' && strlen(si.serial) < sizeof(si.serial));
        CHECK(si.hostname[0] != '\0' && si.board[0] != '\0' && si.version[0] != '\0');
        CHECK(si.cores == 2 && si.cpu_mhz >= 100);
        struct utsname u;
        CHECK(uname(&u) == 0 && strcmp(u.nodename, si.hostname) == 0);
        char host[65];
        CHECK(gethostname(host, sizeof(host)) == 0 && strcmp(host, si.hostname) == 0);
        /* An older program's smaller struct: the fields that fit. */
        bmds_system_info_t old;
        memset(&old, 0x55, sizeof(old));
        old.size = offsetof(bmds_system_info_t, hostname);
        CHECK(bmds_system_info(&old) == 0 && strcmp(old.serial, si.serial) == 0 && old.hostname[0] == 0x55);
        old.size = 0;
        CHECK(bmds_system_info(&old) == -BMDS_EINVAL);
        CHECK(bmds_system_info(NULL) == -BMDS_EFAULT);
        printf("system: all passed\n");
        return 0;
}

/* Text templates (devplan/templates.md): bmds_expand agreeing with system_info, the date's
   format, $$, a cut expansion, the errors. */
static int expand_test(void) {
        CHECK(bmds_sys_has(BMDS_SYS_EXPAND));
        bmds_system_info_t si;
        memset(&si, 0, sizeof(si));
        si.size = sizeof(si);
        CHECK(bmds_system_info(&si) == 0);
        char    out[256];
        char    want[256];
        int32_t n = bmds_expand("${HOME}/${HOSTNAME}/${SERIAL} $$", out, sizeof(out));
        snprintf(want, sizeof(want), "/home/%s/%s $", si.hostname, si.serial);
        CHECK(n == (int32_t)strlen(want) && strcmp(out, want) == 0);
        n = bmds_expand("${HOSTNAME}_${DATE}_${TIME}.png", out, sizeof(out));
        printf("expand: %s\n", out);
        CHECK(n == (int32_t)(strlen(si.hostname) + 24) && out[strlen(si.hostname) + 5] == '-');
        CHECK(bmds_expand("${DATE:%Y}", out, sizeof(out)) == 4 && out[0] >= '0' && out[0] <= '9'); /* the clock may be unset */
        /* Cut: the whole length returned, what fits written. */
        CHECK(bmds_expand("${HOME}", out, 3) == 5 && strcmp(out, "/h") == 0);
        CHECK(bmds_expand("${HOME}", NULL, 0) == 5);
        CHECK(bmds_expand("$HOME", out, sizeof(out)) == -BMDS_EINVAL);
        CHECK(bmds_expand("${NOPE}", out, sizeof(out)) == -BMDS_EINVAL);
        CHECK(bmds_expand("${HOME", out, sizeof(out)) == -BMDS_EINVAL);
        CHECK(bmds_expand(NULL, out, sizeof(out)) == -BMDS_EFAULT);
        printf("expand: all passed\n");
        return 0;
}

/* Pictures (devplan/images.md §3f): a pattern written as PNG and read back exactly, as JPEG and
   read back near, through an fd and from memory, every pixel format; with FILE (a baseline JPEG the
   codec takes), decoded by the codec into a buffer laid out as image_info says (no copy). */
/* --- The system's fonts (devplan/fonts.md §4e): the faces, fonts opened every way, metrics,
   measuring, drawing into each format (clipped), errors, fds closed. */

static int count_ink(const uint8_t *p, size_t n, uint8_t empty) {
        int ink = 0;
        for (size_t i = 0; i < n; ++i) ink += p[i] != empty;
        return ink;
}

static int font_test(void) {
        if (!bmds_sys_has(BMDS_SYS_FONT_OPEN)) {
                printf("fonts: not in this kernel\n");
                return 0;
        }
        // The faces: unscii first, then IBM Plex from /rom.
        bmds_font_face_t face = {.size = sizeof(face)};
        CHECK(bmds_font_list(0, &face) == 0);
        CHECK(strcmp(face.family, "unscii") == 0 && (face.flags & BMDS_FONT_BITMAP) != 0 && face.bitmap_size == 16);
        uint32_t faces = 1, plex = 0;
        while (bmds_font_list(faces, &face) == 0) {
                plex += strncmp(face.family, "IBM Plex", 8) == 0;
                ++faces;
        }
        CHECK(plex >= 6);
        CHECK(bmds_font_list(faces, &face) == -BMDS_ENOENT);
        face.size = 2;
        CHECK(bmds_font_list(0, &face) == -BMDS_EINVAL);

        // By name, by family, by role; what each is.
        const int32_t sans = bmds_font_open_name("IBM Plex Sans 16 bold");
        CHECK(sans >= 0);
        bmds_font_info_t info = {.size = sizeof(info)};
        CHECK(bmds_font_info(sans, &info) == 0);
        CHECK(strcmp(info.name, "IBM Plex Sans 16 bold") == 0 && info.pixel_size == 16);
        CHECK(info.ascent > 0 && info.descent > 0 && info.line_height >= info.ascent + info.descent);
        CHECK((info.flags & BMDS_FONT_BITMAP) == 0 && (info.style & BMDS_FONT_BOLD) != 0);
        bmds_font_spec_t spec = {.size = sizeof(spec), .family = "IBM Plex Mono", .pixel_size = 14};
        const int32_t    mono = bmds_font_open(&spec);
        CHECK(mono >= 0 && bmds_font_info(mono, &info) == 0 && (info.flags & BMDS_FONT_FIXED) != 0);
        spec = (bmds_font_spec_t){.size = sizeof(spec), .role = BMDS_FONT_ROLE_TERMINAL};
        const int32_t term = bmds_font_open(&spec);
        CHECK(term >= 0 && bmds_font_info(term, &info) == 0 && info.line_height > 0);
        const int32_t big = bmds_font_open_name("unscii 32");
        CHECK(big >= 0 && bmds_font_info(big, &info) == 0 && (info.flags & BMDS_FONT_BITMAP) != 0);
        CHECK(info.pixel_size == 32 && info.line_height == 32);

        // Errors.
        CHECK(bmds_font_open_name("") == -BMDS_EINVAL);
        CHECK(bmds_font_open_name("Sans 300") == -BMDS_ERANGE);
        spec = (bmds_font_spec_t){.size = sizeof(spec), .role = 9};
        CHECK(bmds_font_open(&spec) == -BMDS_EINVAL);
        spec = (bmds_font_spec_t){.size = sizeof(spec), .family = "IBM Plex Sans", .style = 0x10};
        CHECK(bmds_font_open(&spec) == -BMDS_EINVAL);
        CHECK(bmds_font_open(NULL) == -BMDS_EFAULT);
        spec = (bmds_font_spec_t){.size = sizeof(spec), .flags = 1, .name = "ui"};
        CHECK(bmds_font_open(&spec) == -BMDS_EINVAL);
        CHECK(bmds_font_info(15, &info) == -BMDS_EBADF); // a handle not open (not an fd: 1 is stdout)
        CHECK(bmds_font_info(-1, &info) == -BMDS_EBADF);
        CHECK(bmds_font_close(77) == -BMDS_EBADF);

        // Measuring: kerned, to a newline, within a width.
        bmds_text_extent_t e = {.size = sizeof(e)}, av = {.size = sizeof(av)}, a = {.size = sizeof(a)},
                           v = {.size = sizeof(v)};
        CHECK(bmds_text_measure(sans, "AV", 2, 0, &av) == 0);
        CHECK(bmds_text_measure(sans, "A", 1, 0, &a) == 0 && bmds_text_measure(sans, "V", 1, 0, &v) == 0);
        CHECK(av.width < a.width + v.width && av.bytes == 2);
        CHECK(bmds_text_measure(sans, "Hello\nworld", 11, 0, &e) == 0 && e.bytes == 5);
        const uint32_t whole = e.width;
        CHECK(bmds_text_measure(sans, "Hello", 5, whole - 1, &e) == 0 && e.bytes < 5 && e.width <= whole - 1);
        CHECK(bmds_text_measure(big, "abc", 3, 0, &e) == 0 && e.width == 48 && e.height == 32);

        // Drawing into each format: ink inside the line, nothing outside the clip.
        static const uint32_t formats[] = {BMDS_PIXEL_BGR24,  BMDS_PIXEL_RGB24, BMDS_PIXEL_BGRA32,
                                           BMDS_PIXEL_RGB565, BMDS_PIXEL_GRAY8, BMDS_PIXEL_L8};
        static const uint32_t bpps[] = {3, 3, 4, 2, 1, 1};
        enum {
                W = 120,
                H = 30
        };
        static uint8_t pic[W * H * 4];
        for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
                const size_t row = (size_t)W * bpps[f];
                memset(pic, 0, sizeof(pic));
                bmds_text_draw_t d = {.size = sizeof(d),
                                      .font = sans,
                                      .pixels = pic,
                                      .format = formats[f],
                                      .width = W,
                                      .height = H,
                                      .x = 4,
                                      .y = 4,
                                      .color = 0xffffff,
                                      .text = "To AVA",
                                      .length = 6};
                if (formats[f] == BMDS_PIXEL_L8) d.color = 9;
                const int32_t w = bmds_text_draw(&d);
                CHECK(bmds_text_measure(sans, "To AVA", 6, 0, &e) == 0);
                CHECK(w > 0 && (uint32_t)w == e.width);
                CHECK(count_ink(pic, row * H, 0) > 20);
                // Clipped to columns 20-40: nothing changes outside them.
                memset(pic, 0, sizeof(pic));
                d.clip_x = 20;
                d.clip_y = 0;
                d.clip_width = 20;
                d.clip_height = H;
                CHECK(bmds_text_draw(&d) > 0);
                for (int y = 0; y < H; ++y) {
                        CHECK(count_ink(pic + (size_t)y * row, 20 * bpps[f], 0) == 0);
                        CHECK(count_ink(pic + (size_t)y * row + 40 * bpps[f], (W - 40) * bpps[f], 0) == 0);
                }
        }
        // On a box: the line's box filled (nothing read), sharp on request.
        memset(pic, 0, sizeof(pic));
        bmds_text_draw_t d = {.size = sizeof(d),
                              .font = sans,
                              .pixels = pic,
                              .format = BMDS_PIXEL_GRAY8,
                              .width = W,
                              .height = H,
                              .color = 0,
                              .background = 0x808080,
                              .flags = BMDS_TEXT_OPAQUE | BMDS_TEXT_SHARP,
                              .text = "Box",
                              .length = 3};
        const int32_t    bw = bmds_text_draw(&d);
        CHECK(bw > 0 && pic[2 * W + 1] == 0x80);
        for (size_t i = 0; i < sizeof(pic) && i < (size_t)W * H; ++i) CHECK(pic[i] == 0 || pic[i] == 0x80);
        d.flags = BMDS_TEXT_SMOOTH | BMDS_TEXT_SHARP;
        CHECK(bmds_text_draw(&d) == -BMDS_EINVAL);
        d.flags = 0;
        d.format = 6;
        CHECK(bmds_text_draw(&d) == -BMDS_EINVAL);
        d.format = BMDS_PIXEL_GRAY8;
        d.stride = W - 1;
        CHECK(bmds_text_draw(&d) == -BMDS_EINVAL);
        d.stride = 0;
        d.font = 15;
        CHECK(bmds_text_draw(&d) == -BMDS_EBADF);

        // Many opened and closed: none left behind; sixteen at once, then no more; fds untouched.
        for (int i = 0; i < 200; ++i) {
                const int32_t f = bmds_font_open_name(i % 2 ? "IBM Plex Serif 20" : "ui");
                CHECK(f >= 0 && bmds_font_close(f) == 0);
        }
        int32_t held[16];
        int     n = 0;
        while (n < 16 && (held[n] = bmds_font_open_name("unscii")) >= 0) ++n;
        CHECK(n == 12); // with the four above
        CHECK(bmds_font_open_name("unscii") == -BMDS_EMFILE);
        const int spare = open("/dev/null", O_RDONLY);
        CHECK(spare >= 0 && close(spare) == 0);
        while (n > 0) CHECK(bmds_font_close(held[--n]) == 0);
        CHECK(bmds_font_close(sans) == 0 && bmds_font_close(mono) == 0 && bmds_font_close(term) == 0);
        CHECK(bmds_font_close(big) == 0);
        CHECK(bmds_font_info(sans, &info) == -BMDS_EBADF);
        printf("fonts: ok (%u faces)\n", (unsigned)faces);
        return 0;
}

static int image_test(const char *file) {
        CHECK(bmds_sys_has(BMDS_SYS_IMAGE_ENCODE));
        enum {
                W = 64,
                H = 48
        };
        static uint8_t pic[W * H * 3], back[W * H * 4];
        for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                        uint8_t *p = pic + (y * W + x) * 3; /* B, G, R: smooth, so JPEG keeps it close */
                        p[0] = (uint8_t)(x * 4);
                        p[1] = (uint8_t)(y * 5);
                        p[2] = (uint8_t)(255 - x * 2 - y);
                }
        static const char *const names[] = {"/tmp/progtest.png", "/tmp/progtest.jpg"};
        for (int k = 0; k < 2; ++k) {
                const int fd = open(names[k], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                CHECK(fd >= 0);
                bmds_image_encode_t e;
                memset(&e, 0, sizeof(e));
                e.size = sizeof(e);
                e.codec = k == 0 ? BMDS_IMAGE_PNG : BMDS_IMAGE_JPEG;
                e.format = BMDS_PIXEL_BGR24;
                e.width = W;
                e.height = H;
                e.pixels = pic;
                e.quality = 95;
                e.sampling = BMDS_JPEG_YUV444;
                const int32_t bytes = bmds_image_encode(fd, &e);
                close(fd);
                printf("image: %s, %d bytes\n", names[k], (int)bytes);
                CHECK(bytes > 0);
                /* Its info, from the fd. */
                const int rfd = open(names[k], O_RDONLY);
                CHECK(rfd >= 0);
                bmds_image_info_t info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                const int32_t got = bmds_image_info(rfd, NULL, 0, NULL, &info);
                if (got != 0) printf("image: image_info from the fd: %d\n", (int)got);
                CHECK(got == 0);
                close(rfd);
                CHECK(info.codec == (k == 0 ? BMDS_IMAGE_PNG : BMDS_IMAGE_JPEG));
                CHECK(info.width == W && info.height == H && info.decode_width == W && info.orientation == 1);
                CHECK(info.decoder == (k == 0 ? BMDS_IMAGE_DECODER_PNG : BMDS_IMAGE_DECODER_HARDWARE));
                /* Decoded from the fd into each format, compared through BGR. */
                for (uint32_t f = BMDS_PIXEL_BGR24; f <= BMDS_PIXEL_GRAY8; ++f) {
                        bmds_image_decode_t d;
                        memset(&d, 0, sizeof(d));
                        d.size = sizeof(d);
                        d.format = f;
                        d.out = back;
                        d.out_size = sizeof(back);
                        const int dfd = open(names[k], O_RDONLY);
                        CHECK(dfd >= 0);
                        CHECK(bmds_image_decode(dfd, &d, NULL) == 0);
                        close(dfd);
                        /* Every format back to B, G, R, compared (5-6-5 and grey lose bits). */
                        int most = 0;
                        for (int i = 0; i < W * H; ++i) {
                                const uint8_t *want = pic + i * 3;
                                int            b, g, r;
                                if (f == BMDS_PIXEL_BGR24) {
                                        b = back[i * 3], g = back[i * 3 + 1], r = back[i * 3 + 2];
                                } else if (f == BMDS_PIXEL_RGB24) {
                                        r = back[i * 3], g = back[i * 3 + 1], b = back[i * 3 + 2];
                                } else if (f == BMDS_PIXEL_BGRA32) {
                                        b = back[i * 4], g = back[i * 4 + 1], r = back[i * 4 + 2];
                                } else if (f == BMDS_PIXEL_RGB565) {
                                        const int v = back[i * 2] | back[i * 2 + 1] << 8;
                                        r = (v >> 11) << 3, g = (v >> 5 & 63) << 2, b = (v & 31) << 3;
                                } else { /* grey: its luma against the picture's, put where B is compared */
                                        const int y = (77 * want[2] + 150 * want[1] + 29 * want[0] + 128) >> 8;
                                        b = back[i] - y + want[0], g = want[1], r = want[2];
                                }
                                const int d[3] = {abs(b - want[0]), abs(g - want[1]), abs(r - want[2])};
                                for (int c = 0; c < 3; ++c) most = d[c] > most ? d[c] : most;
                        }
                        const int allowed =
                                (k == 0 ? 0 : 12) + (f == BMDS_PIXEL_RGB565 ? 8 : 0) + (f == BMDS_PIXEL_GRAY8 ? 2 : 0);
                        printf("image: %s as format %u, largest difference %d\n", names[k], (unsigned)f, most);
                        CHECK(most <= allowed);
                }
        }
        /* From memory, too small a buffer, nonsense. */
        bmds_image_decode_t d;
        memset(&d, 0, sizeof(d));
        d.size = sizeof(d);
        d.out = back;
        d.out_size = 100;
        d.data = pic;
        d.length = sizeof(pic);
        CHECK(bmds_image_decode(-1, &d, NULL) == -BMDS_ENOTSUP); /* not a picture */
        bmds_image_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        CHECK(bmds_image_info(-1, NULL, 0, NULL, &info) == -BMDS_EFAULT);
        d.format = 99;
        CHECK(bmds_image_info(-1, pic, sizeof(pic), &d, &info) == -BMDS_EINVAL);
        if (file != NULL) {
                /* A real JPEG: laid out as image_info says, the codec writes straight in. */
                FILE *f = fopen(file, "rb");
                CHECK(f != NULL);
                fseek(f, 0, SEEK_END);
                const long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                uint8_t *jpeg = malloc((size_t)n);
                CHECK(jpeg != NULL && fread(jpeg, 1, (size_t)n, f) == (size_t)n);
                fclose(f);
                memset(&d, 0, sizeof(d));
                d.size = sizeof(d);
                CHECK(bmds_image_info(-1, jpeg, (uint32_t)n, &d, &info) == 0);
                printf("image: %s %ux%u, decoder %u, buffer align %u stride %u bytes %u\n", file, (unsigned)info.width,
                       (unsigned)info.height, (unsigned)info.decoder, (unsigned)info.align, (unsigned)info.stride,
                       (unsigned)info.bytes);
                CHECK(info.decoder == BMDS_IMAGE_DECODER_HARDWARE && info.align == 64 && info.bytes != 0);
                void *out = bmds_mem_alloc(info.bytes, info.align, 0);
                CHECK(out != NULL);
                d.out = out;
                d.out_size = info.bytes;
                d.stride = info.stride;
                d.data = jpeg;
                d.length = (uint32_t)n;
                CHECK(bmds_image_decode(-1, &d, &info) == 0); /* its speed: `bench run images` */
                /* Reduced for a box: a software decode at 1/2 or more for a big one, else whole. */
                memset(&d, 0, sizeof(d));
                d.size = sizeof(d);
                d.box_width = 320;
                d.box_height = 240;
                CHECK(bmds_image_info(-1, jpeg, (uint32_t)n, &d, &info) == 0);
                printf("image: for 320x240: %ux%u by decoder %u at 1/%u\n", (unsigned)info.decode_width,
                       (unsigned)info.decode_height, (unsigned)info.decoder, 1u << info.shift);
                bmds_mem_free(out);
                free(jpeg);
        }
        unlink(names[0]);
        unlink(names[1]);
        printf("image: all passed\n");
        return 0;
}

/* Notifications (devplan/notifications.md §3.3): an events fd, posting with and without it,
   replacing one's own, closing, the errors; with SERVED, the panel's balloon expiring is read. */
/* Sockets (devplan/network.md §18) through musl, on loopback: nothing outside the board. */
static struct sockaddr_in loopback(uint16_t port) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return a;
}

static int64_t ms_now(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int net_test(void) {
        CHECK(bmds_sys_has(BMDS_SYS_NET_INFO));
        signal(SIGPIPE, SIG_IGN);

        /* What isn't there. */
        errno = 0;
        CHECK(socket(AF_INET6, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);
        CHECK(socket(AF_UNIX, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);
        CHECK(socket(AF_INET, SOCK_DGRAM, IPPROTO_TCP) < 0 && errno == EPROTONOSUPPORT);
        int p[2];
        CHECK(pipe(p) == 0);
        int       v = 1;
        socklen_t vl = sizeof(v);
        CHECK(getsockopt(p[0], SOL_SOCKET, SO_TYPE, &v, &vl) < 0 && errno == ENOTSOCK);
        close(p[0]);
        close(p[1]);

        /* A listener on an ephemeral port. */
        const int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(l >= 0);
        CHECK(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0);
        struct sockaddr_in a = loopback(0);
        CHECK(bind(l, (struct sockaddr *)&a, sizeof(a)) == 0);
        CHECK(listen(l, 4) == 0);
        socklen_t al = sizeof(a);
        CHECK(getsockname(l, (struct sockaddr *)&a, &al) == 0 && al == sizeof(a) && ntohs(a.sin_port) != 0);
        CHECK(getsockopt(l, SOL_SOCKET, SO_ACCEPTCONN, &v, &vl) == 0 && v == 1);
        CHECK(getsockopt(l, SOL_SOCKET, SO_TYPE, &v, &vl) == 0 && v == SOCK_STREAM);
        CHECK(getsockopt(l, SOL_SOCKET, SO_DOMAIN, &v, &vl) == 0 && v == AF_INET);
        CHECK(getsockopt(l, SOL_SOCKET, SO_PROTOCOL, &v, &vl) == 0 && v == IPPROTO_TCP);
        CHECK(fcntl(l, F_GETFD) == FD_CLOEXEC);
        struct sockaddr_in bad = loopback(0);
        bad.sin_addr.s_addr = inet_addr("10.255.255.1");
        const int t = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(t >= 0);
        CHECK(bind(t, (struct sockaddr *)&bad, sizeof(bad)) < 0 && errno == EADDRNOTAVAIL);
        CHECK(bind(t, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == EADDRINUSE);
        close(t);

        /* Connecting without waiting: EINPROGRESS, then writable and SO_ERROR 0. */
        const int c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        CHECK(c >= 0);
        CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0 || errno == EINPROGRESS);
        struct pollfd pc = {.fd = c, .events = POLLOUT};
        CHECK(poll(&pc, 1, 2000) == 1 && (pc.revents & POLLOUT));
        int       err = -1;
        socklen_t el = sizeof(err);
        CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
        struct pollfd pl = {.fd = l, .events = POLLIN};
        CHECK(poll(&pl, 1, 2000) == 1 && (pl.revents & POLLIN));
        struct sockaddr_in peer;
        socklen_t          pl2 = sizeof(peer);
        const int          s = accept4(l, (struct sockaddr *)&peer, &pl2, SOCK_NONBLOCK);
        CHECK(s >= 0);
        CHECK(peer.sin_family == AF_INET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(fcntl(s, F_GETFL) & O_NONBLOCK);
        char buf[4096];
        CHECK(recv(s, buf, sizeof(buf), 0) < 0 && errno == EAGAIN);
        CHECK(read(s, buf, sizeof(buf)) < 0 && errno == EAGAIN);

        /* Both ways; a peek; TCP_NODELAY. */
        CHECK(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0);
        CHECK(getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, &vl) == 0 && v == 1);
        CHECK(write(c, "hello", 5) == 5);
        struct pollfd ps = {.fd = s, .events = POLLIN};
        CHECK(poll(&ps, 1, 2000) == 1);
        CHECK(recv(s, buf, sizeof(buf), MSG_PEEK) == 5 && memcmp(buf, "hello", 5) == 0);
        CHECK(read(s, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0);
        /* 64 KB the other way, sent without waiting: what fits each time. */
        static char big[65536];
        for (size_t i = 0; i < sizeof(big); ++i) big[i] = (char)(i * 13);
        size_t        sent = 0, got = 0;
        const int64_t start = ms_now();
        while (got < sizeof(big) && ms_now() - start < 10000) {
                if (sent < sizeof(big)) {
                        const ssize_t n = send(s, big + sent, sizeof(big) - sent, 0);
                        if (n > 0)
                                sent += (size_t)n;
                        else
                                CHECK(errno == EAGAIN);
                }
                const ssize_t n = recv(c, buf, sizeof(buf), 0);
                if (n > 0) {
                        CHECK(memcmp(buf, big + got, (size_t)n) == 0);
                        got += (size_t)n;
                } else {
                        CHECK(n < 0 && errno == EAGAIN);
                        struct pollfd w[2] = {{.fd = c, .events = POLLIN},
                                              {.fd = s, .events = sent < sizeof(big) ? POLLOUT : 0}};
                        CHECK(poll(w, 2, 1000) >= 1);
                }
        }
        CHECK(sent == sizeof(big) && got == sizeof(big));

        /* Half close: the reader sees the end; the other way still works. */
        CHECK(shutdown(c, SHUT_WR) == 0);
        CHECK(poll(&ps, 1, 2000) == 1);
        CHECK(read(s, buf, sizeof(buf)) == 0);
        CHECK(write(c, "x", 1) < 0 && errno == EPIPE); /* SIGPIPE ignored */
        CHECK(send(s, "bye", 3, MSG_NOSIGNAL) == 3);
        struct pollfd pcin = {.fd = c, .events = POLLIN};
        CHECK(poll(&pcin, 1, 2000) == 1 && read(c, buf, sizeof(buf)) == 3);
        close(s);
        CHECK(poll(&pcin, 1, 2000) == 1 && read(c, buf, sizeof(buf)) == 0);
        close(c);

        /* Refused; a receive timeout. */
        const int          r = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in closed = loopback(9);
        CHECK(connect(r, (struct sockaddr *)&closed, sizeof(closed)) < 0 && errno == ECONNREFUSED);
        close(r);
        const int u = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(u >= 0);
        struct sockaddr_in ua = loopback(0);
        CHECK(bind(u, (struct sockaddr *)&ua, sizeof(ua)) == 0);
        al = sizeof(ua);
        CHECK(getsockname(u, (struct sockaddr *)&ua, &al) == 0);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        CHECK(setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
        struct timeval tv2;
        socklen_t      tl = sizeof(tv2);
        CHECK(getsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv2, &tl) == 0 && tv2.tv_sec == 0 && tv2.tv_usec == 100000);
        int64_t t0 = ms_now();
        CHECK(recv(u, buf, sizeof(buf), 0) < 0 && errno == EAGAIN);
        CHECK(ms_now() - t0 >= 90);

        /* UDP: the source, a cut datagram, MSG_TRUNC. */
        const int us = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(sendto(us, "datagram", 8, 0, (struct sockaddr *)&ua, sizeof(ua)) == 8);
        struct sockaddr_in from;
        socklen_t          fl = sizeof(from);
        CHECK(recvfrom(u, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) == 8 && memcmp(buf, "datagram", 8) == 0);
        CHECK(fl == sizeof(from) && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port != 0);
        CHECK(sendto(us, "0123456789", 10, 0, (struct sockaddr *)&ua, sizeof(ua)) == 10);
        struct iovec  iov = {buf, 4};
        struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov, .msg_iovlen = 1};
        CHECK(recvmsg(u, &m, 0) == 4 && (m.msg_flags & MSG_TRUNC));
        CHECK(sendto(us, "0123456789", 10, 0, (struct sockaddr *)&ua, sizeof(ua)) == 10);
        CHECK(recv(u, buf, 4, MSG_TRUNC) == 10);
        CHECK(send(us, "x", 1, 0) < 0 && errno == EDESTADDRREQ);
        close(us);
        close(u);

        /* Raw ICMP: an echo to 127.0.0.1 and its reply, with the IP header. */
        const int ic = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        CHECK(ic >= 0);
        unsigned char echo[16] = {8, 0, 0, 0, 0x42, 0x42, 0, 1, 'p', 'r', 'o', 'g', 't', 'e', 's', 't'};
        uint32_t      sum = 0;
        for (size_t i = 0; i < sizeof(echo); i += 2) sum += (uint32_t)(echo[i] << 8 | echo[i + 1]);
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        sum = ~sum & 0xffff;
        echo[2] = (unsigned char)(sum >> 8);
        echo[3] = (unsigned char)sum;
        struct sockaddr_in lo = loopback(0);
        CHECK(sendto(ic, echo, sizeof(echo), 0, (struct sockaddr *)&lo, sizeof(lo)) == sizeof(echo));
        int replied = 0;
        for (int i = 0; i < 4 && !replied; ++i) {
                struct pollfd pi = {.fd = ic, .events = POLLIN};
                CHECK(poll(&pi, 1, 1000) == 1);
                const ssize_t n = recv(ic, buf, sizeof(buf), 0);
                CHECK(n >= 20 + (ssize_t)sizeof(echo));
                const int ihl = (buf[0] & 0x0f) * 4;
                replied = (unsigned char)buf[ihl] == 0 && (unsigned char)buf[ihl + 4] == 0x42;
        }
        CHECK(replied);
        close(ic);

        /* The interfaces: net_info and the ioctls agree. */
        bmds_net_info_t info;
        int             interfaces = 0, eth = -1;
        for (uint32_t i = 0;; ++i) {
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (bmds_net_info(i, &info) < 0) break;
                struct in_addr ip = {info.address};
                printf("net: %s index %u flags %#x mtu %u %u Mbit/s %s mode %u dhcp %u lease %u/%u s\n", info.name,
                       (unsigned)info.index, (unsigned)info.flags, (unsigned)info.mtu, (unsigned)info.speed_mbps,
                       inet_ntoa(ip), info.mode, info.dhcp, (unsigned)info.lease_left_s, (unsigned)info.lease_s);
                if (strncmp(info.name, "eth", 3) == 0 && eth < 0) eth = (int)i;
                ++interfaces;
        }
        CHECK(interfaces >= 2);
        info.size = sizeof(info);
        CHECK(bmds_net_info((uint32_t)interfaces, &info) == -BMDS_ENODEV);
        CHECK(bmds_net_info(0, NULL) == -BMDS_EFAULT);
        const int    q = socket(AF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strcpy(ifr.ifr_name, "lo");
        CHECK(ioctl(q, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_LOOPBACK) && (ifr.ifr_flags & IFF_UP));
        CHECK(ioctl(q, SIOCGIFADDR, &ifr) == 0);
        CHECK(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(ioctl(q, SIOCGIFINDEX, &ifr) == 0 && ifr.ifr_ifindex > 0);
        memset(&ifr, 0, sizeof(ifr));
        strcpy(ifr.ifr_name, "nope0");
        CHECK(ioctl(q, SIOCGIFFLAGS, &ifr) < 0 && errno == ENODEV);
        if (eth >= 0) {
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                CHECK(bmds_net_info((uint32_t)eth, &info) == 0);
                memset(&ifr, 0, sizeof(ifr));
                strcpy(ifr.ifr_name, info.name);
                CHECK(ioctl(q, SIOCGIFHWADDR, &ifr) == 0 && memcmp(ifr.ifr_hwaddr.sa_data, info.mac, 6) == 0);
                CHECK(ioctl(q, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu == (int)info.mtu);
                CHECK(ioctl(q, SIOCGIFINDEX, &ifr) == 0 && ifr.ifr_ifindex == (int)info.index);
                memset(&ifr, 0, sizeof(ifr));
                ifr.ifr_ifindex = (int)info.index;
                CHECK(ioctl(q, SIOCGIFNAME, &ifr) == 0 && strcmp(ifr.ifr_name, info.name) == 0);
        }
        struct ifreq  reqs[8];
        struct ifconf conf = {.ifc_len = sizeof(reqs), .ifc_req = reqs};
        CHECK(ioctl(q, SIOCGIFCONF, &conf) == 0 && conf.ifc_len >= (int)sizeof(struct ifreq));
        close(q);

        /* net_watch: nothing to read until a change; whole records; its errors. */
        const int w = bmds_net_watch(BMDS_O_CLOEXEC);
        CHECK(w >= 0 && (fcntl(w, F_GETFD) & FD_CLOEXEC));
        struct pollfd pw = {.fd = w, .events = POLLIN};
        CHECK(poll(&pw, 1, 0) == 0);
        CHECK(read(w, buf, 3) < 0 && errno == EINVAL);
        CHECK(write(w, "x", 1) < 0);
        close(w);
        CHECK(bmds_net_watch(0x80000000u) == -BMDS_EINVAL);
        close(l);
        printf("net: all passed\n");
        return 0;
}

/* Real-time sockets (network.md §30f). Linux's numbers: musl has no <linux/net_tstamp.h>. */
#define RT_TX_HARDWARE (1 << 0)
#define RT_TX_SOFTWARE (1 << 1)
#define RT_RX_HARDWARE (1 << 2)
#define RT_RX_SOFTWARE (1 << 3)
#define RT_SOFTWARE (1 << 4)
#define RT_RAW_HARDWARE (1 << 6)
#define RT_OPT_ID (1 << 7)
#define RT_OPT_CMSG (1 << 10)
#define RT_OPT_TSONLY (1 << 11)

struct rt_time {
                int64_t sec, nsec;
};

struct rt_err {
                uint32_t ee_errno;
                uint8_t  ee_origin, ee_type, ee_code, ee_pad;
                uint32_t ee_info, ee_data;
};

/* What recvmsg's control messages said. */
struct rt_got {
                struct rt_time ts[3];
                int            stamped, timestampns, recverr;
                struct rt_err  err;
                struct rt_time ns;
};

static void rt_control(struct msghdr *m, struct rt_got *g) {
        memset(g, 0, sizeof(*g));
        for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c != NULL; c = CMSG_NXTHDR(m, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING &&
                    c->cmsg_len == CMSG_LEN(sizeof(g->ts))) {
                        memcpy(g->ts, CMSG_DATA(c), sizeof(g->ts));
                        g->stamped = 1;
                }
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPNS &&
                    c->cmsg_len == CMSG_LEN(sizeof(g->ns))) {
                        memcpy(&g->ns, CMSG_DATA(c), sizeof(g->ns));
                        g->timestampns = 1;
                }
                if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR &&
                    c->cmsg_len >= CMSG_LEN(sizeof(g->err))) {
                        memcpy(&g->err, CMSG_DATA(c), sizeof(g->err));
                        g->recverr = 1;
                }
        }
        return;
}

static int64_t rt_ns(const struct rt_time *t) {
        return t->sec * 1000000000LL + t->nsec;
}

static int64_t realtime_ns(void) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Sends to the reflector at @p peer from @p fd, stamped; checks the error queue's report (number
 * @p id, the frame unless @p tsonly) and the reflection's receive stamp. */
static int rt_round(int fd, const struct sockaddr_in *peer, uint32_t id, int tsonly, const char *what) {
        char          out[200], buf[1600], control[256];
        struct rt_got g;
        memset(out, 0, sizeof(out));
        snprintf(out, sizeof(out), "progtest %s %u", what, (unsigned)id);
        CHECK(sendto(fd, out, sizeof(out), 0, (const struct sockaddr *)peer, sizeof(*peer)) == sizeof(out));
        struct pollfd pe = {.fd = fd, .events = POLLPRI};
        CHECK(poll(&pe, 1, 1000) == 1 && (pe.revents & POLLPRI) && (pe.revents & POLLERR));
        struct iovec  iov = {buf, sizeof(buf)};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        const ssize_t n = recvmsg(fd, &m, MSG_ERRQUEUE);
        CHECK(n == (tsonly ? 0 : 42 + (ssize_t)sizeof(out)));
        CHECK((m.msg_flags & MSG_ERRQUEUE) && m.msg_namelen == 0);
        if (!tsonly) CHECK(buf[12] == 0x08 && buf[13] == 0x00 && memcmp(buf + 42, out, sizeof(out)) == 0);
        rt_control(&m, &g);
        CHECK(g.stamped && g.recverr && rt_ns(&g.ts[2]) != 0 && rt_ns(&g.ts[0]) == 0);
        CHECK(g.err.ee_errno == ENOMSG && g.err.ee_origin == 4 && g.err.ee_info == 0 && g.err.ee_data == id);
        const int64_t sent = rt_ns(&g.ts[2]);
        CHECK(recvmsg(fd, &m, MSG_ERRQUEUE) < 0 && errno == EAGAIN);
        /* The reflection, with its hardware arrival. */
        struct pollfd pi = {.fd = fd, .events = POLLIN};
        CHECK(poll(&pi, 1, 1000) == 1);
        iov.iov_len = sizeof(buf);
        m.msg_controllen = sizeof(control);
        CHECK(recvmsg(fd, &m, 0) == sizeof(out) && memcmp(buf, out, sizeof(out)) == 0);
        rt_control(&m, &g);
        CHECK(g.stamped && rt_ns(&g.ts[2]) > sent && rt_ns(&g.ts[0]) != 0);
        printf("rtnet: %s %u: round trip %lld us (hardware stamps)\n", what, (unsigned)id,
               (long long)((rt_ns(&g.ts[2]) - sent) / 1000));
        return 0;
}

static int rt_net_test(const char *peerName) {
        char buf[256], control[256];

        /* Priority: set, and from IP_TOS as Linux (EF 4, low delay 6). */
        const int u = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(u >= 0);
        int       v = 6;
        socklen_t vl = sizeof(v);
        CHECK(setsockopt(u, SOL_SOCKET, SO_PRIORITY, &v, sizeof(v)) == 0);
        CHECK(getsockopt(u, SOL_SOCKET, SO_PRIORITY, &v, &vl) == 0 && v == 6);
        v = 0xb8;
        CHECK(setsockopt(u, IPPROTO_IP, IP_TOS, &v, sizeof(v)) == 0);
        CHECK(getsockopt(u, SOL_SOCKET, SO_PRIORITY, &v, &vl) == 0 && v == 4);
        v = 0x10;
        CHECK(setsockopt(u, IPPROTO_IP, IP_TOS, &v, sizeof(v)) == 0);
        CHECK(getsockopt(u, SOL_SOCKET, SO_PRIORITY, &v, &vl) == 0 && v == 6);

        /* SO_TIMESTAMPING: Linux's flags only; read back as struct so_timestamping. */
        v = 1 << 30;
        CHECK(setsockopt(u, SOL_SOCKET, SO_TIMESTAMPING, &v, sizeof(v)) < 0 && errno == EINVAL);
        v = RT_RX_SOFTWARE | RT_SOFTWARE | RT_RX_HARDWARE | RT_RAW_HARDWARE;
        CHECK(setsockopt(u, SOL_SOCKET, SO_TIMESTAMPING, &v, sizeof(v)) == 0);
        int ts[2] = {-1, -1};
        vl = sizeof(ts);
        CHECK(getsockopt(u, SOL_SOCKET, SO_TIMESTAMPING, ts, &vl) == 0 && vl == sizeof(ts) && ts[0] == v && ts[1] == 0);
        v = 1;
        CHECK(setsockopt(u, SOL_SOCKET, SO_TIMESTAMPNS, &v, sizeof(v)) == 0);

        /* Received on loopback: the system's time in both (no hardware: lo isn't Ethernet). */
        struct sockaddr_in ua = loopback(0);
        socklen_t          al = sizeof(ua);
        CHECK(bind(u, (struct sockaddr *)&ua, sizeof(ua)) == 0 && getsockname(u, (struct sockaddr *)&ua, &al) == 0);
        const int     us = socket(AF_INET, SOCK_DGRAM, 0);
        const int64_t before = realtime_ns();
        CHECK(sendto(us, "stamped", 7, 0, (struct sockaddr *)&ua, sizeof(ua)) == 7);
        struct iovec  iov = {buf, sizeof(buf)};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        CHECK(recvmsg(u, &m, 0) == 7 && (m.msg_flags & MSG_CTRUNC) == 0);
        struct rt_got g;
        rt_control(&m, &g);
        CHECK(g.stamped && g.timestampns && rt_ns(&g.ts[2]) == 0 && rt_ns(&g.ts[0]) == rt_ns(&g.ns));
        CHECK(rt_ns(&g.ns) >= before - 1000000 && rt_ns(&g.ns) <= realtime_ns() + 1000000);
        /* Too little room: cut, said. */
        CHECK(sendto(us, "stamped", 7, 0, (struct sockaddr *)&ua, sizeof(ua)) == 7);
        m.msg_controllen = CMSG_SPACE(sizeof(struct rt_time));
        CHECK(recvmsg(u, &m, 0) == 7 && (m.msg_flags & MSG_CTRUNC));

        /* The error queue: empty answers EAGAIN at once, waiting or not. */
        const int64_t t0 = ms_now();
        m.msg_controllen = sizeof(control);
        CHECK(recvmsg(u, &m, MSG_ERRQUEUE) < 0 && errno == EAGAIN && ms_now() - t0 < 50);
        struct pollfd pu = {.fd = u, .events = POLLPRI};
        CHECK(poll(&pu, 1, 0) == 0);
        close(us);

        /* The fast path: UDP only; not on loopback; read back as an int or the struct. */
        const int t = socket(AF_INET, SOCK_STREAM, 0);
        v = 1;
        CHECK(setsockopt(t, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, sizeof(v)) < 0 && errno == ENOPROTOOPT);
        close(t);
        CHECK(setsockopt(u, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, sizeof(v)) < 0 && errno == ENODEV);
        close(u);
        const int       f = socket(AF_INET, SOCK_DGRAM, 0);
        bmds_fastpath_t fp = {.size = sizeof(fp), .packet_us = 1000, .depth = 9};
        CHECK(setsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &fp, sizeof(fp)) < 0 && errno == EINVAL);
        fp.depth = 2;
        CHECK(setsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &fp, sizeof(fp)) == 0);
        struct sockaddr_in fa;
        al = sizeof(fa);
        CHECK(getsockname(f, (struct sockaddr *)&fa, &al) == 0 && fa.sin_port != 0); /* bound for it */
        bmds_fastpath_t back = {.size = sizeof(back)};
        vl = sizeof(back);
        CHECK(getsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &back, &vl) == 0 && back.depth == 2 &&
              back.packet_us == 1000);
        vl = sizeof(v);
        CHECK(getsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, &vl) == 0 && v == 1);
        /* Another on the same port: taken. */
        const int f2 = socket(AF_INET, SOCK_DGRAM, 0);
        v = 1;
        CHECK(setsockopt(f2, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0);
        CHECK(setsockopt(f, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0);
        if (bind(f2, (struct sockaddr *)&fa, sizeof(fa)) == 0)
                CHECK(setsockopt(f2, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, sizeof(v)) < 0 && errno == EADDRINUSE);
        close(f2);
        v = 0;
        CHECK(setsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, sizeof(v)) == 0);
        vl = sizeof(v);
        CHECK(getsockopt(f, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, &vl) == 0 && v == 0);
        close(f);

        if (peerName == NULL) {
                printf("rtnet: all passed (no peer: no stamps sent)\n");
                return 0;
        }

        /* Against the reflector: the fast path, then lwIP, each stamped both ways. */
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_port = htons(5004);
        CHECK(inet_aton(peerName, &peer.sin_addr) != 0);
        for (int fast = 1; fast >= 0; --fast) {
                for (int tsonly = 0; tsonly <= 1; ++tsonly) {
                        const int s = socket(AF_INET, SOCK_DGRAM, 0);
                        CHECK(s >= 0);
                        v = 1;
                        CHECK(setsockopt(s, SOL_SOCKET, SO_SELECT_ERR_QUEUE, &v, sizeof(v)) == 0);
                        v = RT_TX_HARDWARE | RT_RX_HARDWARE | RT_RX_SOFTWARE | RT_SOFTWARE | RT_RAW_HARDWARE |
                            RT_OPT_ID | (tsonly ? RT_OPT_TSONLY : 0);
                        CHECK(setsockopt(s, SOL_SOCKET, SO_TIMESTAMPING, &v, sizeof(v)) == 0);
                        v = 1;
                        if (fast) CHECK(setsockopt(s, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, sizeof(v)) == 0);
                        CHECK(connect(s, (struct sockaddr *)&peer, sizeof(peer)) == 0);
                        /* The first may go through lwIP while the flow's ARP is answered. */
                        for (uint32_t id = 0; id < 4; ++id) {
                                if (rt_round(s, &peer, id, tsonly, fast ? "fast" : "lwip") != 0) return 1;
                        }
                        close(s);
                }
        }
        printf("rtnet: all passed\n");
        return 0;
}

/* Interval timers (devplan/programs.md §18): alarm, setitimer, getitimer. */
/* --- Scheduling (processes.md §3.12) ---------------------------------- */

static bmds_sched_t sched_of(pid_t tid) {
        bmds_sched_t s = {.size = sizeof(s)};
        if (bmds_sched(BMDS_SCHED_GET, (int32_t)tid, &s) != 0) s.policy = 99;
        return s;
}

static volatile int   sched_go;
static volatile pid_t sched_tid;

static void *sched_child(void *arg) {
        (void)arg;
        sched_tid = gettid();
        while (!sched_go) usleep(1000);
        return (void *)(intptr_t)sched_getscheduler(0);
}

/* Lateness of @p n clock_nanosleep()s to an absolute time @p period_us apart: median and max (µs). */
static void sleep_lateness(int n, int period_us, int64_t *median, int64_t *max) {
        static int64_t  late[1000];
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (n > 1000) n = 1000;
        for (int i = 0; i < n; ++i) {
                t.tv_nsec += period_us * 1000L;
                while (t.tv_nsec >= 1000000000L) {
                        t.tv_nsec -= 1000000000L;
                        ++t.tv_sec;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                late[i] = ((int64_t)(now.tv_sec - t.tv_sec) * 1000000000 + (now.tv_nsec - t.tv_nsec)) / 1000;
        }
        for (int i = 1; i < n; ++i) /* insertion sort: n is small */
                for (int j = i; j > 0 && late[j - 1] > late[j]; --j) {
                        const int64_t x = late[j];
                        late[j] = late[j - 1];
                        late[j - 1] = x;
                }
        *median = late[n / 2];
        *max = late[n - 1];
}

static volatile int64_t spin_until_ms;

static void *spin_rt(void *arg) {
        *(volatile pid_t *)arg = gettid();
        while (now_ms() < spin_until_ms) {}
        return NULL;
}

static int sched_test(const char *self) {
        CHECK(bmds_sys_has(BMDS_SYS_SCHED));
        /* What every thread starts with. */
        const bmds_sched_t first = sched_of(0);
        CHECK(first.policy == BMDS_SCHED_OTHER && first.priority == 0);
        CHECK(sched_getscheduler(0) == SCHED_OTHER);
        struct sched_param sp = {.sched_priority = 7};
        CHECK(sched_getparam(0, &sp) == 0 && sp.sched_priority == 0);
        CHECK(sched_get_priority_min(SCHED_FIFO) == 1 && sched_get_priority_max(SCHED_RR) == 99);
        CHECK(sched_get_priority_min(SCHED_OTHER) == 0 && sched_get_priority_max(SCHED_OTHER) == 0);
        CHECK(sched_get_priority_max(42) == -1 && errno == EINVAL);
        struct timespec slice;
        CHECK(sched_rr_get_interval(0, &slice) == 0 && slice.tv_sec == 0 && slice.tv_nsec == 1000000);
        CHECK(sched_yield() == 0);
        /* Cores: the first thread is pinned to one at its start (processes.md §3.12a), and the
         * system still has two. */
        const int cpu = sched_getcpu();
        CHECK(cpu == 0 || cpu == 1);
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CHECK(sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_COUNT(&cpus) == 1 && CPU_ISSET(cpu, &cpus));
        CHECK(sysconf(_SC_NPROCESSORS_ONLN) == 2 && sysconf(_SC_NPROCESSORS_CONF) == 2);
        CHECK(sched_setaffinity(0, sizeof(cpus), &cpus) == 0); /* its own core: no change */
        cpu_set_t both;
        CPU_ZERO(&both);
        CPU_SET(0, &both);
        CPU_SET(1, &both);
        CHECK(sched_setaffinity(0, sizeof(both), &both) == 0); /* all the cores: a no-op */
        cpu_set_t other;
        CPU_ZERO(&other);
        CPU_SET(1 - cpu, &other);
        CHECK(sched_setaffinity(0, sizeof(other), &other) == -1 && errno == EINVAL); /* pinned */
        CHECK(sched_getcpu() == cpu);
        /* The errors. */
        sp.sched_priority = 0;
        CHECK(sched_setscheduler(0, SCHED_FIFO, &sp) == -1 && errno == EINVAL);
        sp.sched_priority = 100;
        CHECK(sched_setscheduler(0, SCHED_RR, &sp) == -1 && errno == EINVAL);
        sp.sched_priority = 5;
        CHECK(sched_setscheduler(0, SCHED_OTHER, &sp) == -1 && errno == EINVAL);
        sp.sched_priority = 0;
        CHECK(sched_setscheduler(0, 4, &sp) == -1 && errno == EINVAL);
        CHECK(sched_setscheduler(0, SCHED_OTHER, NULL) == -1 && errno == EINVAL);
        CHECK(sched_getscheduler(30000) == -1 && errno == ESRCH);
        CHECK(sched_getscheduler(-1) == -1 && errno == EINVAL);
        bmds_sched_t bad = {.size = sizeof(bad), .flags = 1};
        CHECK(bmds_sched(BMDS_SCHED_SET, 0, &bad) == -BMDS_EINVAL);
        CHECK(bmds_sched(7, 0, &bad) == -BMDS_EINVAL);
        CHECK(bmds_sched(BMDS_SCHED_YIELD, 0, &bad) == -BMDS_EINVAL);
        CHECK(bmds_sched(BMDS_SCHED_GET, 0, NULL) == -BMDS_EFAULT);
        CHECK(sched_getscheduler(0) == SCHED_OTHER); /* nothing changed */
        /* The tick's sleeps, for comparison: 1.3 ms apart, so their deadlines walk round the 1 ms tick.
           1 ms apart they all fell at one phase of it, and a run that began just before a tick saw
           every sleep end under 100 us late (2 failures in 9 runs). */
        int64_t tick_median, tick_max;
        sleep_lateness(200, 1300, &tick_median, &tick_max);
        /* Real time: FIFO 50, read back by both calls; the kernel priority in the band. */
        sp.sched_priority = 50;
        CHECK(sched_setscheduler(0, SCHED_FIFO, &sp) == 0);
        CHECK(sched_getscheduler(0) == SCHED_FIFO);
        CHECK(sched_getparam(0, &sp) == 0 && sp.sched_priority == 50);
        bmds_sched_t s = sched_of(0);
        CHECK(s.policy == BMDS_SCHED_FIFO && s.priority == 50 && s.core == cpu && s.cores == (1u << cpu));
        CHECK(s.kernel_priority > first.kernel_priority && s.kernel_priority < 19);
        printf("sched: OTHER at kernel priority %u, FIFO 50 at %u\n", (unsigned)first.kernel_priority,
               (unsigned)s.kernel_priority);
        sp.sched_priority = 99;
        CHECK(sched_setparam(0, &sp) == 0 && sched_getscheduler(0) == SCHED_FIFO);
        CHECK(sched_of(0).kernel_priority > s.kernel_priority);
        sp.sched_priority = 50;
        CHECK(sched_setparam(0, &sp) == 0);
        /* A real-time sleep ends at its time. */
        int64_t rt_median, rt_max;
        sleep_lateness(1000, 1000, &rt_median, &rt_max);
        CHECK(rt_median < 100 && tick_median >= 100); /* the figures: `bench run kernel.sleep` */
        (void)rt_max;
        (void)tick_max;
        /* A thread inherits; set and read back from here; another thread's explicit policy. */
        pthread_t t;
        sched_go = 0;
        sched_tid = 0;
        CHECK(pthread_create(&t, NULL, sched_child, NULL) == 0);
        while (sched_tid == 0) usleep(1000);
        int                policy = -1;
        struct sched_param tp = {0};
        CHECK(pthread_getschedparam(t, &policy, &tp) == 0 && policy == SCHED_FIFO && tp.sched_priority == 50);
        tp.sched_priority = 10;
        CHECK(pthread_setschedparam(t, SCHED_RR, &tp) == 0);
        CHECK(sched_of(sched_tid).policy == BMDS_SCHED_RR && sched_of(sched_tid).priority == 10);
        CHECK(sched_of(sched_tid).core == -1);
        CHECK(pthread_setschedprio(t, 20) == 0 && sched_of(sched_tid).priority == 20);
        tp.sched_priority = 0;
        CHECK(pthread_setschedparam(t, SCHED_OTHER, &tp) == 0);
        sched_go = 1;
        void *r = NULL;
        CHECK(pthread_join(t, &r) == 0 && (intptr_t)r == SCHED_OTHER);
        pthread_attr_t a;
        CHECK(pthread_attr_init(&a) == 0 && pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED) == 0);
        tp.sched_priority = 30;
        CHECK(pthread_attr_setschedpolicy(&a, SCHED_RR) == 0 && pthread_attr_setschedparam(&a, &tp) == 0);
        sched_go = 1;
        CHECK(pthread_create(&t, &a, sched_child, NULL) == 0 && pthread_join(t, &r) == 0 && (intptr_t)r == SCHED_RR);
        pthread_attr_destroy(&a);
        /* A process inherits, unless SCHED_RESET_ON_FORK; that flag reads back. */
        CHECK(status_of(start(self, NULL, NULL, "schedof", NULL)) == SCHED_FIFO);
        sp.sched_priority = 50;
        CHECK(sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0);
        CHECK(sched_getscheduler(0) == (SCHED_FIFO | SCHED_RESET_ON_FORK));
        CHECK(status_of(start(self, NULL, NULL, "schedof", NULL)) == SCHED_OTHER);
        sched_go = 0;
        sched_tid = 0;
        CHECK(pthread_create(&t, NULL, sched_child, NULL) == 0);
        while (sched_tid == 0) usleep(1000);
        CHECK(sched_of(sched_tid).policy == BMDS_SCHED_OTHER);
        sched_go = 1;
        CHECK(pthread_join(t, &r) == 0);
        /* Two busy real-time threads (both cores): held back for the rest of each second once
         * they have had 95 % of one, so this thread (normal priority again) still runs. */
        sp.sched_priority = 0;
        CHECK(sched_setscheduler(0, SCHED_OTHER, &sp) == 0);
        CHECK(sched_of(0).kernel_priority == first.kernel_priority);
        pthread_t      spinners[2];
        volatile pid_t ids[2] = {0, 0};
        CHECK(pthread_attr_init(&a) == 0 && pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED) == 0);
        tp.sched_priority = 1;
        CHECK(pthread_attr_setschedpolicy(&a, SCHED_FIFO) == 0 && pthread_attr_setschedparam(&a, &tp) == 0);
        spin_until_ms = now_ms() + 2500;
        for (int i = 0; i < 2; ++i) CHECK(pthread_create(&spinners[i], &a, spin_rt, (void *)&ids[i]) == 0);
        pthread_attr_destroy(&a);
        int      wakes = 0;
        int64_t  last = now_ms(), longest = 0;
        uint32_t throttled = 0; /* read while the thread is there */
        while (now_ms() < spin_until_ms - 100) {
                usleep(5000);
                const int64_t n = now_ms();
                if (n - last > longest) longest = n - last;
                last = n;
                ++wakes;
                const bmds_sched_t spinner = sched_of(ids[0]);
                if (spinner.policy == BMDS_SCHED_FIFO && spinner.throttled > throttled) throttled = spinner.throttled;
        }
        for (int i = 0; i < 2; ++i) CHECK(pthread_join(spinners[i], NULL) == 0);
        printf("sched: beside two busy real-time threads for 2.5 s: woke %d times, the longest gap %lld ms; "
               "throttled %u times\n",
               wakes, (long long)longest, (unsigned)throttled);
        CHECK(throttled >= 2 && wakes >= 10);
        printf("sched: all passed\n");
        return 0;
}

static int64_t timer_left_us(void) {
        struct itimerval it;
        if (getitimer(ITIMER_REAL, &it) != 0) return -1;
        return (int64_t)it.it_value.tv_sec * 1000000 + it.it_value.tv_usec;
}

static volatile pid_t alarm_tid;

static void note_alarm_thread(int sig) {
        alarm_tid = gettid();
        ++got[sig];
        return;
}

static void *wait_for_alarm(void *arg) {
        (void)arg;
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        while (got[SIGALRM] == 0) pause();
        return (void *)(intptr_t)gettid();
}

// What a child on a pty's program end sees of its terminal.
static int controlling(void) {
        struct winsize ws = {0};
        const int      ok = ioctl(0, TIOCGWINSZ, &ws) == 0;
        const int      tty = open("/dev/tty", O_RDWR | O_NOCTTY);
        printf("ctty: tty %d %d %d, sid %d, own %d, size %ux%u, /dev/tty %s\n", isatty(0), isatty(1), isatty(2),
               tcgetsid(0) == getpid(), ok, (unsigned)ws.ws_col, (unsigned)ws.ws_row, tty >= 0 ? "yes" : "no");
        if (tty >= 0) close(tty);
        return 0;
}

// Reads @p fd (a pty's emulator end) until its end, into @p out.
static size_t read_all(int fd, char *out, size_t size) {
        size_t got = 0;
        while (got + 1 < size) {
                const ssize_t n = read(fd, out + got, size - 1 - got);
                if (n <= 0) break;
                got += (size_t)n;
        }
        out[got] = '\0';
        return got;
}

// Hex to bytes (the vectors below).
static void unhex(const char *hex, uint8_t *out) {
        for (size_t i = 0; hex[2 * i] != '\0'; ++i) {
                unsigned v;
                sscanf(hex + 2 * i, "%2x", &v);
                out[i] = (uint8_t)v;
        }
        return;
}

static int32_t crypto_op(uint32_t algorithm, const uint8_t *key, uint32_t key_bytes, void *state, const void *in,
                         void *out, uint32_t length) {
        const bmds_crypto_op_t op = {.size = sizeof(op),
                                     .algorithm = algorithm,
                                     .key = key,
                                     .key_bytes = key_bytes,
                                     .state = state,
                                     .in = in,
                                     .out = out,
                                     .length = length};
        return bmds_crypto(&op);
}

// The engines through slot 112: published vectors, state carried across calls, then the speed.
static int cryptos(void) {
        uint8_t key[32], in[64], out[64], want[64], state[64];
        CHECK(bmds_sys_has(BMDS_SYS_CRYPTO));
        // FIPS 197 C.1 and C.3: one block each way.
        unhex("000102030405060708090a0b0c0d0e0f", key);
        unhex("00112233445566778899aabbccddeeff", in);
        unhex("69c4e0d86a7b0430d8cdb78070b4c55a", want);
        CHECK(crypto_op(BMDS_CRYPTO_AES_ECB_ENCRYPT, key, 16, NULL, in, out, 16) == 0 && memcmp(out, want, 16) == 0);
        CHECK(crypto_op(BMDS_CRYPTO_AES_ECB_DECRYPT, key, 16, NULL, out, out, 16) == 0 && memcmp(out, in, 16) == 0);
        unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
        unhex("8ea2b7ca516745bfeafc49904b496089", want);
        CHECK(crypto_op(BMDS_CRYPTO_AES_ECB_ENCRYPT, key, 32, NULL, in, out, 16) == 0 && memcmp(out, want, 16) == 0);
        // SP 800-38A F.2.1 (CBC) and F.5.1 (CTR), two blocks, the second call going on from the first.
        unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
        unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", in);
        unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want);
        unhex("000102030405060708090a0b0c0d0e0f", state);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CBC_ENCRYPT, key, 16, state, in, out, 16) == 0);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CBC_ENCRYPT, key, 16, state, in + 16, out + 16, 16) == 0);
        CHECK(memcmp(out, want, 32) == 0 && memcmp(state, want + 16, 16) == 0);
        unhex("000102030405060708090a0b0c0d0e0f", state);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CBC_DECRYPT, key, 16, state, out, out, 32) == 0 && memcmp(out, in, 32) == 0);
        unhex("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", want);
        unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", state);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CTR, key, 16, state, in, out, 16) == 0);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CTR, key, 16, state, in + 16, out + 16, 16) == 0);
        CHECK(memcmp(out, want, 32) == 0);
        unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdff01", want);
        CHECK(memcmp(state, want, 16) == 0);
        // FIPS 180 "abc": one padded block from each initial value (the words big-endian).
        memset(in, 0, sizeof(in));
        memcpy(in, "abc\x80", 4);
        in[63] = 24;
        unhex("67452301efcdab8998badcfe10325476c3d2e1f0", state);
        unhex("a9993e364706816aba3e25717850c26c9cd0d89d", want);
        CHECK(crypto_op(BMDS_CRYPTO_SHA1, NULL, 0, state, in, NULL, 64) == 0 && memcmp(state, want, 20) == 0);
        unhex("6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19", state);
        unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
        CHECK(crypto_op(BMDS_CRYPTO_SHA256, NULL, 0, state, in, NULL, 64) == 0 && memcmp(state, want, 32) == 0);
        static uint8_t block512[128 + 1];
        memcpy(block512 + 1, "abc\x80", 4); // unaligned on purpose
        block512[128] = 24;
        unhex("6a09e667f3bcc908bb67ae8584caa73b3c6ef372fe94f82ba54ff53a5f1d36f1"
              "510e527fade682d19b05688c2b3e6c1f1f83d9abfb41bd6b5be0cd19137e2179",
              state);
        unhex("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
              want);
        CHECK(crypto_op(BMDS_CRYPTO_SHA512, NULL, 0, state, block512 + 1, NULL, 128) == 0 &&
              memcmp(state, want, 64) == 0);
        // Errors.
        CHECK(crypto_op(BMDS_CRYPTO_AES_CTR, key, 16, state, in, out, 15) == -BMDS_EINVAL);
        CHECK(crypto_op(BMDS_CRYPTO_AES_ECB_ENCRYPT, key, 24, NULL, in, out, 16) == -BMDS_EINVAL);
        CHECK(crypto_op(BMDS_CRYPTO_AES_CBC_ENCRYPT, key, 16, NULL, in, out, 16) == -BMDS_EINVAL);
        CHECK(crypto_op(BMDS_CRYPTO_SHA256, NULL, 0, state, in, NULL, 32) == -BMDS_EINVAL);
        CHECK(crypto_op(99, key, 16, state, in, out, 16) == -BMDS_ENOTSUP);
        CHECK(bmds_crypto(NULL) == -BMDS_EFAULT);
        // Long calls go by DMA (network.md §29): each must equal the same bytes in one-block calls
        // (register mode), at sizes past a run (27 KB), misaligned, in place, the counter carrying
        // past 32 and 64 bits; the state left must match too.
        enum {
                Most = 60000 + 64
        };
        uint8_t *src = malloc(Most), *dst = malloc(Most), *ref = malloc(Most);
        CHECK(src != NULL && dst != NULL && ref != NULL);
        for (uint32_t i = 0; i < Most; ++i) src[i] = (uint8_t)(i * 131 + (i >> 8));
        static const uint32_t modes[] = {BMDS_CRYPTO_AES_CTR, BMDS_CRYPTO_AES_CBC_ENCRYPT, BMDS_CRYPTO_AES_CBC_DECRYPT,
                                         BMDS_CRYPTO_AES_ECB_ENCRYPT, BMDS_CRYPTO_AES_ECB_DECRYPT};
        static const uint32_t lengths[] = {1024, 1040, 4096, 16384, 27776, 27792, 60000};
        static const char *const starts[] = {"000102030405060708090a0b0c0d0e0f", "0001020304050607ffffffffffffff00",
                                             "00010203ffffffffffffffffffffff80", "fffffffffffffffffffffffffffffff0"};
        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
                const int ecb = modes[m] == BMDS_CRYPTO_AES_ECB_ENCRYPT || modes[m] == BMDS_CRYPTO_AES_ECB_DECRYPT;
                for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
                        const uint32_t len = lengths[l];
                        const size_t   start = modes[m] == BMDS_CRYPTO_AES_CTR ? l % 4 : 0;
                        const uint32_t in_off = (uint32_t)(l % 3) * 4, out_off = (uint32_t)(l % 4) * 20;
                        const int      in_place = l % 2 == 1;
                        uint8_t        s1[16], s2[16];
                        unhex(starts[start], s1);
                        memcpy(s2, s1, 16);
                        for (uint32_t at = 0; at < len; at += 16)
                                CHECK(crypto_op(modes[m], key, 32, ecb ? NULL : s1, src + in_off + at, ref + at, 16) ==
                                      0);
                        uint8_t *out = dst + out_off;
                        if (in_place) memcpy(out, src + in_off, len);
                        CHECK(crypto_op(modes[m], key, 32, ecb ? NULL : s2, in_place ? out : src + in_off, out, len) ==
                              0);
                        if (memcmp(out, ref, len) != 0 || (!ecb && memcmp(s1, s2, 16) != 0)) {
                                printf("crypto: mode %u, %u bytes: DMA differs\n", (unsigned)modes[m], (unsigned)len);
                                return 1;
                        }
                }
        }
        free(src);
        free(dst);
        free(ref);
        // Speed, from the heap (PSRAM): SSH's packets are up to 32 KB.
        uint8_t *big = malloc(16384);
        CHECK(big != NULL);
        memset(big, 0x5a, 16384);
        static const uint32_t sizes[] = {64, 512, 1024, 4096, 16384};
        static const struct {
                        const char *name;
                        uint32_t    algorithm;
        } speeds[] = {
                {"aes128-ctr", BMDS_CRYPTO_AES_CTR}, {"sha256", BMDS_CRYPTO_SHA256}, {"sha512", BMDS_CRYPTO_SHA512}};
        for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); ++i) {
                for (size_t z = 0; z < sizeof(sizes) / sizeof(sizes[0]); ++z) {
                        const uint32_t  size = sizes[z];
                        const uint32_t  n = 2000000 / size < 20000 ? 2000000 / size : 20000;
                        const uint32_t  len = speeds[i].algorithm == BMDS_CRYPTO_SHA512 && size == 64 ? 128 : size;
                        struct timespec t0, t1;
                        clock_gettime(CLOCK_MONOTONIC, &t0);
                        for (uint32_t k = 0; k < n; ++k)
                                CHECK(crypto_op(speeds[i].algorithm, key, 16, state, big, big, len) == 0);
                        clock_gettime(CLOCK_MONOTONIC, &t1);
                        const double s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
                        printf("crypto: %-10s %5u bytes %6.1f MB/s\n", speeds[i].name, (unsigned)len,
                               (double)len * n / s / 1e6);
                }
        }
        free(big);
        printf("crypto: ok\n");
        return 0;
}

// syslog() (musl's: an AF_UNIX datagram socket to /dev/log), then such a socket by hand.
static int syslogs(void) {
        openlog("progtest", LOG_PID, LOG_USER);
        syslog(LOG_WARNING, "a warning, %d", 42);
        syslog(LOG_INFO, "info, two\nlines");
        syslog(LOG_DEBUG, "debug (shown with log/level debug)");
        closelog();
        const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        CHECK(fd >= 0);
        struct sockaddr_un to = {.sun_family = AF_UNIX, .sun_path = "/dev/log"};
        CHECK(connect(fd, (struct sockaddr *)&to, sizeof(to)) == 0);
        static const char message[] = "<11>progtest-raw: an error, sent by hand";
        CHECK(send(fd, message, sizeof(message) - 1, 0) == (ssize_t)(sizeof(message) - 1));
        CHECK(sendto(fd, "<14>plain text", 14, 0, NULL, 0) == 14);
        struct sockaddr_un elsewhere = {.sun_family = AF_UNIX, .sun_path = "/tmp/nowhere"};
        CHECK(connect(fd, (struct sockaddr *)&elsewhere, sizeof(elsewhere)) < 0 && errno == ENOENT);
        CHECK(close(fd) == 0);
        CHECK(socket(AF_UNIX, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);
        printf("syslog: ok\n");
        return 0;
}

static int ptys(const char *self) {
        int            m = -1, s = -1;
        char           name[32];
        struct winsize ws = {.ws_col = 100, .ws_row = 30};
        CHECK(openpty(&m, &s, name, NULL, &ws) == 0);
        printf("pty: %s\n", name);
        CHECK(strncmp(name, "/dev/pts/", 9) == 0);
        CHECK(strcmp(ptsname(m), name) == 0);
        CHECK(isatty(m) == 0 || isatty(m) == 1);
        // The program end is a terminal with a name (ttyname: /proc/self/fd/N), before it is anyone's.
        CHECK(isatty(s) == 1);
        CHECK(ttyname(s) != NULL && strcmp(ttyname(s), name) == 0);
        struct termios tio;
        CHECK(tcgetattr(s, &tio) == 0 && (tio.c_lflag & ICANON) != 0);
        CHECK(tcsetattr(s, TCSANOW, &tio) == 0);
        struct stat st;
        CHECK(stat(name, &st) == 0 && S_ISCHR(st.st_mode));
        // Both ways: the program's output as CR LF; input as typed.
        CHECK(write(s, "out\n", 4) == 4);
        char buf[512];
        CHECK(read(m, buf, sizeof(buf)) == 5 && memcmp(buf, "out\r\n", 5) == 0);
        CHECK(write(m, "in", 2) == 2);
        CHECK(read(s, buf, sizeof(buf)) == 2 && memcmp(buf, "in", 2) == 0);
        // A child: a session of its own with the program end as its terminal. Its environment
        // changes before the exec don't reach ours (the memory is shared until then).
        CHECK(setenv("PT_KEEP", "kept", 1) == 0);
        const pid_t pid = vfork();
        if (pid == 0) {
                clearenv();
                (void)setenv("PT_CHILD", "1", 1);
                if (setsid() < 0 || ioctl(s, TIOCSCTTY, 0) != 0) _exit(3);
                if (dup2(s, 0) < 0 || dup2(s, 1) < 0 || dup2(s, 2) < 0) _exit(4);
                close(s);
                close(m);
                char *eargv[] = {"progtest", "ctty", NULL};
                execve(self, eargv, environ);
                _exit(5);
        }
        CHECK(pid > 0);
        CHECK(getenv("PT_KEEP") != NULL && strcmp(getenv("PT_KEEP"), "kept") == 0);
        CHECK(getenv("PT_CHILD") == NULL);
        close(s);
        const size_t n = read_all(m, buf, sizeof(buf));
        int          status = -1;
        CHECK(waitpid(pid, &status, 0) == pid);
        printf("pty: child said %.*s", (int)n, buf);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(strstr(buf, "ctty: tty 1 1 1, sid 1, own 1, size 100x30, /dev/tty yes") != NULL);
        // A new size; the pair goes with its last handle.
        ws.ws_col = 80;
        CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0);
        close(m);
        CHECK(stat(name, &st) != 0 && errno == ENOENT);
        // posix_openpt's way, and its errors.
        m = posix_openpt(O_RDWR | O_NOCTTY);
        CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0);
        s = open(ptsname(m), O_RDWR | O_NOCTTY);
        CHECK(s >= 0);
        unsigned int number = 99;
        CHECK(ioctl(s, TIOCGPTN, &number) != 0 && errno == ENOTTY); // the program's end has no number
        close(s);
        close(m);
        printf("pty: ok\n");
        return 0;
}

static int timers(const char *self) {
        struct itimerval it, old;
        int              fds[2];
        char             b = 0;
        // Nothing armed reads 0.
        CHECK(timer_left_us() == 0);
        // alarm() interrupts a blocking read (no SA_RESTART), never early.
        CHECK(catch_signal(SIGALRM, count_signal, 0) == 0 && pipe(fds) == 0);
        int64_t start_ms = ms_now();
        CHECK(alarm(1) == 0);
        CHECK(read(fds[0], &b, 1) == -1 && errno == EINTR && got[SIGALRM] == 1);
        int64_t took = ms_now() - start_ms;
        printf("timers: alarm(1) after %d ms\n", (int)took);
        CHECK(took >= 1000 && took < 1100);
        CHECK(close(fds[0]) == 0 && close(fds[1]) == 0 && timer_left_us() == 0);
        // alarm() returns what was left (rounded up); 0 disarms.
        CHECK(alarm(5) == 0 && alarm(0) == 5 && timer_left_us() == 0);
        // getitimer counts down; setitimer returns the old value and interval.
        memset(&it, 0, sizeof(it));
        it.it_value.tv_sec = 2;
        it.it_interval.tv_usec = 250000;
        CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0 && usleep(300000) == 0);
        const int64_t left = timer_left_us();
        CHECK(left > 1600000 && left <= 1700000);
        memset(&it, 0, sizeof(it));
        CHECK(setitimer(ITIMER_REAL, &it, &old) == 0);
        CHECK(old.it_interval.tv_sec == 0 && old.it_interval.tv_usec == 250000 && old.it_value.tv_sec == 1);
        CHECK(timer_left_us() == 0);
        // A repeating timer: three SIGALRMs 100 ms apart, never early.
        got[SIGALRM] = 0;
        it.it_value.tv_usec = 100000;
        it.it_interval.tv_usec = 100000;
        start_ms = ms_now();
        CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
        while (got[SIGALRM] < 3) pause();
        took = ms_now() - start_ms;
        memset(&it, 0, sizeof(it));
        CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
        printf("timers: 3 x 100 ms after %d ms\n", (int)took);
        CHECK(took >= 300 && took < 400);
        // Bad values and timers BMDS hasn't.
        it.it_value.tv_usec = 1000000;
        CHECK(setitimer(ITIMER_REAL, &it, NULL) == -1 && errno == EINVAL);
        it.it_value.tv_usec = 1000;
        CHECK(setitimer(ITIMER_PROF, &it, NULL) == -1 && errno == EINVAL);
        CHECK(signal(SIGALRM, SIG_DFL) == count_signal);
        // A child doesn't get the timer; exec keeps it; by default SIGALRM ends the process.
        CHECK(alarm(5) == 0);
        pid_t c = start(self, NULL, NULL, "timerleft", "0", NULL);
        CHECK(c > 0 && status_of(c) == 0);
        CHECK(alarm(0) > 0);
        c = start(self, NULL, NULL, "timerexec", self, NULL);
        CHECK(c > 0 && status_of(c) == 0);
        c = start(self, NULL, NULL, "alarmdie", NULL);
        CHECK(c > 0 && ending_of(c) == 1000 + SIGALRM);
        // Many processes that end with their timer armed: each frees its entry (32 of them).
        for (int i = 0; i < 100; ++i) {
                c = start(self, NULL, NULL, "alarmexit", NULL);
                CHECK(c > 0 && status_of(c) == 0);
        }
        // A process computing, never making a system call, still ends by its SIGALRM.
        start_ms = ms_now();
        c = start(self, NULL, NULL, "alarmspin", NULL);
        CHECK(c > 0 && ending_of(c) == 1000 + SIGALRM);
        took = ms_now() - start_ms;
        printf("timers: a spinning process ended after %d ms\n", (int)took);
        CHECK(took >= 1000 && took < 1500);
        // Several at once: each child checks its own fires on time, never early.
        pid_t at[3];
        CHECK((at[0] = start(self, NULL, NULL, "alarmat", "300", NULL)) > 0);
        CHECK((at[1] = start(self, NULL, NULL, "alarmat", "50", NULL)) > 0);
        CHECK((at[2] = start(self, NULL, NULL, "alarmat", "120", NULL)) > 0);
        for (int i = 0; i < 3; ++i) CHECK(status_of(at[i]) == 0);
        // SA_RESTART: a read the handler interrupted goes on and gets what comes later.
        CHECK(catch_signal(SIGALRM, count_signal, SA_RESTART) == 0 && pipe(fds) == 0);
        char fdbuf[16];
        snprintf(fdbuf, sizeof(fdbuf), "%d", fds[1]);
        got[SIGALRM] = 0;
        c = start(self, NULL, NULL, "later", "300", fdbuf, NULL);
        CHECK(c > 0 && close(fds[1]) == 0);
        CHECK(ualarm(100000, 0) == 0);
        CHECK(read(fds[0], &b, 1) == 1 && b == 'x' && got[SIGALRM] == 1);
        CHECK(status_of(c) == 0 && close(fds[0]) == 0);
        // Stopped when it fires: pending until continued, then handled.
        c = start(self, NULL, NULL, "alarmcatch", NULL);
        CHECK(c > 0 && usleep(50000) == 0 && kill(c, SIGSTOP) == 0 && usleep(400000) == 0);
        int st = 0;
        CHECK(waitpid(c, &st, WNOHANG) == 0);
        CHECK(kill(c, SIGCONT) == 0 && status_of(c) == 7);
        // Threads: the signal goes to the thread that doesn't block it.
        CHECK(catch_signal(SIGALRM, note_alarm_thread, 0) == 0);
        sigset_t set, was;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        CHECK(pthread_sigmask(SIG_BLOCK, &set, &was) == 0);
        got[SIGALRM] = 0;
        pthread_t t;
        void     *tid = NULL;
        CHECK(pthread_create(&t, NULL, wait_for_alarm, NULL) == 0);
        CHECK(ualarm(100000, 0) == 0);
        CHECK(pthread_join(t, &tid) == 0 && got[SIGALRM] == 1 && alarm_tid == (pid_t)(intptr_t)tid);
        CHECK(alarm_tid != gettid() && pthread_sigmask(SIG_SETMASK, &was, NULL) == 0);
        CHECK(signal(SIGALRM, SIG_DFL) == note_alarm_thread);
        // Long values: 63 years reads back, nothing overflows.
        memset(&it, 0, sizeof(it));
        it.it_value.tv_sec = 2000000000;
        it.it_interval.tv_sec = 2000000000;
        CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0 && getitimer(ITIMER_REAL, &old) == 0);
        CHECK(old.it_value.tv_sec >= 1999999999 && old.it_value.tv_sec <= 2000000000);
        CHECK(old.it_interval.tv_sec == 2000000000 && alarm(0) >= 1999999999u);
        printf("timers: all passed\n");
        return 0;
}

static int notify_test(int served) {
        CHECK(bmds_sys_has(BMDS_SYS_NOTIFY_CLOSE));
        const int fd = bmds_notify_open(BMDS_O_CLOEXEC);
        CHECK(fd >= 0);
        bmds_notification_t n;
        memset(&n, 0, sizeof(n));
        n.size = sizeof(n);
        n.urgency = BMDS_NOTIFY_LOW;
        n.flags = BMDS_NOTIFY_SILENT;
        n.timeout_ms = 400;
        snprintf(n.title, sizeof(n.title), "progtest");
        snprintf(n.body, sizeof(n.body), "a notification from progtest notify");
        const int32_t id = bmds_notify_send(fd, &n);
        CHECK(id > 0);
        /* Replacing it: the same id. */
        n.replaces = (uint32_t)id;
        snprintf(n.body, sizeof(n.body), "replaced");
        CHECK(bmds_notify_send(fd, &n) == id);
        /* Fire and forget: no fd. */
        n.replaces = 0;
        const int32_t other = bmds_notify_send(-1, &n);
        CHECK(other > 0 && other != id);
        CHECK(bmds_notify_close((uint32_t)other) == 0);
        CHECK(bmds_notify_close((uint32_t)other) == -BMDS_ENOENT);
        /* Errors: not an events fd, a bad urgency, too many actions, a bad fd. */
        CHECK(bmds_notify_send(1, &n) == -BMDS_ENOTTY);
        CHECK(bmds_notify_send(99, &n) == -BMDS_EBADF);
        n.urgency = 9;
        CHECK(bmds_notify_send(-1, &n) == -BMDS_EINVAL);
        n.urgency = BMDS_NOTIFY_LOW;
        n.action_count = 4;
        CHECK(bmds_notify_send(-1, &n) == -BMDS_EINVAL);
        n.action_count = 0;
        CHECK(bmds_notify_open(0x80) == -BMDS_EINVAL);
        CHECK(bmds_notify_send(-1, NULL) == -BMDS_EFAULT);
        /* Nothing has happened to it yet. */
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (!served) CHECK(poll(&p, 1, 0) == 0);
        if (served) {
                /* The panel shows it for 400 ms: then it expires, and the fd says so. */
                CHECK(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN) != 0);
                bmds_notify_event_t e;
                CHECK(read(fd, &e, sizeof(e)) == (ssize_t)sizeof(e));
                printf("notify: event %u for %u (action %u)\n", (unsigned)e.event, (unsigned)e.id, (unsigned)e.action);
                CHECK(e.size == sizeof(e) && e.id == (uint32_t)id && e.event == BMDS_NOTIFY_EXPIRED);
        }
        CHECK(bmds_notify_close((uint32_t)id) == 0);
        close(fd);
        printf("notify: all passed\n");
        return 0;
}

/* Burns 200 ms, then execs SELF proccheck START_NS: which finds its time and start kept. */
static int proc_exec(const char *self) {
        bmds_proc_info_t p = {.size = sizeof(p)};
        CHECK(bmds_proc_info(getpid(), 0, &p) == getpid());
        burn_ms(200);
        char start_text[32];
        snprintf(start_text, sizeof(start_text), "%lld", (long long)p.start_ns);
        char *eargv[] = {"progtest", "proccheck", start_text, NULL};
        execve(self, eargv, environ);
        return 1;
}

static int proc_check(const char *start_text) {
        bmds_stats_status_t st = {.size = sizeof(st)};
        CHECK(bmds_stats_status(&st) == 0);
        bmds_proc_info_t p = {.size = sizeof(p)};
        CHECK(bmds_proc_info(getpid(), 0, &p) == getpid());
        CHECK(p.start_ns == atoll(start_text));
        CHECK((st.flags & BMDS_STATS_CPU_COUNTED) == 0 || p.cpu_us >= 150000);
        CHECK(strcmp(p.command, "progtest proccheck ") > 0 && strncmp(p.command, "progtest proccheck ", 19) == 0);
        return 0;
}

/* Each interface's state on one line, after the watch's record @p change (0: the first). */
static void print_interfaces(uint32_t change) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (uint32_t i = 0;; ++i) {
                bmds_net_info_t info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (bmds_net_info(i, &info) < 0) break;
                if (info.flags & BMDS_NET_LOOPBACK) continue;
                struct in_addr ip = {info.address};
                printf("netwatch: %lld.%03ld change %u: %s %s%s %u Mbit/s mtu %u %s mode %u dhcp %u lease %u s\n",
                       (long long)now.tv_sec, now.tv_nsec / 1000000, (unsigned)change, info.name,
                       (info.flags & BMDS_NET_UP) ? "up" : "down", (info.flags & BMDS_NET_LINK) ? " link" : "",
                       (unsigned)info.speed_mbps, (unsigned)info.mtu, inet_ntoa(ip), info.mode, info.dhcp,
                       (unsigned)info.lease_s);
        }
        fflush(stdout);
        return;
}

static int64_t ts_ns(const struct timespec *t) {
        return (int64_t)t->tv_sec * 1000000000 + t->tv_nsec;
}

/* CLOCK_TAI and /dev/ptp0 (network.md §31e). "busy": the PTP service runs, so setting the clock
   is refused; otherwise a step and a frequency there and back. */
static int ptp_test(const char *mode) {
        struct timespec utc, tai;
        CHECK(clock_gettime(CLOCK_REALTIME, &utc) == 0);
        CHECK(clock_gettime(CLOCK_TAI, &tai) == 0);
        const int64_t tai_offset = ts_ns(&tai) - ts_ns(&utc);
        printf("ptp: CLOCK_TAI is UTC%+.3f s\n", (double)tai_offset / 1e9);
        CHECK(tai_offset > -1000000 && tai_offset < 100000000000LL);

        const int fd = open("/dev/ptp0", O_RDWR);
        CHECK(fd >= 0);
        const clockid_t clock = FD_TO_CLOCKID(fd);
        struct timespec t, res;
        CHECK(clock_gettime(clock, &t) == 0);
        CHECK(clock_getres(clock, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 1);
        /* Not a PTP clock: a bad clock id. */
        errno = 0;
        CHECK(clock_gettime(FD_TO_CLOCKID(0), &t) == -1 && errno == EINVAL);

        struct ptp_clock_caps caps;
        CHECK(ioctl(fd, PTP_CLOCK_GETCAPS, &caps) == 0);
        CHECK(caps.max_adj == 500000 && caps.n_per_out == 0 && caps.n_pins == 0);
        printf("ptp: max_adj %d ppb, pps %d\n", caps.max_adj, caps.pps);

        struct ptp_sys_offset_extended ext;
        memset(&ext, 0, sizeof(ext));
        ext.n_samples = 5;
        ext.clockid = CLOCK_REALTIME;
        CHECK(ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &ext) == 0);
        int64_t best = INT64_MAX, offset = 0;
        for (unsigned i = 0; i < ext.n_samples; i++) {
                const int64_t before = ext.ts[i][0].sec * 1000000000 + ext.ts[i][0].nsec;
                const int64_t phc = ext.ts[i][1].sec * 1000000000 + ext.ts[i][1].nsec;
                const int64_t after = ext.ts[i][2].sec * 1000000000 + ext.ts[i][2].nsec;
                CHECK(after >= before);
                if (after - before < best) {
                        best = after - before;
                        offset = phc - (before + after) / 2;
                }
        }
        printf("ptp: ptp0 is UTC%+.6f s (read within %lld ns)\n", (double)offset / 1e9, (long long)best);
        ext.clockid = CLOCK_TAI;
        CHECK(ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &ext) == 0);
        ext.clockid = 99;
        errno = 0;
        CHECK(ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &ext) == -1 && errno == EINVAL);
        struct ptp_sys_offset plain;
        memset(&plain, 0, sizeof(plain));
        plain.n_samples = 3;
        CHECK(ioctl(fd, PTP_SYS_OFFSET, &plain) == 0 && plain.ts[6].sec > 0);
        struct ptp_sys_offset_precise precise;
        errno = 0;
        CHECK(ioctl(fd, PTP_SYS_OFFSET_PRECISE, &precise) == -1 && errno == EOPNOTSUPP);

        struct timex tx;
        memset(&tx, 0, sizeof(tx));
        CHECK(clock_adjtime(clock, &tx) == TIME_OK);
        const long freq = tx.freq;
        if (strcmp(mode, "busy") == 0) {
                memset(&tx, 0, sizeof(tx));
                tx.modes = ADJ_FREQUENCY;
                errno = 0;
                CHECK(clock_adjtime(clock, &tx) == -1 && errno == EBUSY);
                CHECK(clock_gettime(clock, &t) == 0);
                errno = 0;
                CHECK(clock_settime(clock, &t) == -1 && errno == EBUSY);
                printf("ptp: the service has it: EBUSY\n");
        } else {
                /* +1 s and back, in one update each. */
                struct timespec a, b;
                CHECK(clock_gettime(clock, &a) == 0);
                memset(&tx, 0, sizeof(tx));
                tx.modes = ADJ_SETOFFSET | ADJ_NANO;
                tx.time.tv_sec = 1;
                CHECK(clock_adjtime(clock, &tx) == TIME_OK);
                CHECK(clock_gettime(clock, &b) == 0);
                const int64_t stepped = ts_ns(&b) - ts_ns(&a);
                tx.time.tv_sec = -1;
                CHECK(clock_adjtime(clock, &tx) == TIME_OK);
                printf("ptp: stepped +1 s: %+.6f s later\n", (double)stepped / 1e9);
                CHECK(stepped > 999000000 && stepped < 1010000000);
                /* +100 ppm, read back, then as it was. */
                memset(&tx, 0, sizeof(tx));
                tx.modes = ADJ_FREQUENCY;
                tx.freq = 100L << 16;
                CHECK(clock_adjtime(clock, &tx) == TIME_OK);
                printf("ptp: frequency set +100 ppm, reads %+.3f ppm\n", tx.freq / 65536.0);
                CHECK(tx.freq > (99L << 16) && tx.freq < (101L << 16));
                tx.freq = freq;
                CHECK(clock_adjtime(clock, &tx) == TIME_OK);
                tx.freq = 200000000L; /* 3052 ppm: past max_adj */
                errno = 0;
                CHECK(clock_adjtime(clock, &tx) == -1 && errno == ERANGE);
        }
        close(fd);
        printf("ptp: ok\n");
        return 0;
}

/* The interfaces, then again after each of @p count changes (net_watch). */
static int net_watch_test(int count) {
        const int w = bmds_net_watch(0);
        CHECK(w >= 0);
        print_interfaces(0);
        for (int i = 0; i < count; ++i) {
                uint32_t change = 0;
                CHECK(read(w, &change, sizeof(change)) == (ssize_t)sizeof(change));
                print_interfaces(change);
        }
        close(w);
        return 0;
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr,
                        "usage: progtest status|exit|leak|env|tty|float|files|time|sleep|term|lines|raw|mem|random|cwd|"
                        "spin|calls|posix|abort|spawn|scripts|signals|catch|ignore|fd|ppid|exec|orphan|tree|poll|"
                        "threads|"
                        "threadspin|filetype|audio|decode|rt|midi|synth|speech|mididecode|hog|stats|tasks|"
                        "procs|"
                        "system|notify|expand|image|fonts|net|netwatch|rtnet|sched|ptp|netfs [arg]\n");
                return 2;
        }
        const char *what = argv[1];
        const char *arg = argc > 2 ? argv[2] : "0";
        if (strcmp(what, "status") == 0) return atoi(arg);
        if (strcmp(what, "audio") == 0) return audio();
        if (strcmp(what, "decode") == 0) return decode(arg);
        if (strcmp(what, "rt") == 0) return rt();
        if (strcmp(what, "midi") == 0) return midi();
        if (strcmp(what, "synth") == 0) return synth(arg);
        if (strcmp(what, "speech") == 0) return speech(arg);
        if (strcmp(what, "mididecode") == 0) return mididecode(arg);
        if (strcmp(what, "stats") == 0) return stats(arg);
        if (strcmp(what, "statswatch") == 0) return stats_watch(argc > 2 ? arg : NULL);
        if (strcmp(what, "statswatching") == 0) return stats_watching();
        if (strcmp(what, "tasks") == 0) return tasks();
        if (strcmp(what, "procs") == 0) return procs(arg);
        if (strcmp(what, "rusage") == 0) return rusage_test(arg);
        if (strcmp(what, "system") == 0) return system_test();
        if (strcmp(what, "expand") == 0) return expand_test();
        if (strcmp(what, "image") == 0) return image_test(argc > 2 ? arg : NULL);
        if (strcmp(what, "fonts") == 0) return font_test();
        if (strcmp(what, "notify") == 0) return notify_test(argc > 2 && strcmp(arg, "served") == 0);
        if (strcmp(what, "net") == 0) return net_test();
        if (strcmp(what, "netwatch") == 0) return net_watch_test(argc > 2 ? atoi(arg) : 1);
        if (strcmp(what, "rtnet") == 0) return rt_net_test(argc > 2 ? arg : NULL);
        if (strcmp(what, "sched") == 0) return sched_test(arg);
        if (strcmp(what, "ptp") == 0) return ptp_test(argc > 2 ? arg : "");
        if (strcmp(what, "schedof") == 0) return sched_getscheduler(0) & 0xff;
        if (strcmp(what, "procexec") == 0) return proc_exec(arg);
        if (strcmp(what, "proccheck") == 0) return proc_check(arg);
        if (strcmp(what, "burn") == 0) {
                burn_ms(atoi(arg));
                return 5;
        }
        if (strcmp(what, "hog") == 0) {
                /* MB of memory at once: more than is free makes the caches give theirs back. */
                const size_t bytes = (size_t)atoi(arg) * 1024 * 1024;
                char        *p = malloc(bytes);
                printf("hog: %s %u MB\n", p != NULL ? "got" : "didn't get", (unsigned)(bytes >> 20));
                if (p != NULL) memset(p, 1, bytes);
                free(p);
                return p != NULL ? 0 : 1;
        }
        if (strcmp(what, "audioexit") == 0) {
                const int           fd = open("/dev/dsp", O_WRONLY);
                bmds_audio_config_t c = {.size = sizeof(c),
                                         .format = BMDS_AUDIO_S16LE,
                                         .channels = 2,
                                         .rate = 48000,
                                         .ring_frames = 48000};
                CHECK(fd >= 0 && bmds_audio_config(fd, &c, NULL) == 0);
                CHECK(write_silence(fd, 24000) == 0);
                return 0; /* the exit closes it: that waits until it has played */
        }
        if (strcmp(what, "filetype") == 0) {
                // filetype PATH [name|content]: the file's type, by the slot (bmds_file_type).
                if (!bmds_sys_has(BMDS_SYS_FILE_TYPE)) {
                        printf("file_type: not in this kernel\n");
                        return 1;
                }
                const char   *how = argc > 3 ? argv[3] : "";
                uint32_t      flags = strcmp(how, "name") == 0      ? BMDS_FILE_TYPE_NAME
                                      : strcmp(how, "content") == 0 ? BMDS_FILE_TYPE_CONTENT
                                                                    : 0;
                char          type[BMDS_FILE_TYPE_MAX + 1];
                char          small[4];
                const int32_t n = bmds_file_type(arg, flags, type, sizeof(type));
                if (n < 0) {
                        printf("%s: error %d\n", arg, (int)-n);
                        return 1;
                }
                printf("%s: %s (%d), small buffer %d\n", arg, type, (int)n,
                       (int)bmds_file_type(arg, flags, small, sizeof(small)));
                return 0;
        }
        if (strcmp(what, "exit") == 0) deep(20, atoi(arg));
        if (strcmp(what, "leak") == 0) {
                const int kb = atoi(arg);
                for (int i = 0; i < kb; ++i) {
                        char *p = malloc(1024);
                        if (p == NULL) {
                                printf("out of memory after %d KB\n", i);
                                return 1;
                        }
                        memset(p, i, 1024);
                }
                void *fast = bmds_mem_alloc(512, 64, BMDS_MEM_FAST);
                printf("leaked %d KB, fast block %s\n", kb, fast != NULL && ((uintptr_t)fast & 63) == 0 ? "ok" : "BAD");
                return 0;
        }
        if (strcmp(what, "env") == 0) {
                const char *v = getenv(arg);
                printf("%s=%s\n", arg, v != NULL ? v : "(unset)");
                return 0;
        }
        if (strcmp(what, "tty") == 0) {
                printf("isatty: %d %d %d\n", isatty(0), isatty(1), isatty(2));
                return 0;
        }
        if (strcmp(what, "float") == 0) {
                volatile float x = 1.0f;
                for (int i = 0; i < 1000; ++i) x = x * 1.001f + 0.5f / (float)(i + 1);
                printf("float: %.4f\n", (double)x);
                return 0;
        }
        if (strcmp(what, "files") == 0) return files(arg);
        if (strcmp(what, "netfs") == 0) return netfs();
        if (strcmp(what, "time") == 0) return timing();
        if (strcmp(what, "sleep") == 0) {
                const int r = usleep((useconds_t)atoi(arg) * 1000);
                printf("slept: %s\n", r == 0 ? "ok" : strerror(errno));
                return r == 0 ? 0 : 1;
        }
        if (strcmp(what, "term") == 0) return term();
        if (strcmp(what, "lines") == 0) return lines();
        if (strcmp(what, "raw") == 0) return raw();
        if (strcmp(what, "mem") == 0) return mem();
        if (strcmp(what, "posix") == 0) return posix(arg);
        if (strcmp(what, "abort") == 0) abort();
        if (strcmp(what, "random") == 0) {
                unsigned char b[16];
                CHECK(getentropy(b, sizeof(b)) == 0);
                for (size_t i = 0; i < sizeof(b); ++i) printf("%02x", b[i]);
                printf("\n");
                return 0;
        }
        if (strcmp(what, "spawn") == 0) return spawn(arg, argc > 3 ? argv[3] : ".");
        if (strcmp(what, "run") == 0) {
                printf("run: %d\n", run_to_file(arg, argc > 3 ? argv[3] : ""));
                return 0;
        }
        if (strcmp(what, "scripts") == 0) return scripts(arg, argc > 3 ? argv[3] : ".");
        if (strcmp(what, "signals") == 0) return signals(arg);
        if (strcmp(what, "timers") == 0) return timers(arg);
        if (strcmp(what, "pty") == 0) return ptys(arg);
        if (strcmp(what, "ctty") == 0) return controlling();
        if (strcmp(what, "syslog") == 0) return syslogs();
        if (strcmp(what, "crypto") == 0) return cryptos();
        if (strcmp(what, "timerleft") == 0) {
                // 0: nothing armed; otherwise more than that many seconds left.
                const int64_t left = timer_left_us();
                return atoi(arg) == 0 ? (left == 0 ? 0 : 1) : (left > (int64_t)atoi(arg) * 1000000 ? 0 : 1);
        }
        if (strcmp(what, "timerexec") == 0) {
                char *eargv[] = {"progtest", "timerleft", "4", NULL};
                alarm(5);
                execve(arg, eargv, environ);
                return 2;
        }
        if (strcmp(what, "alarmexit") == 0) {
                // Ends with its timer armed; failing to arm it means the table filled.
                struct itimerval it = {{0, 0}, {10, 0}};
                return setitimer(ITIMER_REAL, &it, NULL) == 0 && timer_left_us() > 0 ? 0 : 1;
        }
        if (strcmp(what, "alarmspin") == 0) {
                alarm(1);
                for (volatile unsigned n = 0;; ++n) {}
        }
        if (strcmp(what, "alarmat") == 0) {
                const int ms = atoi(arg);
                if (catch_signal(SIGALRM, count_signal, 0) != 0) return 2;
                const int64_t from = ms_now();
                ualarm((useconds_t)ms * 1000, 0);
                while (got[SIGALRM] == 0) pause();
                const int64_t took = ms_now() - from;
                if (took < ms || took >= ms + 50) printf("alarmat %d: after %d ms\n", ms, (int)took);
                return took >= ms && took < ms + 50 ? 0 : 1;
        }
        if (strcmp(what, "alarmcatch") == 0) {
                if (catch_signal(SIGALRM, count_signal, 0) != 0) return 2;
                ualarm(200000, 0);
                while (got[SIGALRM] == 0) pause();
                return 7;
        }
        if (strcmp(what, "alarmdie") == 0) {
                ualarm(100000, 0);
                for (;;) pause();
        }
        if (strcmp(what, "catch") == 0) {
                if (catch_signal(SIGINT, count_signal, 0) != 0 || catch_signal(SIGTERM, count_signal, 0) != 0) return 1;
                printf("catch: ready\n");
                fflush(stdout);
                while (got[SIGINT] == 0 && got[SIGTERM] == 0) pause();
                printf("catch: %s\n", got[SIGINT] != 0 ? "SIGINT" : "SIGTERM");
                return 7;
        }
        if (strcmp(what, "ignore") == 0) {
                signal(SIGINT, SIG_IGN);
                printf("ignore: SIGINT ignored\n");
                fflush(stdout);
                for (;;) sleep(1);
        }
        if (strcmp(what, "poll") == 0) return polling(arg);
        if (strcmp(what, "threads") == 0) return threads(arg);
        if (strcmp(what, "threadspin") == 0) return threadspin(atoi(arg));
        if (strcmp(what, "threadexit") == 0) {
                pthread_t t;
                if (pthread_create(&t, NULL, exit_nine, NULL) != 0) return 1;
                for (;;) pause();
        }
        if (strcmp(what, "mainexit") == 0) {
                pthread_t t;
                if (pthread_create(&t, NULL, sleeper, (void *)200) != 0) return 1;
                pthread_exit(NULL);
        }
        if (strcmp(what, "threadexec") == 0) {
                pthread_t t;
                if (pthread_create(&t, NULL, exec_self, (void *)arg) != 0) return 1;
                for (;;) pause();
        }
        if (strcmp(what, "later") == 0) {
                usleep((useconds_t)atoi(arg) * 1000);
                return argc > 3 && write(atoi(argv[3]), "x", 1) == 1 ? 0 : 1;
        }
        if (strcmp(what, "killer") == 0) {
                usleep((useconds_t)(argc > 3 ? atoi(argv[3]) : 100) * 1000);
                if (kill(atoi(arg), SIGUSR1) != 0) return 1;
                if (argc > 4) {
                        usleep(100000);
                        if (write(atoi(argv[4]), "x", 1) != 1) return 1;
                }
                return 0;
        }
        if (strcmp(what, "pipewrite") == 0) {
                int fds[2];
                if (pipe(fds) != 0 || close(fds[0]) != 0) return 1;
                (void)write(fds[1], "x", 1);
                return 2; // not reached: SIGPIPE
        }
        if (strcmp(what, "winch") == 0) {
                if (catch_signal(SIGWINCH, count_signal, 0) != 0) return 1;
                for (int i = 0; i < 50 && got[SIGWINCH] == 0; ++i) usleep(100000);
                struct winsize ws = {0};
                (void)ioctl(2, TIOCGWINSZ, &ws);
                fprintf(stderr, "winch: %s, %ux%u\n", got[SIGWINCH] != 0 ? "got SIGWINCH" : "none",
                        (unsigned)ws.ws_col, (unsigned)ws.ws_row);
                return got[SIGWINCH] != 0 ? 0 : 1;
        }
        if (strcmp(what, "sigstate") == 0) {
                struct sigaction a1, a2;
                if (sigaction(SIGUSR1, NULL, &a1) != 0 || sigaction(SIGUSR2, NULL, &a2) != 0) return 1;
                return a1.sa_handler == SIG_DFL && a2.sa_handler == SIG_IGN ? 0 : 1;
        }
        if (strcmp(what, "fd") == 0) return fcntl(atoi(arg), F_GETFD) >= 0 ? 0 : 1;
        if (strcmp(what, "ppid") == 0) return getppid() == atoi(arg) ? 0 : 1;
        if (strcmp(what, "exec") == 0) {
                char *eargv[] = {"progtest", "status", "42", NULL};
                execve(arg, eargv, environ);
                printf("exec: %s\n", strerror(errno));
                return 1;
        }
        if (strcmp(what, "orphan") == 0) {
                const pid_t pid = start(arg, NULL, NULL, "sleep", "300", NULL);
                printf("orphan: started %d\n", (int)pid);
                return pid > 0 ? 0 : 1;
        }
        if (strcmp(what, "tree") == 0) {
                const pid_t a = start(arg, NULL, NULL, "spin", NULL);
                const pid_t b = start(arg, NULL, NULL, "spin", NULL);
                printf("tree: %d %d\n", (int)a, (int)b);
                fflush(stdout);
                int         st = 0;
                const pid_t w = waitpid(-1, &st, 0);
                printf("tree: wait %d (%s)\n", (int)w, w < 0 ? strerror(errno) : "ended");
                for (;;) {}
        }
        if (strcmp(what, "cwd") == 0) {
                char buf[BMDS_PATH_MAX + 1];
                printf("cwd: %s\n", getcwd(buf, sizeof(buf)));
                CHECK(chdir(arg) == 0);
                printf("cwd: %s\n", getcwd(buf, sizeof(buf)));
                return 0;
        }
        /* Computing (never in the kernel), or calling it all the time: the
         * first Ctrl-C (SIGINT) ends either. A leak shows if ending them
         * doesn't clean up. */
        if (strcmp(what, "spin") == 0 || strcmp(what, "calls") == 0) {
                const int calls = what[0] == 'c';
                char *volatile block = malloc(100000);
                printf("%s until killed%s\n", what, block != NULL ? "" : " (no memory)");
                fflush(stdout);
                for (volatile uint32_t n = 0;; ++n) {
                        if (calls) {
                                int64_t ns;
                                (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
                                (void)bmds_interrupted();
                        }
                }
        }
        fprintf(stderr, "progtest: %s: unknown test\n", what);
        return 2;
}
