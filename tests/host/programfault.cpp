/**
 * @file      programfault.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Program faults: the signal each trap cause ends a program with, and
 * whether a stack overrun stayed in the gap below the stack (programs.md §21).
 */

#include <doctest.h>

#include <cstring>
#include <bmds/programfault.h>
#include <bmds/sys.h>

using bmds::ProgramFault;

TEST_CASE("ProgramFault: each exception's signal") {
        CHECK(ProgramFault::signalFor(ProgramFault::InstructionAccess) == BMDS_SIGSEGV);
        CHECK(ProgramFault::signalFor(ProgramFault::LoadAccess) == BMDS_SIGSEGV);
        CHECK(ProgramFault::signalFor(ProgramFault::StoreAccess) == BMDS_SIGSEGV);
        CHECK(ProgramFault::signalFor(ProgramFault::IllegalInstruction) == BMDS_SIGILL);
        CHECK(ProgramFault::signalFor(ProgramFault::InstructionMisaligned) == BMDS_SIGBUS);
        CHECK(ProgramFault::signalFor(ProgramFault::LoadMisaligned) == BMDS_SIGBUS);
        CHECK(ProgramFault::signalFor(ProgramFault::StoreMisaligned) == BMDS_SIGBUS);
        CHECK(ProgramFault::signalFor(ProgramFault::Breakpoint) == BMDS_SIGTRAP);
        CHECK(ProgramFault::OverflowSignal == BMDS_SIGSEGV);
        // Stay panics: an ecall from M-mode (11), from U-mode (8), anything else.
        CHECK(ProgramFault::signalFor(8) == 0);
        CHECK(ProgramFault::signalFor(11) == 0);
        CHECK(ProgramFault::signalFor(12) == 0);
        CHECK(ProgramFault::signalFor(0xffffffffu) == 0);
}

TEST_CASE("ProgramFault: the shell's words") {
        CHECK(std::strcmp(ProgramFault::nameOf(BMDS_SIGSEGV), "Segmentation fault") == 0);
        CHECK(std::strcmp(ProgramFault::nameOf(BMDS_SIGILL), "Illegal instruction") == 0);
        CHECK(std::strcmp(ProgramFault::nameOf(BMDS_SIGBUS), "Bus error") == 0);
        CHECK(std::strcmp(ProgramFault::nameOf(BMDS_SIGTRAP), "Trace/breakpoint trap") == 0);
        CHECK(ProgramFault::nameOf(BMDS_SIGTERM) == nullptr);
}

TEST_CASE("ProgramFault: an overrun is contained only within the gap") {
        constexpr uintptr_t Bottom = 0x4ff7ccc0;
        constexpr size_t    Gap = ProgramFault::GuardBytes;
        // A fault with the stack in bounds: the frame is inside the stack.
        CHECK(ProgramFault::isContained(Bottom + 0x4000, Bottom, Gap));
        // What step b saw: SP 544 bytes under the bound, the 160-byte frame under that.
        CHECK(ProgramFault::isContained(Bottom - 544 - 160, Bottom, Gap));
        // The gap's last byte, and one past it.
        CHECK(ProgramFault::isContained(Bottom - Gap, Bottom, Gap));
        CHECK_FALSE(ProgramFault::isContained(Bottom - Gap - 1, Bottom, Gap));
        // A large frame put it far below.
        CHECK_FALSE(ProgramFault::isContained(Bottom - 9000, Bottom, Gap));
        // No gap: only frames at or above the bottom.
        CHECK(ProgramFault::isContained(Bottom, Bottom, 0));
        CHECK_FALSE(ProgramFault::isContained(Bottom - 4, Bottom, 0));
        // A bottom below the gap's size can't wrap around.
        CHECK_FALSE(ProgramFault::isContained(0, 512, Gap));
}
