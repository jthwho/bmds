/**
 * @file      benchlib.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsbench (devplan/bench.md §4f): what a program needs to be one of `bench`'s benchmarks. A
 * static library (libbmdsbench.a, linked by bmds_add_program and bmds-cc): no system calls of its
 * own and no ABI promise. The contract with `bench` is the command line and the output:
 *
 * - `bench` runs the program as its descriptor's `Exec` says, followed by `KEY=VALUE` for each
 *   of the benchmark's parameters, `runs=` and `warmup=` among them.
 * - Each result is a line on standard output, one a run:
 *   `@bench METRIC VALUE UNIT higher|lower`. `bench` gathers a metric's lines and works out its
 *   median, least, most and spread; any other line is kept as a note.
 * - The exit status: 0 done; `BMDS_BENCH_SKIPPED` (77) can't run here (no peer, no data); any
 *   other, failed.
 *
 * @code
 * int main(int argc, char **argv) {
 *         bmds_bench_t b;
 *         bmds_bench_init(&b, argc, argv);
 *         return bmds_bench_measure(&b, "read_mb_s", "MB/s", BMDS_BENCH_HIGHER, sample, NULL) ? 1 : 0;
 * }
 * @endcode
 *
 * Not thread-safe: one thread's.
 */

#ifndef BMDS_BENCHLIB_H
#define BMDS_BENCHLIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The exit status saying the benchmark can't run here (`bench` shows it skipped). */
#define BMDS_BENCH_SKIPPED 77

/** @brief A metric better higher (a rate). */
#define BMDS_BENCH_HIGHER 1
/** @brief A metric better lower (a time). */
#define BMDS_BENCH_LOWER 0

/** @brief The command line `bench` gave: its `KEY=VALUE` arguments, and the runs. */
typedef struct bmds_bench_t {
                int      argc; /**< The arguments, as main() had them. */
                char   **argv;
                uint32_t runs;   /**< `runs=`: samples kept a metric (5 unless given). */
                uint32_t warmup; /**< `warmup=`: samples taken first, not printed (1 unless given). */
} bmds_bench_t;

/** @brief Reads @p argc and @p argv (`runs=` and `warmup=` checked: 1-1000 and 0-100). */
void bmds_bench_init(bmds_bench_t *b, int argc, char **argv);

/** @brief Returns parameter @p name's value (the last `NAME=` given), or @p fallback. */
const char *bmds_bench_param(const bmds_bench_t *b, const char *name, const char *fallback);

/** @brief Returns parameter @p name as a number (decimal), or @p fallback if absent or not one. */
long bmds_bench_param_long(const bmds_bench_t *b, const char *name, long fallback);

/** @brief Returns a monotonic time in nanoseconds. */
int64_t bmds_bench_now_ns(void);

/** @brief Prints one run's figure: `@bench METRIC VALUE UNIT higher|lower`. */
void bmds_bench_result(const char *metric, double value, const char *unit, int higher);

/** @brief One run of the measured work: its figure in @p *out. @return 0, or nonzero to stop. */
typedef int (*bmds_bench_sample_fn)(void *arg, double *out);

/**
 * @brief Runs @p sample `warmup` times, then `runs` times printing each figure as metric
 *        @p metric.
 * @return 0, or the sample's nonzero value (nothing more printed).
 */
int bmds_bench_measure(const bmds_bench_t *b, const char *metric, const char *unit, int higher,
                       bmds_bench_sample_fn sample, void *arg);

#ifdef __cplusplus
}
#endif

#endif
