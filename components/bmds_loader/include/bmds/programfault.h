/**
 * @file      programfault.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a program's fault means (devplan/programs.md §21): the
 *        portable decisions of the trap hook, host-tested.
 *
 * The hook itself (`faults.cpp`) is target only: it wraps IDF's panic entry
 * points and ends a program that faults in its own code as killed by the
 * signal these name, instead of panicking.
 *
 * @par Thread Safety
 * Pure functions: safe from any context, a trap handler included.
 */
struct ProgramFault {
                /** @brief RISC-V `mcause` exception codes the hook recovers from. */
                enum Cause : uint32_t {
                        InstructionMisaligned = 0,
                        InstructionAccess = 1,
                        IllegalInstruction = 2,
                        Breakpoint = 3,
                        LoadMisaligned = 4,
                        LoadAccess = 5,
                        StoreMisaligned = 6,
                        StoreAccess = 7,
                };

                /** @brief The bytes allocated below a program thread's stack (`Task::Config::guardBytes`). */
                static constexpr size_t GuardBytes = 1024;

                /**
                 * @brief The signal an exception ends a program with, or 0 for one that
                 *        stays a panic (an `ecall`, anything unknown).
                 * @param cause The trap's `mcause` exception code (interrupt bit clear).
                 */
                static constexpr int signalFor(uint32_t cause) {
                        switch (cause) {
                                case InstructionAccess:
                                case LoadAccess:
                                case StoreAccess: return BMDS_SIGSEGV;
                                case IllegalInstruction: return BMDS_SIGILL;
                                case InstructionMisaligned:
                                case LoadMisaligned:
                                case StoreMisaligned: return BMDS_SIGBUS;
                                case Breakpoint: return BMDS_SIGTRAP;
                                default: return 0;
                        }
                }

                /** @brief The signal a stack overflow (the stack guard) ends a program with, as Linux's. */
                static constexpr int OverflowSignal = BMDS_SIGSEGV;

                /**
                 * @brief Whether a trap's frame, and so everything the program wrote
                 *        before it, lies in memory the thread owns: at or above the gap
                 *        below its stack.
                 *
                 * The stack guard is asynchronous: a program runs on a few instructions
                 * past its stack's bottom before the trap pushes its frame below that.
                 * Within the gap, ending the program is safe; below it, something else's
                 * memory may be overwritten, and the trap stays a panic.
                 *
                 * @param frame       The trap frame's address (its lowest byte).
                 * @param stackBottom The stack's lowest address (the guard's bound).
                 * @param guardBytes  The gap allocated below it.
                 */
                static constexpr bool isContained(uintptr_t frame, uintptr_t stackBottom, size_t guardBytes) {
                        return stackBottom >= guardBytes && frame >= stackBottom - guardBytes;
                }

                /** @brief The words a shell prints for a program @p signal ended (bash's), or null. */
                static constexpr const char *nameOf(int signal) {
                        switch (signal) {
                                case BMDS_SIGSEGV: return "Segmentation fault";
                                case BMDS_SIGILL: return "Illegal instruction";
                                case BMDS_SIGBUS: return "Bus error";
                                case BMDS_SIGTRAP: return "Trace/breakpoint trap";
                                default: return nullptr;
                        }
                }
};

BMDS_NAMESPACE_END
