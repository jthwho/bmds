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
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
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
        CHECK(open("pt-dir", O_RDONLY) == -1 && errno == EISDIR);
        CHECK(unlink("pt-dir") == -1 && errno == EISDIR);
        CHECK(rmdir("pt-dir") == -1 && errno == ENOTEMPTY);
        DIR *dp = opendir("pt-dir");
        CHECK(dp != NULL);
        struct dirent *e = readdir(dp);
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

static void deep(int depth, int status) {
        if (depth == 0) exit(status);
        deep(depth - 1, status);
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr,
                        "usage: progtest status|exit|leak|env|tty|float|files|time|sleep|term|lines|raw|mem|random|cwd|"
                        "spin|calls [arg]\n");
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
        if (strcmp(what, "random") == 0) {
                unsigned char b[16];
                CHECK(getentropy(b, sizeof(b)) == 0);
                for (size_t i = 0; i < sizeof(b); ++i) printf("%02x", b[i]);
                printf("\n");
                return 0;
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
