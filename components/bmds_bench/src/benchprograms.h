/**
 * @file      benchprograms.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' benchmarks (devplan/bench.md §4f): their descriptors found, and a program run with
 * its output read a line at a time. The `bench` command's (benchcommand.cpp).
 */

#pragma once

#include <bmds/benchtext.h>
#include <bmds/command.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace BenchPrograms {

        /** @brief The longest descriptor file read. */
        inline constexpr size_t MaxDescriptorBytes = 16 * 1024;

        /**
         * @brief Reads every `*.bench` file in the folders of `bench/paths`, calling @p found for
         *        each benchmark declared (a folder missing is skipped; a bad file, reported).
         */
        void find(CommandContext &ctx, void (*found)(void *arg, const BenchText::ProgramBench &bench), void *arg);

        /** @brief How a program ended. */
        struct Ending {
                        bool exited = false; ///< By exit(), with `status`; else by a signal.
                        int  status = 0;     ///< The exit status, or the signal.
                        int  core = -1;      ///< The core its first thread kept to (-1: not known).
        };

        /**
         * @brief Runs @p words (the program, found on `$PATH` if it has no `/`, and its
         *        arguments), its standard output on a pipe: @p line gets each line as it comes
         *        (its newline dropped). Ctrl-C sends the program SIGINT and keeps reading.
         *        @p ending's `core` is set once it starts, whatever its end.
         * @return `Ok` with @p ending; `NotSupported` (no process to start it from, as on the
         *         serial console's shell), `NotExist` (no such program), or the start's error.
         */
        Error run(CommandContext &ctx, Span<const StringView> words, void (*line)(void *arg, StringView text),
                  void *arg, Ending &ending);

} // namespace BenchPrograms

BMDS_NAMESPACE_END
