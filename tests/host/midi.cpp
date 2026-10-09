/**
 * @file      midi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * MIDI's portable parts (devplan/audio.md §21): the parser, the event ring,
 * the router and the USB packer.
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>
#include <bmds/midiparser.h>
#include <bmds/midiqueue.h>
#include <bmds/midirouter.h>
#include <bmds/midiusbpacker.h>

using bmds::Error;
using bmds::MidiEvent;
using bmds::MidiParser;
using bmds::MidiQueue;
using bmds::MidiRouter;
using bmds::MidiSink;

namespace {

        using Bytes = std::vector<uint8_t>;

        // The events @p in makes, each as its bytes.
        std::vector<Bytes> parse(MidiParser &parser, const Bytes &in) {
                std::vector<Bytes> out;
                for (uint8_t b : in) {
                        MidiEvent    events[MidiParser::MaxEvents];
                        const size_t n = parser.feed(b, events);
                        for (size_t i = 0; i < n; ++i)
                                out.emplace_back(events[i].data, events[i].data + events[i].size);
                }
                return out;
        }

        std::vector<Bytes> parse(const Bytes &in) {
                MidiParser parser;
                return parse(parser, in);
        }

        // A sink that keeps what it's given, with room for @p room events.
        class Collector final : public MidiSink {
                public:
                        explicit Collector(size_t room = 1000, bool kicks = false) : _room(room), _kicks(kicks) {}

                        Delivery deliver(const MidiEvent &event) override {
                                if (events.size() >= _room) return Delivery::Dropped;
                                events.push_back(event);
                                return _kicks ? Delivery::TakenKick : Delivery::Taken;
                        }
                        void kick() override { ++kicked; }
                        void portGone() override { gone = true; }

                        std::vector<MidiEvent> events;
                        int                    kicked = 0;
                        bool                   gone = false;

                private:
                        size_t _room;
                        bool   _kicks;
        };

        MidiEvent noteOn(uint8_t note, int64_t timeNs = 0) {
                MidiEvent e;
                e.timeNs = timeNs;
                e.size = 3;
                e.data[0] = 0x90;
                e.data[1] = note;
                e.data[2] = 100;
                return e;
        }

} // namespace

TEST_CASE("MidiParser: whole messages and running status") {
        CHECK(parse({0x90, 0x3c, 0x64}) == std::vector<Bytes>{{0x90, 0x3c, 0x64}});
        // Running status, then a new status, a 1-byte message and pitch bend.
        CHECK(parse({0x90, 0x3c, 0x64, 0x3e, 0x00, 0xc1, 0x05, 0x07, 0xe0, 0x00, 0x40}) ==
              std::vector<Bytes>{
                      {0x90, 0x3c, 0x64}, {0x90, 0x3e, 0x00}, {0xc1, 0x05}, {0xc1, 0x07}, {0xe0, 0x00, 0x40}});
        // System common: no running status after it.
        MidiParser p;
        CHECK(parse(p, {0xf2, 0x10, 0x20, 0x30, 0xf1, 0x01, 0xf3, 0x02, 0xf6}) ==
              std::vector<Bytes>{{0xf2, 0x10, 0x20}, {0xf1, 0x01}, {0xf3, 0x02}, {0xf6}});
        CHECK(p.errors() == 1); // the 0x30
        // Every status's length.
        CHECK(MidiParser::dataBytes(0x80) == 2);
        CHECK(MidiParser::dataBytes(0xaf) == 2);
        CHECK(MidiParser::dataBytes(0xbf) == 2);
        CHECK(MidiParser::dataBytes(0xc0) == 1);
        CHECK(MidiParser::dataBytes(0xdf) == 1);
        CHECK(MidiParser::dataBytes(0xef) == 2);
        CHECK(MidiParser::dataBytes(0xf0) == -1);
        CHECK(MidiParser::dataBytes(0xf4) == -1);
        CHECK(MidiParser::dataBytes(0xf7) == -1);
        CHECK(MidiParser::dataBytes(0xf8) == 0);
        CHECK(MidiParser::dataBytes(0xfd) == -1);
        CHECK(MidiParser::dataBytes(0x7f) == -1);
}

TEST_CASE("MidiParser: real-time bytes inside messages and SysEx") {
        CHECK(parse({0x90, 0xf8, 0x3c, 0xfe, 0x64}) == std::vector<Bytes>{{0xf8}, {0xfe}, {0x90, 0x3c, 0x64}});
        CHECK(parse({0xf0, 0x7e, 0xf8, 0x01, 0xf7}) == std::vector<Bytes>{{0xf8}, {0xf0, 0x7e, 0x01, 0xf7}});
        // The undefined ones go, and change nothing.
        MidiParser p;
        CHECK(parse(p, {0x90, 0x3c, 0xf9, 0xfd, 0x64}) == std::vector<Bytes>{{0x90, 0x3c, 0x64}});
        CHECK(p.takeErrors() == 2);
        CHECK(p.errors() == 0);
}

TEST_CASE("MidiParser: SysEx in pieces, ended or cut") {
        CHECK(parse({0xf0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xf7}) ==
              std::vector<Bytes>{{0xf0, 1, 2, 3, 4}, {5, 6, 7, 8, 9}, {0xf7}});
        CHECK(parse({0xf0, 0xf7}) == std::vector<Bytes>{{0xf0, 0xf7}});
        // Cut by a status: closed with an F7, counted, and the new message goes on.
        MidiParser p;
        CHECK(parse(p, {0xf0, 1, 2, 0x90, 0x3c, 0x64}) == std::vector<Bytes>{{0xf0, 1, 2, 0xf7}, {0x90, 0x3c, 0x64}});
        CHECK(p.takeErrors() == 1);
        // Cut by F6: two events from one byte.
        CHECK(parse(p, {0xf0, 1, 2, 3, 0xf6}) == std::vector<Bytes>{{0xf0, 1, 2, 3, 0xf7}, {0xf6}});
        // Cut by another F0.
        CHECK(parse(p, {0xf0, 1, 0xf0, 2, 0xf7}) == std::vector<Bytes>{{0xf0, 1, 0xf7}, {0xf0, 2, 0xf7}});
        CHECK(p.takeErrors() == 2);
        // No running status after a SysEx.
        CHECK(parse(p, {0x90, 0x3c, 0x64, 0xf0, 0xf7, 0x3c, 0x00}) ==
              std::vector<Bytes>{{0x90, 0x3c, 0x64}, {0xf0, 0xf7}});
        CHECK(p.takeErrors() == 2);
}

TEST_CASE("MidiParser: stray bytes") {
        MidiParser p;
        CHECK(parse(p, {0x3c, 0x64, 0xf7, 0xf4, 0x01, 0xf5}).empty());
        CHECK(p.takeErrors() == 6);
        // A message under way, forgotten.
        CHECK(parse(p, {0x90, 0x3c}).empty());
        p.reset();
        CHECK(parse(p, {0x64}).empty());
        CHECK(p.errors() == 1);
}

TEST_CASE("MidiQueue") {
        MidiQueue q;
        CHECK(q.capacity() == 0);
        MidiEvent e;
        CHECK_FALSE(q.push(e));
        MidiEvent storage[4];
        CHECK_FALSE(q.setStorage(bmds::Span<MidiEvent>(storage, 3)));
        REQUIRE(q.setStorage(bmds::Span<MidiEvent>(storage, 4)));
        for (uint8_t i = 0; i < 4; ++i) CHECK(q.push(noteOn(i)));
        CHECK_FALSE(q.push(noteOn(9)));
        CHECK(q.size() == 4);
        for (uint8_t i = 0; i < 10; ++i) {
                REQUIRE(q.peek() != nullptr);
                CHECK(q.peek()->data[1] == i);
                REQUIRE(q.pop(e));
                CHECK(e.data[1] == i);
                CHECK(q.push(noteOn(static_cast<uint8_t>(i + 4))));
        }
        q.clear();
        CHECK(q.isEmpty());
        CHECK_FALSE(q.pop(e));
        CHECK(q.peek() == nullptr);
}

TEST_CASE("MidiRouter: ports and names") {
        MidiRouter router;
        Collector  sink;
        CHECK(router.addPort(MidiRouter::Kind::Usb, 0, "x", nullptr).error() == Error::Invalid);
        CHECK(router.addPort(MidiRouter::Kind::Usb, MidiRouter::Destination, "x", nullptr).error() == Error::Invalid);
        CHECK(router.addPort(MidiRouter::Kind::Usb, 4, "x", &sink).error() == Error::Invalid);
        auto [a, ea] = router.addPort(MidiRouter::Kind::Usb, MidiRouter::Source, "  Keystation 49  ", nullptr);
        REQUIRE(ea == Error::Ok);
        CHECK(a == 0);
        auto [b, eb] = router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Destination, "synth", &sink);
        REQUIRE(eb == Error::Ok);
        CHECK(b == 1);
        auto [info, ei] = router.port(a);
        REQUIRE(ei == Error::Ok);
        CHECK(std::strcmp(info.name, "Keystation 49") == 0);
        CHECK(info.kind == MidiRouter::Kind::Usb);
        CHECK(router.find("keystation 49").value() == 0);
        CHECK(router.find("midi1").value() == 1);
        CHECK(router.find("1").value() == 1);
        CHECK(router.find("midi2").error() == Error::NotExist);
        CHECK(router.find("nothing").error() == Error::NotExist);
        // Numbers are reused, lowest first.
        router.removePort(a);
        CHECK(router.port(a).error() == Error::NotExist);
        CHECK(router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Source, "again", nullptr).value() == 0);
        // A long name, cut at a character's end.
        std::string longName(29, 'x');
        longName += "\xc3\xa9\xc3\xa9"; // é é: the second one doesn't fit
        CHECK(router.rename(0, longName.c_str()) == Error::Ok);
        CHECK(std::strlen(router.port(0).value().name) == 31);
        CHECK(router.rename(5, "x") == Error::NotExist);
        // Full.
        for (size_t i = 2; i < MidiRouter::MaxPorts; ++i)
                CHECK(router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Source, "p", nullptr).error() == Error::Ok);
        CHECK(router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Source, "p", nullptr).error() == Error::Full);
        MidiRouter::PortInfo all[4];
        CHECK(router.ports(bmds::Span<MidiRouter::PortInfo>(all, 4)) == MidiRouter::MaxPorts);
        CHECK(all[1].number == 1);
}

TEST_CASE("MidiRouter: connections, fan-out, taps") {
        MidiRouter    router;
        Collector     synth, module(2, true), tap(1);
        const uint8_t keys = router.addPort(MidiRouter::Kind::Usb, MidiRouter::Source, "keys", nullptr).value();
        const uint8_t s = router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Destination, "synth", &synth).value();
        const uint8_t m =
                router.addPort(MidiRouter::Kind::Usb, MidiRouter::Source | MidiRouter::Destination, "module", &module)
                        .value();
        CHECK(router.connect(keys, s) == Error::Ok);
        CHECK(router.connect(keys, s) == Error::Exists);
        CHECK(router.connect(keys, m) == Error::Ok);
        CHECK(router.connect(s, m) == Error::Invalid); // the synth has no source
        CHECK(router.connect(keys, keys) == Error::Invalid);
        CHECK(router.connect(keys, 9) == Error::NotExist);
        CHECK(router.attach(keys, tap, true) == Error::Ok);
        CHECK(router.attach(keys, tap, true) == Error::Invalid);
        CHECK(router.attach(s, synth, true) == Error::Invalid); // no source to read
        CHECK(router.send(keys, noteOn(60, 1000)) == Error::Ok);
        CHECK(router.send(keys, noteOn(61, 2000)) == Error::Ok);
        CHECK(router.send(keys, noteOn(62, 3000)) == Error::Ok);
        CHECK(router.send(s, noteOn(1)) == Error::Invalid);
        CHECK(router.send(9, noteOn(1)) == Error::NotExist);
        REQUIRE(synth.events.size() == 3);
        CHECK(synth.events[2].data[1] == 62);
        CHECK(synth.events[2].port == keys);
        CHECK(synth.events[2].timeNs == 3000);
        CHECK(module.events.size() == 2);
        CHECK(module.kicked == 2); // once a send
        CHECK(tap.events.size() == 1);
        auto k = router.port(keys).value().counters;
        CHECK(k.eventsIn == 3);
        CHECK(k.lastNs == 3000);
        auto mc = router.port(m).value().counters;
        CHECK(mc.eventsOut == 2);
        CHECK(mc.dropped == 1);
        // Straight to a destination.
        CHECK(router.deliver(s, noteOn(70)) == Error::Ok);
        CHECK(router.deliver(m, noteOn(70)) == Error::WouldBlock);
        CHECK(router.deliver(keys, noteOn(70)) == Error::Invalid);
        CHECK(synth.events.size() == 4);
        MidiRouter::Connection c[4];
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 2);
        CHECK(router.disconnect(keys, m) == Error::Ok);
        CHECK(router.disconnect(keys, m) == Error::NotExist);
        // A port that goes takes its connections and tells its taps.
        Collector other;
        CHECK(router.attach(s, other, false) == Error::Ok);
        router.removePort(keys);
        CHECK(tap.gone);
        CHECK_FALSE(other.gone);
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 0);
        router.removePort(s);
        CHECK(other.gone);
        router.detach(tap); // already off: nothing
        router.noteParseErrors(m, 3);
        router.noteDeviceError(m);
        CHECK(router.port(m).value().counters.parseErrors == 3);
        CHECK(router.port(m).value().counters.deviceErrors == 1);
}

TEST_CASE("MidiRouter: connections by name") {
        MidiRouter router;
        Collector  synth;
        CHECK(router.setAutoConnect("Keys>Synth, nothing>synth,bad") == Error::Ok);
        const uint8_t s = router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Destination, "synth", &synth).value();
        const uint8_t k = router.addPort(MidiRouter::Kind::Usb, MidiRouter::Source, "USB MIDI", nullptr).value();
        MidiRouter::Connection c[4];
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 0);
        // Its name arrives later.
        CHECK(router.rename(k, "keys") == Error::Ok);
        REQUIRE(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 1);
        CHECK(c[0].source == k);
        CHECK(c[0].destination == s);
        // Unplugged and plugged in again.
        router.removePort(k);
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 0);
        CHECK(router.addPort(MidiRouter::Kind::Usb, MidiRouter::Source, "keys", nullptr).error() == Error::Ok);
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 1);
        // By number, and set later.
        const uint8_t v = router.addPort(MidiRouter::Kind::Virtual, MidiRouter::Source, "seq", nullptr).value();
        CHECK(router.setAutoConnect("midi2>synth") == Error::Ok);
        CHECK(v == 2);
        CHECK(router.connections(bmds::Span<MidiRouter::Connection>(c, 4)) == 2);
        CHECK(router.setAutoConnect(std::string(MidiRouter::MaxAutoConnect, 'x').c_str()) == Error::Invalid);
}

TEST_CASE("MidiUsbPacker") {
        using bmds::MidiUsbPacker;
        MidiUsbPacker         packer;
        MidiUsbPacker::Packet p[MidiUsbPacker::MaxPackets];
        auto                  event = [](const Bytes &bytes) {
                MidiEvent e;
                e.size = static_cast<uint8_t>(bytes.size());
                std::memcpy(e.data, bytes.data(), bytes.size());
                return e;
        };
        auto packet = [&](size_t i) {
                return Bytes(p[i], p[i] + 4);
        };
        // Channel messages: the status's nibble; cable in the top.
        REQUIRE(packer.pack(2, event({0x93, 0x3c, 0x64}), p) == 1);
        CHECK(packet(0) == Bytes{0x29, 0x93, 0x3c, 0x64});
        REQUIRE(packer.pack(0, event({0xc0, 0x05}), p) == 1);
        CHECK(packet(0) == Bytes{0x0c, 0xc0, 0x05, 0x00});
        // System common and real-time.
        REQUIRE(packer.pack(0, event({0xf2, 0x10, 0x20}), p) == 1);
        CHECK(packet(0) == Bytes{0x03, 0xf2, 0x10, 0x20});
        REQUIRE(packer.pack(0, event({0xf1, 0x10}), p) == 1);
        CHECK(packet(0) == Bytes{0x02, 0xf1, 0x10, 0x00});
        REQUIRE(packer.pack(0, event({0xf6}), p) == 1);
        CHECK(packet(0) == Bytes{0x05, 0xf6, 0x00, 0x00});
        REQUIRE(packer.pack(1, event({0xf8}), p) == 1);
        CHECK(packet(0) == Bytes{0x1f, 0xf8, 0x00, 0x00});
        // SysEx: three bytes a packet across events, a clock inside, ended by 1, 2 or 3 bytes.
        CHECK(packer.pack(0, event({0xf0, 0x7e}), p) == 0);
        REQUIRE(packer.pack(0, event({0xf8}), p) == 1);
        CHECK(packet(0) == Bytes{0x0f, 0xf8, 0x00, 0x00});
        REQUIRE(packer.pack(0, event({0x01, 0x02, 0x03, 0x04, 0xf7}), p) == 3);
        CHECK(packet(0) == Bytes{0x04, 0xf0, 0x7e, 0x01});
        CHECK(packet(1) == Bytes{0x04, 0x02, 0x03, 0x04});
        CHECK(packet(2) == Bytes{0x05, 0xf7, 0x00, 0x00});
        REQUIRE(packer.pack(0, event({0xf0, 0xf7}), p) == 1);
        CHECK(packet(0) == Bytes{0x06, 0xf0, 0xf7, 0x00});
        REQUIRE(packer.pack(0, event({0xf0, 0x01, 0xf7}), p) == 1);
        CHECK(packet(0) == Bytes{0x07, 0xf0, 0x01, 0xf7});
        // Data outside a SysEx: nothing. A status cuts one under way.
        CHECK(packer.pack(0, event({0x01, 0x02}), p) == 0);
        CHECK(packer.pack(0, event({0xf0, 0x01}), p) == 0);
        REQUIRE(packer.pack(0, event({0x80, 0x3c, 0x00}), p) == 1);
        CHECK(packet(0) == Bytes{0x08, 0x80, 0x3c, 0x00});
        CHECK(packer.pack(0, event({0x02}), p) == 0);
        CHECK(packer.pack(0, MidiEvent(), p) == 0);
}
