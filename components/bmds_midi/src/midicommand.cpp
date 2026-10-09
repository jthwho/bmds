/**
 * @file      midicommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `midi` command: ports, connections, counters, a monitor, and bytes
 * sent (devplan/audio.md §21). Its output formats are a userspace interface
 * once shipped: keep the lines and their order, add only at the end.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/midi.h>
#include <bmds/midifile.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        const char *sidesName(uint8_t sides) {
                switch (sides & (MidiRouter::Source | MidiRouter::Destination)) {
                        case MidiRouter::Source: return "src";
                        case MidiRouter::Destination: return "dst";
                        default: return "src+dst";
                }
        }

        // The port @p arg names (midiN, N, /dev/midiN, or a name), or NoPort (told why).
        uint8_t portArg(CommandContext &ctx, const char *arg) {
                StringView name(arg);
                if (name.startsWith("/dev/")) name = name.mid(5);
                auto [n, err] = Midi::router().find(name);
                if (err.isError()) {
                        ctx.eprintf("midi: no port %s\n", arg);
                        return MidiRouter::NoPort;
                }
                return n;
        }

        int listPorts(CommandContext &ctx) {
                MidiRouter::PortInfo ports[MidiRouter::MaxPorts];
                const size_t count = Midi::router().ports(Span<MidiRouter::PortInfo>(ports, MidiRouter::MaxPorts));
                if (count == 0) ctx.printf("midi: no ports\n");
                for (size_t i = 0; i < count; ++i)
                        ctx.printf("midi%-2u  %-7s  %-7s  %s\n", static_cast<unsigned>(ports[i].number),
                                   Midi::kindName(ports[i].kind), sidesName(ports[i].sides), ports[i].name);
                MidiRouter::Connection c[MidiRouter::MaxConnections];
                const size_t           connections =
                        Midi::router().connections(Span<MidiRouter::Connection>(c, MidiRouter::MaxConnections));
                if (connections > 0) ctx.printf("Connections:\n");
                for (size_t i = 0; i < connections; ++i)
                        ctx.printf("  midi%u > midi%u\n", static_cast<unsigned>(c[i].source),
                                   static_cast<unsigned>(c[i].destination));
                return Shell::ExitSuccess;
        }

        void showPort(CommandContext &ctx, const MidiRouter::PortInfo &p) {
                const MidiRouter::Counters &c = p.counters;
                ctx.printf("midi%u  %s (%s, %s)\n", static_cast<unsigned>(p.number), p.name, Midi::kindName(p.kind),
                           sidesName(p.sides));
                if ((p.sides & MidiRouter::Source) != 0)
                        ctx.printf("  From its source:    %llu events\n", static_cast<unsigned long long>(c.eventsIn));
                if ((p.sides & MidiRouter::Destination) != 0)
                        ctx.printf("  To its destination: %llu events, %lu dropped\n",
                                   static_cast<unsigned long long>(c.eventsOut), static_cast<unsigned long>(c.dropped));
                ctx.printf("  Errors:             %lu parse, %lu device\n", static_cast<unsigned long>(c.parseErrors),
                           static_cast<unsigned long>(c.deviceErrors));
                if (c.lastNs != 0) {
                        const int64_t ago = (Clock::monotonicNs() - c.lastNs) / 1000000;
                        ctx.printf("  Last event:         %lld.%03lld s ago\n", static_cast<long long>(ago / 1000),
                                   static_cast<long long>(ago % 1000));
                }
                return;
        }

        int showStatus(CommandContext &ctx, Args args) {
                if (args.size() > 3) return ctx.usage();
                if (args.size() == 3) {
                        const uint8_t n = portArg(ctx, args[2]);
                        if (n == MidiRouter::NoPort) return Shell::ExitFailure;
                        auto [info, err] = Midi::router().port(n);
                        if (err.isError()) return Shell::ExitFailure;
                        showPort(ctx, info);
                        return Shell::ExitSuccess;
                }
                MidiRouter::PortInfo ports[MidiRouter::MaxPorts];
                const size_t count = Midi::router().ports(Span<MidiRouter::PortInfo>(ports, MidiRouter::MaxPorts));
                if (count == 0) ctx.printf("midi: no ports\n");
                for (size_t i = 0; i < count; ++i) showPort(ctx, ports[i]);
                return Shell::ExitSuccess;
        }

        int connect(CommandContext &ctx, Args args, bool make) {
                if (args.size() != 4) return ctx.usage();
                const uint8_t source = portArg(ctx, args[2]);
                const uint8_t destination = portArg(ctx, args[3]);
                if (source == MidiRouter::NoPort || destination == MidiRouter::NoPort) return Shell::ExitFailure;
                const Error err = make ? Midi::router().connect(source, destination)
                                       : Midi::router().disconnect(source, destination);
                if (err.isOk()) return Shell::ExitSuccess;
                if (err == Error::Invalid)
                        ctx.eprintf("midi: midi%u has no source, or midi%u no destination\n",
                                    static_cast<unsigned>(source), static_cast<unsigned>(destination));
                else
                        ctx.eprintf("midi: %s\n", err.desc());
                return Shell::ExitFailure;
        }

        // What a message is, for the monitor.
        void describe(const MidiEvent &e, char *out, size_t size) {
                const uint8_t s = e.data[0];
                const auto    ch = static_cast<unsigned>((s & 0x0f) + 1);
                const auto    d1 = static_cast<unsigned>(e.data[1]);
                const auto    d2 = static_cast<unsigned>(e.data[2]);
                out[0] = '\0';
                if (s < 0x80) return; // a SysEx's middle
                switch (s & 0xf0) {
                        case 0x80: std::snprintf(out, size, "note off   ch %u  %u  %u", ch, d1, d2); return;
                        case 0x90: std::snprintf(out, size, "note on    ch %u  %u  %u", ch, d1, d2); return;
                        case 0xa0: std::snprintf(out, size, "key press  ch %u  %u  %u", ch, d1, d2); return;
                        case 0xb0: std::snprintf(out, size, "control    ch %u  %u  %u", ch, d1, d2); return;
                        case 0xc0: std::snprintf(out, size, "program    ch %u  %u", ch, d1); return;
                        case 0xd0: std::snprintf(out, size, "pressure   ch %u  %u", ch, d1); return;
                        case 0xe0:
                                std::snprintf(out, size, "pitch bend ch %u  %d", ch,
                                              static_cast<int>(d1 | d2 << 7) - 8192);
                                return;
                        default: break;
                }
                switch (s) {
                        case 0xf0: std::snprintf(out, size, "sysex"); return;
                        case 0xf8: std::snprintf(out, size, "clock"); return;
                        case 0xfa: std::snprintf(out, size, "start"); return;
                        case 0xfb: std::snprintf(out, size, "continue"); return;
                        case 0xfc: std::snprintf(out, size, "stop"); return;
                        case 0xfe: std::snprintf(out, size, "active sensing"); return;
                        case 0xff: std::snprintf(out, size, "reset"); return;
                        default: return;
                }
        }

        // One line an event: its time (seconds since boot), the gap since the last, its port, bytes, meaning.
        int monitor(CommandContext &ctx, Args args) {
                if (args.size() != 3) return ctx.usage();
                const uint8_t n = portArg(ctx, args[2]);
                if (n == MidiRouter::NoPort) return Shell::ExitFailure;
                char path[16];
                std::snprintf(path, sizeof(path), "/dev/midi%u", static_cast<unsigned>(n));
                auto [handle, err] = Vfs::open(StringView(path), Vfs::Read, ctx.vfs());
                MidiFile *file = err.isOk() ? handle->stream().midiFile() : nullptr;
                if (file == nullptr) {
                        ctx.eprintf("midi: %s: %s\n", path, err.isError() ? err.desc() : "not a MIDI port");
                        return Shell::ExitFailure;
                }
                MidiFile::Config c;
                MidiFile::Config got;
                c.flags = MidiFile::Events;
                c.eventSize = sizeof(MidiEvent);
                err = file->configure(c, got);
                if (err.isError()) {
                        ctx.eprintf("midi: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("Monitoring midi%u; Ctrl+C stops\n", static_cast<unsigned>(n));
                int64_t last = 0;
                for (;;) {
                        MidiEvent  events[8];
                        const auto buf = WritableByteSpan(reinterpret_cast<uint8_t *>(events), sizeof(events));
                        auto [bytes, rerr] = handle->read(buf, WaitForever);
                        if (rerr == Error::Interrupted) return Shell::ExitInterrupted;
                        if (rerr == Error::EndOfFile) {
                                ctx.printf("midi%u is gone\n", static_cast<unsigned>(n));
                                return Shell::ExitSuccess;
                        }
                        if (rerr.isError()) {
                                ctx.eprintf("midi: %s\n", rerr.desc());
                                return Shell::ExitFailure;
                        }
                        for (size_t i = 0; i < bytes / sizeof(MidiEvent); ++i) {
                                const MidiEvent &e = events[i];
                                const int64_t    us = e.timeNs / 1000;
                                const int64_t    gap = last == 0 ? 0 : (e.timeNs - last) / 1000;
                                last = e.timeNs;
                                char hex[3 * MidiEvent::MaxBytes + 1] = "";
                                for (uint8_t b = 0; b < e.size; ++b)
                                        std::snprintf(hex + 3 * b, sizeof(hex) - 3 * b, "%02x ", e.data[b]);
                                char what[40];
                                describe(e, what, sizeof(what));
                                ctx.printf("%5lld.%06lld  +%8lld us  midi%-2u  %-15s %s\n",
                                           static_cast<long long>(us / 1000000), static_cast<long long>(us % 1000000),
                                           static_cast<long long>(gap), static_cast<unsigned>(e.port), hex, what);
                        }
                }
        }

        int send(CommandContext &ctx, Args args) {
                if (args.size() < 4) return ctx.usage();
                const uint8_t n = portArg(ctx, args[2]);
                if (n == MidiRouter::NoPort) return Shell::ExitFailure;
                uint8_t bytes[64];
                size_t  count = 0;
                for (size_t i = 3; i < args.size(); ++i) {
                        char      *end = nullptr;
                        const long v = std::strtol(args[i], &end, 16);
                        if (end == args[i] || *end != '\0' || v < 0 || v > 0xff || count == sizeof(bytes)) {
                                ctx.eprintf("midi: %s: not a byte in hex\n", args[i]);
                                return Shell::ExitFailure;
                        }
                        bytes[count++] = static_cast<uint8_t>(v);
                }
                char path[16];
                std::snprintf(path, sizeof(path), "/dev/midi%u", static_cast<unsigned>(n));
                auto [handle, err] = Vfs::open(StringView(path), Vfs::Write, ctx.vfs());
                if (err.isError()) {
                        ctx.eprintf("midi: %s: %s\n", path, err == Error::NotSupported ? "no destination" : err.desc());
                        return Shell::ExitFailure;
                }
                auto [written, werr] = handle->write(ByteSpan(bytes, count), WaitForever);
                (void)written;
                if (werr.isError()) {
                        ctx.eprintf("midi: %s: %s\n", path, werr.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int runMidi(CommandContext &ctx) {
                const Args args = ctx.args();
                if (!Midi::isReady()) {
                        ctx.eprintf("midi: not started\n");
                        return Shell::ExitFailure;
                }
                Midi::applySettings();
                if (args.size() == 1) return listPorts(ctx);
                const char *sub = args[1];
                if (std::strcmp(sub, "status") == 0) return showStatus(ctx, args);
                if (std::strcmp(sub, "connect") == 0) return connect(ctx, args, true);
                if (std::strcmp(sub, "disconnect") == 0) return connect(ctx, args, false);
                if (std::strcmp(sub, "monitor") == 0) return monitor(ctx, args);
                if (std::strcmp(sub, "send") == 0) return send(ctx, args);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(midi, runMidi,
                   "[status [PORT] | connect SRC DST | disconnect SRC DST | monitor PORT | send PORT HEX...]",
                   "MIDI ports and connections; a port's events as they come; bytes sent to a port")

BMDS_NAMESPACE_END
