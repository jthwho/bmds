/**
 * @file      timesync.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * NTP's and PTP's state as summaries for applications (devplan/controlpanel.md §7).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/timesync.h>

#include "ntpclient.h"
#include "ptpservice.h"

BMDS_NAMESPACE_BEGIN

namespace {

        void identity(const Ptp::ClockIdentity &id, char (&out)[24]) {
                const uint8_t *b = id.bytes;
                std::snprintf(out, sizeof(out), "%02x%02x%02x.%02x%02x.%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
                              b[5], b[6], b[7]);
                return;
        }

} // namespace

TimeSync::Ntp TimeSync::ntp() {
        NtpClient::Status s;
        NtpClient::status(s);
        Ntp out;
        out.enabled = s.enabled;
        out.synced = s.synced;
        out.stratum = s.stratum;
        out.pollLog2 = s.poll;
        out.offsetNs = s.offsetNs;
        out.delayNs = s.delayNs;
        out.jitterNs = s.jitterNs;
        out.servers = static_cast<uint8_t>(s.count);
        for (size_t i = 0; i < s.count && i < NtpClient::MaxServers; ++i) {
                const NtpClient::ServerStatus &v = s.servers[i];
                if (v.reach != 0) ++out.reachable;
                if (!v.chosen) continue;
                std::snprintf(out.server, sizeof(out.server), "%s", v.name);
                out.sinceAnswerNs = v.sinceAnswerNs;
        }
        return out;
}

TimeSync::Ptp TimeSync::ptp() {
        Ptp out;
        // The service's status is large (its foreign masters): in PSRAM, not on the caller's stack.
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(PtpService::Status));
        if (mem == nullptr) {
                out.error = Error::NoMem;
                return out;
        }
        auto *s = ::new (mem) PtpService::Status();
        PtpService::status(*s);
        out.enabled = s->enabled;
        out.running = s->running;
        out.error = s->error;
        std::snprintf(out.interface, sizeof(out.interface), "%s", s->interface);
        out.state = ::bmds::Ptp::Port::stateName(s->port.state);
        out.stateForNs = s->stateForNs;
        out.haveOffset = s->port.haveOffset;
        out.offsetNs = s->port.offsetNs;
        out.pathDelayNs = s->port.meanPathDelayNs;
        if (s->port.state != ::bmds::Ptp::Port::State::Master && !s->port.parent.clock.isZero())
                identity(s->port.parent.clock, out.master);
        identity(s->port.grandmaster.grandmaster, out.grandmaster);
        out.systemFollows = s->follow == PtpService::Follow::SystemFollows;
        out.ptpFollows = s->follow == PtpService::Follow::PtpFollows;
        out.utcOffset = s->utcOffset;
        s->~Status();
        Memory::free(mem);
        return out;
}

BMDS_NAMESPACE_END
