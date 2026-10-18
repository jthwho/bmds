/**
 * @file      bench.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsbench (bmds/benchlib.h): a program's side of `bench` (devplan/bench.md §4f).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <bmds/benchlib.h>

void bmds_bench_init(bmds_bench_t *b, int argc, char **argv) {
        b->argc = argc;
        b->argv = argv;
        const long runs = bmds_bench_param_long(b, "runs", 5);
        const long warmup = bmds_bench_param_long(b, "warmup", 1);
        b->runs = runs >= 1 && runs <= 1000 ? (uint32_t)runs : 5;
        b->warmup = warmup >= 0 && warmup <= 100 ? (uint32_t)warmup : 1;
        return;
}

const char *bmds_bench_param(const bmds_bench_t *b, const char *name, const char *fallback) {
        const size_t n = strlen(name);
        const char  *found = fallback;
        for (int i = 1; i < b->argc; ++i) {
                const char *a = b->argv[i];
                if (strncmp(a, name, n) == 0 && a[n] == '=') found = a + n + 1;
        }
        return found;
}

long bmds_bench_param_long(const bmds_bench_t *b, const char *name, long fallback) {
        const char *text = bmds_bench_param(b, name, NULL);
        if (text == NULL || *text == '\0') return fallback;
        char      *end = NULL;
        const long v = strtol(text, &end, 10);
        return *end == '\0' ? v : fallback;
}

int64_t bmds_bench_now_ns(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

void bmds_bench_result(const char *metric, double value, const char *unit, int higher) {
        // Each line whole: `bench` reads them from a pipe.
        printf("@bench %s %.9g %s %s\n", metric, value, unit, higher ? "higher" : "lower");
        fflush(stdout);
        return;
}

int bmds_bench_measure(const bmds_bench_t *b, const char *metric, const char *unit, int higher,
                       bmds_bench_sample_fn sample, void *arg) {
        for (uint32_t i = 0; i < b->warmup + b->runs; ++i) {
                double    v = 0;
                const int r = sample(arg, &v);
                if (r != 0) return r;
                if (i >= b->warmup) bmds_bench_result(metric, v, unit, higher);
        }
        return 0;
}
