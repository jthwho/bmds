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
 */

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
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
        CHECK(bmds_term_set(0, t.flags & ~(BMDS_TERM_LINE | BMDS_TERM_SIGNALS)) == 0);
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

        // system() needs /bin/sh (step 8b).
        printf("system: %d\n", system("true"));
        printf("spawn: all passed\n");
        return 0;
}

static void deep(int depth, int status) {
        if (depth == 0) exit(status);
        deep(depth - 1, status);
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr,
                        "usage: progtest status|exit|leak|env|tty|float|files|time|sleep|term|lines|raw|mem|random|cwd|"
                        "spin|calls|posix|abort|spawn|fd|ppid|exec|orphan|tree [arg]\n");
                return 2;
        }
        const char *what = argv[1];
        const char *arg = argc > 2 ? argv[2] : "0";
        if (strcmp(what, "status") == 0) return atoi(arg);
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
        /* Ignoring Ctrl-C: computing (never in the kernel), or calling it
         * all the time; a second Ctrl-C kills either. A leak shows if the
         * kill doesn't clean up. */
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
