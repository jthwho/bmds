/**
 * @file      cat.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Copies standard input to standard output with the raw system calls
 * (files by name come with the open call). A Ctrl-C while it waits for
 * input ends it with status 130, as a shell command would.
 */

#include <bmds/errno.h>
#include <bmds/sys.h>

int main(void) {
        static char buf[4096];
        for (;;) {
                const int32_t n = bmds_read(BMDS_STDIN, buf, sizeof(buf));
                if (n == 0) return 0;
                if (n == -BMDS_EINTR) return 130;
                if (n < 0) return 1;
                if (bmds_write(BMDS_STDOUT, buf, (uint32_t)n) < 0) return 1;
        }
}
