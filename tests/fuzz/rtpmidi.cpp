/**
 * @file      rtpmidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for network MIDI's portable parts (devplan/audio.md §23),
 * which read every datagram the board's AppleMIDI ports receive. The input's
 * first byte picks:
 * - 0: one datagram, through RTP-MIDI's reader into a MIDI parser;
 * - 1: datagrams (each a length byte and its bytes, a channel and time step
 *   from its first bits) given to an AppleMIDI that accepts, ticked between;
 *   after an invitation of its own;
 * - 2: the rest as MIDI bytes, parsed into events, written into packets: each
 *   must read back without an error, and parse into the same events.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <bmds/applemidi.h>
#include <bmds/midiparser.h>
#include <bmds/rtpmidi.h>

using bmds::AppleMidi;
using bmds::ByteSpan;
using bmds::MidiEvent;
using bmds::MidiParser;
using bmds::RtpMidiWriter;
namespace RtpMidi = bmds::RtpMidi;

namespace {

        void toParser(void *arg, uint8_t byte) {
                MidiEvent events[MidiParser::MaxEvents];
                (void)static_cast<MidiParser *>(arg)->feed(byte, events);
                return;
        }

        struct Quiet final : AppleMidi::Transport {
                        void send(AppleMidi::Channel, const AppleMidi::Endpoint &, ByteSpan b) override {
                                if (b.size() > 80) std::abort(); // a session packet's most
                        }
                        void up(size_t s) override {
                                if (s >= AppleMidi::MaxSessions) std::abort();
                        }
                        void down(size_t s, AppleMidi::End) override {
                                if (s >= AppleMidi::MaxSessions) std::abort();
                        }
                        void received(size_t s, const RtpMidi::Packet &p, int64_t) override {
                                if (s >= AppleMidi::MaxSessions) std::abort();
                                MidiParser parser;
                                uint8_t    status = 0;
                                (void)RtpMidi::commands(p, status, toParser, &parser);
                        }
        };

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        if (size < 1) return 0;
        const uint8_t mode = data[0] % 3;
        ++data;
        --size;
        if (mode == 0) {
                RtpMidi::Packet p;
                if (RtpMidi::parse(ByteSpan(data, size), p).isError()) return 0;
                if (p.commands.data() < data || p.commands.data() + p.commands.size() > data + size) std::abort();
                MidiParser parser;
                uint8_t    status = 0;
                (void)RtpMidi::commands(p, status, toParser, &parser);
                return 0;
        }
        if (mode == 1) {
                static Quiet quiet;
                AppleMidi    m;
                m.start(quiet, 0x1234, "fuzz", true, 7);
                int64_t now = 0;
                (void)m.invite(AppleMidi::Endpoint{1, 5004}, now);
                while (size >= 2) {
                        const uint8_t head = data[0];
                        size_t        n = data[1];
                        data += 2;
                        size -= 2;
                        if (n > size) n = size;
                        const AppleMidi::Channel ch =
                                (head & 1) ? AppleMidi::Channel::Data : AppleMidi::Channel::Control;
                        const AppleMidi::Endpoint from{uint32_t((head >> 1) & 1), uint16_t(5004 + ((head >> 2) & 1))};
                        m.receive(ch, from, ByteSpan(data, n), now);
                        now += int64_t(head >> 3) * 1'000'000'000;
                        (void)m.tick(now);
                        data += n;
                        size -= n;
                }
                m.endAll();
                return 0;
        }
        // The writer: events a parser made of the input (as the router gives them), written,
        // read back and parsed again, are the same events (unless the input had errors: a SysEx
        // cut short is cancelled, its pieces already given).
        static MidiEvent in[4096], out[4096];
        size_t           nin = 0, nout = 0;
        MidiParser       first, second;
        RtpMidiWriter    w;
        uint8_t          status = 0;
        uint16_t         sequence = 0;
        auto             readBack = [&](ByteSpan pkt) {
                RtpMidi::Packet p;
                if (RtpMidi::parse(pkt, p).isError()) std::abort();
                struct Sink {
                                MidiParser *parser;
                                MidiEvent  *out;
                                size_t     *n;
                } sink{&second, out, &nout};
                if (RtpMidi::commands(
                            p, status,
                            [](void *arg, uint8_t byte) {
                                    auto        *k = static_cast<Sink *>(arg);
                                    MidiEvent    events[MidiParser::MaxEvents];
                                    const size_t c = k->parser->feed(byte, events);
                                    for (size_t i = 0; i < c && *k->n < 4096; ++i) k->out[(*k->n)++] = events[i];
                            },
                            &sink)
                            .isError())
                        std::abort(); // what the writer writes always reads
        };
        w.begin(0, 0, 1);
        for (size_t i = 0; i < size && nin < 4096; ++i) {
                MidiEvent    events[MidiParser::MaxEvents];
                const size_t c = first.feed(data[i], events);
                for (size_t k = 0; k < c && nin < 4096; ++k) {
                        in[nin++] = events[k];
                        if (w.add(events[k])) continue;
                        readBack(w.finish());
                        w.begin(++sequence, 0, 1);
                        if (!w.add(events[k])) std::abort(); // an event always fits an empty packet
                }
        }
        readBack(w.finish());
        if (first.takeErrors() != 0 || nin >= 4096) return 0;
        if (nout != nin) std::abort();
        for (size_t i = 0; i < nin; ++i)
                if (in[i].size != out[i].size || std::memcmp(in[i].data, out[i].data, in[i].size) != 0) std::abort();
        return 0;
}
