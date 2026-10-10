/**
 * @file      source.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stat.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Everything the System Monitor reads, one function per system call
 *        of `devplan/sysmon.md` §3.6: the built-in answers from the kernel's
 *        classes (`KernelSource`), a program built from the SDK would answer
 *        from the calls, the window unchanged. Tests give a fake.
 *
 * @par Thread Safety
 * As the implementation's; the window calls it from its loop.
 */
class Source {
        public:
                /** @brief A counter's health not judged now (nobody watches). */
                static constexpr uint8_t Unjudged = 0xff;

                /** @brief A counter (`stats_list`). */
                struct Counter {
                                uint32_t     id = 0;
                                uint32_t     generation = 0;
                                Stat::Unit   unit = Stat::Unit::Count;
                                Stat::Kind   kind = Stat::Kind::Level;
                                uint8_t      flags = 0;
                                Stat::Limits limits;   ///< In force.
                                uint16_t     hold = 0; ///< Seconds, the event default filled in.
                                char         name[Stat::MaxName + 1] = {};
                                char         labels[64] = {};
                                char         help[128] = {};
                };

                /** @brief The registry as a whole (`stats_status`). */
                struct Status {
                                uint32_t     generation = 0;
                                uint32_t     count = 0;
                                uint32_t     watchers = 0;
                                uint32_t     intervalMs = 1000;
                                uint32_t     cores = 1;
                                bool         cpuCounted = false;
                                uint64_t     lastChange = 0;
                                Stat::Health unseen = Stat::Health::Normal;
                                int64_t      lastSampleNs = 0;
                };

                /** @brief A task (`task_list`). */
                struct Task {
                                char     name[16] = {};
                                int32_t  pid = 0; ///< 0: the kernel's.
                                int32_t  tid = 0;
                                uint8_t  state = 0; ///< 0 running, 1 ready, 2 blocked, 3 suspended, 4 deleted.
                                uint8_t  priority = 0;
                                uint8_t  basePriority = 0;
                                int8_t   core = -1;
                                uint32_t stackFree = 0;
                                uint32_t number = 0;
                                uint64_t cpuUs = 0;
                                bool     main = false;
                };

                /** @brief A process (`proc_info`). */
                struct Process {
                                enum class State : uint8_t {
                                        Running,
                                        Vfork,
                                        Ending,
                                        Zombie,
                                        Stopped,
                                };

                                int32_t  pid = 0;
                                int32_t  parent = 0;
                                State    state = State::Running;
                                uint32_t threads = 0;
                                int32_t  status = 0;
                                uint64_t memory = 0;
                                uint64_t cpuUs = 0;
                                int64_t  startNs = 0;
                                char     command[48] = {};
                };

                virtual ~Source() = default;

                /** @brief Describes the counter at @p index. @return `Ok`, or `NotFound` past the last. */
                virtual Error counter(size_t index, Counter &out) = 0;

                /**
                 * @brief Reads @p ids at one moment into @p values and @p health
                 *        (`Unjudged` while nobody watches); @p timeNs the moment.
                 * @return How many were read.
                 */
                virtual size_t read(Span<const uint32_t> ids, Span<int64_t> values, Span<uint8_t> health,
                                    int64_t &timeNs) = 0;

                /** @brief Copies counter @p id's samples, the oldest first. @return How many. */
                virtual size_t history(uint32_t id, Span<int32_t> out) = 0;

                /** @brief Copies the problems changed after @p after. @return How many. */
                virtual size_t problems(uint64_t after, Span<Stat::ProblemLog::Episode> out) = 0;

                /** @brief Starts or stops watching. @return `Ok` or `NoMem`. */
                virtual Error watch(bool on) = 0;

                /** @brief Marks the problems up to @p change seen. */
                virtual void markSeen(uint64_t change) = 0;

                /** @brief Forgets the problems that ended. */
                virtual void clearProblems() = 0;

                /** @brief Returns the registry's state. */
                virtual Status status() = 0;

                /** @brief Fills @p out with the tasks. @return How many there are. */
                virtual size_t tasks(Span<Task> out) = 0;

                /** @brief Describes process @p pid, or with @p next the first after it. @return `Ok` or `NotExist`. */
                virtual Error process(int32_t pid, bool next, Process &out) = 0;

                /** @brief Sends signal @p sig to process @p pid. @return `Ok`, `NotExist` or `PermissionDenied`. */
                virtual Error signal(int32_t pid, int sig) = 0;
};

BMDS_NAMESPACE_END
