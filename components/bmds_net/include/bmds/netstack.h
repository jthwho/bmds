/**
 * @file      netstack.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The IP stack (devplan/network.md §3, §15): lwIP, its task and its lock.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The IP stack: lwIP (vendored, `NO_SYS`), the network task that
 *        feeds it frames and runs its timers, and the core lock under which
 *        all of lwIP runs (devplan/network.md §15).
 *
 * The network task (`net`, `CONFIG_BMDS_NET_STACK_PRIORITY`) sleeps until
 * lwIP's next timeout or a wake: frames received, a call queued, a setting
 * changed. Any task may call lwIP while it holds the core lock (`Locker`);
 * lwIP's callbacks run with it held, so they must not take it again.
 *
 * @par Thread Safety
 * Thread-safe. `init()` once, at boot.
 */
namespace NetStack {

        /**
         * @brief Boot stage (after the Ethernet devices): lwIP, `lo`, and an interface for each
         *        Ethernet device, configured from sysconf (`net/ethN/...`).
         * @return `Ok`, `NoMem`, or the error starting the task.
         */
        Error init();

        /** @brief True once `init()` has succeeded. */
        bool isReady();

        /** @brief Takes the core lock. Not from lwIP's callbacks. @blocking */
        void lock();

        /** @brief Releases the core lock, waking the network task to look at lwIP's timers again. */
        void unlock();

        /**
         * @brief Releases the core lock without waking the network task: for a
         *        holder that only looked (it moved no timer and queued nothing).
         */
        void unlockQuietly();

        /** @brief True if the calling task holds the core lock. @nonblocking */
        bool isLocked();

        /** @brief A function the network task calls (core lock held). */
        using Call = void (*)(void *arg);

        /**
         * @brief Has the network task call @p fn with @p arg in @p delayMs (lwIP's
         *        timers: a kernel service on the network task, `NetUdp`'s). Core
         *        lock held; a holder on another task wakes the network task as it
         *        lets go. (A timer comes from lwIP's heap; lwIP asserts if it has none.)
         */
        void callLater(uint32_t delayMs, Call fn, void *arg);

        /** @brief Takes back every call of @p fn with @p arg not made yet. Core lock held. */
        void cancelCall(Call fn, void *arg);

        /** @brief Wakes the network task (frames or work for it). Any task. @nonblocking */
        void wake();

        /**
         * @brief Finds @p name's IPv4 address (network order) into @p ipv4: a dotted quad
         *        as it is; localhost, the host name and `net/hosts` at once; otherwise
         *        asks the DNS servers (`net/dns/servers`, else DHCP's), waiting up to
         *        @p timeoutMs. Not with the core lock held.
         * @return `Ok`, `NotExist` (no such name), `Timeout`, `Interrupted`, `NotReady`
         *         (no DNS server), `Busy` (too many lookups at once), `Invalid`.
         * @blocking
         */
        Error resolve(StringView name, uint32_t &ipv4, unsigned int timeoutMs);

        /** @brief Holds the core lock for its scope. */
        class Locker {
                public:
                        Locker() { lock(); }
                        ~Locker() { unlock(); }
                        Locker(const Locker &) = delete;
                        Locker &operator=(const Locker &) = delete;
        };

        /** @brief Holds the core lock for its scope, only to look (`unlockQuietly()`). */
        class QuietLocker {
                public:
                        QuietLocker() { lock(); }
                        ~QuietLocker() { unlockQuietly(); }
                        QuietLocker(const QuietLocker &) = delete;
                        QuietLocker &operator=(const QuietLocker &) = delete;
        };

} // namespace NetStack

BMDS_NAMESPACE_END
