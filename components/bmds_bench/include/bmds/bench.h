/**
 * @file      bench.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A benchmark's tags (`devplan/bench.md` §4a): what it needs and does,
 *        so a run can pick by them (`bench run @quick`).
 */
enum BenchTag : uint16_t {
        BenchQuick = 1u << 0,   ///< `quick`: a few seconds; disturbs nothing.
        BenchLong = 1u << 1,    ///< `long`: a minute or more.
        BenchScreen = 1u << 2,  ///< `screen`: draws on or takes over the display.
        BenchAudio = 1u << 3,   ///< `audio`: makes sound.
        BenchWrites = 1u << 4,  ///< `writes`: writes files (under its `dir=`).
        BenchPeer = 1u << 5,    ///< `peer`: needs a host on the network.
        BenchProgram = 1u << 6, ///< `program`: runs a program.
};

/** @brief Whether a metric is better higher (a rate) or lower (a time). */
enum class BenchBetter : uint8_t {
        Higher = 0,
        Lower = 1,
};

/** @brief A benchmark's parameter: `KEY=VALUE` on `bench run`'s line. */
struct BenchParam {
                const char *name;         ///< `mb`.
                const char *defaultValue; ///< `8`.
                const char *help;         ///< One line.
};

class BenchContext;

/**
 * @brief A benchmark, as its component declares it (rodata, `BMDS_BENCH`).
 */
struct Bench {
                const char *name;                ///< Dotted: `storage.sd.read`.
                const char *help;                ///< One line saying what it measures.
                Error (*run)(BenchContext &ctx); ///< `Ok`, or what stopped it.
                uint16_t          tags;          ///< `BenchTag` bits.
                const BenchParam *params;        ///< Its parameters (`runs` and `warmup` are every bench's).
                uint8_t           paramCount;
};

/**
 * @brief What a benchmark measures with (`devplan/bench.md` §4b): its
 *        parameters, the repeat-and-keep loop, single figures, notes, and
 *        Ctrl-C. Made by the `bench` command for each benchmark it runs.
 *
 * @code
 * Error runRead(BenchContext &ctx) {
 *         uint64_t mb = 0;
 *         if (Error err = ctx.paramUnsigned("mb", mb); err.isError()) return err;
 *         return ctx.measure("direct_65536", "MB/s", BenchBetter::Higher, [&](double &out) {
 *                 Stopwatch watch;
 *                 ...
 *                 out = double(bytes) / double(watch.elapsedUs());
 *                 return Error(Error::Ok);
 *         });
 * }
 * @endcode
 *
 * @par Thread Safety
 * The benchmark's own thread (the command's).
 */
class BenchContext {
        public:
                /** @brief A sample: the figure one run of the measured code gives (`Ok`, or what stopped it). */
                using Sample = Error (*)(void *arg, double &out);

                /** @brief The longest metric name, in bytes. */
                static constexpr size_t MaxMetricName = 47;

                /** @brief The longest unit, in bytes. */
                static constexpr size_t MaxUnit = 11;

                /** @brief Returns parameter @p name's value: given on the line, else its default ("" if none). */
                virtual StringView param(StringView name) const = 0;

                /**
                 * @brief Reads parameter @p name as an unsigned number.
                 * @return `Ok`, or `Invalid` (not a number: the command says which).
                 */
                Error paramUnsigned(StringView name, uint64_t &out) const;

                /** @brief Returns true after Ctrl-C: stop between runs (`measure()` checks on its own). */
                virtual bool isCancelled() const = 0;

                /**
                 * @brief Measures metric @p metric: @p sample once to warm up (`warmup=`),
                 *        then `runs=` times (5 unless given), keeping every figure; the
                 *        median, the least, the most and the spread are reported.
                 * @return `Ok`; `Cancelled`; or the sample's error.
                 */
                virtual Error measureWith(StringView metric, StringView unit, BenchBetter better, Sample sample,
                                          void *arg) = 0;

                /** @brief `measureWith()` for a callable `Error (double &out)`. */
                template <typename F>
                Error measure(StringView metric, StringView unit, BenchBetter better, F &&sample) {
                        using Fn = std::remove_reference_t<F>;
                        return measureWith(
                                metric, unit, better,
                                [](void *arg, double &out) -> Error { return (*static_cast<Fn *>(arg))(out); },
                                &sample);
                }

                /** @brief Reports a figure the benchmark worked out itself (one run). */
                virtual void report(StringView metric, StringView unit, BenchBetter better, double value) = 0;

                /** @brief Prints a line of text under the benchmark, and keeps it in the results. */
                virtual void note(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3) = 0;

        protected:
                BenchContext() = default;
                virtual ~BenchContext() = default;
                BenchContext(const BenchContext &) = delete;
                BenchContext &operator=(const BenchContext &) = delete;
};

BMDS_NAMESPACE_END

/**
 * @def BMDS_BENCH
 * @brief Registers a benchmark with no parameters of its own.
 *
 * Each goes in its own input section, `.bmds_bench.<Name>`; the `bmds_bench`
 * linker fragment collects them sorted by name into one rodata table. No
 * constructor and no RAM. As with commands, the object file must be linked:
 * build the component `WHOLE_ARCHIVE`.
 *
 * @param Id   An identifier for the object.
 * @param Name The dotted name, a string literal (`"storage.sd.read"`).
 * @param Fn   `bmds::Error Fn(bmds::BenchContext &)`.
 * @param Tags `BenchTag` bits.
 * @param Help One line.
 */
#define BMDS_BENCH(Id, Name, Fn, Tags, Help)                                                                           \
        [[gnu::used, gnu::section(".bmds_bench." Name)]] constexpr ::bmds::Bench bmdsBench_##Id = {Name, Help,    Fn,  \
                                                                                                   Tags, nullptr, 0};

/**
 * @def BMDS_BENCH_WITH
 * @brief `BMDS_BENCH` with parameters: @p Params, a `constexpr BenchParam[]`.
 */
#define BMDS_BENCH_WITH(Id, Name, Fn, Tags, Params, Help)                                                              \
        [[gnu::used, gnu::section(".bmds_bench." Name)]] constexpr ::bmds::Bench bmdsBench_##Id = {                    \
                Name, Help, Fn, Tags, Params, static_cast<uint8_t>(sizeof(Params) / sizeof(Params[0]))};
