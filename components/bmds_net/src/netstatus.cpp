/**
 * @file      netstatus.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network interfaces' state (netstatus.h), read from lwIP's netifs,
 * the interfaces and their devices under the core lock, and its changes
 * told to watchers (devplan/network.md §20).
 */

#include <atomic>
#include <cstring>
#include <new>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/netstack.h>
#include <bmds/netstatus.h>
#include <bmds/semaphore.h>
#include <bmds/waitlist.h>

#include "netchanges.h"
#include "netinterface.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The netif at @p position in index order, or null. Core lock held.
        struct netif *netifAt(size_t position) {
                size_t seen = 0;
                for (unsigned int index = 1; index <= 255; ++index) {
                        struct netif *n = netif_get_by_index(static_cast<u8_t>(index));
                        if (n == nullptr) continue;
                        if (seen++ == position) return n;
                }
                return nullptr;
        }

        NetInterface *interfaceOf(struct netif *n) {
                for (size_t i = 0; i < NetInterface::count(); ++i) {
                        if (&NetInterface::at(i).netif() == n) return &NetInterface::at(i);
                }
                return nullptr;
        }

        // The interfaces told of: one per Ethernet device, and lo.
        constexpr size_t MaxTold = NetInterface::MaxInterfaces + 1;

        // The watches, and the state last told. The list and `told` are the core lock's.
        struct Changes {
                        NetStatus::Watcher   *watchers = nullptr;
                        std::atomic<bool>     pending{true}; // looked at once the task runs
                        std::atomic<uint32_t> count{0};
                        NetStatus::Info       told[MaxTold] = {};
                        size_t                toldCount = 0;
        };

        constinit Changes changeState;

        // True if @p a and @p b tell the same: all but the counters and the lease's time left.
        bool sameState(const NetStatus::Info &a, const NetStatus::Info &b) {
                return std::strncmp(a.name, b.name, sizeof(a.name)) == 0 && a.index == b.index && a.up == b.up &&
                       a.link == b.link && a.fullDuplex == b.fullDuplex && a.loopback == b.loopback &&
                       a.broadcast == b.broadcast && a.multicast == b.multicast && a.speedMbps == b.speedMbps &&
                       a.mtu == b.mtu && std::memcmp(a.mac, b.mac, sizeof(a.mac)) == 0 && a.mode == b.mode &&
                       a.dhcp == b.dhcp && a.address == b.address && a.netmask == b.netmask && a.gateway == b.gateway &&
                       a.dhcpServer == b.dhcpServer && a.leaseSeconds == b.leaseSeconds && a.dns[0] == b.dns[0] &&
                       a.dns[1] == b.dns[1];
        }

        // Fills @p info with interface @p position. Core lock held.
        Error fill(size_t position, NetStatus::Info &info) {
                using Mode = NetStatus::Mode;
                using Dhcp = NetStatus::Dhcp;
                info = NetStatus::Info{};
                struct netif *n = netifAt(position);
                if (n == nullptr) return Error::NotExist;
                NetInterface *ni = interfaceOf(n);
                const char   *name = ni != nullptr ? ni->name() : "lo";
                std::strncpy(info.name, name, sizeof(info.name) - 1);
                info.index = netif_get_index(n);
                info.up = netif_is_up(n);
                info.loopback = ni == nullptr;
                info.broadcast = (n->flags & NETIF_FLAG_BROADCAST) != 0;
                info.multicast = (n->flags & NETIF_FLAG_IGMP) != 0;
                info.mtu = n->mtu;
                info.address = ip4_addr_get_u32(netif_ip4_addr(n));
                info.netmask = ip4_addr_get_u32(netif_ip4_netmask(n));
                info.gateway = ip4_addr_get_u32(netif_ip4_gw(n));
                if (ni == nullptr) {
                        info.link = netif_is_link_up(n);
                        info.mode = Mode::Static;
                        return Error::Ok;
                }
                std::memcpy(info.mac, n->hwaddr, sizeof(info.mac));
                const EthernetDevice::Link link = ni->device().link();
                info.link = link.up;
                info.fullDuplex = link.fullDuplex;
                info.speedMbps = link.speedMbps;
                switch (ni->mode()) {
                        case NetInterface::Mode::Dhcp: info.mode = Mode::Dhcp; break;
                        case NetInterface::Mode::Static: info.mode = Mode::Static; break;
                        case NetInterface::Mode::Off: info.mode = Mode::Off; break;
                }
                const struct dhcp *d = netif_dhcp_data(n);
                if (info.mode == Mode::Dhcp && d != nullptr) {
                        info.dhcp = dhcp_supplied_address(n) ? Dhcp::Bound : Dhcp::Asking;
                        if (info.dhcp == Dhcp::Bound) {
                                info.dhcpServer = ip4_addr_get_u32(ip_2_ip4(&d->server_ip_addr));
                                info.leaseSeconds = d->offered_t0_lease;
                                const uint32_t used = static_cast<uint32_t>(d->lease_used) * DHCP_COARSE_TIMER_SECS;
                                info.leaseLeftSeconds = used < info.leaseSeconds ? info.leaseSeconds - used : 0;
                        }
                }
                info.dns[0] = ni->leaseServers().dns[0];
                info.dns[1] = ni->leaseServers().dns[1];
                const EthernetDevice::Counters c = ni->device().counters();
                const NetInterface::Counters  &k = ni->counters();
                info.rxBytes = c.rxBytes;
                info.txBytes = c.txBytes;
                info.rxFrames = static_cast<uint32_t>(c.rxFrames);
                info.txFrames = static_cast<uint32_t>(c.txFrames);
                info.rxErrors = c.rxErrors;
                info.txErrors = c.txErrors;
                info.rxDropped = c.rxNoBuffer + c.rxMissed + c.rxOverflow + k.rxDropped;
                info.txDropped = k.txDropped;
                return Error::Ok;
        }

        // A watch as a file (the SDK's net_watch): a read gives changes() after a change, as one
        // uint32_t, and waits while there has been none since the last.
        class WatchStream final : public Stream {
                public:
                        Error start() {
                                _seen.store(changeState.count.load());
                                return _watcher.start(changed, this);
                        }

                        Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                if (buf.size() < sizeof(uint32_t) || buf.size() % sizeof(uint32_t) != 0)
                                        return makeError<size_t>(Error::Invalid);
                                Deadline deadline(timeoutMs);
                                if (_readLock.lock(deadline).isError()) return makeError<size_t>(Error::Timeout);
                                Result<size_t> result;
                                for (;;) {
                                        const uint32_t now = changeState.count.load();
                                        if (now != _seen.load()) {
                                                _seen.store(now);
                                                std::memcpy(buf.data(), &now, sizeof(now));
                                                result = makeResult(sizeof(now));
                                                break;
                                        }
                                        const Error err = Interrupt::waitCurrent(_changed, deadline);
                                        if (err.isError()) {
                                                result = makeError<size_t>(err);
                                                break;
                                        }
                                }
                                _readLock.unlock();
                                return result;
                        }

                        Result<size_t> write(ByteSpan, unsigned int) override {
                                return makeError<size_t>(Error::NotSupported);
                        }

                        uint8_t readiness() override { return changeState.count.load() != _seen.load() ? Readable : 0; }

                        WaitList *readinessWaiters() override { return &_pollers; }

                        // Its only handle (it is opened once) went: the watch ends.
                        void handleClosed(uint8_t) override {
                                _watcher.stop();
                                this->~WatchStream();
                                Memory::free(this);
                                return;
                        }

                private:
                        // On the network task (must not block).
                        static void changed(void *arg) {
                                auto *self = static_cast<WatchStream *>(arg);
                                self->_changed.release();
                                self->_pollers.notify();
                                return;
                        }

                        NetStatus::Watcher    _watcher;
                        std::atomic<uint32_t> _seen{0};
                        Mutex                 _readLock;
                        Semaphore             _changed{1, 0}; // released at each change told of
                        WaitListOf<4>         _pollers;
        };

} // namespace

Error NetStatus::Watcher::start(Notify notify, void *arg) {
        if (notify == nullptr) return Error::Invalid;
        if (!NetStack::isReady()) return Error::InvalidState;
        NetStack::QuietLocker lock;
        if (_notify != nullptr) return Error::InvalidState;
        _notify = notify;
        _arg = arg;
        nextWatcher = changeState.watchers;
        changeState.watchers = this;
        return Error::Ok;
}

void NetStatus::Watcher::stop() {
        if (_notify == nullptr) return;
        NetStack::QuietLocker lock;
        for (Watcher **w = &changeState.watchers; *w != nullptr; w = &(*w)->nextWatcher) {
                if (*w == this) {
                        *w = nextWatcher;
                        break;
                }
        }
        nextWatcher = nullptr;
        _notify = nullptr;
        _arg = nullptr;
        return;
}

uint32_t NetStatus::changes() {
        return changeState.count.load();
}

void NetStatus::noteChange() {
        changeState.pending.store(true);
        return;
}

void NetStatus::publish() {
        Changes &c = changeState;
        if (!c.pending.exchange(false)) return;
        bool   differs = false;
        size_t n = 0;
        for (; n < MaxTold; ++n) {
                Info now;
                if (fill(n, now).isError()) break;
                if (n >= c.toldCount || !sameState(now, c.told[n])) differs = true;
                c.told[n] = now;
        }
        if (n != c.toldCount) differs = true;
        c.toldCount = n;
        if (!differs) return;
        c.count.fetch_add(1);
        for (Watcher *w = c.watchers; w != nullptr; w = w->nextWatcher) w->tell();
        return;
}

Result<FileHandle::Ptr> NetStatus::openWatch() {
        if (!NetStack::isReady()) return makeError<FileHandle::Ptr>(Error::InvalidState);
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(WatchStream));
        if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *stream = ::new (memory) WatchStream();
        Error err = stream->start();
        if (err.isOk()) {
                auto [handle, herr] = FileHandle::open(*stream, FileHandle::Read);
                if (herr.isOk()) return makeResult(std::move(handle));
                err = herr;
        }
        stream->~WatchStream();
        Memory::free(stream);
        return makeError<FileHandle::Ptr>(err);
}

Error NetStatus::renewLease(size_t position) {
        if (!NetStack::isReady()) return Error::NotExist;
        NetStack::Locker lock; // the task's timers: dhcp_renew() set them
        struct netif    *n = netifAt(position);
        if (n == nullptr) return Error::NotExist;
        NetInterface *ni = interfaceOf(n);
        if (ni == nullptr || ni->mode() != NetInterface::Mode::Dhcp || !dhcp_supplied_address(n))
                return Error::InvalidState;
        return dhcp_renew(n) == ERR_OK ? Error::Ok : Error::IoError;
}

size_t NetStatus::count() {
        if (!NetStack::isReady()) return 0;
        NetStack::QuietLocker lock;
        size_t                n = 0;
        struct netif         *i;
        NETIF_FOREACH(i) {
                (void)i;
                ++n;
        }
        return n;
}

Error NetStatus::info(size_t position, Info &info) {
        info = Info{};
        if (!NetStack::isReady()) return Error::NotExist;
        NetStack::QuietLocker lock;
        return fill(position, info);
}

BMDS_NAMESPACE_END
