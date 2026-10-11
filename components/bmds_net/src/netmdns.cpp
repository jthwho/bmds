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
#include <bmds/netstack.h>
#include <bmds/staticobject.h>
#include <bmds/sysconf.h>

#include "netinterface.h"
#include "netmdns.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // Names tried after a conflict, NAME-2 to NAME-9, before giving up.
        constexpr unsigned int MaxSuffix = 9;

        struct Responder {
                        bool              added = false;
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
                                const err_t e = mdns_resp_add_netif(&ni.netif(), r.name);
                                r.added = e == ERR_OK;
                                if (!r.added) bmdsErr("%s: mDNS not started (lwIP error %d)", ni.name(), e);
                        } else if (conflict || std::strcmp(want, r.name) != 0) {
                                std::memcpy(r.name, want, sizeof(r.name));
                                (void)mdns_resp_rename_netif(&ni.netif(), r.name);
                        }
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
