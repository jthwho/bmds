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
 *   progtest files DIR    files, directories, descriptors, cwd and their errors, in DIR
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
 */

#define _GNU_SOURCE /* gettid() */
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
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
        // Too many directories open at once, then all closed at exit.
        DIR *many[9];
        int  opened = 0;
        while (opened < 9 && (many[opened] = opendir(".")) != NULL) ++opened;
        CHECK(opened == 8 && errno == EMFILE);
        printf("files: ok\n");
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
        // One file, one id: by name (any case), by fd, in its directory.
        CHECK(stat(path, &a) == 0 && fstat(fd, &b) == 0);
        CHECK(a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_ino != 0);
        CHECK(stat("PT-POSIX.TXT", &b) == 0 && a.st_ino == b.st_ino);
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

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr,
                        "usage: progtest status|exit|leak|env|tty|float|files|time|sleep|term|lines|raw|mem|random|cwd|"
                        "spin|calls|posix|abort|spawn|scripts|signals|catch|ignore|fd|ppid|exec|orphan|tree|poll|"
                        "threads|"
                        "threadspin|filetype "
                        "[arg]\n");
                return 2;
        }
        const char *what = argv[1];
        const char *arg = argc > 2 ? argv[2] : "0";
        if (strcmp(what, "status") == 0) return atoi(arg);
        if (strcmp(what, "audio") == 0) return audio();
        if (strcmp(what, "decode") == 0) return decode(arg);
        if (strcmp(what, "rt") == 0) return rt();
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
