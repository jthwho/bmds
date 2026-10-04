/**
 * @file      cat.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * cat [file...]: copies the files (or standard input, also for `-`) to
 * standard output, with the raw system calls. A Ctrl-C while it waits for
 * input ends it with status 130, as a shell command would.
 */

#include <string.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

static char buf[32768];

static void say(const char *s) {
        (void)bmds_write(BMDS_STDERR, s, (uint32_t)strlen(s));
        return;
}

/* Copies @p fd to stdout: 0, or the negated error. */
static int32_t copy(int32_t fd) {
        for (;;) {
                const int32_t n = bmds_read(fd, buf, sizeof(buf));
                if (n <= 0) return n;
                const int32_t w = bmds_write(BMDS_STDOUT, buf, (uint32_t)n);
                if (w < 0) return w;
        }
}

int main(int argc, char **argv) {
        int status = 0;
        if (argc < 2) return copy(BMDS_STDIN) < 0 ? 1 : 0;
        for (int i = 1; i < argc; ++i) {
                const int     stdinput = strcmp(argv[i], "-") == 0;
                const int32_t fd = stdinput ? BMDS_STDIN : bmds_open(argv[i], BMDS_O_READ);
                const int32_t r = fd < 0 ? fd : copy(fd);
                if (!stdinput && fd >= 0) (void)bmds_close(fd);
                if (r == -BMDS_EINTR || r == -BMDS_EPIPE) return 1;
                if (r < 0) {
                        say("cat: ");
                        say(argv[i]);
                        say(": ");
                        say(strerror(-r));
                        say("\n");
                        status = 1;
                }
        }
        return status;
}
