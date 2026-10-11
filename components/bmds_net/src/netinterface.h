/**
 * @file      netinterface.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An Ethernet device as an lwIP interface (devplan/network.md §6, §15).
 * Private to bmds_net: it shows lwIP's types.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/ethernetdevice.h>
#include <bmds/hostname.h>
#include <bmds/multicastfilter.h>
#include <bmds/spinlock.h>
#include <bmds/stringview.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An `EthernetDevice` joined to lwIP's `netif`, named after it (`eth0`).
 *
 * Received frames are queued by the device's task and given to lwIP by the
 * network task (`poll()`), in the driver's buffers while lwIP holds few of
 * them, copied when it holds many. Sending gathers lwIP's buffers into one
 * of the driver's. The settings (`net/eth0/...`) are declared when the
 * interface is made and watched: a change applies at once.
 *
 * @par Thread Safety
 * Everything but the device's callbacks runs under the core lock.
 */
class NetInterface {
        public:
                /** @brief Interfaces the system can have (one per Ethernet device). */
                static constexpr size_t MaxInterfaces = EthernetDevice::MaxDevices;

                /** @brief Received buffers lwIP may hold before frames are copied instead. */
                static constexpr uint32_t MaxHeld = 6;

                /** @brief How IPv4 addresses are found (`net/ethN/ipv4/mode`). */
                enum class Mode : uint8_t {
                        Dhcp,
                        Static,
                        Off,
                };

                /** @brief Counters since boot, beyond the device's and lwIP's. */
                struct Counters {
                                uint32_t rxCopied = 0;  ///< Frames copied because lwIP held many buffers.
                                uint32_t rxDropped = 0; ///< Frames dropped (interface down, no memory).
                                uint32_t txDropped = 0; ///< Frames lwIP sent that the device didn't take.
                };

                /** @brief Makes the interface for @p device (core lock held). @return `Ok`, `Full`, `NoMem`. */
                static Error create(EthernetDevice &device);

                /** @brief Returns the number of interfaces. */
                static size_t count();

                /** @brief Returns interface @p index (below `count()`). */
                static NetInterface &at(size_t index);

                /** @brief Returns the interface named @p name, or nullptr. */
                static NetInterface *find(StringView name);

                /** @brief Network task, core lock held: frames in, the link, the settings. */
                static void pollAll();

                explicit NetInterface(EthernetDevice &device);
                NetInterface(const NetInterface &) = delete;
                NetInterface &operator=(const NetInterface &) = delete;

                /** @brief Returns the name ("eth0"). */
                const char *name() const { return _device.name(); }

                /** @brief Returns lwIP's interface. */
                struct netif &netif() { return _netif; }

                /** @brief Returns the device. */
                EthernetDevice &device() { return _device; }

                /** @brief Returns the address mode in effect. */
                Mode mode() const { return _mode; }

                /** @brief Returns the multicast groups' MAC addresses. */
                const MulticastFilter &groups() const { return _groups; }

                /** @brief Returns the counters. */
                const Counters &counters() const { return _counters; }

                /** @brief Returns the received buffers lwIP holds now. */
                uint32_t held() const { return _held.load(std::memory_order_relaxed); }

        private:
                struct RxPbuf {
                                struct pbuf_custom      custom; // first: lwIP's pbuf is its address
                                NetInterface           *owner = nullptr;
                                EthernetDevice::RxFrame frame;
                };

                enum Setting : uint8_t {
                        Enabled,
                        Mac,
                        Mtu,
                        Ipv4Mode,
                        Ipv4Address,
                        Ipv4Gateway,
                        SettingCount,
                };

                static void  received(void *arg, EthernetDevice &device, const EthernetDevice::RxFrame &frame);
                static void  linkChanged(void *arg, EthernetDevice &device);
                static void  settingsChanged(void *arg);
                static err_t netifInit(struct netif *netif);
                static err_t linkOutput(struct netif *netif, struct pbuf *p);
                static err_t igmpFilter(struct netif *netif, const ip4_addr_t *group,
                                        enum netif_mac_filter_action action);
                static void  statusChanged(struct netif *netif);
                static void  freeRx(struct pbuf *p);

                Error declareSettings();
                void  poll();
                void  input(const EthernetDevice::RxFrame &frame);
                void  applySettings();
                void  apply(Mode was);
                void  applyLink();
                void  logAddress();

                EthernetDevice &_device;
                struct netif    _netif = {};
                Mode            _mode = Mode::Off;
                MulticastFilter _groups;
                Counters        _counters;

                // Frames the device's task has queued for the network task: each holds one of
                // the device's buffers, so the queue can't hold more than there are.
                SpinLock                _rxLock;
                EthernetDevice::RxFrame _rxQueue[EthernetDevice::RxBuffers];
                size_t                  _rxHead = 0;
                size_t                  _rxCount = 0;
                RxPbuf                  _rxPbufs[EthernetDevice::RxBuffers];
                std::atomic<uint32_t>   _held{0};

                std::atomic<bool> _linkPending{true};
                std::atomic<bool> _settingsPending{true};
                SysConf::Watcher  _watcher;
                SysConf::Watcher  _hostnameWatcher;
                SysConf::Id       _settings[SettingCount] = {};
                char              _hostname[HostName::MaxBytes + 1] = {};
                bool              _applying = false;
                uint32_t          _loggedAddress = 0;
                uint32_t          _loggedMask = 0;
                uint32_t          _loggedGateway = 0;
};

BMDS_NAMESPACE_END
