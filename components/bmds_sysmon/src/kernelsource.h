/**
 * @file      kernelsource.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>

#include "source.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The built-in's `Source`: the kernel's public classes (`Stats`,
 *        `TaskList`, `Loader`, `CpuTime`), as the system calls answer from them.
 *
 * Its watch is its own: dropped when it is destroyed.
 *
 * @par Thread Safety
 * As the kernel's classes (thread-safe); one watch per object.
 */
class KernelSource final : public Source {
        public:
                KernelSource() = default;
                ~KernelSource() override;
                KernelSource(const KernelSource &) = delete;
                KernelSource &operator=(const KernelSource &) = delete;

                Error  counter(size_t index, Counter &out) override;
                size_t read(Span<const uint32_t> ids, Span<int64_t> values, Span<uint8_t> health,
                            int64_t &timeNs) override;
                size_t history(uint32_t id, Span<int32_t> out) override;
                size_t problems(uint64_t after, Span<Stat::ProblemLog::Episode> out) override;
                Error  watch(bool on) override;
                void   markSeen(uint64_t change) override;
                void   clearProblems() override;
                Status status() override;
                size_t tasks(Span<Task> out) override;
                Error  process(int32_t pid, bool next, Process &out) override;
                Error  signal(int32_t pid, int sig) override;

        private:
                bool _watching = false;
};

BMDS_NAMESPACE_END
