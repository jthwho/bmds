/**
 * @file      ntp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for NTP's parser (bmds/ntp.h, devplan/network.md §22): the
 * input's first 8 bytes are the request's cookie and the next 8 our clock's
 * T1, the rest the reply. What parses goes on through the sample, a filter and
 * the discipline, as the kernel's client takes it; nothing may fault, read out
 * of bounds or overflow.
 */

#include <cstdint>
#include <cstring>
#include <bmds/ntp.h>

using bmds::Span;
using bmds::Timestamp;
namespace Ntp = bmds::Ntp;

namespace {

        uint64_t take64(const uint8_t *&data, size_t &size) {
                uint64_t     v = 0;
                const size_t n = size < 8 ? size : 8;
                std::memcpy(&v, data, n);
                data += n;
                size -= n;
                return v;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const uint64_t cookie = take64(data, size);
        // Our clock: anywhere from the unset clock's 1955 to 2200.
        constexpr int64_t From = -450'000'000 * Timestamp::NsPerSecond;
        constexpr int64_t Span200Years = int64_t(200) * 365 * 86400 * Timestamp::NsPerSecond;
        const Timestamp   t1{From + static_cast<int64_t>(take64(data, size) % uint64_t(Span200Years + 1))};

        static Ntp::Filter     filter;
        static Ntp::Discipline discipline;
        static int64_t         mono = 0;

        Ntp::Reply         reply;
        const Ntp::Verdict v = Ntp::parse(Span<const uint8_t>(data, size), cookie, reply);
        if (v != Ntp::Verdict::Ok) return 0;
        // The reply comes back between 0 and about 4 s later.
        const Timestamp   t4{t1.ns + static_cast<int64_t>(cookie & 0xffffffffu)};
        const Ntp::Sample s = Ntp::sampleOf(t1, reply.receive, reply.transmit, t4, mono);
        filter.add(s);
        mono += 64 * Timestamp::NsPerSecond;
        const int64_t                 jitter = filter.jitterNs(mono);
        const Ntp::Discipline::Action a = discipline.update(mono, filter.best(mono).offsetNs, (cookie & 1) != 0,
                                                            static_cast<int32_t>(cookie >> 40) % 500'000, 0, jitter);
        if (a.step) filter.clear();
        (void)filter.newestIsBest(mono);
        return 0;
}
