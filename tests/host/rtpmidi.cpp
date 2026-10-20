/**
 * @file      rtpmidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Network MIDI's portable parts (devplan/audio.md §23): RTP-MIDI's packets
 * and AppleMIDI's sessions.
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>
#include <bmds/applemidi.h>
#include <bmds/midiparser.h>
#include <bmds/rtpmidi.h>

using bmds::AppleMidi;
using bmds::ByteSpan;
using bmds::Error;
using bmds::MidiEvent;
using bmds::MidiParser;
using bmds::RtpMidiWriter;
namespace RtpMidi = bmds::RtpMidi;

namespace {

        using Bytes = std::vector<uint8_t>;

        constexpr int64_t Ms = 1'000'000;
        constexpr int64_t Second = 1'000 * Ms;

        // An RTP-MIDI packet: the header (sequence 7, time 1000, SSRC 0x11223344), then @p header
        // byte(s) and @p list.
        Bytes packet(const Bytes &list, uint8_t flags = 0, bool longHeader = false) {
                Bytes p = {0x80, 0x61, 0x00, 0x07, 0x00, 0x00, 0x03, 0xe8, 0x11, 0x22, 0x33, 0x44};
                if (longHeader || list.size() > 15) {
                        p.push_back(uint8_t(0x80 | flags | ((list.size() >> 8) & 0x0f)));
                        p.push_back(uint8_t(list.size()));
                } else {
                        p.push_back(uint8_t(flags | list.size()));
                }
                p.insert(p.end(), list.begin(), list.end());
                return p;
        }

        void collect(void *arg, uint8_t byte) {
                static_cast<Bytes *>(arg)->push_back(byte);
                return;
        }

        // The MIDI bytes a packet's commands give, with @p status carried in and out.
        Bytes bytesOf(const Bytes &datagram, uint8_t &status, Error *err = nullptr) {
                RtpMidi::Packet p;
                Bytes           out;
                const Error     parsed = RtpMidi::parse(ByteSpan(datagram.data(), datagram.size()), p);
                if (parsed.isError()) {
                        if (err != nullptr) *err = parsed;
                        return out;
                }
                const Error e = RtpMidi::commands(p, status, collect, &out);
                if (err != nullptr) *err = e;
                return out;
        }

        Bytes bytesOf(const Bytes &datagram) {
                uint8_t status = 0;
                return bytesOf(datagram, status);
        }

        MidiEvent event(const Bytes &bytes) {
                MidiEvent e;
                e.size = uint8_t(bytes.size());
                std::memcpy(e.data, bytes.data(), bytes.size());
                return e;
        }

        // The events a byte stream makes, each as its bytes.
        std::vector<Bytes> eventsOf(const Bytes &in) {
                MidiParser         parser;
                std::vector<Bytes> out;
                for (uint8_t b : in) {
                        MidiEvent    events[MidiParser::MaxEvents];
                        const size_t n = parser.feed(b, events);
                        for (size_t i = 0; i < n; ++i)
                                out.emplace_back(events[i].data, events[i].data + events[i].size);
                }
                return out;
        }

} // namespace

TEST_CASE("rtpmidi: the header") {
        RtpMidi::Packet p;
        Bytes           d = packet({0x90, 0x3c, 0x64});
        CHECK(RtpMidi::parse(ByteSpan(d.data(), d.size()), p) == Error::Ok);
        CHECK(p.sequence == 7);
        CHECK(p.timestamp == 1000);
        CHECK(p.ssrc == 0x11223344u);
        CHECK(p.commands.size() == 3);
        CHECK(!p.journal);

        Bytes bad = d;
        bad[0] = 0x40; // version 1
        CHECK(RtpMidi::parse(ByteSpan(bad.data(), bad.size()), p) == Error::Invalid);
        bad = d;
        bad[1] = 0x60; // payload type 96
        CHECK(RtpMidi::parse(ByteSpan(bad.data(), bad.size()), p) == Error::Invalid);
        bad = d;
        bad[1] = 0xe1; // the marker bit: still 97
        CHECK(RtpMidi::parse(ByteSpan(bad.data(), bad.size()), p) == Error::Ok);
        bad = d;
        bad[12] = 0x05; // longer than the datagram
        CHECK(RtpMidi::parse(ByteSpan(bad.data(), bad.size()), p) == Error::Invalid);
        CHECK(RtpMidi::parse(ByteSpan(d.data(), 12), p) == Error::Invalid);

        // A long header, a journal after the commands, a contributing source.
        d = packet({0x90, 0x3c, 0x64}, 0x40, true);
        d.insert(d.end(), {0x01, 0x02, 0x03}); // "the journal"
        CHECK(RtpMidi::parse(ByteSpan(d.data(), d.size()), p) == Error::Ok);
        CHECK(p.journal);
        CHECK(p.commands.size() == 3);
        d = packet({0x90, 0x3c, 0x64});
        d[0] = 0x81;
        d.insert(d.begin() + 12, {0xaa, 0xbb, 0xcc, 0xdd});
        CHECK(RtpMidi::parse(ByteSpan(d.data(), d.size()), p) == Error::Ok);
        CHECK(p.commands.size() == 3);
        CHECK(p.commands[0] == 0x90);
}

TEST_CASE("rtpmidi: commands") {
        // Running status and delta times (one byte, and two), real-time between.
        CHECK(bytesOf(packet({0x90, 0x3c, 0x64, 0x00, 0x3e, 0x64, 0x81, 0x00, 0xf8, 0x05, 0x40, 0x00})) ==
              Bytes{0x90, 0x3c, 0x64, 0x90, 0x3e, 0x64, 0xf8, 0x90, 0x40, 0x00});
        // Z: a delta before the first command.
        CHECK(bytesOf(packet({0x0a, 0xc0, 0x05}, 0x20)) == Bytes{0xc0, 0x05});
        // System common ends running status.
        Error   err = Error::Ok;
        uint8_t status = 0;
        CHECK(bytesOf(packet({0x90, 0x3c, 0x64, 0x00, 0xf3, 0x01, 0x00, 0x3e, 0x64}), status, &err) ==
              Bytes{0x90, 0x3c, 0x64, 0xf3, 0x01});
        CHECK(err == Error::Invalid);
        // Phantom status: the last packet's.
        status = 0;
        (void)bytesOf(packet({0xb0, 0x07, 0x50}), status);
        CHECK(status == 0xb0);
        CHECK(bytesOf(packet({0x0a, 0x10}, 0x10), status) == Bytes{0xb0, 0x0a, 0x10});
        // Data with no status at all.
        status = 0;
        CHECK(bytesOf(packet({0x3c, 0x64}), status, &err).empty());
        CHECK(err == Error::Invalid);
        // A command cut short.
        CHECK(bytesOf(packet({0x90, 0x3c}), status, &err).empty());
        CHECK(err == Error::Invalid);
        // A delta at the end with no command: what came before.
        CHECK(bytesOf(packet({0xc0, 0x05, 0x00})) == Bytes{0xc0, 0x05});
}

TEST_CASE("rtpmidi: SysEx segments") {
        // Whole.
        CHECK(bytesOf(packet({0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7})) == Bytes{0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7});
        // First, middle, last, across packets; a clock inside one.
        uint8_t status = 0x90;
        Bytes   all = bytesOf(packet({0xf0, 0x01, 0x02, 0xf0}), status);
        CHECK(status == 0);
        Bytes more = bytesOf(packet({0xf7, 0x03, 0xf8, 0x04, 0xf0}), status);
        all.insert(all.end(), more.begin(), more.end());
        more = bytesOf(packet({0xf7, 0x05, 0xf7, 0x00, 0x90, 0x3c, 0x64}), status);
        all.insert(all.end(), more.begin(), more.end());
        CHECK(all == Bytes{0xf0, 0x01, 0x02, 0x03, 0xf8, 0x04, 0x05, 0xf7, 0x90, 0x3c, 0x64});
        CHECK(eventsOf(all).size() == 4); // the SysEx in pieces, the clock, the note
        // Cancelled: dropped.
        CHECK(bytesOf(packet({0xf0, 0x01, 0x02, 0xf4, 0x00, 0xc0, 0x01})) == Bytes{0xc0, 0x01});
        // Unended, and a status byte inside.
        Error err = Error::Ok;
        status = 0;
        CHECK(bytesOf(packet({0xf0, 0x01, 0x02}), status, &err).empty());
        CHECK(err == Error::Invalid);
        CHECK(bytesOf(packet({0xf0, 0x01, 0x90, 0xf7}), status, &err).empty());
        CHECK(err == Error::Invalid);
}

TEST_CASE("rtpmidi: written and read back") {
        // Every kind of event, SysEx in pieces with a clock among them, through as many packets as
        // they need: the same events come back.
        std::vector<Bytes> in;
        for (int round = 0; round < 60; ++round) {
                in.push_back({0x90, uint8_t(round), 0x64});
                in.push_back({0xc1, 0x05});
                in.push_back({0xf8});
                in.push_back({0xf0, 0x7d, 0x01, 0x02, 0x03});
                in.push_back({0x04, 0x05, 0x06, 0x07, 0x08});
                in.push_back({0xfe});
                in.push_back({0x09, 0xf7});
                in.push_back({0xf2, 0x10, 0x20});
        }
        RtpMidiWriter w;
        Bytes         stream;
        uint8_t       status = 0;
        size_t        packets = 0;
        size_t        i = 0;
        while (i < in.size()) {
                w.begin(uint16_t(packets), 0, 0x55);
                while (i < in.size() && w.add(event(in[i]))) ++i;
                REQUIRE(!w.isEmpty());
                const ByteSpan out = w.finish();
                CHECK(out.size() <= RtpMidiWriter::MaxPacket);
                Error       err = Error::Ok;
                const Bytes b = bytesOf(Bytes(out.data(), out.data() + out.size()), status, &err);
                CHECK(err == Error::Ok);
                stream.insert(stream.end(), b.begin(), b.end());
                ++packets;
        }
        CHECK(packets > 1);
        CHECK(eventsOf(stream) == in);

        // A SysEx cut short by a note: cancelled, the note kept.
        w.reset();
        w.begin(0, 0, 0x55);
        CHECK(w.add(event({0xf0, 0x01, 0x02})));
        CHECK(w.add(event({0x90, 0x3c, 0x64})));
        ByteSpan out = w.finish();
        CHECK(bytesOf(Bytes(out.data(), out.data() + out.size())) == Bytes{0x90, 0x3c, 0x64});
        // Cut in the next packet.
        w.begin(0, 0, 0x55);
        CHECK(w.add(event({0xf0, 0x01, 0x02})));
        out = w.finish();
        status = 0;
        CHECK(bytesOf(Bytes(out.data(), out.data() + out.size()), status) == Bytes{0xf0, 0x01, 0x02});
        w.begin(1, 0, 0x55);
        CHECK(w.add(event({0xc0, 0x03})));
        out = w.finish();
        CHECK(bytesOf(Bytes(out.data(), out.data() + out.size()), status) == Bytes{0xc0, 0x03});
}

namespace {

        // A transport that keeps what the sessions say.
        struct Wire final : AppleMidi::Transport {
                        struct Sent {
                                        AppleMidi::Channel  channel;
                                        AppleMidi::Endpoint to;
                                        Bytes               bytes;
                                        std::string         command() const {
                                                return bytes.size() >= 4 && bytes[0] == 0xff
                                                                       ? std::string(bytes.begin() + 2, bytes.begin() + 4)
                                                                       : std::string("RTP");
                                        }
                        };
                        std::vector<Sent>                              sent;
                        std::vector<size_t>                            ups;
                        std::vector<std::pair<size_t, AppleMidi::End>> downs;
                        std::vector<size_t>                            packets;

                        void send(AppleMidi::Channel channel, const AppleMidi::Endpoint &to, ByteSpan bytes) override {
                                sent.push_back({channel, to, Bytes(bytes.data(), bytes.data() + bytes.size())});
                        }
                        void up(size_t s) override { ups.push_back(s); }
                        void down(size_t s, AppleMidi::End why) override { downs.emplace_back(s, why); }
                        void received(size_t s, const RtpMidi::Packet &, int64_t) override { packets.push_back(s); }
        };

        void put32(Bytes &b, uint32_t v) {
                for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
        }

        void put64(Bytes &b, uint64_t v) {
                put32(b, uint32_t(v >> 32));
                put32(b, uint32_t(v));
        }

        Bytes session(const char *command, uint32_t token, uint32_t ssrc, const char *name = nullptr) {
                Bytes b = {0xff, 0xff, uint8_t(command[0]), uint8_t(command[1])};
                put32(b, 2);
                put32(b, token);
                put32(b, ssrc);
                if (name != nullptr) b.insert(b.end(), name, name + std::strlen(name) + 1);
                return b;
        }

        Bytes sync(uint32_t ssrc, uint8_t count, uint64_t t0, uint64_t t1, uint64_t t2) {
                Bytes b = {0xff, 0xff, 'C', 'K'};
                put32(b, ssrc);
                b.insert(b.end(), {count, 0, 0, 0});
                put64(b, t0);
                put64(b, t1);
                put64(b, t2);
                return b;
        }

        uint32_t at32(const Bytes &b, size_t i) {
                return (uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) | (uint32_t(b[i + 2]) << 8) | b[i + 3];
        }

        void give(AppleMidi &m, AppleMidi::Channel ch, AppleMidi::Endpoint from, const Bytes &b, int64_t now) {
                m.receive(ch, from, ByteSpan(b.data(), b.size()), now);
                return;
        }

        constexpr AppleMidi::Endpoint MacControl{0x0a000002, 5004};
        constexpr AppleMidi::Endpoint MacData{0x0a000002, 5005};
        constexpr uint32_t            MacSsrc = 0xabcdef01;

} // namespace

TEST_CASE("applemidi: accepting a session") {
        Wire      wire;
        AppleMidi m;
        m.start(wire, 0x1234, "bmds0", true, 99);
        using C = AppleMidi::Channel;

        give(m, C::Control, MacControl, session("IN", 77, MacSsrc, "Jason's Mac"), 0);
        REQUIRE(wire.sent.size() == 1);
        CHECK(wire.sent[0].command() == "OK");
        CHECK(wire.sent[0].channel == C::Control);
        CHECK(wire.sent[0].to == MacControl);
        CHECK(at32(wire.sent[0].bytes, 8) == 77);
        CHECK(at32(wire.sent[0].bytes, 12) == 0x1234);
        CHECK(std::string(reinterpret_cast<const char *>(wire.sent[0].bytes.data() + 16)) == "bmds0");
        CHECK(m.info(0).state == AppleMidi::State::Accepting);
        CHECK(wire.ups.empty());

        give(m, C::Data, MacData, session("IN", 77, MacSsrc, "Jason's Mac"), Ms);
        CHECK(wire.sent.back().command() == "OK");
        CHECK(wire.sent.back().channel == C::Data);
        REQUIRE(wire.ups == std::vector<size_t>{0});
        CHECK(m.isUp(0));
        CHECK(std::string(m.info(0).name) == "Jason's Mac");

        // Their sync: answered with our time; their last one gives the offset.
        give(m, C::Data, MacData, sync(MacSsrc, 0, 5000, 0, 0), 2 * Ms);
        CHECK(wire.sent.back().command() == "CK");
        CHECK(wire.sent.back().bytes[8] == 1);
        give(m, C::Data, MacData, sync(MacSsrc, 2, 5000, 20, 5010), 3 * Ms);
        CHECK(m.info(0).syncs == 1);
        CHECK(m.info(0).roundTripUnits == 10);
        CHECK(m.info(0).offsetUnits == 5005 - 20);

        // MIDI: given on; a gap counted; feedback sent within a second.
        Bytes p = packet({0x90, 0x3c, 0x64});
        p[8] = 0xab, p[9] = 0xcd, p[10] = 0xef, p[11] = 0x01;
        give(m, C::Data, MacData, p, 4 * Ms);
        p[3] = 10; // sequence 7 to 10: 2 missed
        give(m, C::Data, MacData, p, 5 * Ms);
        CHECK(wire.packets.size() == 2);
        CHECK(m.info(0).lost == 2);
        (void)m.tick(6 * Ms);
        CHECK(wire.sent.back().command() == "RS");
        CHECK(wire.sent.back().to == MacControl);
        // From another sender or address: ignored.
        p[11] = 0x02;
        give(m, C::Data, MacData, p, 7 * Ms);
        CHECK(wire.packets.size() == 2);

        // They leave.
        give(m, C::Control, MacControl, session("BY", 77, MacSsrc), 8 * Ms);
        REQUIRE(wire.downs.size() == 1);
        CHECK(wire.downs[0].second == AppleMidi::End::Theirs);
        CHECK(!m.isUp(0));
}

TEST_CASE("applemidi: refusing, timeouts, a full table") {
        Wire      wire;
        AppleMidi m;
        m.start(wire, 0x1234, "bmds0", false, 99);
        using C = AppleMidi::Channel;
        give(m, C::Control, MacControl, session("IN", 1, MacSsrc, "x"), 0);
        CHECK(wire.sent.back().command() == "NO");
        CHECK(m.info(0).state == AppleMidi::State::Free);

        m.setAccept(true);
        // The data port's invitation never comes: forgotten, nothing said.
        give(m, C::Control, MacControl, session("IN", 2, MacSsrc, "x"), 0);
        (void)m.tick(6 * Second);
        CHECK(m.info(0).state == AppleMidi::State::Free);
        CHECK(wire.downs.empty());
        // A data port invitation with no control one first.
        give(m, C::Data, MacData, session("IN", 3, MacSsrc, "x"), 7 * Second);
        CHECK(wire.sent.back().command() == "NO");

        // Up, then silent for a minute: BY, and down.
        give(m, C::Control, MacControl, session("IN", 4, MacSsrc, "x"), 10 * Second);
        give(m, C::Data, MacData, session("IN", 4, MacSsrc, "x"), 10 * Second);
        CHECK(m.isUp(0));
        CHECK(m.tick(11 * Second) == 70 * Second);
        (void)m.tick(70 * Second);
        CHECK(wire.sent.back().command() == "BY");
        REQUIRE(wire.downs.size() == 1);
        CHECK(wire.downs[0].second == AppleMidi::End::Silent);

        // Full.
        for (uint32_t i = 0; i < AppleMidi::MaxSessions; ++i)
                give(m, C::Control, AppleMidi::Endpoint{i + 100, 5004}, session("IN", i, i + 100, "x"), 80 * Second);
        give(m, C::Control, MacControl, session("IN", 9, MacSsrc, "x"), 80 * Second);
        CHECK(wire.sent.back().command() == "NO");
}

TEST_CASE("applemidi: inviting") {
        Wire      wire;
        AppleMidi m;
        m.start(wire, 0x1234, "bmds0", true, 99);
        using C = AppleMidi::Channel;
        auto [s, err] = m.invite(MacControl, 0);
        REQUIRE(err == Error::Ok);
        CHECK(m.invite(MacControl, 0).error() == Error::Exists);
        REQUIRE(wire.sent.size() == 1);
        CHECK(wire.sent[0].command() == "IN");
        CHECK(wire.sent[0].to == MacControl);
        const uint32_t token = at32(wire.sent[0].bytes, 8);

        // Asked again a second later.
        CHECK(m.tick(500 * Ms) == Second);
        (void)m.tick(Second);
        CHECK(wire.sent.size() == 2);

        // Accepted: the data port (control + 1), then up and syncing at once.
        give(m, C::Control, MacControl, session("OK", token, MacSsrc, "Mac"), 2 * Second);
        CHECK(wire.sent.back().command() == "IN");
        CHECK(wire.sent.back().to == MacData);
        give(m, C::Data, MacData, session("OK", token, MacSsrc, "Mac"), 2 * Second);
        REQUIRE(wire.ups == std::vector<size_t>{s});
        CHECK(std::string(m.info(s).name) == "Mac");
        (void)m.tick(2 * Second);
        CHECK(wire.sent.back().command() == "CK");
        CHECK(wire.sent.back().bytes[8] == 0);
        CHECK(wire.sent.back().to == MacData);
        // Their answer: we finish it.
        give(m, C::Data, MacData, sync(MacSsrc, 1, 20000, 90000, 0), 2 * Second + 2 * Ms);
        CHECK(wire.sent.back().bytes[8] == 2);
        CHECK(m.info(s).syncs == 1);
        CHECK(m.info(s).roundTripUnits == 20);
        // The next sync 1.5 s on.
        CHECK(m.tick(2 * Second + 3 * Ms) == 3 * Second + 500 * Ms);

        // Sequence numbers for what we send.
        CHECK(m.takeSequence(s) == 0);
        CHECK(m.takeSequence(s) == 1);
        CHECK(m.info(s).packetsOut == 2);

        // Ended by us: BY to their control port.
        m.end(s);
        CHECK(wire.sent.back().command() == "BY");
        CHECK(wire.sent.back().to == MacControl);
        REQUIRE(wire.downs.size() == 1);
        CHECK(wire.downs[0].second == AppleMidi::End::Ours);

        // Refused, and unanswered.
        auto [s2, e2] = m.invite(MacControl, 10 * Second);
        REQUIRE(e2 == Error::Ok);
        give(m, C::Control, MacControl, session("NO", at32(wire.sent.back().bytes, 8), MacSsrc), 10 * Second);
        CHECK(wire.downs.back().second == AppleMidi::End::Refused);
        (void)m.invite(MacControl, 20 * Second);
        for (int i = 1; i <= 12; ++i) (void)m.tick(20 * Second + i * Second);
        CHECK(wire.downs.back().second == AppleMidi::End::NoAnswer);
        CHECK(m.info(0).state == AppleMidi::State::Free);
}
