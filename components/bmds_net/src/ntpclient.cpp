/**
 * @file      ntpclient.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's NTP client (ntpclient.h, devplan/network.md §22): servers from
 * net/ntp/servers and DHCP, a burst at start, then a poll from 64 s to 1024 s,
 * every exchange through the clock filter (bmds/ntp.h), and the chosen
 * server's samples disciplining the system clock. All of it in the network
 * task under the core lock, run by lwIP's timers and receive callback.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/timeouts.h>
#include <lwip/udp.h>
#include <bmds/clock.h>
#include <bmds/ethernet.h>
#include <bmds/log.h>
#include <bmds/netstack.h>
#include <bmds/ntp.h>
#include <bmds/random.h>
#include <bmds/psramobject.h>
#include <bmds/stats.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

#include "netinterface.h"
#include "ntpclient.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned int BurstCount = 4;
        constexpr u32_t        BurstIntervalMs = 2000;
        // Polls without an answer before a name is looked up again (a pool gives another server).
        constexpr uint8_t ResolveAfter = 8;
        constexpr int32_t MaxConfigured = 255;

        enum Setting : uint8_t {
                Enabled,
                Servers,
                SettingCount,
        };

        struct Server {
                        char        name[NetNames::MaxHostName + 1] = {};
                        bool        fromDhcp = false;
                        bool        literal = false; // an address, not a name
                        bool        resolving = false;
                        bool        denied = false;
                        bool        outstanding = false; // a request without its answer yet
                        uint32_t    address = 0;         // network order
                        uint8_t     reach = 0;
                        uint8_t     unanswered = 0;
                        uint64_t    cookie = 0;
                        Timestamp   sent;            // T1
                        int64_t     answeredNs = -1; // monotonic
                        Ntp::Reply  reply;           // the last used
                        Ntp::Filter filter;
        };

        struct Client {
                        SysConf::Id       settings[SettingCount] = {};
                        SysConf::Watcher  watcher;
                        std::atomic<bool> settingsPending{true};
                        std::atomic<bool> serversPending{false};
                        std::atomic<bool> addressPending{false};
                        Stats::Group      group;
                        // Under the core lock:
                        bool            enabled = false;
                        bool            synced = false;
                        bool            hadAddress = false;
                        uint32_t        generation = 0; // the server list's, for lookups answering late
                        struct udp_pcb *pcb = nullptr;
                        Server          servers[NtpClient::MaxServers];
                        size_t          count = 0;
                        int             chosen = -1;
                        unsigned int    burstLeft = 0;
                        Ntp::Discipline discipline;
                        int64_t         offsetNs = 0;
                        int64_t         delayNs = 0;
                        int64_t         jitterNs = 0;
                        uint32_t        requests = 0;
                        uint32_t        replies = 0;
                        uint32_t        rejected = 0;
                        char            configured[MaxConfigured + 1] = {}; // net/ntp/servers as last read

                        Client();
        };

        void readCounters(void *arg, Span<int64_t> v);

        using Unit = Stat::Unit;
        using Kind = Stat::Kind;

        // net.ntp: names are an interface once shipped.
        constexpr Stat::Desc Counters[] = {
                {"synced", Unit::Count, Kind::State, 0, {}, "no|yes", "The clock follows an NTP server"},
                {"offset_us",
                 Unit::Micros,
                 Kind::Level,
                 Stat::KeepHistory,
                 {},
                 "",
                 "The last offset from the server corrected (positive: the clock was behind)"},
                {"delay_us", Unit::Micros, Kind::Level, 0, {}, "", "That exchange's round trip"},
                {"jitter_us", Unit::Micros, Kind::Level, 0, {}, "", "The server's jitter (its offsets' spread)"},
                {"stratum", Unit::Count, Kind::Info, 0, {}, "", "The server's stratum (1: it has a reference clock)"},
                {"poll_s", Unit::Count, Kind::Info, 0, {}, "", "Seconds between polls (64 to 1024)"},
                {"requests", Unit::Count, Kind::Total, 0, {}, "", "Requests sent"},
                {"replies", Unit::Count, Kind::Total, 0, {}, "", "Replies used"},
                {"rejected",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Replies not used: not ours, unsynchronised, bogus, or a kiss-o'-death"},
        };

        Client::Client() : group(Counters, sizeof(Counters) / sizeof(Counters[0]), readCounters, nullptr) {}

        constinit PsramObject<Client>  client;
        constinit bool                 started = false;

        void tick(void *);

        int64_t nowNs() {
                return Clock::monotonicNs();
        }

        Error checkServers(StringView value) {
                size_t     count = 0;
                const bool ok = NetNames::forEachItem(value, [&](StringView item) {
                        return NetNames::isHostName(item) && ++count <= NtpClient::MaxSettingServers;
                });
                return ok ? Error::Ok : Error::Invalid;
        }

        void settingsChanged(void *) {
                client->settingsPending.store(true);
                NetStack::wake();
                return;
        }

        bool listed(uint32_t address) {
                for (size_t i = 0; i < client->count; ++i)
                        if (client->servers[i].address == address) return true;
                return false;
        }

        // The servers: the setting's, then each interface's from DHCP (core lock held).
        void buildServers() {
                ++client->generation;
                for (Server &s : client->servers) s = Server{};
                client->count = 0;
                client->chosen = -1;
                (void)NetNames::forEachItem(StringView(client->configured), [](StringView item) {
                        if (client->count == NtpClient::MaxSettingServers || !NetNames::isHostName(item)) return true;
                        Server &s = client->servers[client->count++];
                        std::memcpy(s.name, item.data(), item.size());
                        s.name[item.size()] = '\0';
                        uint32_t a = 0;
                        if (Ipv4::parse(item, a)) {
                                s.literal = true;
                                s.address = lwip_htonl(a);
                        }
                        return true;
                });
                for (size_t n = 0; n < NetInterface::count(); ++n) {
                        for (uint32_t a : NetInterface::at(n).leaseServers().ntp) {
                                if (a == 0 || listed(a) || client->count == NtpClient::MaxServers) continue;
                                Server &s = client->servers[client->count++];
                                Ipv4::format(lwip_ntohl(a), s.name);
                                s.fromDhcp = true;
                                s.literal = true;
                                s.address = a;
                        }
                }
                return;
        }

        void startBurst() {
                sys_untimeout(&tick, nullptr);
                if (!client->enabled) return;
                client->burstLeft = BurstCount;
                tick(nullptr);
                return;
        }

        // The settings and servers, again (core lock held): a burst with the new list.
        void restart() {
                client->discipline.reset();
                client->synced = false;
                buildServers();
                if (!client->enabled) {
                        sys_untimeout(&tick, nullptr);
                        if (client->pcb != nullptr) udp_remove(client->pcb);
                        client->pcb = nullptr;
                        return;
                }
                startBurst();
                return;
        }

        // --- Choosing and disciplining ----------------------------------------------------

        // The least root distance among the servers with samples; -1 if none.
        int choose(int64_t now) {
                int      best = -1;
                uint64_t bestDistance = UINT64_MAX;
                for (size_t i = 0; i < client->count; ++i) {
                        const Server &s = client->servers[i];
                        if (s.denied || s.reach == 0 || s.filter.count() == 0) continue;
                        const uint64_t d = uint64_t(s.reply.rootDelayNs / 2) + uint64_t(s.reply.rootDispersionNs) +
                                           uint64_t(s.filter.best(now).delayNs / 2) + uint64_t(s.filter.jitterNs(now));
                        if (d < bestDistance) {
                                bestDistance = d;
                                best = static_cast<int>(i);
                        }
                }
                return best;
        }

        void formatOffset(int64_t ns, char (&out)[24]) {
                const uint64_t m = ns < 0 ? 0 - static_cast<uint64_t>(ns) : static_cast<uint64_t>(ns);
                std::snprintf(out, sizeof(out), "%c%llu.%06llu s", ns < 0 ? '-' : '+',
                              static_cast<unsigned long long>(m / Timestamp::NsPerSecond),
                              static_cast<unsigned long long>(m % Timestamp::NsPerSecond / 1000));
                return;
        }

        void discipline(Server &s, const Ntp::Sample &sample, int64_t now) {
                const ClockStatus             status = Clock::status();
                const int64_t                 jitter = s.filter.jitterNs(now);
                const Ntp::Discipline::Action a = client->discipline.update(
                        now, sample.offsetNs, Clock::isSet(), status.frequencyPpb, status.slewRemainingNs, jitter);
                if (a.step) {
                        char shown[24];
                        formatOffset(a.offsetNs, shown);
                        (void)Clock::set(Timestamp{Clock::now().ns + a.offsetNs}, ClockSource::Ntp);
                        bmdsInfo("ntp: clock stepped %s (%s, stratum %u)", shown, s.name, s.reply.stratum);
                        // Every offset kept was measured against the clock before the step.
                        for (size_t i = 0; i < client->count; ++i) client->servers[i].filter.clear();
                } else {
                        if (a.setFrequency) (void)Clock::setFrequency(a.frequencyPpb);
                        (void)Clock::adjust(a.offsetNs);
                }
                if (!client->synced) bmdsInfo("ntp: synced to %s", s.name);
                client->synced = true;
                client->offsetNs = sample.offsetNs;
                client->delayNs = sample.delayNs;
                client->jitterNs = jitter;
                return;
        }

        // --- Sending and receiving --------------------------------------------------------

        void send(Server &s) {
                if (client->pcb == nullptr || s.address == 0 || s.denied) return;
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, Ntp::PacketBytes, PBUF_RAM);
                if (p == nullptr) return;
                do Random::fill(&s.cookie, sizeof(s.cookie));
                while (s.cookie == 0);
                uint8_t packet[Ntp::PacketBytes];
                Ntp::buildRequest(packet, s.cookie);
                (void)pbuf_take(p, packet, sizeof(packet));
                ip_addr_t to;
                ip_addr_set_ip4_u32(&to, s.address);
                s.sent = Clock::now();
                const err_t err = udp_sendto(client->pcb, p, &to, Ntp::Port);
                pbuf_free(p);
                s.outstanding = err == ERR_OK;
                if (err == ERR_OK) ++client->requests;
                return;
        }

        void resolved(const char *, const ip_addr_t *address, void *arg) {
                const uintptr_t tag = reinterpret_cast<uintptr_t>(arg);
                const size_t    i = tag % NtpClient::MaxServers;
                if (!started || tag / NtpClient::MaxServers != (client->generation & 0xffffffu)) return; // a list since
                Server &s = client->servers[i];
                s.resolving = false;
                if (address == nullptr) return;
                s.address = ip_addr_get_ip4_u32(address);
                send(s);
                return;
        }

        void resolve(Server &s, size_t i) {
                if (s.resolving) return;
                const uintptr_t tag = uintptr_t(client->generation & 0xffffffu) * NtpClient::MaxServers + i;
                ip_addr_t       a;
                const err_t     err = dns_gethostbyname(s.name, &a, &resolved, reinterpret_cast<void *>(tag));
                if (err == ERR_OK) {
                        s.address = ip_addr_get_ip4_u32(&a);
                        send(s);
                } else if (err == ERR_INPROGRESS) {
                        s.resolving = true;
                } // else no DNS server yet: an address coming starts a burst
                return;
        }

        // An answer (or anything to our port), in the network task.
        void received(void *, struct udp_pcb *, struct pbuf *p, const ip_addr_t *from, u16_t port) {
                const Timestamp t4 = Clock::now(); // first: the arrival
                const int64_t   now = nowNs();
                uint8_t         packet[Ntp::PacketBytes];
                const u16_t     n = pbuf_copy_partial(p, packet, sizeof(packet), 0);
                pbuf_free(p);
                if (port != Ntp::Port) return;
                for (size_t i = 0; i < client->count; ++i) {
                        Server &s = client->servers[i];
                        if (!s.outstanding || s.address != ip_addr_get_ip4_u32(from)) continue;
                        Ntp::Reply         r;
                        const Ntp::Verdict v = Ntp::parse(Span<const uint8_t>(packet, n), s.cookie, r);
                        if (v == Ntp::Verdict::NotOurs) continue; // maybe another's
                        s.outstanding = false;
                        if (v == Ntp::Verdict::Kiss) {
                                ++client->rejected;
                                if (std::strcmp(r.kiss, "RATE") == 0) client->discipline.slowDown();
                                if (std::strcmp(r.kiss, "DENY") == 0 || std::strcmp(r.kiss, "RSTR") == 0) {
                                        bmdsWarn("ntp: %s refuses us (%s)", s.name, r.kiss);
                                        s.denied = true;
                                }
                                return;
                        }
                        if (v != Ntp::Verdict::Ok) {
                                ++client->rejected;
                                return;
                        }
                        ++client->replies;
                        s.reach |= 1;
                        s.unanswered = 0;
                        s.answeredNs = now;
                        s.reply = r;
                        const Ntp::Sample sample = Ntp::sampleOf(s.sent, r.receive, r.transmit, t4, now);
                        s.filter.add(sample);
                        client->chosen = choose(now);
                        if (client->chosen == static_cast<int>(i) && s.filter.newestIsBest(now))
                                discipline(s, sample, now);
                        return;
                }
                ++client->rejected;
                return;
        }

        // A poll: each server asked (or its name looked up), and the next poll set.
        void tick(void *) {
                if (!client->enabled) return;
                if (client->pcb == nullptr) {
                        client->pcb = udp_new();
                        if (client->pcb == nullptr) return; // no memory: an address change tries again
                        (void)udp_bind(client->pcb, IP_ANY_TYPE, 0);
                        udp_recv(client->pcb, &received, nullptr);
                }
                for (size_t i = 0; i < client->count; ++i) {
                        Server &s = client->servers[i];
                        if (s.denied) continue;
                        s.reach = static_cast<uint8_t>(s.reach << 1);
                        if (s.unanswered < UINT8_MAX) ++s.unanswered;
                        if (!s.literal && s.unanswered > ResolveAfter) {
                                s.address = 0; // a pool's name: another server, maybe
                                s.unanswered = 0;
                        }
                        if (s.address == 0)
                                resolve(s, i);
                        else
                                send(s);
                }
                if (client->chosen >= 0 && client->servers[client->chosen].reach == 0) {
                        if (client->synced) bmdsWarn("ntp: %s stopped answering", client->servers[client->chosen].name);
                        client->synced = false;
                        client->chosen = choose(nowNs());
                }
                u32_t next = BurstIntervalMs;
                if (client->burstLeft > 0) --client->burstLeft;
                if (client->burstLeft == 0) next = u32_t(1000) << client->discipline.poll();
                sys_timeout(next, &tick, nullptr);
                return;
        }

        bool anyAddress() {
                for (size_t n = 0; n < NetInterface::count(); ++n)
                        if (!ip4_addr_isany_val(*netif_ip4_addr(&NetInterface::at(n).netif()))) return true;
                return false;
        }

        // --- Counters ---------------------------------------------------------------------

        int64_t micros(int64_t ns) {
                return ns / 1000;
        }

        void readCounters(void *, Span<int64_t> v) {
                NetStack::QuietLocker lock;
                const Client         &c = *client;
                v[0] = c.synced ? 1 : 0;
                v[1] = micros(c.offsetNs);
                v[2] = micros(c.delayNs);
                v[3] = micros(c.jitterNs);
                v[4] = c.chosen >= 0 ? c.servers[c.chosen].reply.stratum : 0;
                v[5] = int64_t(1) << c.discipline.poll();
                v[6] = c.requests;
                v[7] = c.replies;
                v[8] = c.rejected;
                return;
        }

} // namespace

Error NtpClient::init() {
        if (client.create() == nullptr) return Error::NoMem;
        static constexpr struct {
                        const char   *key;
                        Config::Type  type;
                        const char   *defaultValue;
                        const char   *help;
                        int32_t       max;
                        Config::Check check;
        } Decls[SettingCount] = {
                {"net/ntp/enabled", Config::Type::Bool, "true", "Set the clock from NTP servers", 0, nullptr},
                {"net/ntp/servers", Config::Type::String, "pool.ntp.org",
                 "NTP servers, names or addresses, ';'-separated (DHCP's are added)", MaxConfigured, &checkServers},
        };
        for (size_t i = 0; i < SettingCount; ++i) {
                Config::Param p;
                p.key = Decls[i].key;
                p.type = Decls[i].type;
                p.defaultValue = Decls[i].defaultValue;
                p.help = Decls[i].help;
                p.max = Decls[i].max;
                p.check = Decls[i].check;
                auto [id, err] = SysConf::declareKernel(p);
                if (err.isError()) {
                        bmdsErr("ntp: setting %s not declared: %s", Decls[i].key, err.desc());
                        return err;
                }
                client->settings[i] = id;
        }
        const Error err = client->watcher.start(StringView("net/ntp"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        client->group.setName("net.ntp");
        (void)Stats::add(client->group); // counters only: the client works without them
        started = true;
        return Error::Ok;
}

void NtpClient::poll() {
        if (!started) return;
        bool again = client->serversPending.exchange(false);
        if (client->settingsPending.exchange(false)) {
                SysConf::Id id;
                while (client->watcher.next(id)) {}
                (void)client->watcher.overflowed();
                auto [enabled, err] = SysConf::getBool(client->settings[Enabled]);
                client->enabled = err.isError() || enabled;
                String servers;
                (void)SysConf::get(client->settings[Servers], servers);
                const size_t n =
                        servers.size() < sizeof(client->configured) ? servers.size() : sizeof(client->configured) - 1;
                std::memcpy(client->configured, servers.data(), n);
                client->configured[n] = '\0';
                again = true;
        }
        bool burst = false;
        if (client->addressPending.exchange(false)) {
                const bool has = anyAddress();
                burst = has && !client->hadAddress; // names can be looked up now, servers reached
                client->hadAddress = has;
        }
        if (again)
                restart(); // with a burst
        else if (burst)
                startBurst();
        return;
}

void NtpClient::serversChanged() {
        if (!started) return;
        client->serversPending.store(true);
        return;
}

void NtpClient::addressChanged() {
        if (!started) return;
        client->addressPending.store(true);
        return;
}

Error NtpClient::burst() {
        if (!started) return Error::NotReady;
        NetStack::Locker lock;
        if (!client->enabled) return Error::InvalidState;
        for (size_t i = 0; i < client->count; ++i) {
                Server &s = client->servers[i];
                s.denied = false;
                if (!s.literal) s.address = 0; // looked up again
        }
        startBurst();
        return Error::Ok;
}

void NtpClient::status(Status &out) {
        out = Status{};
        if (!started) return;
        NetStack::QuietLocker lock;
        const Client         &c = *client;
        const int64_t         now = nowNs();
        out.enabled = c.enabled;
        out.synced = c.synced;
        out.poll = c.discipline.poll();
        out.stratum = c.chosen >= 0 ? c.servers[c.chosen].reply.stratum : 0;
        out.offsetNs = c.offsetNs;
        out.delayNs = c.delayNs;
        out.jitterNs = c.jitterNs;
        out.requests = c.requests;
        out.replies = c.replies;
        out.rejected = c.rejected;
        out.count = c.count;
        for (size_t i = 0; i < c.count; ++i) {
                const Server &s = c.servers[i];
                ServerStatus &o = out.servers[i];
                std::memcpy(o.name, s.name, sizeof(o.name));
                o.fromDhcp = s.fromDhcp;
                o.resolving = s.resolving;
                o.denied = s.denied;
                o.chosen = c.chosen == static_cast<int>(i);
                o.address = s.address;
                o.stratum = s.reply.stratum;
                o.reach = s.reach;
                o.samples = static_cast<uint8_t>(s.filter.count());
                o.sinceAnswerNs = s.answeredNs >= 0 ? now - s.answeredNs : -1;
                const Ntp::Sample b = s.filter.best(now);
                o.offsetNs = b.offsetNs;
                o.delayNs = b.delayNs;
                o.jitterNs = s.filter.jitterNs(now);
        }
        return;
}

BMDS_NAMESPACE_END
