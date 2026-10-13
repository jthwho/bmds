/**
 * @file      ptp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for PTP (bmds/ptp.h, devplan/network.md §31): every message parsed, built
 * again and parsed again to the same fields; and an ordinary clock's port driven by the input,
 * a record at a time:
 *
 *   op & 3 == 0: a message: one byte of length, then that many bytes, received now
 *   op & 3 == 1: the time moves on by the next byte's milliseconds (times 8 if op & 4); the timers run
 *   op & 3 == 2: the departure stamp of a recent send (op >> 2 sends back), now
 *   op & 3 == 3: a new configuration from op's bits (priority, slave only, class, domain)
 *
 * The port's clock is stepped and slewed as it asks. Nothing may fault, overflow or loop.
 */

#include <cstdint>
#include <cstring>
#include <bmds/ptp.h>

using bmds::Span;
namespace Ptp = bmds::Ptp;

namespace {

        // The port's world: a clock it steps and slews, and the tickets of what it sent.
        struct Io final : Ptp::Port::Io {
                        uint64_t clock =
                                1'800'000'000'000'000'000ull; // 2027 (wrapping: the input may step it anywhere)
                        double   ppb = 0;
                        uint32_t tickets = 0;

                        bool send(bool, Span<const uint8_t> bytes, uint32_t *ticket) override {
                                Ptp::Message m;
                                const auto   v = Ptp::parse(bytes, m);
                                if (v != Ptp::Verdict::Ok) __builtin_trap(); // what the port builds must parse
                                if (ticket != nullptr) *ticket = ++tickets;
                                return true;
                        }
                        int64_t              now() override { return static_cast<int64_t>(clock); }
                        void                 step(int64_t ns) override { clock += static_cast<uint64_t>(ns); }
                        void                 setFrequency(double v) override { ppb = v; }
                        double               frequency() override { return ppb; }
                        Ptp::Port::UtcOffset utcOffset() override { return Ptp::Port::UtcOffset{}; }
        };

        void checkRoundTrip(Span<const uint8_t> bytes) {
                Ptp::Message m;
                if (Ptp::parse(bytes, m) != Ptp::Verdict::Ok) return;
                uint8_t      out[Ptp::MaxMessageBytes];
                const size_t n = Ptp::build(m, Span<uint8_t>(out, sizeof(out)));
                if (n == 0) __builtin_trap(); // every type that parses is one that builds
                Ptp::Message back;
                if (Ptp::parse(Span<const uint8_t>(out, n), back) != Ptp::Verdict::Ok) __builtin_trap();
                if (back.timeNs != m.timeNs || back.header.sequence != m.header.sequence ||
                    !(back.header.source == m.header.source) || back.header.correction != m.header.correction ||
                    back.priority1 != m.priority1 || !(back.grandmaster == m.grandmaster) ||
                    !(back.requesting == m.requesting))
                        __builtin_trap();
                return;
        }

        Ptp::ClockIdentity ours() {
                Ptp::ClockIdentity id;
                const uint8_t      b[8] = {0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x01};
                std::memcpy(id.bytes, b, 8);
                return id;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        checkRoundTrip(Span<const uint8_t>(data, size));

        Io                io;
        Ptp::Port::Config config;
        Ptp::Port         port(io, ours(), config);
        int64_t           mono = 0;
        port.start(mono);
        while (size > 0) {
                const uint8_t op = *data++;
                --size;
                switch (op & 3) {
                        case 0: {
                                if (size == 0) break;
                                size_t n = *data++;
                                --size;
                                if (n > size) n = size;
                                checkRoundTrip(Span<const uint8_t>(data, n));
                                port.receive(Span<const uint8_t>(data, n), io.now(), mono);
                                data += n;
                                size -= n;
                                break;
                        }
                        case 1: {
                                if (size == 0) break;
                                const int64_t ms = *data++ * ((op & 4) ? 8 : 1);
                                --size;
                                const int64_t until = mono + ms * 1'000'000;
                                io.clock += static_cast<uint64_t>(ms * 1'000'000);
                                // The timers due on the way, as the owner's task runs them; a bound so a
                                // timer that never moves on shows as a hang, not as a slow input.
                                for (int i = 0; i < 100'000 && port.deadline() <= until; ++i) {
                                        mono = port.deadline() > mono ? port.deadline() : mono;
                                        port.advance(mono);
                                        if (i == 99'999) __builtin_trap();
                                }
                                mono = until;
                                break;
                        }
                        case 2: port.transmitted(io.tickets - (op >> 2), io.now(), mono); break;
                        case 3:
                                config.priority1 = static_cast<uint8_t>(op & 0xf0);
                                config.slaveOnly = (op & 4) != 0;
                                config.quality.clockClass = (op & 8) ? 6 : 248;
                                config.domain = (op & 0x80) ? 1 : 0;
                                port.setConfig(config, mono);
                                break;
                }
        }
        (void)port.status();
        return 0;
}
