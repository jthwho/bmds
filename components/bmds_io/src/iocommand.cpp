/**
 * @file      iocommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `io` command (devplan/io.md §3.11): every pad's wiring, holder, routing
 * and level; the peripheral instances' holders; the matrix's signals. It
 * shows and reads; settings are sysconf's. Its forms and output are a
 * userspace interface: append only (new columns at the end of a line, new
 * lines at the end of a block).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/io.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/spidevice.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // The input signals' sources (Io::readInputSources()): read once per command.
        constexpr size_t MaxInputs = 256;

        struct Sources {
                        int16_t source[MaxInputs] = {};
                        size_t  count = 0;

                        void read() {
                                count = ioChip().inputSignals.size();
                                if (count > MaxInputs) count = MaxInputs;
                                Io::readInputSources(Span<int16_t>(source, count));
                                return;
                        }
        };

        // A pad number: `14`, `GPIO14`, `gpio14`.
        bool parsePad(const char *text, unsigned &out) {
                auto [r, err] = ioChip().findResource(text);
                if (err.isOk() && r < ioChip().padCount()) {
                        out = r;
                        return true;
                }
                unsigned n = 0;
                if (*text == '\0') return false;
                for (const char *p = text; *p != '\0'; ++p) {
                        if (*p < '0' || *p > '9' || n > 1000) return false;
                        n = n * 10 + static_cast<unsigned>(*p - '0');
                }
                if (n >= ioChip().padCount()) return false;
                out = n;
                return true;
        }

        int showTable(CommandContext &ctx) {
                const size_t padCount = ioChip().padCount();
                // In PSRAM: every pad's row (~140 bytes each) is more than a shell's stack wants.
                auto *rows = static_cast<IoPinRow *>(Memory::alloc(MemSpace::Psram, padCount * sizeof(IoPinRow)));
                if (rows == nullptr) {
                        ctx.eprintf("io: out of memory\n");
                        return Shell::ExitFailure;
                }
                for (size_t i = 0; i < padCount; ++i) ::new (&rows[i]) IoPinRow();
                const size_t n = Io::readPins(0, Span<IoPinRow>(rows, padCount));
                ctx.printf("%-6s %-3s %-6s %-8s %-26s %-22s %-9s %-7s %s\n", "GPIO", "LVL", "CLASS", "OWNER",
                           "FUNCTION", "ROUTE", "DIR", "PULL", "WHERE");
                int status = Shell::ExitSuccess;
                for (size_t i = 0; i < n; ++i) {
                        if (ctx.isInterrupted()) {
                                status = Shell::ExitInterrupted;
                                break;
                        }
                        const IoPinRow &r = rows[i];
                        ctx.printf("%-6s %-3c %-6s %-8s %-26.26s %-22.22s %-9s %-7s %s\n", r.name, r.level,
                                   ioClassName(r.resourceClass), r.owner[0] != '\0' ? r.owner : "-",
                                   r.function[0] != '\0' ? r.function : "-", r.route, r.direction, r.pull,
                                   r.where[0] != '\0' ? r.where : "-");
                }
                Memory::free(rows);
                return status;
        }

        int showPin(CommandContext &ctx, unsigned pad) {
                // In PSRAM: the details are ~1.8 KB.
                auto *d = static_cast<IoPinDetail *>(Memory::alloc(MemSpace::Psram, sizeof(IoPinDetail)));
                if (d == nullptr) {
                        ctx.eprintf("io: out of memory\n");
                        return Shell::ExitFailure;
                }
                ::new (d) IoPinDetail();
                const Error err = Io::readPin(pad, *d);
                if (err.isOk()) {
                        ctx.printf("%s\n", d->name);
                        for (size_t f = 0; f < IoPinFieldCount; ++f) {
                                const auto field = static_cast<IoPinField>(f);
                                // Note, Forced and Edges are shown only when there is one.
                                if (ioPinFieldIsOptional(field) && d->at(field)[0] == '\0') continue;
                                char label[16];
                                std::snprintf(label, sizeof(label), "%s:", ioPinFieldName(field));
                                ctx.printf("  %-10s %s\n", label, d->at(field));
                        }
                } else {
                        ctx.eprintf("io: GPIO%u: %s\n", pad, err.name());
                }
                Memory::free(d);
                return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int showResources(CommandContext &ctx) {
                const IoChip &chip = ioChip();
                ctx.printf("%-16s %-6s %-8s %s\n", "RESOURCE", "CLASS", "OWNER", "ROLE");
                for (size_t r = chip.padCount(); r < chip.resourceCount(); ++r) {
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        char name[24];
                        (void)chip.resourceName(static_cast<IoResource>(r), name, sizeof(name));
                        IoHolder h;
                        (void)Io::registry().holder(static_cast<IoResource>(r), h);
                        ctx.printf("%-16s %-6s %-8s %s\n", name, ioClassName(h.resourceClass),
                                   h.isHeld() ? h.owner : "-", h.role[0] != '\0' ? h.role : "-");
                }
                return Shell::ExitSuccess;
        }

        int showSignals(CommandContext &ctx, const char *group) {
                const IoChip &chip = ioChip();
                if (group == nullptr) {
                        ctx.printf("Groups:");
                        for (const char *g : chip.groups) ctx.printf(" %s", g);
                        ctx.printf("\n");
                        return Shell::ExitSuccess;
                }
                size_t groupIndex = chip.groups.size();
                for (size_t i = 0; i < chip.groups.size(); ++i) {
                        if (StringView(group).equalsIgnoreCase(StringView(chip.groups[i]))) groupIndex = i;
                }
                if (groupIndex == chip.groups.size()) {
                        ctx.eprintf("io: no signal group '%s' ('io signals' lists them)\n", group);
                        return Shell::ExitFailure;
                }
                Sources sources;
                sources.read();
                // What each pad outputs, read once.
                constexpr size_t MaxPads = 64;
                uint16_t         outputs[MaxPads];
                const size_t     padCount = chip.padCount() < MaxPads ? chip.padCount() : MaxPads;
                for (unsigned pad = 0; pad < padCount; ++pad) {
                        IoPadState s;
                        (void)Io::readPad(pad, s);
                        outputs[pad] = s.function == chip.gpioFunction ? s.outputSignal : chip.gpioOutputSignal;
                }
                for (size_t i = 0; i < chip.inputSignals.size(); ++i) {
                        if (chip.inputSignals[i] == nullptr || chip.inputGroups[i] != groupIndex) continue;
                        const int src = i < sources.count ? sources.source[i] : -1;
                        if (src == chip.constZeroInput)
                                ctx.printf("in  %3u %-24s <- 0\n", static_cast<unsigned>(i), chip.inputSignals[i]);
                        else if (src == chip.constOneInput)
                                ctx.printf("in  %3u %-24s <- 1\n", static_cast<unsigned>(i), chip.inputSignals[i]);
                        else if (src >= 0)
                                ctx.printf("in  %3u %-24s <- GPIO%d\n", static_cast<unsigned>(i), chip.inputSignals[i],
                                           src);
                        else
                                ctx.printf("in  %3u %-24s -\n", static_cast<unsigned>(i), chip.inputSignals[i]);
                }
                for (size_t i = 0; i < chip.outputSignals.size(); ++i) {
                        if (chip.outputSignals[i] == nullptr || chip.outputGroups[i] != groupIndex) continue;
                        ctx.printf("out %3u %-24s ->", static_cast<unsigned>(i), chip.outputSignals[i]);
                        size_t n = 0;
                        for (unsigned pad = 0; pad < padCount; ++pad) {
                                if (outputs[pad] == i && i != chip.gpioOutputSignal) {
                                        ctx.printf(" GPIO%u", pad);
                                        ++n;
                                }
                        }
                        ctx.printf("%s\n", n == 0 ? " -" : "");
                }
                return Shell::ExitSuccess;
        }

        // ESP-IDF's own view of the pads (gpio_dump_io_configuration()), read by its code rather
        // than ours: what `io`'s readings are checked against (devplan/io.md §8). One pad at a time
        // into a small buffer; its banner lines left out.
        int dumpPads(CommandContext &ctx, unsigned first, unsigned last) {
                for (unsigned pad = first; pad <= last; ++pad) {
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        char  text[512] = {};
                        FILE *f = fmemopen(text, sizeof(text) - 1, "w");
                        if (f == nullptr) {
                                ctx.eprintf("io: no stream for the dump\n");
                                return Shell::ExitFailure;
                        }
                        const esp_err_t err = gpio_dump_io_configuration(f, 1ull << pad);
                        std::fclose(f);
                        if (err != ESP_OK) {
                                ctx.eprintf("io: GPIO%u: %s\n", pad, esp_err_to_name(err));
                                return Shell::ExitFailure;
                        }
                        for (char *line = text; *line != '\0';) {
                                char *end = std::strchr(line, '\n');
                                if (end != nullptr) *end = '\0';
                                if (line[0] != '=' && line[0] != '\0') ctx.printf("%s\n", line);
                                if (end == nullptr) break;
                                line = end + 1;
                        }
                }
                return Shell::ExitSuccess;
        }

        // A pad's edges (devplan/io.md §10.5): what its ring has, the time before now and the way;
        // with @p follow, each as it comes until interrupted.
        int showEdges(CommandContext &ctx, unsigned pad, bool follow) {
                Io::EdgeInfo info;
                if (Io::edgeInfo(pad, info).isError()) {
                        ctx.eprintf("io: GPIO%u's edges aren't watched: add edges (or rising, falling) to its io/gpio "
                                    "entry\n",
                                    pad);
                        return Shell::ExitFailure;
                }
                constexpr size_t Batch = 16;
                Io::Edge         edges[Batch];
                uint32_t         after = 0;
                const int64_t    start = Clock::monotonicNs();
                for (;;) {
                        uint32_t next = 0, lost = 0;
                        auto [n, err] = Io::readEdges(pad, after, edges, next, lost);
                        if (err.isError()) {
                                ctx.eprintf("io: GPIO%u isn't watched any more\n", pad);
                                return Shell::ExitFailure;
                        }
                        if (lost != 0) ctx.printf("  (%u lost)\n", unsigned(lost));
                        const int64_t now = Clock::monotonicNs();
                        for (size_t i = 0; i < n; ++i) {
                                // Following: the time since it began; else: how long ago.
                                const int64_t t = follow ? edges[i].timeNs - start : edges[i].timeNs - now;
                                ctx.printf("%6u %+12.6f s  %s\n", unsigned(edges[i].seq), double(t) / 1e9,
                                           edges[i].rising ? "rising" : "falling");
                        }
                        after = next;
                        if (n == Batch) continue; // more waiting
                        if (!follow) break;
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        Thread::sleep(20);
                }
                if (after == 0) ctx.printf("no edges yet\n");
                return Shell::ExitSuccess;
        }

        // io spi B.C HEX...: one full-duplex transfer at /dev/spidevB.C, the bytes read printed.
        int spiTransfer(CommandContext &ctx, Span<char *const> args) {
                constexpr size_t MaxBytes = 256;
                unsigned         bus = 0, index = 0;
                char             tail = 0;
                if (std::sscanf(args[2], "%u.%u%c", &bus, &index, &tail) != 2) return ctx.usage();
                SpiDevice *d = SpiDevice::find(bus, index);
                if (d == nullptr || !d->isRunning()) {
                        ctx.eprintf("io: no /dev/spidev%u.%u (io/spi%u sets an SPI port up)\n", bus, index, bus);
                        return Shell::ExitFailure;
                }
                uint8_t out[MaxBytes], in[MaxBytes];
                size_t  n = 0;
                for (size_t a = 3; a < args.size(); ++a) {
                        const char  *hex = args[a];
                        const size_t length = std::strlen(hex);
                        if (length == 0 || length % 2 != 0) {
                                ctx.eprintf("io: '%s' isn't bytes in hex (two digits each)\n", hex);
                                return Shell::ExitFailure;
                        }
                        for (size_t i = 0; i < length; i += 2) {
                                const char    pair[3] = {hex[i], hex[i + 1], '\0'};
                                char         *end = nullptr;
                                unsigned long v = std::strtoul(pair, &end, 16);
                                if (end != pair + 2) {
                                        ctx.eprintf("io: '%s' isn't bytes in hex (two digits each)\n", hex);
                                        return Shell::ExitFailure;
                                }
                                if (n == MaxBytes) {
                                        ctx.eprintf("io: %u bytes at most\n", unsigned(MaxBytes));
                                        return Shell::ExitFailure;
                                }
                                out[n++] = uint8_t(v);
                        }
                }
                SpiDevice::Settings settings;
                if (d->settings(settings, nullptr).isOk() && (settings.mode & SpiDevice::ThreeWire) != 0) {
                        ctx.eprintf(
                                "io: spidev%u.%u is 3-wire: data goes one way at a time (write to it, or read it)\n",
                                bus, index);
                        return Shell::ExitFailure;
                }
                SpiDevice::Transfer t;
                t.tx = out;
                t.rx = in;
                t.length = uint32_t(n);
                auto [moved, err] = d->transfer(Span<const SpiDevice::Transfer>(&t, 1));
                if (err.isError()) {
                        ctx.eprintf("io: spidev%u.%u: %s\n", bus, index, err.desc());
                        return Shell::ExitFailure;
                }
                for (size_t i = 0; i < moved; ++i)
                        ctx.printf("%02x%s", in[i], (i + 1) % 16 == 0 || i + 1 == moved ? "\n" : " ");
                return Shell::ExitSuccess;
        }

        int runIo(CommandContext &ctx) {
                const Args args = ctx.args();
                if (!Io::isReady()) {
                        ctx.eprintf("io: not started\n");
                        return Shell::ExitFailure;
                }
                if (args.size() == 1) return showTable(ctx);
                const StringView verb(args[1]);
                unsigned         pad = 0;
                if (verb == "pin" && args.size() == 3) {
                        if (!parsePad(args[2], pad)) {
                                ctx.eprintf("io: no pin '%s'\n", args[2]);
                                return Shell::ExitFailure;
                        }
                        return showPin(ctx, pad);
                }
                if (verb == "get" && args.size() == 3) {
                        if (!parsePad(args[2], pad)) {
                                ctx.eprintf("io: no pin '%s'\n", args[2]);
                                return Shell::ExitFailure;
                        }
                        IoPadState s;
                        (void)Io::readPad(pad, s);
                        const char level = ioLevelChar(s);
                        if (level == '-' || level == '?') {
                                ctx.eprintf("io: GPIO%u has no level to read (%s)\n", pad,
                                            s.lpIo ? "the LP IO domain has it" : "its input and output are off");
                                return Shell::ExitFailure;
                        }
                        ctx.printf("%c\n", level);
                        return Shell::ExitSuccess;
                }
                if (verb == "edges" && (args.size() == 3 || (args.size() == 4 && std::strcmp(args[3], "-f") == 0))) {
                        if (!parsePad(args[2], pad)) {
                                ctx.eprintf("io: no pin '%s'\n", args[2]);
                                return Shell::ExitFailure;
                        }
                        return showEdges(ctx, pad, args.size() == 4);
                }
                if (verb == "pulse" && args.size() >= 3 && args.size() <= 5) {
                        // io pulse N [MS] [--force]
                        bool        force = false;
                        const char *msText = nullptr;
                        for (size_t i = 3; i < args.size(); ++i) {
                                if (std::strcmp(args[i], "--force") == 0)
                                        force = true;
                                else if (msText == nullptr)
                                        msText = args[i];
                                else
                                        return ctx.usage();
                        }
                        if (!parsePad(args[2], pad)) {
                                ctx.eprintf("io: no pin '%s'\n", args[2]);
                                return Shell::ExitFailure;
                        }
                        unsigned ms = 10;
                        if (msText != nullptr) {
                                char         *end = nullptr;
                                unsigned long v = std::strtoul(msText, &end, 10);
                                if (end == msText || *end != '\0' || v == 0 || v > Io::MaxPulseMs) {
                                        ctx.eprintf("io: a pulse is 1 to %u ms, not '%s'\n", Io::MaxPulseMs, msText);
                                        return Shell::ExitFailure;
                                }
                                ms = unsigned(v);
                        }
                        char        why[256];
                        const Error err = Io::pulse(pad, ms, force, why, sizeof(why));
                        if (err.isError()) {
                                ctx.eprintf("io: %s\n", why[0] != '\0' ? why : err.desc());
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                if (verb == "dump" && args.size() <= 3) {
                        if (args.size() == 2) return dumpPads(ctx, 0, static_cast<unsigned>(ioChip().padCount() - 1));
                        if (!parsePad(args[2], pad)) {
                                ctx.eprintf("io: no pin '%s'\n", args[2]);
                                return Shell::ExitFailure;
                        }
                        return dumpPads(ctx, pad, pad);
                }
                if (verb == "spi" && args.size() >= 4) return spiTransfer(ctx, args);
                if (verb == "resources" && args.size() == 2) return showResources(ctx);
                if (verb == "signals" && args.size() <= 3)
                        return showSignals(ctx, args.size() == 3 ? args[2] : nullptr);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(
        io, runIo,
        "[pin N | get N | edges N [-f] | pulse N [MS] [--force] | spi B.C HEX... | resources | signals [GROUP] | "
        "dump [N]]",
        "Show the chip's pins: wiring, holder, routing, level (devplan/io.md)")

BMDS_NAMESPACE_END
