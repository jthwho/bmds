/**
 * @file      ptp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PTP (bmds/ptp.h, devplan/network.md §31): the messages against their layout, the dataset
 * comparison, the servo, and ordinary clocks on a simulated network: each with its own
 * oscillator, the messages delayed, jittered, reordered or lost, the stamps quantized as the
 * EMAC's. Election, lock, failover with holdover, a better master arriving, slave-only clocks.
 */

#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <queue>
#include <random>
#include <vector>

#include <bmds/ptp.h>

using bmds::Span;
namespace Ptp = bmds::Ptp;

namespace {

        constexpr int64_t Us = 1'000;
        constexpr int64_t Ms = 1'000'000;
        constexpr int64_t S = 1'000'000'000;

        Ptp::ClockIdentity identity(uint8_t last) {
                Ptp::ClockIdentity id;
                const uint8_t      bytes[8] = {0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, last};
                std::memcpy(id.bytes, bytes, 8);
                return id;
        }

        Ptp::Message roundTrip(const Ptp::Message &m, size_t expectBytes) {
                uint8_t      bytes[Ptp::MaxMessageBytes];
                const size_t n = Ptp::build(m, Span<uint8_t>(bytes, sizeof(bytes)));
                CHECK(n == expectBytes);
                Ptp::Message back;
                CHECK(Ptp::parse(Span<const uint8_t>(bytes, n), back) == Ptp::Verdict::Ok);
                return back;
        }

        // --- The simulated network ------------------------------------------------------------

        // An oscillator: a reading that runs at its own rate, plus the corrections applied to it.
        struct SimClock {
                        int64_t base = 0;     // the reading at t0
                        int64_t t0 = 0;       // true time of the last change
                        double  driftPpb = 0; // its own error
                        double  adjPpb = 0;   // the correction set through Io

                        int64_t read(int64_t t) const {
                                const double elapsed = static_cast<double>(t - t0);
                                return base + std::llround(elapsed * (1.0 + (driftPpb + adjPpb) * 1e-9));
                        }

                        void rebase(int64_t t) {
                                base = read(t);
                                t0 = t;
                                return;
                        }
        };

        struct Sim;

        struct Node final : Ptp::Port::Io {
                        Sim                       &sim;
                        SimClock                   clock;
                        std::unique_ptr<Ptp::Port> port;
                        bool                       up = true;
                        uint32_t                   nextTicket = 0;
                        int                        index = 0;

                        Node(Sim &s, int i, uint8_t id, const Ptp::Port::Config &config, double driftPpb,
                             int64_t startNs);

                        bool                 send(bool event, Span<const uint8_t> bytes, uint32_t *ticket) override;
                        int64_t              now() override;
                        void                 step(int64_t ns) override;
                        void                 setFrequency(double ppb) override;
                        double               frequency() override { return clock.adjPpb; }
                        Ptp::Port::UtcOffset utcOffset() override {
                                Ptp::Port::UtcOffset u;
                                u.valid = true;
                                return u;
                        }
        };

        struct Sim {
                        struct Event {
                                        int64_t              at = 0;
                                        uint64_t             order = 0;
                                        bool                 stamp = false; // a departure stamp (else a delivery)
                                        int                  to = 0;
                                        uint32_t             ticket = 0;
                                        int64_t              ns = 0;
                                        std::vector<uint8_t> bytes;
                        };
                        struct Later {
                                        bool operator()(const Event &a, const Event &b) const {
                                                return a.at != b.at ? a.at > b.at : a.order > b.order;
                                        }
                        };

                        std::vector<std::unique_ptr<Node>>                    nodes;
                        std::priority_queue<Event, std::vector<Event>, Later> queue;
                        std::mt19937_64                                       rng{12345};
                        int64_t                                               now = 0;
                        uint64_t                                              order = 0;

                        // The network: one switch's worth.
                        int64_t pathNs = 30 * Us;        // event messages
                        int64_t generalPathNs = 30 * Us; // general messages (shorter: Follow_Ups overtake Syncs)
                        int64_t jitterNs = 1 * Us;       // uniform, added
                        double  loss = 0;                // each delivery
                        int64_t quantumNs = 8;           // the stamps' resolution
                        int64_t stampLatencyNs = 20 * Us;

                        Node &add(uint8_t id, const Ptp::Port::Config &config, double driftPpb, int64_t startNs) {
                                nodes.push_back(std::make_unique<Node>(*this, static_cast<int>(nodes.size()), id,
                                                                       config, driftPpb, startNs));
                                return *nodes.back();
                        }

                        void push(Event e) {
                                e.order = order++;
                                queue.push(std::move(e));
                                return;
                        }

                        double uniform() { return std::uniform_real_distribution<double>(0, 1)(rng); }

                        int64_t quantize(int64_t ns) const { return ns - ((ns % quantumNs) + quantumNs) % quantumNs; }

                        void send(Node &from, bool event, Span<const uint8_t> bytes, uint32_t ticket) {
                                if (event) {
                                        Event e;
                                        e.at = now + stampLatencyNs;
                                        e.stamp = true;
                                        e.to = from.index;
                                        e.ticket = ticket;
                                        e.ns = quantize(from.clock.read(now));
                                        push(std::move(e));
                                }
                                for (auto &n : nodes) {
                                        if (n.get() == &from) continue;
                                        if (loss > 0 && uniform() < loss) continue;
                                        Event e;
                                        e.at = now + (event ? pathNs : generalPathNs) +
                                               static_cast<int64_t>(uniform() * static_cast<double>(jitterNs));
                                        e.to = n->index;
                                        e.bytes.assign(bytes.data(), bytes.data() + bytes.size());
                                        push(std::move(e));
                                }
                                return;
                        }

                        void start() {
                                for (auto &n : nodes) n->port->start(now);
                                return;
                        }

                        void run(int64_t until) {
                                for (;;) {
                                        int64_t next = queue.empty() ? INT64_MAX : queue.top().at;
                                        for (auto &n : nodes) next = std::min(next, n->port->deadline());
                                        if (next > until) break;
                                        now = next;
                                        while (!queue.empty() && queue.top().at <= now) {
                                                Event e = queue.top();
                                                queue.pop();
                                                Node &n = *nodes[static_cast<size_t>(e.to)];
                                                if (e.stamp) {
                                                        n.port->transmitted(e.ticket, e.ns, now);
                                                } else if (n.up) {
                                                        n.port->receive(
                                                                Span<const uint8_t>(e.bytes.data(), e.bytes.size()),
                                                                quantize(n.clock.read(now)), now);
                                                }
                                        }
                                        for (auto &n : nodes) {
                                                if (n->port->deadline() <= now) n->port->advance(now);
                                        }
                                }
                                now = until;
                                return;
                        }

                        // The worst |a - b| over [from, to], sampled every 100 ms.
                        int64_t worstOffset(Node &a, Node &b, int64_t to) {
                                int64_t worst = 0;
                                while (now < to) {
                                        run(now + 100 * Ms);
                                        const int64_t d = a.clock.read(now) - b.clock.read(now);
                                        worst = std::max(worst, d < 0 ? -d : d);
                                }
                                return worst;
                        }
        };

        Node::Node(Sim &s, int i, uint8_t id, const Ptp::Port::Config &config, double driftPpb, int64_t startNs)
            : sim(s), index(i) {
                clock.base = startNs;
                clock.driftPpb = driftPpb;
                port = std::make_unique<Ptp::Port>(*this, identity(id), config);
        }

        bool Node::send(bool event, Span<const uint8_t> bytes, uint32_t *ticket) {
                if (!up) return false;
                const uint32_t t = ++nextTicket;
                if (ticket != nullptr) *ticket = t;
                sim.send(*this, event, bytes, t);
                return true;
        }

        int64_t Node::now() {
                return clock.read(sim.now);
        }

        void Node::step(int64_t ns) {
                clock.rebase(sim.now);
                clock.base += ns;
                return;
        }

        void Node::setFrequency(double ppb) {
                clock.rebase(sim.now);
                clock.adjPpb = ppb;
                return;
        }

        Ptp::Port::Config media() {
                return Ptp::Port::Config{};
        }

        Ptp::Port::Config withPriority(uint8_t priority1) {
                Ptp::Port::Config c;
                c.priority1 = priority1;
                return c;
        }

        constexpr int64_t Epoch = 1'800'000'000 * S; // 2027

} // namespace

TEST_CASE("PTP: intervals, identities") {
        CHECK(Ptp::intervalNs(0) == S);
        CHECK(Ptp::intervalNs(1) == 2 * S);
        CHECK(Ptp::intervalNs(-3) == 125 * Ms);
        CHECK(Ptp::intervalNs(-20) == Ptp::intervalNs(-7));
        const Ptp::ClockIdentity id = Ptp::ClockIdentity::fromMac({{0xe8, 0xf6, 0x0a, 0xe2, 0xe2, 0x22}});
        const uint8_t            want[8] = {0xe8, 0xf6, 0x0a, 0xff, 0xfe, 0xe2, 0xe2, 0x22};
        CHECK(std::memcmp(id.bytes, want, 8) == 0);
        CHECK(Ptp::compare(identity(1), identity(2)) < 0);
        CHECK(Ptp::ClockIdentity{}.isZero());
}

TEST_CASE("PTP: messages, byte by byte") {
        Ptp::Message m;
        m.header.type = Ptp::MessageType::Sync;
        m.header.domain = 3;
        m.header.flags = Ptp::TwoStep;
        m.header.correction = int64_t(1500) << 16;
        m.header.source = {identity(7), 1};
        m.header.sequence = 0x1234;
        m.header.logInterval = -3;
        m.timeNs = 1'800'000'000 * S + 123'456'789;
        uint8_t b[Ptp::MaxMessageBytes];
        REQUIRE(Ptp::build(m, Span<uint8_t>(b, sizeof(b))) == 44);
        CHECK(b[0] == 0x00);
        CHECK(b[1] == 0x02);
        CHECK(b[2] == 0);
        CHECK(b[3] == 44);
        CHECK(b[4] == 3);
        CHECK(b[6] == 0x02); // twoStepFlag: the first flag byte
        CHECK(b[7] == 0x00);
        CHECK(b[12] == 0x05); // 1500 << 16 = 0x05dc0000
        CHECK(b[13] == 0xdc);
        CHECK(b[27] == 7);
        CHECK(b[29] == 1);
        CHECK(b[30] == 0x12);
        CHECK(b[31] == 0x34);
        CHECK(b[32] == 0);    // controlField: Sync
        CHECK(b[33] == 0xfd); // -3
        CHECK(b[34] == 0);    // seconds, 48 bits: 1800000000 = 0x6b49d200
        CHECK(b[36] == 0x6b);
        CHECK(b[39] == 0x00);
        CHECK(b[40] == 0x07); // 123456789 = 0x075bcd15
        CHECK(b[43] == 0x15);

        Ptp::Message back = roundTrip(m, 44);
        CHECK(back.header.type == Ptp::MessageType::Sync);
        CHECK(back.header.domain == 3);
        CHECK(back.header.flags == Ptp::TwoStep);
        CHECK(Ptp::correctionNs(back.header) == 1500);
        CHECK(back.header.source == m.header.source);
        CHECK(back.header.sequence == 0x1234);
        CHECK(back.header.logInterval == -3);
        CHECK(back.timeNs == m.timeNs);

        Ptp::Message a;
        a.header.type = Ptp::MessageType::Announce;
        a.header.flags = Ptp::PtpTimescale | Ptp::UtcOffsetValid;
        a.utcOffset = 37;
        a.priority1 = 100;
        a.quality = {6, 0x21, 0x4e5d};
        a.priority2 = 99;
        a.grandmaster = identity(9);
        a.stepsRemoved = 2;
        a.timeSource = 0x20;
        REQUIRE(Ptp::build(a, Span<uint8_t>(b, sizeof(b))) == 64);
        CHECK(b[32] == 5); // controlField: other
        CHECK(b[45] == 37);
        CHECK(b[47] == 100);
        CHECK(b[48] == 6);
        CHECK(b[49] == 0x21);
        CHECK(b[50] == 0x4e);
        CHECK(b[52] == 99);
        CHECK(b[60] == 9);
        CHECK(b[62] == 2);
        CHECK(b[63] == 0x20);
        back = roundTrip(a, 64);
        CHECK(back.utcOffset == 37);
        CHECK(back.priority1 == 100);
        CHECK(back.quality.clockClass == 6);
        CHECK(back.quality.accuracy == 0x21);
        CHECK(back.quality.variance == 0x4e5d);
        CHECK(back.priority2 == 99);
        CHECK(back.grandmaster == identity(9));
        CHECK(back.stepsRemoved == 2);
        CHECK(back.timeSource == 0x20);
        CHECK(back.header.flags == (Ptp::PtpTimescale | Ptp::UtcOffsetValid));

        Ptp::Message r;
        r.header.type = Ptp::MessageType::DelayResp;
        r.timeNs = 5 * S + 1;
        r.requesting = {identity(4), 1};
        back = roundTrip(r, 54);
        CHECK(back.requesting == r.requesting);
        CHECK(back.timeNs == r.timeNs);

        Ptp::Message f;
        f.header.type = Ptp::MessageType::FollowUp;
        REQUIRE(Ptp::build(f, Span<uint8_t>(b, sizeof(b))) == 44);
        CHECK(b[32] == 2);
        Ptp::Message d;
        d.header.type = Ptp::MessageType::DelayReq;
        REQUIRE(Ptp::build(d, Span<uint8_t>(b, sizeof(b))) == 44);
        CHECK(b[32] == 1);

        Ptp::Message p;
        p.header.type = Ptp::MessageType::PdelayReq;
        CHECK(Ptp::build(p, Span<uint8_t>(b, sizeof(b))) == 0); // not built here
        CHECK(Ptp::build(a, Span<uint8_t>(b, 63)) == 0);
}

TEST_CASE("PTP: messages refused") {
        Ptp::Message m;
        m.header.type = Ptp::MessageType::Announce;
        uint8_t b[80] = {};
        REQUIRE(Ptp::build(m, Span<uint8_t>(b, sizeof(b))) == 64);
        Ptp::Message out;
        CHECK(Ptp::parse(Span<const uint8_t>(b, 33), out) == Ptp::Verdict::Short);
        CHECK(Ptp::parse(Span<const uint8_t>(b, 63), out) == Ptp::Verdict::Short); // the length says 64

        // A TLV after the body: its length counts it; the body still reads.
        b[3] = 72;
        CHECK(Ptp::parse(Span<const uint8_t>(b, 72), out) == Ptp::Verdict::Ok);
        b[3] = 50; // shorter than an Announce's body
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Short);
        b[3] = 64;

        b[1] = 0x12; // 1588-2019: minor version 1
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Ok);
        b[1] = 0x01;
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Version);
        b[1] = 0x02;

        b[40] = 0x3b; // nanoseconds 0x3b9aca00 = 1e9
        b[41] = 0x9a;
        b[42] = 0xca;
        b[43] = 0x00;
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Bogus);
        std::memset(b + 40, 0, 4);
        b[34] = 0xff; // seconds past 2262
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Bogus);
        b[34] = 0;

        b[0] = 0x02; // Pdelay_Req: the header only
        b[1] = 0x02;
        CHECK(Ptp::parse(Span<const uint8_t>(b, 64), out) == Ptp::Verdict::Unsupported);
        CHECK(out.header.type == Ptp::MessageType::PdelayReq);
}

TEST_CASE("PTP: the dataset comparison") {
        Ptp::Dataset a, b;
        a.grandmaster = identity(1);
        b.grandmaster = identity(2);
        a.sender = a.receiver = b.receiver = {identity(10), 1};
        b.sender = {identity(11), 1};
        CHECK(Ptp::compare(a, b) == -2); // the identity, last
        CHECK(Ptp::compare(b, a) == 2);
        b.priority2 = 127;
        CHECK(Ptp::compare(a, b) == 2);
        a.quality.variance = 0x4000;
        CHECK(Ptp::compare(a, b) == -2);
        b.quality.accuracy = 0x21;
        CHECK(Ptp::compare(a, b) == 2);
        a.quality.clockClass = 6;
        CHECK(Ptp::compare(a, b) == -2);
        b.priority1 = 1;
        CHECK(Ptp::compare(a, b) == 2);

        // The same grandmaster: the shorter path, then the topology.
        Ptp::Dataset x, y;
        x.grandmaster = y.grandmaster = identity(5);
        x.receiver = y.receiver = {identity(20), 1};
        x.sender = {identity(30), 1};
        y.sender = {identity(31), 1};
        x.stepsRemoved = 1;
        y.stepsRemoved = 3;
        CHECK(Ptp::compare(x, y) == -2);
        CHECK(Ptp::compare(y, x) == 2);
        y.stepsRemoved = 2; // one more: y's receiver (20) below its sender (31): x better
        CHECK(Ptp::compare(x, y) == -2);
        y.receiver = {identity(40), 1}; // above: x better by topology
        CHECK(Ptp::compare(x, y) == -1);
        y.stepsRemoved = 1;
        CHECK(Ptp::compare(x, y) == -1); // equal steps: the lower sender
        y.sender = x.sender;
        y.receiver = {identity(20), 2};
        CHECK(Ptp::compare(x, y) == -1); // the same sender: the lower receiving port
        CHECK(Ptp::compare(x, x) == 0);
}

TEST_CASE("PTP: the servo locks out a constant drift") {
        Ptp::Servo servo;
        servo.configure(Ptp::Servo::Config{}, -3);
        CHECK(servo.kp() == doctest::Approx(1.3064).epsilon(0.001));
        CHECK(servo.ki() == doctest::Approx(0.1306).epsilon(0.001));
        servo.reset(0);
        // A clock 50 ppm fast and 5 ms ahead, sampled every 125 ms.
        double  offset = 5 * Ms;
        double  correction = 0;
        int64_t local = 0;
        int     jumps = 0;
        for (int i = 0; i < 400; ++i) {
                const double ppb = servo.sample(std::llround(offset), local);
                if (servo.state() == Ptp::Servo::State::Jump) {
                        offset = 0; // stepped by -offset
                        ++jumps;
                }
                if (servo.state() != Ptp::Servo::State::Unlocked) correction = ppb;
                offset += (50'000 + correction) * 0.125;
                local += 125 * Ms;
        }
        CHECK(jumps == 1);
        CHECK(std::fabs(offset) < 5);
        CHECK(correction == doctest::Approx(-50'000).epsilon(0.001));

        // Within the first-step threshold: no step.
        servo.reset(0);
        Ptp::Servo fresh;
        fresh.configure(Ptp::Servo::Config{}, -3);
        fresh.reset(0);
        (void)fresh.sample(10 * Us, 0);
        (void)fresh.sample(10 * Us, 125 * Ms);
        CHECK(fresh.state() == Ptp::Servo::State::Locked);
}

TEST_CASE("PTP: a slave locks to its master") {
        Sim   sim;
        Node &master = sim.add(1, media(), 30'000, Epoch);
        Node &slave = sim.add(2, media(), -40'000, Epoch + 1500 * Ms);
        sim.start();
        sim.run(40 * S);
        CHECK(master.port->state() == Ptp::Port::State::Master);
        CHECK(slave.port->state() == Ptp::Port::State::Slave);
        const int64_t worst = sim.worstOffset(slave, master, 70 * S);
        MESSAGE("worst offset over the last 30 s: " << worst << " ns; path delay "
                                                    << slave.port->status().meanPathDelayNs << " ns");
        CHECK(worst < 500);
        CHECK(slave.port->counts().steps == 1);
        CHECK(slave.port->status().meanPathDelayNs == doctest::Approx(30'500).epsilon(0.02));
        CHECK(slave.clock.adjPpb == doctest::Approx(70'000 * (1 + 30e-6)).epsilon(0.001));
        CHECK(slave.port->status().grandmaster.grandmaster == identity(1));
        CHECK(slave.port->status().utcOffset.seconds == 37);
        CHECK(slave.port->status().utcOffset.valid);
        CHECK(master.port->counts().followUpsMissed == 0);
        CHECK(master.port->counts().sent[static_cast<int>(Ptp::MessageType::DelayResp)] > 0);
}

TEST_CASE("PTP: the lowest identity is elected when nothing else differs") {
        Sim   sim;
        Node &c = sim.add(3, media(), 10'000, Epoch);
        Node &a = sim.add(1, media(), -20'000, Epoch + 3 * S);
        Node &b = sim.add(2, media(), 5'000, Epoch - 2 * S);
        sim.start();
        sim.run(60 * S);
        CHECK(a.port->state() == Ptp::Port::State::Master);
        CHECK(b.port->state() == Ptp::Port::State::Slave);
        CHECK(c.port->state() == Ptp::Port::State::Slave);
        CHECK(sim.worstOffset(b, a, 70 * S) < 500);
        CHECK(sim.worstOffset(c, a, 80 * S) < 500);
}

TEST_CASE("PTP: priority1 decides before the identity; a slave-only clock never masters") {
        Sim               sim;
        Ptp::Port::Config only;
        only.slaveOnly = true;
        Node &a = sim.add(1, only, 0, Epoch);
        Node &b = sim.add(2, media(), 0, Epoch);
        Node &c = sim.add(3, withPriority(100), 0, Epoch);
        sim.start();
        sim.run(60 * S);
        CHECK(c.port->state() == Ptp::Port::State::Master);
        CHECK(b.port->state() == Ptp::Port::State::Slave);
        CHECK(a.port->state() == Ptp::Port::State::Slave);

        Sim   alone;
        Node &x = alone.add(1, only, 0, Epoch);
        alone.start();
        alone.run(60 * S);
        CHECK(x.port->state() == Ptp::Port::State::Listening);
        CHECK(x.port->counts().announceTimeouts >= 5);
}

TEST_CASE("PTP: the master falls silent; the next takes over without a jump") {
        Sim   sim;
        Node &a = sim.add(1, withPriority(100), 20'000, Epoch);
        Node &b = sim.add(2, withPriority(110), -35'000, Epoch + 2 * S);
        Node &c = sim.add(3, withPriority(120), 60'000, Epoch - 1 * S);
        sim.start();
        sim.run(60 * S);
        REQUIRE(a.port->state() == Ptp::Port::State::Master);
        REQUIRE(b.port->state() == Ptp::Port::State::Slave);
        REQUIRE(c.port->state() == Ptp::Port::State::Slave);
        const double   bFrequency = b.clock.adjPpb;
        const uint32_t bSteps = b.port->counts().steps;

        a.up = false; // its cable pulled: its clock runs on, as it was
        sim.run(sim.now + 15 * S);
        CHECK(b.port->state() == Ptp::Port::State::Master);
        CHECK(c.port->state() == Ptp::Port::State::Slave);
        CHECK(c.port->status().grandmaster.grandmaster == identity(2));
        CHECK(b.port->counts().steps == bSteps);
        // Holdover: the rate averaged while it followed A (A at +20 ppm, B at -35: 55000 ppb),
        // not the last correction, which carries the last samples' noise.
        MESSAGE("held " << b.clock.adjPpb << " ppb (the last correction was " << bFrequency << ")");
        CHECK(std::fabs(b.clock.adjPpb - 55'000) < 50);
        CHECK(c.port->counts().steps == 1); // C followed B without a step
        const int64_t drift = sim.worstOffset(b, a, sim.now + 30 * S);
        MESSAGE("the new master against the old one's free run, 45 s on: " << drift << " ns");
        CHECK(drift < 5 * Us);
        CHECK(sim.worstOffset(c, b, sim.now + 20 * S) < 500);
}

TEST_CASE("PTP: a better master arriving takes over") {
        Sim   sim;
        Node &a = sim.add(1, withPriority(120), 10'000, Epoch);
        Node &d = sim.add(4, withPriority(100), -10'000, Epoch + 5 * S);
        d.up = false;
        sim.start();
        sim.run(30 * S);
        CHECK(a.port->state() == Ptp::Port::State::Master);
        d.up = true;
        sim.run(sim.now + 20 * S);
        CHECK(d.port->state() == Ptp::Port::State::Master);
        CHECK(a.port->state() == Ptp::Port::State::Slave);
        CHECK(sim.worstOffset(a, d, sim.now + 20 * S) < 500);
}

TEST_CASE("PTP: locks through loss, jitter, and Follow_Ups ahead of their Syncs") {
        Sim sim;
        sim.loss = 0.1;
        sim.jitterNs = 5 * Us;
        sim.generalPathNs = 2 * Us; // Follow_Ups arrive before their Syncs
        Node &a = sim.add(1, media(), 0, Epoch);
        Node &b = sim.add(2, media(), 80'000, Epoch + 50 * Ms);
        sim.start();
        sim.run(60 * S);
        CHECK(b.port->state() == Ptp::Port::State::Slave);
        const int64_t worst = sim.worstOffset(b, a, 90 * S);
        MESSAGE("10 % loss, 5 us jitter: worst " << worst << " ns");
        CHECK(worst < 3 * Us);
}

TEST_CASE("PTP: another domain's master is a stranger; a primary clock goes passive") {
        Sim               sim;
        Ptp::Port::Config other;
        other.domain = 1;
        Node &a = sim.add(1, media(), 0, Epoch);
        Node &b = sim.add(2, other, 0, Epoch);
        sim.start();
        sim.run(30 * S);
        CHECK(a.port->state() == Ptp::Port::State::Master);
        CHECK(b.port->state() == Ptp::Port::State::Master);
        CHECK(b.port->counts().ignored > 0);

        Sim               primary;
        Ptp::Port::Config gps;
        gps.quality.clockClass = 6;
        Ptp::Port::Config worse = gps;
        worse.priority2 = 200;
        Node &x = primary.add(1, gps, 0, Epoch);
        Node &y = primary.add(2, worse, 0, Epoch);
        primary.start();
        primary.run(30 * S);
        CHECK(x.port->state() == Ptp::Port::State::Master);
        CHECK(y.port->state() == Ptp::Port::State::Passive);
}

TEST_CASE("PTP: a priority changed live re-runs the election") {
        Sim   sim;
        Node &a = sim.add(1, media(), 0, Epoch);
        Node &b = sim.add(2, media(), 0, Epoch);
        sim.start();
        sim.run(30 * S);
        REQUIRE(a.port->state() == Ptp::Port::State::Master);
        b.port->setConfig(withPriority(50), sim.now);
        sim.run(sim.now + 10 * S);
        CHECK(b.port->state() == Ptp::Port::State::Master);
        CHECK(a.port->state() == Ptp::Port::State::Slave);
}

// The board's service moves ptpclock0 while it is master (to TAI, or after NTP): back to a master
// whose time is elsewhere, the new master's first offset is stepped, not slewed for hours.
TEST_CASE("PTP: a master's clock moved, then back to a slave: the new master's first offset is stepped") {
        Sim   sim;
        Node &a = sim.add(1, withPriority(100), 0, Epoch);
        Node &b = sim.add(2, media(), 20'000, Epoch + 1500 * Ms);
        sim.start();
        sim.run(40 * S);
        REQUIRE(b.port->state() == Ptp::Port::State::Slave);
        REQUIRE(b.port->counts().steps == 1);

        a.up = false;
        sim.run(sim.now + 15 * S);
        REQUIRE(b.port->state() == Ptp::Port::State::Master);
        b.step(37 * S); // as the service does, from a master on UTC to TAI

        a.up = true;
        sim.run(sim.now + 20 * S);
        CHECK(b.port->state() == Ptp::Port::State::Slave);
        CHECK(b.port->counts().steps == 2);
        CHECK(sim.worstOffset(b, a, sim.now + 20 * S) < 500);
}

// linuxptp on software stamps masters on the arbitrary timescale (UTC). The Announce that chooses
// it comes before it is the parent: its timescale must be the port's from then, not the one the
// port had as master until the next Announce (the system clock follows on it once locked).
TEST_CASE("PTP: a new parent's time properties are taken when it is chosen") {
        Sim               sim;
        Ptp::Port::Config arbitrary = withPriority(100);
        arbitrary.timeFlags = 0;
        Node &a = sim.add(1, arbitrary, 0, Epoch);
        Node &b = sim.add(2, media(), 0, Epoch + 37 * S);
        a.up = false;
        sim.start();
        sim.run(30 * S);
        REQUIRE(b.port->state() == Ptp::Port::State::Master);
        CHECK((b.port->status().timeFlags & Ptp::PtpTimescale) != 0);
        a.up = true;
        while (b.port->state() == Ptp::Port::State::Master && sim.now < 60 * S) sim.run(sim.now + 1 * Ms);
        REQUIRE(b.port->state() != Ptp::Port::State::Master);
        CHECK((b.port->status().timeFlags & Ptp::PtpTimescale) == 0);
}
