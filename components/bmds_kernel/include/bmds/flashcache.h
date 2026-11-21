/**
 * @file      flashcache.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IRAM-safe interrupts' use of the cache, held off from the flash's writes
 * (docs/platforms/esp32p4.md: spi_flash_cache_enabled() is stale by the time
 * it is used).
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/placement.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Lets an IRAM-safe interrupt touch flash and PSRAM (call code in
 *        flash, wake waiters on PSRAM stacks) only when no flash operation can
 *        suspend the cache under it.
 *
 * IDF stalls the other core for a flash operation at task level only: its
 * IRAM-safe interrupts keep coming while the cache is suspended, so checking
 * the cache's state first and using it later races the write. Instead, an
 * interrupt that may touch the cache enters (`enterFromIsr()`) before it does
 * and leaves after; IDF's start of a flash operation (wrapped, `flashcache.cpp`)
 * marks the flash busy, then waits for every interrupt already inside to
 * leave before it suspends the cache. An interrupt that finds the flash busy
 * isn't counted and must not touch the cache: it defers that work to a later
 * interrupt.
 *
 * Sequentially consistent on both sides (an interrupt counts itself then
 * reads the flag; the writer sets the flag then reads the count), so at least
 * one sees the other. The writer waits at most one interrupt's body.
 *
 * @par Thread Safety
 * Thread-safe, both cores.
 */
namespace FlashCache {

        /** @brief Interrupts inside, that may touch the cache (internal RAM: flashcache.cpp). */
        extern uint32_t users;
        /** @brief Flash operations under way, from before their cache suspend to after its resume. */
        extern uint32_t flashOps;

        /**
         * @brief Enters: true when the cache may be touched until `leaveFromIsr()`;
         *        false when the flash is busy (nothing to leave, the cache not to be touched).
         * @isrsafe Inlined into IRAM code: nothing in flash.
         * @hotpath
         */
        BMDS_ISR_ATTR inline __attribute__((always_inline)) bool enterFromIsr() {
                __atomic_fetch_add(&users, 1u, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&flashOps, __ATOMIC_SEQ_CST) == 0) return true;
                __atomic_fetch_sub(&users, 1u, __ATOMIC_RELEASE);
                return false;
        }

        /**
         * @brief Leaves, after an `enterFromIsr()` that returned true: the cache not touched again.
         * @isrsafe Inlined into IRAM code.
         * @hotpath
         */
        BMDS_ISR_ATTR inline __attribute__((always_inline)) void leaveFromIsr() {
                __atomic_fetch_sub(&users, 1u, __ATOMIC_RELEASE);
                return;
        }

        /** @brief What `addAfterHook()` takes. */
        using AfterHook = void (*)(void *arg);

        /** @brief Hooks a pause can leave work for, at most. */
        constexpr unsigned MaxAfterHooks = 8;

        /**
         * @brief Calls @p hook with @p arg at the end of every flash operation (an interrupt that
         *        found the flash busy deferred work: the hook delivers it, or raises the interrupt
         *        again). It runs in the flash operation's task once ESP-IDF has given back the
         *        cache, the other core, non-IRAM interrupts and the scheduler: ordinary code, brief,
         *        never blocking (it may take a spinlock). Set once, at start.
         * @return `Ok`, or `Full` (`MaxAfterHooks`).
         * @blocking
         */
        Error addAfterHook(AfterHook hook, void *arg);

        /**
         * @brief Waits until no hook runs (a flash operation's end, under way on the other core,
         *        past): what a hook reads may then be changed, its effects undone.
         * @blocking Spins for at most the hooks' bodies.
         */
        void waitHooks();

} // namespace FlashCache

BMDS_NAMESPACE_END
