/**
 * @file      netstatus.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network interfaces' state for the rest of the system (devplan/network.md
 * §17, §18): the `net_info` system call, the interface ioctls, the tray's icon.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Snapshots of the network interfaces, taken under the core lock: no
 *        lwIP or driver types; and their changes, told to watchers.
 *
 * A change is one to anything in `Info` but the counters and the lease's time
 * left: an interface enabled or not, its link, its settings, its addresses,
 * DHCP's state. The network task compares the interfaces' state after each
 * event that may have changed it (a link's interrupt, a setting, an address,
 * a DHCP acknowledgement) with the state last told, and tells the watchers
 * only of a real change: nothing is looked at on a timer.
 *
 * @par Thread Safety
 * Thread-safe; not from lwIP's callbacks (the core lock).
 */
namespace NetStatus {

        /** @brief How an interface's IPv4 address is found. */
        enum class Mode : uint8_t {
                Dhcp,   ///< DHCP.
                Static, ///< Set (`net/ethN/ipv4/address`).
                Off,    ///< None.
        };

        /** @brief DHCP's state. */
        enum class Dhcp : uint8_t {
                None,   ///< Not used.
                Asking, ///< No lease yet (or it was lost).
                Bound,  ///< A lease is held.
        };

        /** @brief One interface. Addresses are IPv4 in network order; 0 is none. */
        struct Info {
                        char     name[16] = {};      ///< "eth0", "lo".
                        uint32_t index = 0;          ///< Its interface index (`if_nametoindex`).
                        bool     up = false;         ///< Enabled.
                        bool     link = false;       ///< The link is up.
                        bool     fullDuplex = false; ///< The link is full duplex.
                        bool     loopback = false;   ///< `lo`.
                        bool     broadcast = false;  ///< Has a broadcast address.
                        bool     multicast = false;  ///< Joins multicast groups.
                        uint16_t speedMbps = 0;      ///< 0 with no link.
                        uint32_t mtu = 0;
                        uint8_t  mac[6] = {};
                        Mode     mode = Mode::Off;
                        Dhcp     dhcp = Dhcp::None;
                        uint32_t address = 0;
                        uint32_t netmask = 0;
                        uint32_t gateway = 0;
                        uint32_t dhcpServer = 0;
                        uint32_t leaseSeconds = 0;     ///< The lease's length.
                        uint32_t leaseLeftSeconds = 0; ///< To the minute.
                        uint32_t dns[2] = {};          ///< The DNS servers DHCP gave.
                        uint64_t rxBytes = 0;
                        uint64_t txBytes = 0;
                        uint32_t rxFrames = 0;
                        uint32_t txFrames = 0;
                        uint32_t rxErrors = 0;
                        uint32_t txErrors = 0;
                        uint32_t rxDropped = 0; ///< No buffer, the FIFO full, or the stack had no memory.
                        uint32_t txDropped = 0;
        };

        /**
         * @brief Told of each change (`changes()` moved on), on the network task with
         *        the core lock held: it must not block, nor take the core lock.
         *
         * Owned by the listener. `start()` and `stop()` take the core lock: a
         * notification running on the network task has ended when `stop()` returns.
         */
        class Watcher {
                public:
                        /** @brief The notification. */
                        using Notify = void (*)(void *arg);

                        constexpr Watcher() = default;
                        ~Watcher() { stop(); }
                        Watcher(const Watcher &) = delete;
                        Watcher &operator=(const Watcher &) = delete;

                        /**
                         * @brief Starts telling @p notify (not null) of changes.
                         * @return `Ok`, `InvalidState` (started, or no network stack), `Invalid`.
                         */
                        Error start(Notify notify, void *arg);

                        /** @brief Stops (nothing if not started). @blocking (the core lock) */
                        void stop();

                        /** @brief Returns true if started. */
                        bool isStarted() const { return _notify != nullptr; }

                        // The watch list's (core lock held).
                        Watcher *nextWatcher = nullptr;
                        void     tell() { _notify(_arg); }

                private:
                        Notify _notify = nullptr;
                        void  *_arg = nullptr;
        };

        /** @brief Returns the number of changes told since boot (it wraps). @nonblocking */
        uint32_t changes();

        /**
         * @brief Opens a watch as a file (the SDK's `net_watch`, `devplan/network.md` §20):
         *        a read gives one `uint32_t` record, `changes()` after the change, waiting
         *        while nothing changed since the handle's last read (or its opening); it is
         *        readable for `io_poll` when something did. The watch ends with the handle.
         * @return The handle (`FileHandle::Read`), or `InvalidState` (no network stack), `NoMem`.
         */
        Result<FileHandle::Ptr> openWatch();

        /**
         * @brief Asks DHCP to renew interface @p position's lease now.
         * @return `Ok`, `NotExist`, `InvalidState` (the interface holds no DHCP lease), or `IoError`.
         */
        Error renewLease(size_t position);

        /** @brief Returns the number of interfaces (with `lo`). */
        size_t count();

        /**
         * @brief Fills @p info with interface @p position: 0 up to `count()`, in the
         *        order of their interface indexes (`lo` first).
         * @return `Ok`, or `NotExist` (past the last, or before the stack is up).
         */
        Error info(size_t position, Info &info);

} // namespace NetStatus

BMDS_NAMESPACE_END
