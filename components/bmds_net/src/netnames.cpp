/**
 * @file      netnames.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Names (netnames.h, devplan/network.md §8, §18): the DNS servers lwIP's
 * resolver asks, the local names it answers itself, NetStack::resolve(),
 * and /etc's resolv.conf, hosts, hostname, services and protocols.
 */

#include <atomic>
#include <cstring>
#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <bmds/ethernet.h>
#include <bmds/hostname.h>
#include <bmds/interrupt.h>
#include <bmds/log.h>
#include <bmds/netstack.h>
#include <bmds/semaphore.h>
#include <bmds/psramobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

#include "lwipport.h"
#include "netinterface.h"
#include "netmdns.h"
#include "netnames.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // net/hosts, parsed: names the resolver answers itself.
        constexpr size_t MaxHosts = 16;
        constexpr size_t MaxHostName = NetNames::MaxHostName;

        // Lookups waiting for lwIP's resolver at once (NetStack::resolve()).
        constexpr size_t MaxLookups = 4;

        enum Setting : uint8_t {
                DnsServers,
                DnsSearch,
                Hosts,
                SettingCount,
        };

        struct Host {
                        char     name[MaxHostName + 1] = {};
                        uint32_t ipv4 = 0; // network order
        };

        // One NetStack::resolve() waiting. A lookup that timed out is abandoned: its
        // slot stays taken until lwIP's answer (or its own timeout) comes.
        struct Lookup {
                        Semaphore done{1, 0};
                        uint32_t  ipv4 = 0;
                        bool      found = false;
                        bool      answered = false;
                        bool      busy = false;
                        bool      abandoned = false;
        };

        struct Names {
                        SysConf::Id       settings[SettingCount] = {};
                        SysConf::Watcher  dnsWatcher;
                        SysConf::Watcher  hostsWatcher;
                        std::atomic<bool> settingsPending{true};
                        std::atomic<bool> serversPending{true};
                        // Under the core lock:
                        Host     hosts[MaxHosts];
                        size_t   hostCount = 0;
                        uint32_t servers[DNS_MAX_SERVERS] = {}; // net/dns/servers, parsed (network order)
                        Lookup   lookups[MaxLookups];
        };

        constinit PsramObject<Names>  names;
        constinit bool                started = false;

        uint32_t networkOrder(uint32_t hostOrder) {
                return lwip_htonl(hostOrder);
        }

        Error checkServers(StringView value) {
                size_t     count = 0;
                const bool ok = NetNames::forEachItem(value, [&](StringView item) {
                        uint32_t a = 0;
                        return Ipv4::parse(item, a) && ++count <= DNS_MAX_SERVERS;
                });
                return ok ? Error::Ok : Error::Invalid;
        }

        Error checkSearch(StringView value) {
                return NetNames::forEachItem(value, [](StringView item) { return NetNames::isHostName(item); })
                               ? Error::Ok
                               : Error::Invalid;
        }

        // "name=address", ';'-separated.
        Error checkHosts(StringView value) {
                size_t     count = 0;
                const bool ok = NetNames::forEachItem(value, [&](StringView item) {
                        const size_t eq = item.indexOf('=');
                        uint32_t     a = 0;
                        return eq != StringView::NPos && NetNames::isHostName(item.left(eq)) &&
                               Ipv4::parse(item.mid(eq + 1), a) && ++count <= MaxHosts;
                });
                return ok ? Error::Ok : Error::Invalid;
        }

        void settingsChanged(void *) {
                names->settingsPending.store(true);
                NetStack::wake();
                return;
        }

        // The settings into the tables (core lock held).
        void readSettings() {
                SysConf::Id id;
                for (SysConf::Watcher *w : {&names->dnsWatcher, &names->hostsWatcher}) {
                        while (w->next(id)) {}
                        (void)w->overflowed();
                }
                String text;
                for (uint32_t &s : names->servers) s = 0;
                if (SysConf::get(names->settings[DnsServers], text).isOk()) {
                        size_t n = 0;
                        (void)NetNames::forEachItem(text.view(), [&](StringView item) {
                                uint32_t a = 0;
                                if (n < DNS_MAX_SERVERS && Ipv4::parse(item, a)) names->servers[n++] = networkOrder(a);
                                return true;
                        });
                }
                names->hostCount = 0;
                if (SysConf::get(names->settings[Hosts], text).isOk()) {
                        (void)NetNames::forEachItem(text.view(), [](StringView item) {
                                const size_t eq = item.indexOf('=');
                                uint32_t     a = 0;
                                if (eq == StringView::NPos || names->hostCount == MaxHosts ||
                                    !NetNames::isHostName(item.left(eq)) || !Ipv4::parse(item.mid(eq + 1), a))
                                        return true;
                                Host &h = names->hosts[names->hostCount++];
                                std::memcpy(h.name, item.data(), eq);
                                h.name[eq] = '\0';
                                h.ipv4 = networkOrder(a);
                                return true;
                        });
                }
                return;
        }

        // The servers in effect: the setting's, else the first interface's from DHCP. Core lock held.
        void effectiveServers(uint32_t (&out)[DNS_MAX_SERVERS]) {
                for (size_t i = 0; i < DNS_MAX_SERVERS; ++i) out[i] = names->servers[i];
                if (out[0] != 0) return;
                for (size_t n = 0; n < NetInterface::count(); ++n) {
                        const NetInterface::LeaseServers &l = NetInterface::at(n).leaseServers();
                        if (l.dns[0] == 0) continue;
                        for (size_t i = 0; i < DNS_MAX_SERVERS && i < 2; ++i) out[i] = l.dns[i];
                        return;
                }
                return;
        }

        void applyServers() {
                uint32_t servers[DNS_MAX_SERVERS];
                effectiveServers(servers);
                for (size_t i = 0; i < DNS_MAX_SERVERS; ++i) {
                        ip_addr_t a;
                        ip_addr_set_ip4_u32(&a, servers[i]);
                        dns_setserver(static_cast<u8_t>(i), &a);
                }
                return;
        }

        // The first interface's address (network order), or 0. Core lock held.
        uint32_t ownAddress() {
                for (size_t n = 0; n < NetInterface::count(); ++n) {
                        const uint32_t a = ip4_addr_get_u32(netif_ip4_addr(&NetInterface::at(n).netif()));
                        if (a != 0) return a;
                }
                return 0;
        }

        const char *ownName() {
                return NetInterface::count() > 0 ? NetInterface::at(0).hostname() : "";
        }

        void found(const char *, const ip_addr_t *address, void *arg) {
                auto &l = *static_cast<Lookup *>(arg);
                l.found = address != nullptr;
                l.ipv4 = address != nullptr ? ip_addr_get_ip4_u32(address) : 0;
                l.answered = true;
                if (l.abandoned) {
                        l.busy = false;
                        l.abandoned = false;
                } else {
                        l.done.release();
                }
                return;
        }

        // --- /etc ---------------------------------------------------------------------------

        Error appendIpv4(String &out, uint32_t networkOrderAddress) {
                char text[Ipv4::TextBytes + 1];
                Ipv4::format(lwip_ntohl(networkOrderAddress), text);
                return out.append(StringView(text));
        }

        Error resolvConf(void *, String &out) {
                uint32_t servers[DNS_MAX_SERVERS];
                {
                        NetStack::QuietLocker lock;
                        effectiveServers(servers);
                }
                Error err;
                for (uint32_t s : servers) {
                        if (s == 0) continue;
                        if (err.isOk()) err = out.append("nameserver ");
                        if (err.isOk()) err = appendIpv4(out, s);
                        if (err.isOk()) err = out.append('\n');
                }
                String search;
                if (err.isOk() && SysConf::get(names->settings[DnsSearch], search).isOk() && !search.isEmpty()) {
                        err = out.append("search");
                        (void)NetNames::forEachItem(search.view(), [&](StringView item) {
                                if (err.isOk()) err = out.append(' ');
                                if (err.isOk()) err = out.append(item);
                                return err.isOk();
                        });
                        if (err.isOk()) err = out.append('\n');
                }
                return err;
        }

        Error hostsFile(void *, String &out) {
                char host[HostName::MaxBytes + 1] = {};
                (void)SysConf::hostname(host);
                Host     hosts[MaxHosts];
                size_t   count;
                uint32_t own;
                {
                        NetStack::QuietLocker lock;
                        count = names->hostCount;
                        for (size_t i = 0; i < count; ++i) hosts[i] = names->hosts[i];
                        own = ownAddress();
                }
                Error err = out.append("127.0.0.1\tlocalhost\n");
                if (err.isOk() && own != 0 && host[0] != '\0') {
                        err = appendIpv4(out, own);
                        if (err.isOk()) err = out.appendf("\t%s\n", host);
                }
                for (size_t i = 0; i < count && err.isOk(); ++i) {
                        err = appendIpv4(out, hosts[i].ipv4);
                        if (err.isOk()) err = out.appendf("\t%s\n", hosts[i].name);
                }
                return err;
        }

        Error hostnameFile(void *, String &out) {
                char host[HostName::MaxBytes + 1] = {};
                (void)SysConf::hostname(host);
                return out.appendf("%s\n", host);
        }

        // The common ones (IANA's numbers), and what BMDS serves and uses.
        constexpr StringView Services = "echo\t\t7/tcp\n"
                                        "echo\t\t7/udp\n"
                                        "discard\t\t9/tcp\n"
                                        "discard\t\t9/udp\n"
                                        "daytime\t\t13/tcp\n"
                                        "ftp-data\t20/tcp\n"
                                        "ftp\t\t21/tcp\n"
                                        "ssh\t\t22/tcp\n"
                                        "telnet\t\t23/tcp\n"
                                        "smtp\t\t25/tcp\n"
                                        "time\t\t37/tcp\n"
                                        "domain\t\t53/tcp\n"
                                        "domain\t\t53/udp\n"
                                        "bootps\t\t67/udp\n"
                                        "bootpc\t\t68/udp\n"
                                        "tftp\t\t69/udp\n"
                                        "http\t\t80/tcp\n"
                                        "pop3\t\t110/tcp\n"
                                        "ntp\t\t123/udp\n"
                                        "imap\t\t143/tcp\n"
                                        "snmp\t\t161/udp\n"
                                        "ptp-event\t319/udp\n"
                                        "ptp-general\t320/udp\n"
                                        "ldap\t\t389/tcp\n"
                                        "https\t\t443/tcp\n"
                                        "syslog\t\t514/udp\n"
                                        "rtsp\t\t554/tcp\n"
                                        "submission\t587/tcp\n"
                                        "ipp\t\t631/tcp\n"
                                        "imaps\t\t993/tcp\n"
                                        "pop3s\t\t995/tcp\n"
                                        "mdns\t\t5353/udp\n"
                                        "http-alt\t8080/tcp\n";

        constexpr StringView Protocols = "ip\t0\tIP\n"
                                         "icmp\t1\tICMP\n"
                                         "igmp\t2\tIGMP\n"
                                         "tcp\t6\tTCP\n"
                                         "udp\t17\tUDP\n"
                                         "ipv6\t41\tIPv6\n"
                                         "ipv6-icmp\t58\tIPv6-ICMP\n";

        Error fixedFile(void *arg, String &out) {
                return out.append(*static_cast<const StringView *>(arg));
        }

        constexpr StringView ServicesText = Services;
        constexpr StringView ProtocolsText = Protocols;

} // namespace

bool NetNames::isHostName(StringView name) {
        if (name.isEmpty() || name.size() > MaxHostName) return false;
        for (char c : name) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.';
                if (!ok) return false;
        }
        return true;
}

Error NetNames::init() {
        if (names.create() == nullptr) return Error::NoMem;
        static constexpr struct {
                        const char   *key;
                        const char   *help;
                        int32_t       max;
                        Config::Check check;
        } Decls[SettingCount] = {
                {"net/dns/servers", "DNS servers, ';'-separated (empty: the ones DHCP gives)", 40, &checkServers},
                {"net/dns/search", "Domains searched for short names, ';'-separated", 255, &checkSearch},
                {"net/hosts", "Names known without DNS: name=address, ';'-separated", 1023, &checkHosts},
        };
        for (size_t i = 0; i < SettingCount; ++i) {
                Config::Param p;
                p.key = Decls[i].key;
                p.type = Config::Type::String;
                p.defaultValue = "";
                p.help = Decls[i].help;
                p.max = Decls[i].max;
                p.check = Decls[i].check;
                auto [id, err] = SysConf::declareKernel(p);
                if (err.isError()) {
                        bmdsErr("net: setting %s not declared: %s", Decls[i].key, err.desc());
                        return err;
                }
                names->settings[i] = id;
        }
        Error err = names->dnsWatcher.start(StringView("net/dns"), &settingsChanged, nullptr);
        if (err.isOk()) err = names->hostsWatcher.start(StringView("net/hosts"), &settingsChanged, nullptr);
        static const Vfs::EtcFile Files[] = {
                {"resolv.conf", &resolvConf, nullptr},
                {"hosts", &hostsFile, nullptr},
                {"hostname", &hostnameFile, nullptr},
                {"services", &fixedFile, const_cast<StringView *>(&ServicesText)},
                {"protocols", &fixedFile, const_cast<StringView *>(&ProtocolsText)},
        };
        for (const Vfs::EtcFile &f : Files) {
                if (err.isOk()) err = Vfs::addEtcFile(f);
        }
        if (err.isOk()) started = true;
        return err;
}

void NetNames::poll() {
        if (!started) return;
        bool apply = names->serversPending.exchange(false);
        if (names->settingsPending.exchange(false)) {
                readSettings();
                apply = true;
        }
        if (apply) applyServers();
        return;
}

void NetNames::serversChanged() {
        if (!started) return;
        names->serversPending.store(true);
        return;
}

Error NetStack::resolve(StringView name, uint32_t &ipv4, unsigned int timeoutMs) {
        uint32_t literal = 0;
        if (Ipv4::parse(name, literal)) {
                ipv4 = networkOrder(literal);
                return Error::Ok;
        }
        char text[DNS_MAX_NAME_LENGTH];
        if (!started || name.isEmpty() || name.size() >= sizeof(text)) return Error::Invalid;
        std::memcpy(text, name.data(), name.size());
        text[name.size()] = '\0';
        Lookup *l = nullptr;
        {
                Locker lock;
                for (Lookup &k : names->lookups) {
                        if (!k.busy) {
                                l = &k;
                                break;
                        }
                }
                if (l == nullptr) return Error::Busy;
                (void)l->done.tryAcquire(); // a release left from an earlier use
                l->busy = true;
                l->answered = false;
                l->found = false;
                ip_addr_t   a;
                const err_t err = dns_gethostbyname(text, &a, &found, l);
                if (err == ERR_OK) {
                        ipv4 = ip_addr_get_ip4_u32(&a);
                        l->busy = false;
                        return Error::Ok;
                }
                if (err != ERR_INPROGRESS) {
                        l->busy = false;
                        return err == ERR_VAL ? Error::NotReady : Error::Invalid; // ERR_VAL: no server
                }
        }
        const Error waited = Interrupt::waitCurrent(l->done, Deadline(timeoutMs));
        Locker      lock;
        if (!l->answered) {
                l->abandoned = true; // lwIP answers it later
                return waited.isError() ? waited : Error::Timeout;
        }
        l->busy = false;
        if (!l->found) return Error::NotExist;
        ipv4 = l->ipv4;
        return Error::Ok;
}

BMDS_NAMESPACE_END

// --- lwIP's hooks (lwipopts.h) ----------------------------------------------------------------

extern "C" {

int bmds_dns_local(const char *name, size_t length, void *address) {
        using namespace bmds;
        const StringView n(name, length);
        uint32_t         a = 0;
        if (n.equalsIgnoreCase("localhost")) {
                a = lwip_htonl(0x7f000001);
        } else if (started) {
                const char *own = ownName();
                if ((own[0] != '\0' && n.equalsIgnoreCase(own)) || NetMdns::isOwnName(n)) a = ownAddress();
                for (size_t i = 0; a == 0 && i < names->hostCount; ++i) {
                        if (n.equalsIgnoreCase(names->hosts[i].name)) a = names->hosts[i].ipv4;
                }
        }
        if (a == 0) return -1;
        ip_addr_set_ip4_u32(static_cast<ip_addr_t *>(address), a);
        return 0;
}

void bmds_dhcp_option(struct netif *netif, unsigned int type, unsigned int option, unsigned int length,
                      const struct pbuf *p, unsigned int offset) {
        for (size_t i = 0; i < bmds::NetInterface::count(); ++i) {
                bmds::NetInterface &n = bmds::NetInterface::at(i);
                if (&n.netif() == netif) n.dhcpOption(type, option, length, p, offset);
        }
        return;
}

} // extern "C"
