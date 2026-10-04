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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static void deep(int depth, int status) {
        if (depth == 0) exit(status);
        deep(depth - 1, status);
}

int main(int argc, char **argv) {
        if (argc < 2) {
                fprintf(stderr, "usage: progtest status|exit|leak|env|tty|float [arg]\n");
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
        fprintf(stderr, "progtest: %s: unknown test\n", what);
        return 2;
}
