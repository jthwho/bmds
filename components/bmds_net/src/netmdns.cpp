/**
 * @file      netmdns.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Multicast DNS (netmdns.h, devplan/network.md §22): lwIP's responder, one
 * per Ethernet interface, answering for the host name with `.local`. A name
 * another host has is given up for NAME-2, NAME-3, ..., as Avahi does.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <lwip/apps/mdns.h>
#include <lwip/netif.h>
#include <bmds/hostname.h>
#include <bmds/log.h>
#include <bmds/netservices.h>
#include <bmds/netstack.h>
#include <bmds/staticobject.h>
#include <bmds/sysconf.h>

#include "netinterface.h"
#include "netmdns.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // Names tried after a conflict, NAME-2 to NAME-9, before giving up.
        constexpr unsigned int MaxSuffix = 9;

        // A service advertised (NetServices), under the core lock.
        struct Service {
                        char     type[NetServices::MaxType + 1] = {};     // "_ssh"; empty: a free slot
                        char     instance[NetServices::MaxName + 1] = {}; // empty: the host's name
                        bool     udp = false;
                        uint16_t port = 0;
                        uint32_t generation = 0; // changed: each interface's is added again
        };

        // A service as an interface has it.
        struct Added {
                        s8_t     slot = -1; // lwIP's, or -1
                        uint32_t generation = 0;
                        uint32_t hostName = 0; // the interface's name generation it was added with
        };

        struct Responder {
                        bool              added = false;
                        uint32_t          nameGeneration = 0; // moves with each new name
                        Added             services[NetServices::MaxServices];
                        unsigned int      suffix = 0; // 0: the host name as it is
                        std::atomic<bool> conflict{false};
                        char              name[NetMdns::MaxName + 1] = {};
        };

        struct State {
                        SysConf::Id       enabledId = {};
                        SysConf::Watcher  watcher;
                        SysConf::Watcher  hostnameWatcher;
                        std::atomic<bool> pending{true};
                        // Under the core lock:
                        Service   services[NetServices::MaxServices];
                        uint32_t  generation = 0;
                        Responder responders[NetInterface::MaxInterfaces];
        };

        constinit StaticObject<State> state;
        constinit bool                started = false;

        void changed(void *) {
                state->pending.store(true);
                NetStack::wake();
                return;
        }

        // lwIP's word on a name (core lock held).
        void nameResult(struct netif *netif, u8_t result, s8_t) {
                for (size_t i = 0; i < NetInterface::count(); ++i) {
                        if (&NetInterface::at(i).netif() != netif) continue;
                        Responder &r = state->responders[i];
                        if (result == MDNS_PROBING_SUCCESSFUL) {
                                bmdsInfo("%s: mDNS answers for %s.local", NetInterface::at(i).name(), r.name);
                        } else {
                                bmdsWarn("%s: mDNS: another host has %s.local", NetInterface::at(i).name(), r.name);
                                r.conflict.store(true);
                                state->pending.store(true);
                        }
                }
                return;
        }

        // The name with its suffix, cut to fit.
        void nameFor(const char *host, unsigned int suffix, char (&out)[NetMdns::MaxName + 1]) {
                if (suffix == 0) {
                        std::snprintf(out, sizeof(out), "%s", host);
                        return;
                }
                char         tail[8];
                const int    t = std::snprintf(tail, sizeof(tail), "-%u", suffix);
                const size_t keep = std::strlen(host) + size_t(t) <= NetMdns::MaxName ? std::strlen(host)
                                                                                      : NetMdns::MaxName - size_t(t);
                std::snprintf(out, sizeof(out), "%.*s%s", static_cast<int>(keep), host, tail);
                return;
        }

        // The services as they should be on @p ni: each added again when it changed (or the host's
        // name did, for one named as the host).
        void services(NetInterface &ni, Responder &r) {
                for (size_t i = 0; i < NetServices::MaxServices; ++i) {
                        const Service &s = state->services[i];
                        Added         &a = r.services[i];
                        const bool     asHost = s.instance[0] == '\0';
                        if (a.generation == s.generation && (a.slot >= 0) == (s.port != 0) &&
                            (!asHost || a.hostName == r.nameGeneration))
                                continue;
                        if (a.slot >= 0) (void)mdns_resp_del_service(&ni.netif(), u8_t(a.slot));
                        a = Added();
                        a.generation = s.generation;
                        if (s.port == 0 || s.type[0] == '\0') continue;
                        a.hostName = r.nameGeneration;
                        a.slot = mdns_resp_add_service(&ni.netif(), asHost ? r.name : s.instance, s.type,
                                                       s.udp ? DNSSD_PROTO_UDP : DNSSD_PROTO_TCP, s.port, nullptr,
                                                       nullptr);
                        if (a.slot < 0)
                                bmdsWarn("%s: mDNS: %s.%s not advertised", ni.name(), s.type, s.udp ? "_udp" : "_tcp");
                }
                return;
        }

        void apply() {
                SysConf::Id id;
                for (SysConf::Watcher *w : {&state->watcher, &state->hostnameWatcher}) {
                        while (w->next(id)) {}
                        (void)w->overflowed();
                }
                auto [enabled, err] = SysConf::getBool(state->enabledId);
                const bool on = err.isError() || enabled;
                char       host[HostName::MaxBytes + 1];
                (void)SysConf::hostname(host);
                for (size_t i = 0; i < NetInterface::count(); ++i) {
                        Responder    &r = state->responders[i];
                        NetInterface &ni = NetInterface::at(i);
                        if (!on) {
                                if (r.added) (void)mdns_resp_remove_netif(&ni.netif());
                                r.added = false;
                                for (Added &a : r.services) a = Added();
                                continue;
                        }
                        const bool conflict = r.conflict.exchange(false);
                        char       now[NetMdns::MaxName + 1];
                        nameFor(host, r.suffix, now);
                        if (std::strcmp(now, r.name) != 0 || !r.added) r.suffix = 0; // a new host name
                        if (conflict) {
                                if (r.suffix == MaxSuffix) {
                                        bmdsErr("%s: mDNS: no free name (%s-2 to -%u taken)", ni.name(), host,
                                                MaxSuffix);
                                        continue;
                                }
                                r.suffix = r.suffix == 0 ? 2 : r.suffix + 1;
                        }
                        char want[NetMdns::MaxName + 1];
                        nameFor(host, r.suffix, want);
                        if (!r.added) {
                                std::memcpy(r.name, want, sizeof(r.name));
                                ++r.nameGeneration;
                                const err_t e = mdns_resp_add_netif(&ni.netif(), r.name);
                                r.added = e == ERR_OK;
                                if (!r.added) bmdsErr("%s: mDNS not started (lwIP error %d)", ni.name(), e);
                        } else if (conflict || std::strcmp(want, r.name) != 0) {
                                std::memcpy(r.name, want, sizeof(r.name));
                                ++r.nameGeneration;
                                (void)mdns_resp_rename_netif(&ni.netif(), r.name);
                        }
                        if (r.added) services(ni, r);
                }
                return;
        }

} // namespace

Error NetMdns::init() {
        state.create();
        Config::Param p;
        p.key = "net/mdns/enabled";
        p.type = Config::Type::Bool;
        p.defaultValue = "true";
        p.help = "Answer multicast DNS for the host name with .local";
        auto [id, err] = SysConf::declareKernel(p);
        if (err.isError()) {
                bmdsErr("net: setting %s not declared: %s", p.key, err.desc());
                return err;
        }
        state->enabledId = id;
        err = state->watcher.start(StringView("net/mdns"), &changed, nullptr);
        if (err.isOk()) err = state->hostnameWatcher.start(StringView("system/hostname"), &changed, nullptr);
        if (err.isError()) return err;
        {
                NetStack::Locker lock;
                mdns_resp_init();
                mdns_resp_register_name_result_cb(&nameResult);
        }
        started = true;
        return Error::Ok;
}

void NetMdns::poll() {
        if (!started || !state->pending.exchange(false)) return;
        apply();
        return;
}

bool NetMdns::name(size_t position, char (&out)[MaxName + 1]) {
        out[0] = '\0';
        if (!started || position >= NetInterface::count() || !state->responders[position].added) return false;
        std::memcpy(out, state->responders[position].name, sizeof(out));
        return true;
}

Error NetServices::advertise(StringView service, bool udp, uint16_t port, StringView instance) {
        if (!started) return Error::InvalidState;
        if (service.isEmpty() || service.size() > MaxType || instance.size() > MaxName) return Error::Invalid;
        {
                NetStack::Locker lock;
                Service         *slot = nullptr;
                for (Service &s : state->services) {
                        if (s.type[0] != '\0' && service == StringView(s.type) && s.udp == udp) slot = &s;
                }
                if (slot == nullptr && port == 0) return Error::Ok;
                for (Service &s : state->services)
                        if (slot == nullptr && s.type[0] == '\0') slot = &s;
                if (slot == nullptr) return Error::Full;
                std::snprintf(slot->type, sizeof(slot->type), "%.*s", int(service.size()), service.data());
                std::snprintf(slot->instance, sizeof(slot->instance), "%.*s", int(instance.size()), instance.data());
                slot->udp = udp;
                slot->port = port;
                slot->generation = ++state->generation;
                if (port == 0)
                        slot->type[0] = '\0'; // taken back: the slot is free once each interface has
                                              // dropped it (its generation moved)
        }
        state->pending.store(true);
        NetStack::wake();
        return Error::Ok;
}

bool NetMdns::isOwnName(StringView name) {
        constexpr StringView Local(".local");
        if (!started || name.size() <= Local.size() || !name.right(Local.size()).equalsIgnoreCase(Local)) return false;
        const StringView label = name.left(name.size() - Local.size());
        for (size_t i = 0; i < NetInterface::count(); ++i) {
                const Responder &r = state->responders[i];
                if (r.added && label.equalsIgnoreCase(StringView(r.name))) return true;
        }
        return false;
}

BMDS_NAMESPACE_END
