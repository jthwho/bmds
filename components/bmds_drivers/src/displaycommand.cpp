/**
 * @file      displaycommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `display` command: the HDMI bridge, the monitor, and surface 0
 * (devplan/display.md). Its output format is a userspace interface once
 * shipped: keep the lines and their order, add new ones only at the end.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/edid.h>
#include <bmds/i2cbus.h>
#include <bmds/lt8912b.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/sysconf.h>
#include <bmds/task.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace detail {
        Error drawTestPicture(Surface &surface);   // below; benchdisplay.cpp's too
        int runDisplayPpa(CommandContext &ctx);  // displayppa.cpp
        int runDisplayRows(CommandContext &ctx); // displayppa.cpp
        int runDisplayDsi(CommandContext &ctx, Span<char *const> args);  // displayregs.cpp
        int runDisplayChip(CommandContext &ctx, Span<char *const> args); // displayregs.cpp
} // namespace detail

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int TimeoutMs = 100;

        // Modes by name.
        struct TestMode {
                        const char *name;
                        VideoTiming timing;
        };
        constexpr TestMode TestModes[] = {
                // Espressif's timings: pixel clocks the DPI clock divides from its PLL.
                {"720", {80'000, 1280, 408, 32, 80, 720, 3, 5, 13, true, false, false}},
                {"1080", {70'000, 1920, 48, 32, 80, 1080, 3, 5, 8, true, false, false}},
        };

        // The mode @p text names: a name of TestModes, or WIDTHxHEIGHT@HZ.
        Result<VideoTiming> modeOf(StringView text) {
                for (const TestMode &m : TestModes)
                        if (text == StringView(m.name)) return makeResult(m.timing);
                return VideoModes::parse(text);
        }

        Error checkMode(StringView value) {
                return modeOf(value).error();
        }

        // What `display start` shows (devplan/sysconf.md §13).
        BMDS_SYSCONF(displayMode, "display/mode", Config::Type::String, "720",
                     "The mode 'display start' sets: 720, 1080, or WIDTHxHEIGHT@HZ", 0, 32, nullptr, checkMode)
        BMDS_SYSCONF(displayBuffering, "display/buffering", Config::Type::Enum, "double",
                     "Surface 0's buffers: double (no tearing) or single (half the memory)", 0, 0, "single|double",
                     nullptr)
        BMDS_SYSCONF(
                displayLaneMbps, "display/lanembps", Config::Type::Int, "0",
                "The DSI lanes' rate in Mbps for 'display start' (0: a margin over the mode's need; at least the need)",
                0, 1500, nullptr, nullptr)

        // The board's bridge, attached on first use.
        Result<Lt8912b *> attachBridge() {
                auto [d, err] = Display::display(0);
                if (err.isError()) return makeError<Lt8912b *>(err);
                return makeResult(&d->bridge());
        }
        // True, with a message, while another user (the desktop) has the display.
        bool isClaimed(CommandContext &ctx) {
                auto [d, err] = Display::display(0);
                if (err.isError() || d->owner() == nullptr) return false;
                ctx.eprintf("display: in use by the %s\n", d->owner());
                return true;
        }

        // The bridge, or null with a message.
        Lt8912b *bridge(CommandContext &ctx) {
                auto [b, err] = attachBridge();
                if (err == Error::NotSupported) {
                        ctx.eprintf("display: this board has no HDMI bridge\n");
                } else if (err.isError()) {
                        ctx.eprintf("display: bridge: %s\n", err.desc());
                }
                return err.isOk() ? b : nullptr;
        }
        // EDID block @p n (0 or 1) from the monitor, over the bridge's bus.
        Error readEdid(I2cBus &bus, uint8_t n, uint8_t (&block)[Edid::BlockBytes]) {
                return Display::readEdidBlock(bus, n, block);
        }
        void printTiming(CommandContext &ctx, const VideoTiming &t, const char *note) {
                const uint32_t mhz = t.refreshMilliHz();
                // Interlaced: the frame's lines, the field rate.
                ctx.printf("Mode:     %ux%u%s at %u.%03u Hz, %u.%03u MHz (%s)\n", t.hActive,
                           t.interlaced ? t.vActive * 2 : t.vActive, t.interlaced ? "i" : "",
                           static_cast<unsigned>(mhz / 1000), static_cast<unsigned>(mhz % 1000),
                           static_cast<unsigned>(t.pixelKhz / 1000), static_cast<unsigned>(t.pixelKhz % 1000), note);
                ctx.printf("Timing:   h %u %u %u %u %c, v %u %u %u %u %c\n", t.hActive, t.hFront, t.hSync, t.hBack,
                           t.hSyncHigh ? '+' : '-', t.vActive, t.vFront, t.vSync, t.vBack, t.vSyncHigh ? '+' : '-');
                return;
        }
        int showStatus(CommandContext &ctx) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                auto [id, err] = b->chipId();
                if (err.isError()) {
                        ctx.eprintf("display: bridge at i2c%u 0x%02x: %s\n", static_cast<unsigned>(b->bus().number()),
                                    b->address(), err.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("Bridge:   LT8912B on i2c%u at 0x%02x, id 0x%04x%s\n",
                           static_cast<unsigned>(b->bus().number()), b->address(), id,
                           id == Lt8912b::ChipId ? "" : " (unexpected)");
                auto [connected, cerr] = b->isMonitorConnected();
                if (cerr.isError()) {
                        ctx.eprintf("display: %s\n", cerr.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("Monitor:  %s\n", connected ? "connected" : "not connected");
                if (!connected) return Shell::ExitSuccess;
                uint8_t block[Edid::BlockBytes];
                Error   rerr = readEdid(b->bus(), 0, block);
                auto [edid, perr] = rerr.isOk() ? Edid::parse(ByteSpan(block, sizeof(block))) : makeError<Edid>(rerr);
                if (perr.isError()) {
                        ctx.printf("EDID:     %s\n", perr.desc());
                        return Shell::ExitSuccess;
                }
                if (edid.extensions > 0 && readEdid(b->bus(), 1, block).isOk())
                        (void)edid.parseExtension(ByteSpan(block, sizeof(block)));
                ctx.printf("Name:     %s (%s, product 0x%04x, EDID %u.%u, %s)\n", edid.name[0] ? edid.name : "-",
                           edid.maker, edid.product, edid.version, edid.revision, edid.hdmi ? "HDMI" : "DVI");
                if (edid.preferred.pixelKhz != 0) printTiming(ctx, edid.preferred, "preferred");
                return Shell::ExitSuccess;
        }
        // The raw EDID: the base block and the first extension.
        // A list of modes, a few to a line under @p label.
        class ModeList {
                public:
                        ModeList(CommandContext &ctx, const char *label) : _ctx(ctx), _label(label) {}
                        ~ModeList() {
                                if (_count % PerLine != 0) _ctx.printf("\n");
                                return;
                        }
                        void add(const char *text) {
                                if (_count % PerLine == 0) _ctx.printf("%-10s", _count == 0 ? _label : "");
                                _ctx.printf(" %-19s", text);
                                if (++_count % PerLine == 0) _ctx.printf("\n");
                                return;
                        }
                        void add(const Edid::Mode &m, const char *suffix = "") {
                                char text[32];
                                std::snprintf(text, sizeof(text), "%ux%u%s@%u%s", m.width, m.height,
                                              m.interlaced ? "i" : "", m.hz, suffix);
                                add(text);
                                return;
                        }

                private:
                        static constexpr int PerLine = 4;
                        CommandContext      &_ctx;
                        const char          *_label;
                        int                  _count = 0;
        };

        // display edid [raw]: the monitor's EDID decoded, or its bytes.
        int showEdid(CommandContext &ctx, bool raw) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                uint8_t blocks[2][Edid::BlockBytes];
                uint8_t count = 0;
                for (uint8_t n = 0; n < 2; ++n) {
                        const Error err = readEdid(b->bus(), n, blocks[n]);
                        if (err.isError()) {
                                if (n == 0) {
                                        ctx.eprintf("display: EDID: %s\n",
                                                    err == Error::IoError ? "no answer (no monitor?)" : err.desc());
                                        return Shell::ExitFailure;
                                }
                                break;
                        }
                        ++count;
                        if (n == 0 && blocks[0][126] == 0) break;
                }
                if (raw) {
                        for (uint8_t n = 0; n < count; ++n) {
                                for (size_t i = 0; i < Edid::BlockBytes; i += 16) {
                                        ctx.printf("%03x:", static_cast<unsigned>(n * Edid::BlockBytes + i));
                                        for (size_t j = 0; j < 16; ++j) ctx.printf(" %02x", blocks[n][i + j]);
                                        ctx.printf("\n");
                                }
                        }
                        return Shell::ExitSuccess;
                }
                auto [e, perr] = Edid::parse(ByteSpan(blocks[0], Edid::BlockBytes));
                if (perr.isError()) {
                        ctx.eprintf("display: EDID: %s ('display edid raw' shows the bytes)\n", perr.desc());
                        return Shell::ExitFailure;
                }
                if (count > 1) (void)e.parseExtension(ByteSpan(blocks[1], Edid::BlockBytes));
                ctx.printf("Monitor:  %s (%s, product 0x%04x%s%s), EDID %u.%u\n", e.name[0] ? e.name : "-", e.maker,
                           e.product, e.serial[0] ? ", serial " : "", e.serial, e.version, e.revision);
                if (e.week == 255)
                        ctx.printf("Made:     model year %u\n", e.year);
                else if (e.week != 0)
                        ctx.printf("Made:     week %u of %u\n", e.week, e.year);
                else
                        ctx.printf("Made:     %u\n", e.year);
                ctx.printf("Screen:   %u x %u cm, %s input, %s\n", e.widthCm, e.heightCm,
                           e.digital ? "digital" : "analog", e.hdmi ? "HDMI" : "DVI");
                if (e.range.present) {
                        ctx.printf("Ranges:   %u-%u Hz vertical, %u-%u kHz horizontal", e.range.minHz, e.range.maxHz,
                                   e.range.minKhz, e.range.maxKhz);
                        if (e.range.maxPixelKhz != 0)
                                ctx.printf(", pixel clock up to %u MHz",
                                           static_cast<unsigned>(e.range.maxPixelKhz / 1000));
                        ctx.printf("\n");
                } else {
                        ctx.printf("Ranges:   not stated\n");
                }
                for (uint8_t i = 0; i < e.detailedCount; ++i)
                        printTiming(ctx, e.detailed[i], i == 0 ? "preferred" : "detailed");
                for (uint8_t i = 0; i < e.extDetailedCount; ++i) printTiming(ctx, e.extDetailed[i], "CEA detailed");
                {
                        ModeList list(ctx, "Standard:");
                        for (uint8_t i = 0; i < e.standardCount; ++i) list.add(e.standard[i]);
                }
                {
                        ModeList list(ctx, "Older:");
                        for (int bit = 23; bit >= 7; --bit) {
                                Edid::Mode m;
                                if ((e.established >> bit & 1) != 0 && Edid::establishedMode(bit, m)) list.add(m);
                        }
                }
                {
                        ModeList list(ctx, "CEA:");
                        for (uint8_t i = 0; i < e.videoModeCount; ++i) {
                                Edid::Mode m;
                                uint8_t    vic = 0;
                                bool       native = false;
                                if (Edid::videoMode(e.videoModes[i], m, vic, native)) {
                                        list.add(m, native ? " native" : "");
                                } else {
                                        char text[16];
                                        std::snprintf(text, sizeof(text), "VIC %u", vic);
                                        list.add(text);
                                }
                        }
                }
                return Shell::ExitSuccess;
        }

        // Every address that answers on the bridge's bus.
        int scanBus(CommandContext &ctx) {
                const BoardDisplay &d = board().display;
                I2cBus             *bus = I2cBus::bus(static_cast<size_t>(d.i2c));
                if (bus == nullptr) {
                        ctx.eprintf("display: no I2C bus\n");
                        return Shell::ExitFailure;
                }
                ctx.printf("i2c%u:", static_cast<unsigned>(bus->number()));
                for (uint8_t a = 0x08; a < 0x78; ++a) {
                        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                        if (bus->probe(a, TimeoutMs).isOk()) ctx.printf(" 0x%02x", a);
                }
                ctx.printf("\n");
                return Shell::ExitSuccess;
        }

        // Display 0, or null with a message.
        Display *display0(CommandContext &ctx) {
                auto [d, err] = Display::display(0);
                if (err == Error::NotSupported) {
                        ctx.eprintf("display: this board has no HDMI bridge\n");
                } else if (err.isError()) {
                        ctx.eprintf("display: %s\n", err.desc());
                }
                return err.isOk() ? d : nullptr;
        }

        // Display 0 if it is showing, or null with a message.
        Display *running(CommandContext &ctx) {
                Display *d = display0(ctx);
                if (d != nullptr && !d->isRunning()) {
                        ctx.eprintf("display: not started ('display start')\n");
                        return nullptr;
                }
                return d;
        }

        // display start: in the mode and buffering of display/mode and display/buffering.
        int runStart(CommandContext &ctx, Args args) {
                if (args.size() != 2) return ctx.usage();
                Display *d = display0(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                String text;
                if (SysConf::get(bmdsConf_displayMode.key, text).isError())
                        (void)text.assign(bmdsConf_displayMode.defaultValue);
                auto [mode, merr] = modeOf(text.view());
                if (merr.isError()) {
                        ctx.eprintf("display: display/mode %s: not a mode\n", text.cStr());
                        return Shell::ExitFailure;
                }
                const Surface::Buffering buffering =
                        SysConf::get(bmdsConf_displayBuffering.key, text).isOk() && text.view() == "single"
                                ? Surface::Buffering::Single
                                : Surface::Buffering::Double;
                const Result<int32_t> laneMbps = SysConf::getInt(StringView(bmdsConf_displayLaneMbps.key));
                const Error err =
                        d->start(mode, buffering, laneMbps.isOk() ? static_cast<uint32_t>(laneMbps.value()) : 0);
                if (err.isError()) {
                        ctx.eprintf("display: start: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const VideoTiming &t = d->timing();
                const uint32_t     mhz = t.refreshMilliHz();
                ctx.printf("Display:  %ux%u at %u.%03u Hz, %u.%03u MHz, surface 0 %s-buffered (RGB888, PSRAM)\n",
                           t.hActive, t.vActive, static_cast<unsigned>(mhz / 1000), static_cast<unsigned>(mhz % 1000),
                           static_cast<unsigned>(t.pixelKhz / 1000), static_cast<unsigned>(t.pixelKhz % 1000),
                           buffering == Surface::Buffering::Double ? "double" : "single");
                return Shell::ExitSuccess;
        }

        int runOff(CommandContext &ctx) {
                Display *d = display0(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                d->stop();
                (void)d->bridge().setOutput(false);
                return Shell::ExitSuccess;
        }

        int runStats(CommandContext &ctx) {
                Display *d = running(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                const Scanout::Stats s = d->scanout().stats();
                const Surface       &surface = d->surface();
                ctx.printf("Scanout:  %u frames, %u blocks (%u a frame), %u swaps, %u pictures switched to (%u with "
                           "the link up)\n",
                           static_cast<unsigned>(s.frames), static_cast<unsigned>(s.blocks),
                           static_cast<unsigned>(s.blocksPerFrame), static_cast<unsigned>(s.swaps),
                           static_cast<unsigned>(s.switches), static_cast<unsigned>(s.restarts));
                ctx.printf("Problems: %u underruns, %u stalls, %u resyncs, %u rebuilds (%u failed)\n",
                           static_cast<unsigned>(s.underruns), static_cast<unsigned>(s.stalls),
                           static_cast<unsigned>(s.resyncs), static_cast<unsigned>(s.rebuilds),
                           static_cast<unsigned>(s.failures));
                ctx.printf("Surface:  %ux%u %s-buffered, %u presents shown\n", surface.width(), surface.height(),
                           surface.buffering() == Surface::Buffering::Double ? "double" : "single",
                           static_cast<unsigned>(surface.shownPresents()));
                const Surface::Stats ss = surface.stats();
                if (ss.presents > 0)
                        ctx.printf("Swaps:    %u presents; copied forward %u rectangles, %llu pixels, %llu us (%u at "
                                   "most); waited %llu us (%u at most)\n",
                                   static_cast<unsigned>(ss.presents), static_cast<unsigned>(ss.forwards),
                                   static_cast<unsigned long long>(ss.forwardPixels),
                                   static_cast<unsigned long long>(ss.forwardUs),
                                   static_cast<unsigned>(ss.forwardUsMax), static_cast<unsigned long long>(ss.waitUs),
                                   static_cast<unsigned>(ss.waitUsMax));
                ctx.printf("Buffer:   shown at %p, %u bytes\n", static_cast<const void *>(surface.shown()),
                           static_cast<unsigned>(surface.stride() * surface.height()));
                ctx.printf("Detail:   block interrupt %u cycles at most; DMA on block %u; bridge FIFO %u\n",
                           static_cast<unsigned>(s.isrCyclesMax), static_cast<unsigned>(s.dmaItem),
                           static_cast<unsigned>(s.bridgeFifo));
                const Scanout::Config &c = d->scanout().config();
                ctx.printf("Link:     %u lanes at %u Mbps, clock %s, frame ack %s; errors in %u frames (INT_ST0 "
                           "0x%08x, INT_ST1 0x%08x)\n",
                           static_cast<unsigned>(d->scanout().lanes()), static_cast<unsigned>(s.laneMbps),
                           c.continuousClock ? "hs" : "auto", c.frameAck ? "on" : "off",
                           static_cast<unsigned>(s.linkErrorFrames), static_cast<unsigned>(s.linkErrors0),
                           static_cast<unsigned>(s.linkErrors1));
                return Shell::ExitSuccess;
        }

        // `display link [clock=auto|hs] [ack=on|off] [mbps=N]`: the DSI link's options, at once
        // (the lane rate by building the link anew: the monitor resyncs).
        int runLink(CommandContext &ctx, Args args) {
                Display *d = running(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                const Scanout::Config &c = d->scanout().config();
                bool                   clock = c.continuousClock, ack = c.frameAck, rate = false;
                uint32_t               mbps = c.laneMbps;
                for (size_t i = 2; i < args.size(); ++i) {
                        const StringView a(args[i]);
                        if (a == "clock=auto" || a == "clock=hs") {
                                clock = a == "clock=hs";
                        } else if (a == "ack=on" || a == "ack=off") {
                                ack = a == "ack=on";
                        } else if (a.size() > 5 && std::strncmp(args[i], "mbps=", 5) == 0) {
                                mbps = static_cast<uint32_t>(std::strtoul(args[i] + 5, nullptr, 0));
                                rate = true;
                                if (mbps > 1500) return ctx.usage();
                        } else {
                                return ctx.usage();
                        }
                }
                d->scanout().setLinkOptions(clock, ack);
                if (rate) {
                        if (d->scanout().surface() != &d->surface()) {
                                ctx.eprintf("display: another picture is shown (a full screen)\n");
                                return Shell::ExitFailure;
                        }
                        d->scanout().setLaneMbps(mbps);
                        const Error err = d->showDesktop(1000);
                        if (err.isError()) {
                                ctx.eprintf("display: link: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                }
                ctx.printf("Link:     %u Mbps, clock %s, frame ack %s\n",
                           static_cast<unsigned>(d->scanout().stats().laneMbps), clock ? "hs" : "auto",
                           ack ? "on" : "off");
                return Shell::ExitSuccess;
        }

        // display try MODE [NAME=VALUE...]: the test picture (a white frame round it) in another
        // mode, for 10 seconds (seconds=), then the desktop's mode again; the link's lanes, the
        // bridge's settle and DDS word, a minimum pixel clock and the sync widths changeable. For
        // trying what the bridge and a monitor take (devplan/games.md §20).
        int runTry(CommandContext &ctx, Args args) {
                Display *d = running(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                if (args.size() < 3) return ctx.usage();
                auto [parsed, merr] = VideoModes::parse(StringView(args[2]));
                if (merr.isError()) {
                        ctx.eprintf("display: %s: not a mode (WIDTHxHEIGHT@HZ)\n", args[2]);
                        return Shell::ExitFailure;
                }
                VideoTiming mode = parsed;
                // Then NAME=VALUE: lanes, seconds, settle, dds, min (MHz), vsync and hsync (taken
                // from or given to the back porch: the totals stay).
                unsigned lanes = 0, seconds = 10, settle = 0;
                uint16_t hBack = 0;
                uint32_t dds = 0, minKhz = 0;
                for (size_t i = 3; i < args.size(); ++i) {
                        const char *eq = std::strchr(args[i], '=');
                        if (eq == nullptr) return ctx.usage();
                        const StringView    name(args[i], static_cast<size_t>(eq - args[i]));
                        const unsigned long v = std::strtoul(eq + 1, nullptr, 0);
                        if (name == "lanes") {
                                lanes = static_cast<unsigned>(v);
                        } else if (name == "seconds") {
                                seconds = static_cast<unsigned>(v);
                        } else if (name == "settle") {
                                settle = static_cast<unsigned>(v);
                        } else if (name == "dds") {
                                dds = static_cast<uint32_t>(v);
                        } else if (name == "min") {
                                minKhz = static_cast<uint32_t>(v * 1000);
                        } else if (name == "vsync" && v >= 1 && v < uint32_t(mode.vSync) + mode.vBack) {
                                mode.vBack = static_cast<uint16_t>(mode.vBack + mode.vSync - v);
                                mode.vSync = static_cast<uint16_t>(v);
                        } else if (name == "hsync" && v >= 1 && v < uint32_t(mode.hSync) + mode.hBack) {
                                mode.hBack = static_cast<uint16_t>(mode.hBack + mode.hSync - v);
                                mode.hSync = static_cast<uint16_t>(v);
                        } else if (name == "hback" && v >= 1) {
                                hBack = static_cast<uint16_t>(v); // taken from the front porch once fitted
                        } else {
                                return ctx.usage();
                        }
                }
                if (lanes > 4 || seconds == 0 || seconds > 600 || settle > 0x3f || dds > 0xffffff) return ctx.usage();
                if (d->scanout().surface() != &d->surface()) {
                        ctx.eprintf("display: another picture is shown (a full screen)\n");
                        return Shell::ExitFailure;
                }
                auto [fittedMode, ferr] = Display::fitMode(mode, static_cast<uint8_t>(lanes), minKhz);
                if (ferr.isError()) {
                        ctx.eprintf("display: %s: %s\n", args[2], ferr.desc());
                        return Shell::ExitFailure;
                }
                VideoTiming fitted = fittedMode;
                if (hBack != 0) {
                        const uint32_t room = uint32_t(fitted.hFront) + fitted.hBack;
                        if (hBack >= room) return ctx.usage();
                        fitted.hFront = static_cast<uint16_t>(room - hBack);
                        fitted.hBack = hBack;
                }
                const unsigned n = lanes != 0 ? lanes : board().display.dsiLanes;
                printTiming(ctx, fitted, "as sent");
                ctx.printf("Link:     %u lanes at %u Mbps\n", n,
                           static_cast<unsigned>(fitted.pixelKhz * 24 / n / 1000));
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(Surface));
                if (memory == nullptr) return Shell::ExitFailure;
                auto *surface = ::new (memory) Surface();
                Error err = surface->start(fitted.hActive, fitted.vActive, Surface::Buffering::Double);
                if (err.isOk()) err = detail::drawTestPicture(*surface);
                // A white frame: the picture's edges, to see the monitor's cropping and scaling.
                const uint16_t w = fitted.hActive, h = fitted.vActive;
                const Rect     edges[4] = {
                        {0, 0, w, 2}, {0, uint16_t(h - 2), w, 2}, {0, 0, 2, h}, {uint16_t(w - 2), 0, 2, h}};
                for (const Rect &r : edges)
                        if (err.isOk()) err = surface->fill(r, 0xffffff, TimeoutMs);
                if (err.isOk()) err = surface->present(TimeoutMs).error();
                d->bridge().setSettle(static_cast<uint8_t>(settle));
                d->bridge().setDds(dds);
                // Already fitted: refitting keeps it (its clock is one the PLLs make).
                if (err.isOk()) err = d->show(*surface, fitted, static_cast<uint8_t>(lanes), minKhz, 1000);
                if (err.isOk()) {
                        ctx.printf("Showing for %u s (the bridge's settle 0x%02x)\n", seconds,
                                   static_cast<unsigned>(d->bridge().settle()));
                        Task::sleep(seconds > 2 ? 2000 : seconds * 1000);
                        uint16_t first = 0, lines = 0;
                        if (d->bridge().measured(first, lines).isOk())
                                ctx.printf("Bridge:   measures 0x9c-9d %u, lines a frame %u (sent %u)\n",
                                           static_cast<unsigned>(first), static_cast<unsigned>(lines),
                                           static_cast<unsigned>(fitted.vTotal()));
                        if (seconds > 2) Task::sleep((seconds - 2) * 1000);
                        const Scanout::Stats st = d->scanout().stats();
                        ctx.printf("Scan:     %u underruns, %u stalls, %u resyncs, %u fallbacks\n",
                                   static_cast<unsigned>(st.underruns), static_cast<unsigned>(st.stalls),
                                   static_cast<unsigned>(st.resyncs), static_cast<unsigned>(st.fallbacks));
                }
                d->bridge().setSettle(0);
                d->bridge().setDds(0);
                const Error back = d->showDesktop(1000);
                surface->~Surface();
                Memory::free(memory);
                if (err.isError() || back.isError()) {
                        ctx.eprintf("display: %s\n", (err.isError() ? err : back).desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int runTestPicture(CommandContext &ctx) {
                Display *d = running(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                const Error err = detail::drawTestPicture(d->surface());
                if (err.isError()) {
                        ctx.eprintf("display: test: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        // display fill X Y W H RRGGBB
        int runFill(CommandContext &ctx, Args args) {
                if (args.size() != 7) return ctx.usage();
                Display *d = running(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                const Rect     r{static_cast<uint16_t>(std::atoi(args[2])), static_cast<uint16_t>(std::atoi(args[3])),
                             static_cast<uint16_t>(std::atoi(args[4])), static_cast<uint16_t>(std::atoi(args[5]))};
                const uint32_t rgb = static_cast<uint32_t>(std::strtoul(args[6], nullptr, 16));
                Error          err = d->surface().fill(r, rgb, TimeoutMs);
                if (err.isOk()) err = d->surface().present(TimeoutMs).error();
                if (err.isError()) {
                        ctx.eprintf("display: fill: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        // `display underrun [US]`: the bridge starved on purpose (300 µs by default), to test that
        // the resync puts the picture back at the top.
        int runUnderrun(CommandContext &ctx, const Args &args) {
                Display *d = running(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                unsigned int us = 300;
                if (args.size() == 3) {
                        char         *end = nullptr;
                        unsigned long v = std::strtoul(args[2], &end, 10);
                        if (end == args[2] || *end != '\0' || v == 0 || v > 20000) return ctx.usage();
                        us = unsigned(v);
                }
                const Error err = d->scanout().starve(us);
                if (err.isError()) {
                        ctx.eprintf("display: underrun: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int runDisplay(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showStatus(ctx);
                const char *what = args[1];
                if (args.size() == 2 && std::strcmp(what, "edid") == 0) return showEdid(ctx, false);
                if (args.size() == 3 && std::strcmp(what, "edid") == 0 && std::strcmp(args[2], "raw") == 0)
                        return showEdid(ctx, true);
                if (args.size() == 2 && std::strcmp(what, "scan") == 0) return scanBus(ctx);
                if (std::strcmp(what, "start") == 0) return runStart(ctx, args);
                if (args.size() == 2 && std::strcmp(what, "off") == 0) return runOff(ctx);
                if (args.size() == 2 && std::strcmp(what, "stats") == 0) return runStats(ctx);
                if (args.size() == 2 && std::strcmp(what, "test") == 0) return runTestPicture(ctx);
                if (std::strcmp(what, "link") == 0) return runLink(ctx, args);
                if (std::strcmp(what, "dsi") == 0) return detail::runDisplayDsi(ctx, args);
                if (std::strcmp(what, "chip") == 0) return detail::runDisplayChip(ctx, args);
                if (std::strcmp(what, "try") == 0) return runTry(ctx, args);
                if (std::strcmp(what, "fill") == 0) return runFill(ctx, args);
                if (std::strcmp(what, "ppa") == 0) return detail::runDisplayPpa(ctx);
                if (std::strcmp(what, "rows") == 0) return detail::runDisplayRows(ctx);
                if (args.size() <= 3 && std::strcmp(what, "underrun") == 0) return runUnderrun(ctx, args);
                return ctx.usage();
        }

} // namespace

namespace detail {

        // The test picture: eight bars over three quarters, a gray ramp below;
        // drawn into a strip of lines in internal RAM and copied in by DMA.
        Error drawTestPicture(Surface &surface) {
                static constexpr uint32_t Bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                     0xff00ff, 0xff0000, 0x0000ff, 0x000000};
                static constexpr uint16_t StripLines = 16;
                const uint16_t            w = surface.width(), h = surface.height();
                auto                     *strip =
                        static_cast<uint8_t *>(Memory::alloc(MemSpace::InternalDma, surface.stride() * StripLines));
                if (strip == nullptr) return Error::NoMem;
                Error err;
                for (uint16_t y = 0; y < h && err.isOk(); y += StripLines) {
                        const uint16_t lines = static_cast<uint16_t>(h - y < StripLines ? h - y : StripLines);
                        for (uint16_t r = 0; r < lines; ++r) {
                                uint8_t   *line = strip + r * surface.stride();
                                const bool ramp = y + r >= h * 3 / 4;
                                for (uint16_t x = 0; x < w; ++x) {
                                        const uint32_t rgb = ramp ? (x * 256u / w) * 0x010101u : Bars[x * 8 / w];
                                        line[3 * x] = static_cast<uint8_t>(rgb);
                                        line[3 * x + 1] = static_cast<uint8_t>(rgb >> 8);
                                        line[3 * x + 2] = static_cast<uint8_t>(rgb >> 16);
                                }
                        }
                        err = surface.blit(Rect{0, y, w, lines}, strip, surface.stride(), TimeoutMs);
                }
                Memory::free(strip);
                if (err.isOk()) err = surface.present(TimeoutMs).error();
                return err;
        }

} // namespace detail

BMDS_SHELL_COMMAND(
        display, runDisplay,
        "[edid [raw]|scan|start|off|stats|link [clock=auto|hs] [ack=on|off] [mbps=N]|dsi [host|bridge] [PATTERN]|dsi "
        "set "
        "host|bridge REG[.FIELD] VALUE|chip [main|dsi|avi]|chip set PAGE REG VALUE|chip reset|chip configure|"
        "test|try "
        "WxH@HZ [lanes=N seconds=N settle=N dds=N min=MHZ vsync=N "
        "hsync=N hback=N]|fill X Y W H RRGGBB|"
        "ppa|rows N "
        "[stripes] [irq K]|underrun [US]]",
        "Show the HDMI bridge and the monitor; 'edid' decodes the monitor's EDID ('raw': its bytes), "
        "'scan' lists the bus, 'start' shows surface 0 (sysconf's display/mode and display/buffering), "
        "'test' and 'fill' draw on it, 'try' shows a test picture in another mode for a while, "
        "'ppa' checks the PPA (its speed: 'bench run display'); 'link' sets the DSI link's options, 'dsi' shows the "
        "DSI host's and "
        "bridge's registers by field and sets them, 'chip' the HDMI bridge chip's, 'underrun' starves the bridge "
        "for US microseconds (300) to test the resync (test aids)")

BMDS_NAMESPACE_END
