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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_cache.h>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/edid.h>
#include <bmds/i2cbus.h>
#include <bmds/lt8912b.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/psramobject.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace detail {
        int runDisplayPpa(CommandContext &ctx);  // displayppa.cpp
        int runDisplayRows(CommandContext &ctx); // displayppa.cpp
} // namespace detail

namespace {

        using Args = Span<char *const>;

        constexpr uint8_t      EdidAddress = 0x50;
        constexpr uint32_t     DdcHz = 100'000; // DDC's standard clock
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
                auto [ddc, err] = bus.device(EdidAddress, DdcHz, TimeoutMs);
                if (err.isError()) return err;
                const uint8_t offset = static_cast<uint8_t>(n * Edid::BlockBytes);
                return ddc->writeRead(ByteSpan(&offset, 1), WritableByteSpan(block, sizeof(block)), TimeoutMs);
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

        void stopAnim(); // below

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
                stopAnim();
                const Error err = d->start(mode, buffering);
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
                stopAnim();
                d->stop();
                (void)d->bridge().setOutput(false);
                return Shell::ExitSuccess;
        }

        int runStats(CommandContext &ctx) {
                Display *d = running(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                const Scanout::Stats s = d->scanout().stats();
                const Surface       &surface = d->surface();
                ctx.printf("Scanout:  %u frames, %u blocks (%u a frame), %u swaps\n", static_cast<unsigned>(s.frames),
                           static_cast<unsigned>(s.blocks), static_cast<unsigned>(s.blocksPerFrame),
                           static_cast<unsigned>(s.swaps));
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
                return Shell::ExitSuccess;
        }

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

        int runTestPicture(CommandContext &ctx) {
                Display *d = running(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                stopAnim();
                const uint64_t start = System::uptimeUs();
                const Error    err = drawTestPicture(d->surface());
                if (err.isError()) {
                        ctx.eprintf("display: test: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("test:     drawn in %llu us\n", static_cast<unsigned long long>(System::uptimeUs() - start));
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
                const uint64_t start = System::uptimeUs();
                Error          err = d->surface().fill(r, rgb, TimeoutMs);
                const uint64_t filled = System::uptimeUs();
                if (err.isOk()) err = d->surface().present(TimeoutMs).error();
                if (err.isError()) {
                        ctx.eprintf("display: fill: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("fill:     %u pixels in %llu us\n", static_cast<unsigned>(r.area()),
                           static_cast<unsigned long long>(filled - start));
                return Shell::ExitSuccess;
        }

        // display present [N]: N presents, each waited for (a double buffer
        // swaps at a frame's start: one frame each).
        int runPresent(CommandContext &ctx, const char *count) {
                Display *d = running(ctx);
                if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                stopAnim();
                const int n = count == nullptr ? 1 : std::atoi(count);
                uint64_t  total = 0, longest = 0;
                for (int i = 0; i < n && !ctx.isInterrupted(); ++i) {
                        const uint64_t start = System::uptimeUs();
                        auto [number, err] = d->surface().present(TimeoutMs);
                        if (err.isOk()) err = d->surface().waitShown(number, TimeoutMs);
                        const uint64_t us = System::uptimeUs() - start;
                        if (err.isError()) {
                                ctx.eprintf("display: present: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                        total += us;
                        if (us > longest) longest = us;
                }
                ctx.printf("present:  %d shown, %llu us each on average, %llu at most\n", n,
                           static_cast<unsigned long long>(n > 0 ? total / n : 0),
                           static_cast<unsigned long long>(longest));
                return Shell::ExitSuccess;
        }

        // `display anim`: squares bouncing on a flat background, drawn with fills
        // and presented every frame, from a task of its own.
        class AnimTest {
                public:
                        static constexpr int MaxSquares = 64;

                        bool isRunning() const { return _running; }

                        Error start(Surface &surface, int count) {
                                stop();
                                if (count < 1 || count > MaxSquares) return Error::Invalid;
                                _surface = &surface;
                                _count = count;
                                _stats = Stats();
                                const int w = surface.width(), h = surface.height();
                                for (int i = 0; i < count; ++i) {
                                        Square &q = _squares[i];
                                        q.x = ((i * 97) % (w - Size)) & ~3;
                                        q.y = (i * 53) % (h - Size);
                                        q.dx = (4 + 4 * (i % 3)) * (i % 2 ? -1 : 1);
                                        q.dy = (1 + i % 5) * (i % 3 ? 1 : -1);
                                        q.px = q.x;
                                        q.py = q.y;
                                        q.color = Colors[i % 6];
                                }
                                Error err = surface.fill(surface.bounds(), Background, TimeoutMs);
                                for (int i = 0; i < count && err.isOk(); ++i) err = draw(_squares[i]);
                                if (err.isOk()) err = surface.present(TimeoutMs).error();
                                if (err.isError()) return err;
                                _stopping.store(false);
                                _exited.store(false);
                                Task::Config tc;
                                tc.name = "anim";
                                tc.stackBytes = 3072;
                                tc.priority = 5;
                                tc.core = 0;
                                err = _task.start(tc, entry, this);
                                if (err.isOk()) _running = true;
                                return err;
                        }

                        // Stops the task; the picture stays.
                        void stop() {
                                if (!_running) return;
                                _stopping.store(true);
                                for (int i = 0; i < 200 && !_exited.load(); ++i) Task::sleep(1);
                                (void)_task.stop();
                                _running = false;
                                return;
                        }

                        void report(CommandContext &ctx) const {
                                const uint64_t us = System::uptimeUs() - _stats.startUs;
                                const uint32_t f = _stats.frames;
                                ctx.printf("anim:     %d squares, %u frames in %llu ms (%u.%u fps), %u errors\n",
                                           _count, static_cast<unsigned>(f), static_cast<unsigned long long>(us / 1000),
                                           static_cast<unsigned>(us == 0 ? 0 : uint64_t(f) * 1'000'000 / us),
                                           static_cast<unsigned>(us == 0 ? 0 : uint64_t(f) * 10'000'000 / us % 10),
                                           static_cast<unsigned>(_stats.errors));
                                ctx.printf("draw:     %llu us a frame on average, %llu at most; present %llu us on "
                                           "average, %llu at most\n",
                                           static_cast<unsigned long long>(f == 0 ? 0 : _stats.drawUs / f),
                                           static_cast<unsigned long long>(_stats.drawUsMax),
                                           static_cast<unsigned long long>(f == 0 ? 0 : _stats.presentUs / f),
                                           static_cast<unsigned long long>(_stats.presentUsMax));
                                return;
                        }

                        // Every pixel of the shown picture: a square's color where the
                        // topmost square is, the background elsewhere.
                        void check(CommandContext &ctx) const {
                                const Surface &s = *_surface;
                                const uint8_t *buffer = s.shown();
                                const size_t   bytes = s.stride() * s.height();
                                for (size_t off = 0; off < bytes; off += 64 * 1024)
                                        (void)esp_cache_msync(const_cast<uint8_t *>(buffer) + off,
                                                              bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                                uint32_t wrong = 0;
                                int      fx = -1, fy = -1, lx = -1, ly = -1;
                                uint32_t fgot = 0, fwant = 0;
                                for (int y = 0; y < s.height(); ++y) {
                                        const uint8_t *line = buffer + size_t(y) * s.stride();
                                        for (int x = 0; x < s.width(); ++x) {
                                                uint32_t want = Background;
                                                for (int i = 0; i < _count; ++i) {
                                                        const Square &q = _squares[i];
                                                        if (x >= q.px && x < q.px + Size && y >= q.py &&
                                                            y < q.py + Size)
                                                                want = q.color;
                                                }
                                                const uint8_t *p = line + 3 * x;
                                                const uint32_t got = uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
                                                if (got == want) continue;
                                                if (wrong++ == 0) {
                                                        fx = x;
                                                        fy = y;
                                                        fgot = got;
                                                        fwant = want;
                                                }
                                                lx = x;
                                                ly = y;
                                        }
                                }
                                ctx.printf("check:    %u of %u pixels wrong", static_cast<unsigned>(wrong),
                                           static_cast<unsigned>(uint32_t(s.width()) * s.height()));
                                if (wrong > 0)
                                        ctx.printf(" (the first at %d,%d: %06lx, not %06lx; the last at %d,%d)", fx, fy,
                                                   static_cast<unsigned long>(fgot), static_cast<unsigned long>(fwant),
                                                   lx, ly);
                                ctx.printf("\n");
                                return;
                        }

                private:
                        static constexpr int      Size = 64;
                        static constexpr uint32_t Background = 0x303030;
                        static constexpr uint32_t Colors[6] = {0xff4040, 0x40ff40, 0x4040ff,
                                                               0xffff40, 0x40ffff, 0xff40ff};

                        struct Square {
                                        int      x = 0, y = 0, dx = 0, dy = 0;
                                        int      px = 0, py = 0; // where it was drawn last
                                        uint32_t color = 0;
                        };

                        struct Stats {
                                        uint64_t startUs = System::uptimeUs();
                                        uint32_t frames = 0;
                                        uint32_t errors = 0;
                                        uint64_t drawUs = 0, presentUs = 0, drawUsMax = 0, presentUsMax = 0;
                        };

                        static void entry(void *arg) {
                                auto *self = static_cast<AnimTest *>(arg);
                                while (!self->_stopping.load()) self->frame();
                                self->_exited.store(true);
                                for (;;) Task::sleep(1000);
                        }

                        Error draw(const Square &q) {
                                return _surface->fill(
                                        Rect{uint16_t(q.x), uint16_t(q.y), uint16_t(Size), uint16_t(Size)}, q.color,
                                        TimeoutMs);
                        }

                        // One frame: every square's last place cleared, every square moved
                        // and drawn (in order: later ones on top), presented.
                        void frame() {
                                const uint64_t start = System::uptimeUs();
                                Surface       &s = *_surface;
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        if (s.fill(Rect{uint16_t(q.px), uint16_t(q.py), uint16_t(Size), uint16_t(Size)},
                                                   Background, TimeoutMs)
                                                    .isError())
                                                ++_stats.errors;
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        q.x += q.dx;
                                        q.y += q.dy;
                                        if (q.x < 0 || q.x > s.width() - Size) {
                                                q.dx = -q.dx;
                                                q.x += 2 * q.dx;
                                        }
                                        if (q.y < 0 || q.y > s.height() - Size) {
                                                q.dy = -q.dy;
                                                q.y += 2 * q.dy;
                                        }
                                        if (draw(q).isError()) ++_stats.errors;
                                        q.px = q.x;
                                        q.py = q.y;
                                }
                                const uint64_t drawn = System::uptimeUs();
                                if (s.present(TimeoutMs).isError()) ++_stats.errors;
                                const uint64_t presented = System::uptimeUs();
                                // A single buffer is never waited for: a frame's pace.
                                if (s.buffering() == Surface::Buffering::Single) {
                                        const uint32_t seen = s.frames();
                                        while (s.frames() == seen && !_stopping.load()) Task::sleep(1);
                                }
                                const uint64_t d = drawn - start, p = presented - drawn;
                                _stats.drawUs += d;
                                _stats.presentUs += p;
                                if (d > _stats.drawUsMax) _stats.drawUsMax = d;
                                if (p > _stats.presentUsMax) _stats.presentUsMax = p;
                                ++_stats.frames;
                                return;
                        }

                        Surface          *_surface = nullptr;
                        Square            _squares[MaxSquares];
                        int               _count = 0;
                        bool              _running = false;
                        Stats             _stats;
                        std::atomic<bool> _stopping{false};
                        std::atomic<bool> _exited{false};
                        Task              _task;
        };

        constinit PsramObject<AnimTest> animObject; // the test's, made when first run

        // The test, once made (`display anim start`).
        AnimTest &anim() {
                return *animObject;
        }

        void stopAnim() {
                if (animObject.isCreated()) anim().stop();
                return;
        }

        // display anim start [N] | anim stop | anim
        int runAnim(CommandContext &ctx, Args args) {
                if (args.size() >= 3 && std::strcmp(args[2], "stop") == 0) {
                        if (animObject.isCreated() && anim().isRunning()) {
                                anim().stop();
                                anim().report(ctx);
                                Display *d = running(ctx);
                                // The last present shown (two frames) before the picture is read.
                                if (d != nullptr) {
                                        Task::sleep(40);
                                        anim().check(ctx);
                                }
                        }
                        return Shell::ExitSuccess;
                }
                if (args.size() >= 3 && std::strcmp(args[2], "start") == 0) {
                        Display *d = running(ctx);
                        if (d == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                        const int n = args.size() > 3 ? std::atoi(args[3]) : 8;
                        if (!animObject.isCreated() && animObject.create() == nullptr) {
                                ctx.eprintf("display: anim: %s\n", Error(Error::NoMem).desc());
                                return Shell::ExitFailure;
                        }
                        const Error err = anim().start(d->surface(), n);
                        if (err.isError()) {
                                ctx.eprintf("display: anim: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                if (args.size() == 2 && animObject.isCreated() && anim().isRunning()) {
                        anim().report(ctx);
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

        // Bring-up: memory traffic from this task, to see what disturbs the scanout.
        int runStress(CommandContext &ctx, const char *what) {
                const bool   psram = std::strcmp(what, "psram") == 0;
                const bool   write = std::strcmp(what, "write") == 0;
                const size_t bytes = psram || write ? 1024 * 1024 : 64 * 1024;
                auto        *a = static_cast<uint8_t *>(
                        Memory::alloc(psram || write ? MemSpace::Psram : MemSpace::Internal, bytes));
                auto *b = static_cast<uint8_t *>(
                        Memory::alloc(psram || write ? MemSpace::Psram : MemSpace::Internal, bytes));
                if (a == nullptr || b == nullptr) {
                        Memory::free(a);
                        Memory::free(b);
                        return Shell::ExitFailure;
                }
                const uint64_t start = System::uptimeUs();
                size_t         moved = 0;
                while (System::uptimeUs() - start < 2'000'000) {
                        if (write) {
                                std::memset(a, static_cast<int>(moved), bytes);
                        } else {
                                std::memcpy(b, a, bytes);
                        }
                        moved += bytes;
                }
                ctx.printf("stress %s: %u MB in 2 s\n", what, static_cast<unsigned>(moved >> 20));
                Memory::free(a);
                Memory::free(b);
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
                if (std::strcmp(what, "fill") == 0) return runFill(ctx, args);
                if (args.size() <= 3 && std::strcmp(what, "present") == 0)
                        return runPresent(ctx, args.size() == 3 ? args[2] : nullptr);
                if (std::strcmp(what, "anim") == 0) return runAnim(ctx, args);
                if (args.size() == 3 && std::strcmp(what, "stress") == 0) return runStress(ctx, args[2]);
                if (std::strcmp(what, "ppa") == 0) return detail::runDisplayPpa(ctx);
                if (std::strcmp(what, "rows") == 0) return detail::runDisplayRows(ctx);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(display, runDisplay,
                   "[edid [raw]|scan|start|off|stats|test|fill X Y W H RRGGBB|"
                   "present [N]|anim [start [N]|stop]|stress psram|write|internal|ppa [soak SECONDS|path]|rows N "
                   "[stripes] [irq K]]",
                   "Show the HDMI bridge and the monitor; 'edid' decodes the monitor's EDID ('raw': its bytes), "
                   "'scan' lists the bus, 'start' shows surface 0 (sysconf's display/mode and display/buffering), "
                   "'test', 'fill' and 'anim' draw on it, 'ppa' measures the PPA")

BMDS_NAMESPACE_END
