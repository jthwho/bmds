/**
 * @file      profiler.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The sampling profiler (devplan/sysmon.md §9): where the CPU goes, a sample per tick.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Samples the interrupted program counter on each chosen core at every FreeRTOS tick
 *        (1 kHz): its PC and return address, the task, the core, and the program module.
 *
 * Interrupt handlers' own time isn't seen, and time inside a critical section lands on its exit.
 * While stopped it costs a flag test a tick; its samples (768 KB of PSRAM) last from `start()` to
 * `clear()`.
 *
 * Thread-safe: `start()`, `stop()` and `clear()` are serialized by the caller (the `prof`
 * command); the samples are read only while stopped.
 */
class Profiler {
        public:
                /** @brief Samples kept, at most (32 s of both cores). */
                static constexpr size_t MaxSamples = 32768;

                /** @brief Modules kept: the kernel (0) and up to 15 programs. */
                static constexpr size_t MaxModules = 16;

                /** @brief A task's name as kept. */
                static constexpr size_t NameBytes = 12;

                /** @brief A program module's name as kept (its command's first word). */
                static constexpr size_t ModuleNameBytes = 32;

                /** @brief One sample. */
                struct Sample {
                                uint32_t pc;              ///< Where the task was (0: in an interrupt).
                                uint32_t ra;              ///< Its return address.
                                char     task[NameBytes]; ///< The task's name, cut ("(isr)": an interrupt).
                                uint8_t  module;          ///< Its module (0: the kernel's).
                                uint8_t  core;            ///< The core.
                                uint16_t reserved;
                };

                /** @brief A module: the kernel, or a program's image. */
                struct Module {
                                uint32_t base;                  ///< Its image's address (0: the kernel).
                                uint32_t bytes;                 ///< Its image's size.
                                char     name[ModuleNameBytes]; ///< Its program ("kernel" for 0).
                };

                /**
                 * @brief Gives the program image task @p task runs in.
                 * @return false for a kernel task.
                 * @isrsafe Called from the tick hook: plain reads only.
                 */
                using Locator = bool (*)(void *task, uint32_t &base, uint32_t &bytes, const char *&name);

                /** @brief Sets the locator (the loader's, at boot). */
                static void setLocator(Locator locator);

                /**
                 * @brief Starts sampling, the previous samples dropped.
                 * @param coreMask The cores to sample (bit N: core N).
                 * @return `Ok`, `NoMem`, `Invalid` (no such core), or a hook's error.
                 */
                static Error start(uint32_t coreMask);

                /** @brief Stops sampling. */
                static void stop();

                /** @brief True while sampling. */
                static bool isRunning();

                /** @brief The samples taken (stopped: stable). */
                static size_t sampleCount();

                /** @brief Sample @p index (below `sampleCount()`). */
                static const Sample &sample(size_t index);

                /** @brief The modules seen. */
                static size_t moduleCount();

                /** @brief Module @p index (below `moduleCount()`). */
                static const Module &module(size_t index);

                /** @brief Stops, and frees the samples. */
                static void clear();
};

BMDS_NAMESPACE_END
