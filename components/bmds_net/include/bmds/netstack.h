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

#include <bmds/config.h>
#include <bmds/error.h>

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

        /** @brief True if the calling task holds the core lock. @nonblocking */
        bool isLocked();

        /** @brief Wakes the network task (frames or work for it). Any task. @nonblocking */
        void wake();

        /** @brief Holds the core lock for its scope. */
        class Locker {
                public:
                        Locker() { lock(); }
                        ~Locker() { unlock(); }
                        Locker(const Locker &) = delete;
                        Locker &operator=(const Locker &) = delete;
        };

} // namespace NetStack

BMDS_NAMESPACE_END
