/**
 * @file      ee_printf.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * CoreMark's output on BMDS (built with HAS_PRINTF=0, so core_main.c's ee_printf is this): printed
 * as it is, and its score also as `bench`'s result lines (bmds/benchlib.h, devplan/bench.md §15):
 * iterations a second, and per MHz of the CPU's clock.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <bmds/benchlib.h>
#include <bmds/sys.h>

int ee_printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        double score = 0;
        int    scored = strncmp(fmt, "Iterations/Sec   : %f", 21) == 0;
        if (scored) {
                va_list copy;
                va_copy(copy, args);
                score = va_arg(copy, double);
                va_end(copy);
        }
        const int n = vprintf(fmt, args);
        va_end(args);
        if (scored) {
                bmds_system_info_t info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                const unsigned mhz = bmds_system_info(&info) == 0 && info.cpu_mhz != 0 ? info.cpu_mhz : 360;
                bmds_bench_result("iterations_s", score, "/s", BMDS_BENCH_HIGHER);
                bmds_bench_result("coremark_mhz", score / mhz, "/MHz", BMDS_BENCH_HIGHER);
        }
        return n;
}
