/**
 * @file      usermode.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_loader: programs in U-mode (devplan/protection.md §4, §14). The assembly is
 * usermode.S; trapentry.S's stubs lead there.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/pmpset.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "usermodeoffsets.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief A program thread's kernel side while it runs in U-mode: what a trap from U-mode saves
 *        and the trampoline restores, the two stacks' bounds, its process's PMP entries.
 *
 * Lives at the top of the thread's kernel stack (its FreeRTOS stack): SP is its
 * address whenever nothing is on that stack, and `mscratch` holds it while the thread is in
 * U-mode. The offsets are usermodeoffsets.h's (checked in usermode.cpp).
 *
 * @par Thread Safety
 * Its thread's own, and the trap code's on its core.
 */
struct alignas(16) UserBlock {
                uint32_t      t0 = 0;        ///< The trampoline's and the entry's scratch.
                uint32_t      t1 = 0;        ///< Likewise; a system call's slot number.
                uint32_t      t2 = 0;        ///< The trampoline's scratch.
                uint32_t      sp = 0;        ///< The program's SP.
                uint32_t      gp = 0;        ///< The program's gp (the ABI's: the kernel's value).
                uint32_t      pc = 0;        ///< Where the program goes on.
                uint32_t      ra = 0;        ///< A system call's: the stub's return address.
                uint32_t      link = 0;      ///< The trap stub's, back from the entry.
                uint32_t      kernelLo = 0;  ///< The kernel stack's bottom (the stack guard's bound).
                uint32_t      userLo = 0;    ///< The program's stack: its bottom,
                uint32_t      userHi = 0;    ///< and its top (the guard's bounds in U-mode).
                const PmpSet *pmp = nullptr; ///< Its process's (`UserSpace::pmp()`).
};

/**
 * @brief A process's U-mode memory: the PMP entries its threads run with (protection.md §14):
 *        the stub page, PSRAM, and the internal RAM it was granted (stacks, `BMDS_MEM_FAST`).
 *
 * In internal RAM: the trampoline reads its `PmpSet` with interrupts off, on either core.
 * Changing it rebuilds and rewrites the set under a spinlock (interrupts off) with the stamp odd
 * meanwhile (a trampoline reading it waits, then reads it again); a revoke then interrupts the
 * other core, whose trampoline loads the new set before it goes back to U-mode, so nothing
 * revoked is reachable once `revoke()` returns.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class UserSpace final : public UserGrants {
        public:
                /** @brief The internal regions it holds at most (TOR pairs in the 14 entries after the fixed two). */
                static constexpr unsigned MaxRegions = 7;

                /** @brief A new one in internal RAM, with the fixed entries; null: no memory. */
                static UserSpace *create();

                /** @brief Frees @p space (no thread of its process may run in U-mode). */
                static void destroy(UserSpace *space);

                /**
                 * @brief Grants [@p base, @p base + @p bytes) to U-mode with @p access
                 *        (`PmpSet::Access` bits); whole `PmpSet::Granule`s only.
                 * @return `Error::Full` if its entries don't fit (use PSRAM instead),
                 *         `Error::Invalid` for a boundary off a granule.
                 */
                Error grant(uintptr_t base, size_t bytes, uint8_t access) override;

                /** @brief Takes back the region granted at @p base; waits until no core holds it. */
                void revoke(uintptr_t base) override;

                /** @brief The set the trampoline loads. */
                const PmpSet *pmp() const { return &_pmp; }

        private:
                struct Region {
                                uintptr_t base;
                                size_t    bytes;
                                uint8_t   access;
                };

                UserSpace() = default;
                ~UserSpace() = default;
                Error update();

                PmpSet       _pmp;
                Region       _regions[MaxRegions] = {};
                unsigned     _count = 0;
                portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
};

/**
 * @brief Running programs in U-mode: whether it can (and is wanted), the table they get, and a
 *        thread's first entry.
 */
namespace UserMode {

        /** @brief Rounds @p bytes up to whole PMP granules. */
        constexpr size_t granules(size_t bytes) {
                return (bytes + PmpSet::Granule - 1) & ~size_t(PmpSet::Granule - 1);
        }

        /**
         * @brief True if programs can run in U-mode: the trap entry on both cores, the PMP free
         *        on both, the stub page as built. Decided once (at the first call).
         */
        bool isAvailable();

        /** @brief True if a program starting now runs in U-mode: available and `programs/protect`. */
        bool isWanted();

        /** @brief The table a U-mode program gets (the stub page). */
        const bmds_sys_t *table();

        /** @brief Where a program's `_start` returns to (exit), in the stub page. */
        uint32_t exitAddress();

        /** @brief Where a thread's entry returns to (thread_exit), in the stub page. */
        uint32_t threadExitAddress();

        /**
         * @brief Into U-mode for the first time, at @p block's `pc` with its `sp`: from the
         *        thread's kernel code, @p block at the top of what it uses of its stack. The
         *        program gets @p a0-@p a3, @p ra and @p tp, every other register clear.
         */
        [[noreturn]] void start(UserBlock &block, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ra,
                                uint32_t tp);

        /** @brief True if @p pc is the trampoline: a trap frame from U-mode (faults.cpp). */
        bool isReturn(uint32_t pc);

        /** @brief Logs one line at boot when protection can't be had (with the reason). */
        void logState();

} // namespace UserMode

BMDS_NAMESPACE_END
