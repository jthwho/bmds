/**
 * @file      hello.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The first BMDS program: its arguments, through printf.
 */

#include <stdio.h>

int main(int argc, char **argv) {
        printf("Hello from a BMDS program!\n");
        for (int i = 0; i < argc; ++i) printf("argv[%d] = \"%s\"\n", i, argv[i]);
        return 0;
}
