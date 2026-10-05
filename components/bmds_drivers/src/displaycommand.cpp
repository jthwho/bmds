/**
 * @file      displaycommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `display` command: the HDMI bridge and the monitor (devplan/display.md
 * step 1). Its output format is a userspace interface once shipped: keep the
 * lines and their order, add new ones only at the end.
 */

#include <cstdlib>
#include <sdkconfig.h>
#include <cstring>
#include <esp_cache.h>
#include <hal/axi_icm_ll.h>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/displaytest.h>
#include <bmds/log.h>
#include <bmds/edid.h>
#include <bmds/i2cbus.h>
#include <bmds/lt8912b.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/paletteexpander.h>
#include <bmds/scanout.h>
#include <bmds/shell.h>
#include <bmds/staticobject.h>
#include <bmds/system.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr uint8_t      EdidAddress = 0x50;
        constexpr uint32_t     DdcHz = 100'000; // DDC's standard clock
        constexpr unsigned int TimeoutMs = 100;

        constinit StaticObject<Lt8912b> bridgeObject;

        // Step 2's test picture (devplan/display.md): bars, a gray ramp and a
        // stripe that moves each frame, so a lost or late block shows.
        struct TestMode {
                        const char *name;
                        VideoTiming timing;
        };
        constexpr TestMode TestModes[] = {
                // Espressif's timings: pixel clocks the DPI clock divides from its PLL.
                {"720", {80'000, 1280, 408, 32, 80, 720, 3, 5, 13, true, false, false}},
                {"1080", {70'000, 1920, 48, 32, 80, 1080, 3, 5, 8, true, false, false}},
        };

        class TestPattern final : public LineSource {
                public:
                        // The two kinds of line, drawn once (internal RAM, freed by the next set-up).
                        Error setUp(Scanout::Format format, uint16_t width, uint16_t height) {
                                _bytes = format == Scanout::Format::Indexed8 ? 1
                                         : format == Scanout::Format::Rgb888 ? 3
                                                                             : 2;
                                _width = width;
                                _height = height;
                                _frame = 0;
                                Memory::free(_lines);
                                _lines = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, 2 * width * _bytes));
                                if (_lines == nullptr) return Error::NoMem;
                                static constexpr uint32_t Bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                                     0xff00ff, 0xff0000, 0x0000ff, 0x000000};
                                if (_bytes == 1) {
                                        // Indices: 0-7 the bars, 8-255 the ramp; the palette says what they are.
                                        for (uint16_t x = 0; x < width; ++x) {
                                                _lines[x] = static_cast<uint8_t>(x * 8 / width);
                                                _lines[width + x] = static_cast<uint8_t>(8 + x * 248 / width);
                                        }
                                        return Error::Ok;
                                }
                                for (uint16_t x = 0; x < width; ++x) {
                                        put(_lines, x, Bars[x * 8 / width]);
                                        put(_lines + width * _bytes, x, (x * 256 / width) * 0x010101u);
                                }
                                return Error::Ok;
                        }

                        BMDS_HOT_ATTR void render(uint16_t first, uint16_t count, uint8_t *dst,
                                                  size_t stride) override {
                                if (first == 0) ++_frame;
                                const uint16_t stripe = static_cast<uint16_t>((_frame * 4) % (_width - 16));
                                for (uint16_t i = 0; i < count; ++i) {
                                        const uint16_t y = static_cast<uint16_t>(first + i);
                                        uint8_t       *out = dst + i * stride;
                                        std::memcpy(out, _lines + (y < _height * 3 / 4 ? 0 : _width * _bytes), stride);
                                        for (size_t k = stripe * _bytes; k < (stripe + 16u) * _bytes; ++k)
                                                out[k] ^= 0xff;
                                }
                                return;
                        }

                        // Framebuffer: the stripe's old place back from the line template,
                        // its new one inverted.
                        // A still picture (`display test MODE ... still`): frame() changes nothing.
                        void setStill(bool still) {
                                _still = still;
                                return;
                        }

                        BMDS_HOT_ATTR void frame(uint8_t *fb, size_t stride) override {
                                if (_still) return;
                                const uint16_t old = static_cast<uint16_t>((_frame * 4) % (_width - 16));
                                ++_frame;
                                const uint16_t now = static_cast<uint16_t>((_frame * 4) % (_width - 16));
                                const size_t   span = 16u * _bytes;
                                for (uint16_t y = 0; y < _height; ++y) {
                                        uint8_t       *line = fb + y * stride;
                                        const uint8_t *tmpl = _lines + (y < _height * 3 / 4 ? 0 : _width * _bytes);
                                        std::memcpy(line + old * _bytes, tmpl + old * _bytes, span);
                                        for (size_t k = now * _bytes; k < now * _bytes + span; ++k)
                                                line[k] = tmpl[k] ^ 0xff;
                                        // Written back for the DMA: the two places, as one span.
                                        const uint16_t from = old < now ? old : now;
                                        const uint16_t to = static_cast<uint16_t>((old < now ? now : old) + 16);
                                        (void)esp_cache_msync(line + from * _bytes, size_t(to - from) * _bytes,
                                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                                                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                                }
                                return;
                        }

                private:
                        void put(uint8_t *line, uint16_t x, uint32_t rgb) const {
                                const uint8_t r = static_cast<uint8_t>(rgb >> 16), g = static_cast<uint8_t>(rgb >> 8),
                                              b = static_cast<uint8_t>(rgb);
                                if (_bytes == 2) {
                                        const uint16_t p =
                                                static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
                                        line[2 * x] = static_cast<uint8_t>(p);
                                        line[2 * x + 1] = static_cast<uint8_t>(p >> 8);
                                } else {
                                        line[3 * x] = b;
                                        line[3 * x + 1] = g;
                                        line[3 * x + 2] = r;
                                }
                                return;
                        }

                        uint8_t *_lines = nullptr;
                        size_t   _bytes = 2;
                        bool     _still = false;
                        uint16_t _width = 0;
                        uint16_t _height = 0;
                        uint32_t _frame = 0;
        };

        constinit StaticObject<Scanout> scanoutObject;
        TestPattern                     testPattern;
        uint64_t                        testStartUs = 0;

        // The board's bridge, attached on first use.
        Result<Lt8912b *> attachBridge() {
                const BoardDisplay &d = board().display;
                if (d.bridge != BoardDisplayBridge::Lt8912b) return makeError<Lt8912b *>(Error::NotSupported);
                if (!bridgeObject.isCreated()) {
                        I2cBus *bus = I2cBus::bus(static_cast<size_t>(d.i2c));
                        if (bus == nullptr) return makeError<Lt8912b *>(Error::NotExist);
                        const Error err = bridgeObject.create(*bus, d.i2cAddr).attach();
                        if (err.isError()) return makeError<Lt8912b *>(err);
                }
                return makeResult(&*bridgeObject);
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
                ctx.printf("Mode:     %ux%u%s at %u.%03u Hz, %u.%03u MHz (%s)\n", t.hActive, t.vActive,
                           t.interlaced ? "i" : "", static_cast<unsigned>(mhz / 1000),
                           static_cast<unsigned>(mhz % 1000), static_cast<unsigned>(t.pixelKhz / 1000),
                           static_cast<unsigned>(t.pixelKhz % 1000), note);
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
        int showEdid(CommandContext &ctx) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                uint8_t block[Edid::BlockBytes];
                for (uint8_t n = 0; n < 2; ++n) {
                        const Error err = readEdid(b->bus(), n, block);
                        if (err.isError()) {
                                if (n == 0) {
                                        ctx.eprintf("display: EDID: %s\n",
                                                    err == Error::IoError ? "no answer (no monitor?)" : err.desc());
                                        return Shell::ExitFailure;
                                }
                                break;
                        }
                        for (size_t i = 0; i < sizeof(block); i += 16) {
                                ctx.printf("%03x:", static_cast<unsigned>(n * Edid::BlockBytes + i));
                                for (size_t j = 0; j < 16; ++j) ctx.printf(" %02x", block[i + j]);
                                ctx.printf("\n");
                        }
                        if (n == 0 && block[126] == 0) break;
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

        Scanout &scanout() {
                if (!scanoutObject.isCreated()) scanoutObject.create(0);
                return *scanoutObject;
        }

        // Sets the bridge up and starts the test picture in @p mode.
        // The mode running, for a rebuild's bridge set-up.
        VideoTiming runningTiming;

        // A rebuilt DSI link: the bridge set up again, as at the start (after a
        // link drop its receiver alone doesn't always lock).
        void onLinkRestart(void *arg) {
                auto &b = *static_cast<Lt8912b *>(arg);
                if (b.configure(runningTiming, board().display.dsiLanes, true).isOk()) (void)b.resetReceiver();
                return;
        }

        // Bring-up knobs for 16-bit links (`display test MODE 565 ... pack=N lane24`).
        uint8_t testPacking = 0;
        bool    testFullLane = false;
        bool    testFree = true; // the free-running ring, unless `display test MODE ... strict`
        uint8_t *testFrame = nullptr; // `display test MODE fb565`: the RGB565 framebuffer (PSRAM)
        bool     testStill = false;

        Error startTest(Lt8912b &b, const VideoTiming &timing, Scanout::Format format, uint16_t lines, uint8_t blocks,
                        bool framebuffer, bool fb565 = false) {
                scanout().stop();
                Memory::free(testFrame);
                testFrame = nullptr;
                const BoardDisplay &d = board().display;
                Scanout::Config     c;
                c.timing = timing;
                runningTiming = timing;
                c.format = format;
                c.lanes = d.dsiLanes;
                c.phyLdo = d.phyLdo;
                c.phyLdoMv = d.phyLdoMv;
                c.linesPerBlock = lines;
                c.blocks = blocks;
                c.framebuffer = framebuffer;
                c.linkPacking = testPacking;
                c.fullLaneRate = testFullLane;
                c.freeRunning = testFree;
                // The test palette: the eight bars, then a gray ramp.
                static uint32_t           palette[PaletteExpander::PaletteSize];
                static constexpr uint32_t Bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                     0xff00ff, 0xff0000, 0x0000ff, 0x000000};
                for (size_t i = 0; i < PaletteExpander::PaletteSize; ++i)
                        palette[i] = i < 8 ? Bars[i] : static_cast<uint32_t>((i - 8) * 255 / 247) * 0x010101u;
                c.palette = palette;
                // A rebuilt link: the bridge chip set up again.
                c.onRestart = onLinkRestart;
                c.onRestartArg = &b;
                Error err = b.configure(c.timing, c.lanes, true);
                if (err.isOk()) err = testPattern.setUp(c.format, c.timing.hActive, c.timing.vActive);
                testPattern.setStill(testStill);
                if (err.isOk() && fb565) {
                        // The picture drawn once into the framebuffer, written back in pieces
                        // (a cache operation runs with interrupts off).
                        const size_t stride = size_t(c.timing.hActive) * 2;
                        const size_t bytes = stride * c.timing.vActive;
                        testFrame = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::PsramDma, bytes, 128));
                        if (testFrame == nullptr) return Error::NoMem;
                        testPattern.render(0, c.timing.vActive, testFrame, stride);
                        for (size_t off = 0; off < bytes; off += 64 * 1024)
                                (void)esp_cache_msync(testFrame + off,
                                                      bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        c.frameSource = testFrame;
                }
                if (err.isOk()) err = scanout().start(c, testPattern);
                if (err.isError()) return err;
                testStartUs = System::uptimeUs();
                return b.resetReceiver();
        }

        // display test 720|1080|WxH@Hz [cpu|pal|565|dma|fb|fb565] [lines per block] [blocks] | off
        int runTest(CommandContext &ctx, Args args) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                if (std::strcmp(args[2], "off") == 0) {
                        scanout().stop();
                        Memory::free(testFrame);
                        testFrame = nullptr;
                        (void)b->setOutput(false);
                        return Shell::ExitSuccess;
                }
                // Knobs anywhere after the mode: pack=N, lane24, strict (the ring stops for a late block).
                testPacking = 0;
                testFullLane = false;
                testFree = true;
                testStill = false;
                size_t positional = args.size();
                for (size_t i = 3; i < args.size(); ++i) {
                        const bool pack = std::strncmp(args[i], "pack=", 5) == 0;
                        if (pack) testPacking = static_cast<uint8_t>(std::atoi(args[i] + 5));
                        const bool lane24 = std::strcmp(args[i], "lane24") == 0;
                        const bool strict = std::strcmp(args[i], "strict") == 0;
                        const bool still = std::strcmp(args[i], "still") == 0;
                        if (still) testStill = true;
                        if (lane24) testFullLane = true;
                        if (strict) testFree = false;
                        if ((pack || lane24 || strict || still) && i < positional) positional = i;
                }
                if (positional > 6) return ctx.usage();
                args = Args(args.data(), positional);
                VideoTiming timing;
                bool        found = false;
                for (const TestMode &m : TestModes) {
                        if (std::strcmp(m.name, args[2]) == 0) {
                                timing = m.timing;
                                found = true;
                        }
                }
                if (!found) {
                        auto [t, perr] = VideoModes::parse(args[2]);
                        if (perr.isError()) {
                                ctx.eprintf("display: %s: not a mode (720, 1080, or WIDTHxHEIGHT@HZ)\n", args[2]);
                                return Shell::ExitUsage;
                        }
                        timing = t;
                }
                // The DSI link carries 24 bits a pixel on its lanes, 1.5 Gbps each at
                // most; the pixel clock is a fixed PLL divided, so the mode is fitted
                // to one (its front porch takes up the difference).
                const BoardDisplay       &d = board().display;
                const uint32_t            maxKhz = d.dsiLanes * 1'500'000u / 24u;
                static constexpr uint32_t Plls[] = {240'000, 160'000};
                auto [fitted, ferr] = VideoModes::fitToClock(timing, Plls, 2, maxKhz);
                if (ferr.isError()) {
                        ctx.eprintf(
                                "display: %s needs a %u.%03u MHz pixel clock; the DSI link carries %u MHz at most\n",
                                args[2], static_cast<unsigned>(timing.pixelKhz / 1000),
                                static_cast<unsigned>(timing.pixelKhz % 1000), static_cast<unsigned>(maxKhz / 1000));
                        return Shell::ExitFailure;
                }
                timing = fitted;
                Scanout::Format format = Scanout::Format::Rgb888;
                bool            framebuffer = false;
                bool            fb565 = false;
                if (args.size() > 3) {
                        if (std::strcmp(args[3], "fb") == 0) {
                                framebuffer = true;
                        } else if (std::strcmp(args[3], "fb565") == 0) {
                                fb565 = true;
                                format = Scanout::Format::Rgb565Expanded;
                        } else if (std::strcmp(args[3], "565") == 0) {
                                format = Scanout::Format::Rgb565;
                        } else if (std::strcmp(args[3], "dma") == 0) {
                                format = Scanout::Format::Rgb565Expanded;
                        } else if (std::strcmp(args[3], "pal") == 0 || std::strcmp(args[3], "ppa") == 0) {
                                format = Scanout::Format::Indexed8;
                        } else if (std::strcmp(args[3], "888") != 0 && std::strcmp(args[3], "cpu") != 0) {
                                return ctx.usage();
                        }
                }
                uint16_t lines = args.size() > 4 ? static_cast<uint16_t>(std::atoi(args[4])) : 8;
                uint8_t  blocks = args.size() > 5 ? static_cast<uint8_t>(std::atoi(args[5])) : 3;
                if (framebuffer) {
                        // The frame in as many blocks (at most 16) as divide its lines.
                        blocks = 16;
                        while (timing.vActive % blocks != 0) --blocks;
                        lines = static_cast<uint16_t>(timing.vActive / blocks);
                }
                if (lines == 0 || timing.vActive % lines != 0) {
                        ctx.eprintf("display: %u lines a block doesn't divide %u lines\n", lines, timing.vActive);
                        return Shell::ExitFailure;
                }
                const Error err = startTest(*b, timing, format, lines, blocks, framebuffer, fb565);
                if (err.isError()) {
                        ctx.eprintf("display: test: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const VideoTiming &t = timing;
                const uint32_t     mhz = t.refreshMilliHz();
                ctx.printf("Test:     %ux%u at %u.%03u Hz, %u.%03u MHz, %s path, %u blocks of %u lines (%u bytes)\n",
                           t.hActive, t.vActive, static_cast<unsigned>(mhz / 1000), static_cast<unsigned>(mhz % 1000),
                           static_cast<unsigned>(t.pixelKhz / 1000), static_cast<unsigned>(t.pixelKhz % 1000),
                           framebuffer                                 ? "framebuffer (PSRAM)"
                           : fb565                                     ? "RGB565 framebuffer (PSRAM), 2D-DMA widened"
                           : format == Scanout::Format::Rgb565         ? "CPU (RGB565)"
                           : format == Scanout::Format::Rgb888         ? "CPU"
                           : format == Scanout::Format::Rgb565Expanded ? "2D-DMA (RGB565 widened)"
                                                                       : "PPA (8-bit palette)",
                           blocks, lines, static_cast<unsigned>(scanout().ringBytes()));
                ctx.printf("Timing:   h %u %u %u %u %c, v %u %u %u %u %c\n", t.hActive, t.hFront, t.hSync, t.hBack,
                           t.hSyncHigh ? '+' : '-', t.vActive, t.vFront, t.vSync, t.vBack, t.vSyncHigh ? '+' : '-');
                return Shell::ExitSuccess;
        }

        int runStats(CommandContext &ctx) {
                if (!scanoutObject.isCreated() || !scanout().isRunning()) {
                        ctx.eprintf("display: no scanout running\n");
                        return Shell::ExitFailure;
                }
                const Scanout::Stats s = scanout().stats();
                const uint64_t       us = System::uptimeUs() - testStartUs;
                const uint64_t       mhz = 360; // cycles per microsecond (the CPU clock)
                const uint64_t       avg = s.blocks == 0 ? 0 : s.renderCycles / s.blocks;
                const uint64_t       loadTenths = us == 0 ? 0 : s.renderCycles * 1000 / (us * mhz);
                ctx.printf("Scanout:  %u blocks, %u frames in %llu ms (%llu.%01llu fps)\n",
                           static_cast<unsigned>(s.blocks), static_cast<unsigned>(s.frames),
                           static_cast<unsigned long long>(us / 1000),
                           static_cast<unsigned long long>(us == 0 ? 0 : uint64_t(s.frames) * 1000000 / us),
                           static_cast<unsigned long long>(us == 0 ? 0 : uint64_t(s.frames) * 10000000 / us % 10));
                ctx.printf(
                        "Problems: %u late, %u skipped, %u stalls, %u underruns, %u resyncs, %u rebuilds (%u failed)\n",
                        static_cast<unsigned>(s.late), static_cast<unsigned>(s.skipped),
                        static_cast<unsigned>(s.stalls), static_cast<unsigned>(s.underruns),
                        static_cast<unsigned>(s.resyncs), static_cast<unsigned>(s.rebuilds),
                        static_cast<unsigned>(s.resyncFailures));
                ctx.printf("Sync:     %u cycles at most; stalls at blocks", static_cast<unsigned>(s.syncCyclesMax));
                for (uint32_t at : s.stallAt) ctx.printf(" %u", static_cast<unsigned>(at));
                ctx.printf("\n");
                if (s.expandCycles != 0)
                        ctx.printf("PPA:      %llu cycles a block on average, %u at most, %u errors\n",
                                   static_cast<unsigned long long>(s.blocks == 0 ? 0 : s.expandCycles / s.blocks),
                                   static_cast<unsigned>(s.expandCyclesMax), static_cast<unsigned>(s.expandErrors));
                ctx.printf("DMA:      on buffer %u, events 0x%08x, %u moved; bridge FIFO %u\n",
                           static_cast<unsigned>(s.dmaItem), static_cast<unsigned>(s.dmaStatus),
                           static_cast<unsigned>(s.dmaAmount), static_cast<unsigned>(s.bridgeFifo));
                ctx.printf(
                        "Render:   %llu cycles a block on average (%llu writing back), %u at most; %llu.%01llu%% of a "
                        "core\n",
                        static_cast<unsigned long long>(avg),
                        static_cast<unsigned long long>(s.blocks == 0 ? 0 : s.syncCycles / s.blocks),
                        static_cast<unsigned>(s.renderCyclesMax), static_cast<unsigned long long>(loadTenths / 10),
                        static_cast<unsigned long long>(loadTenths % 10));
                return Shell::ExitSuccess;
        }

        // Bring-up diagnostics: I2C read reliability at two clocks, and what the
        // bridge sees of the DSI input (main 0x9c-0x9f: the measured sync counts).
        // Bring-up: `display verify` checks the ring against the framebuffer it was
        // widened from (`display test MODE fb565 still`): every line one of the
        // picture's two kinds, each RGB888 pixel's top bits its RGB565 pixel's.
        int runVerify(CommandContext &ctx) {
                if (testFrame == nullptr || !scanout().isRunning()) {
                        ctx.eprintf("display: verify needs `display test MODE fb565 still`\n");
                        return Shell::ExitFailure;
                }
                const VideoTiming &t = runningTiming;
                const size_t       ringBytes = scanout().ringBytes();
                const uint8_t     *ring = scanout().ring();
                (void)esp_cache_msync(const_cast<uint8_t *>(ring), ringBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                const size_t   stride888 = size_t(t.hActive) * 3;
                const uint8_t *kinds[2] = {testFrame, testFrame + size_t(t.vActive - 1) * t.hActive * 2};
                size_t         lines = 0, bad = 0;
                uint32_t       lowBits = 0;
                for (size_t off = 0; off + stride888 <= ringBytes; off += stride888, ++lines) {
                        const uint8_t *out = ring + off;
                        bool           matched = false;
                        for (const uint8_t *in : kinds) {
                                size_t x = 0;
                                for (; x < t.hActive; ++x) {
                                        const uint32_t p = in[x * 2] | uint32_t(in[x * 2 + 1]) << 8;
                                        const uint8_t  b = out[x * 3], g = out[x * 3 + 1], r = out[x * 3 + 2];
                                        if ((r >> 3) != (p >> 11) || (g >> 2) != ((p >> 5) & 63) ||
                                            (b >> 3) != (p & 31))
                                                break;
                                        lowBits |= (r & 7u) << 16 | (g & 3u) << 8 | (b & 7u);
                                }
                                if (x == t.hActive) matched = true;
                        }
                        if (!matched) {
                                if (bad == 0) {
                                        const uint32_t p = kinds[0][0] | uint32_t(kinds[0][1]) << 8;
                                        ctx.printf("first bad line %u: out %02x %02x %02x, in %04x\n",
                                                   static_cast<unsigned>(lines), out[0], out[1], out[2],
                                                   static_cast<unsigned>(p));
                                }
                                ++bad;
                        }
                }
                ctx.printf("verify: %u of %u ring lines wrong; low bits seen %06lx\n", static_cast<unsigned>(bad),
                           static_cast<unsigned>(lines), static_cast<unsigned long>(lowBits));
                return bad == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int runCheck(CommandContext &ctx) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                for (uint32_t hz : {400'000u, 100'000u}) {
                        auto [d, err] = b->bus().device(b->address(), hz, TimeoutMs);
                        if (err.isError()) return Shell::ExitFailure;
                        unsigned bad = 0, failed = 0;
                        for (int i = 0; i < 200; ++i) {
                                auto [v, rerr] = d->readRegister(0x00, TimeoutMs);
                                if (rerr.isError()) {
                                        ++failed;
                                } else if (v != 0x12) {
                                        ++bad;
                                }
                        }
                        ctx.printf("i2c %u kHz: 200 reads of 0x00, %u wrong, %u failed\n",
                                   static_cast<unsigned>(hz / 1000), bad, failed);
                }
                auto [d, err] = b->bus().device(b->address(), 100'000, TimeoutMs);
                if (err.isError()) return Shell::ExitFailure;
                ctx.printf("main:");
                for (uint8_t r : {0x9c, 0x9d, 0x9e, 0x9f, 0x33, 0x44, 0xb2, 0xab, 0xc1}) {
                        auto [v, rerr] = d->readRegister(r, TimeoutMs);
                        ctx.printf(" %02x=%02x%s", r, v, rerr.isError() ? "!" : "");
                }
                ctx.printf("\n");
                auto [dsi, derr] = b->bus().device(static_cast<uint8_t>(b->address() + 1), 100'000, TimeoutMs);
                if (derr.isError()) return Shell::ExitFailure;
                ctx.printf("dsi: ");
                for (uint8_t r : {0x10, 0x11, 0x13, 0x18, 0x19, 0x1c, 0x1d, 0x34, 0x35, 0x36, 0x37}) {
                        auto [v, rerr] = dsi->readRegister(r, TimeoutMs);
                        ctx.printf(" %02x=%02x%s", r, v, rerr.isError() ? "!" : "");
                }
                ctx.printf("\n");
                return Shell::ExitSuccess;
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

        // Bring-up: the AXI interconnect's arbitration (devplan/display.md §8,
        // the QoS experiment). Fields of the ICM's priority and QoS registers.
        struct QosField {
                        const char        *name;
                        volatile uint32_t *reg;
                        uint8_t            shift;
                        uint8_t            width;
        };
        constexpr QosField QosFields[] = {
                {"prio.cpu", &AXI_ICM.mst_arb_priority_reg0.val, 0, 4},
                {"prio.cache", &AXI_ICM.mst_arb_priority_reg0.val, 4, 4},
                {"prio.dma2d", &AXI_ICM.mst_arb_priority_reg0.val, 8, 4},
                {"prio.gdma1", &AXI_ICM.mst_arb_priority_reg0.val, 12, 4},
                {"prio.gdma2", &AXI_ICM.mst_arb_priority_reg0.val, 16, 4},
                {"prio.pdma", &AXI_ICM.mst_arb_priority_reg0.val, 28, 4},
                {"slv.l2mem", &AXI_ICM.slv_arb_priority.val, 3, 3},
                {"slv.psram", &AXI_ICM.slv_arb_priority.val, 15, 3},
                {"arqos.cpu", &AXI_ICM.mst_arqos_reg0.val, 0, 4},
                {"arqos.cache", &AXI_ICM.mst_arqos_reg0.val, 4, 4},
                {"arqos.dma2d", &AXI_ICM.mst_arqos_reg0.val, 8, 4},
                {"arqos.gdma1", &AXI_ICM.mst_arqos_reg0.val, 12, 4},
                {"arqos.gdma2", &AXI_ICM.mst_arqos_reg0.val, 16, 4},
                {"awqos.cpu", &AXI_ICM.mst_awqos_reg0.val, 0, 4},
                {"awqos.cache", &AXI_ICM.mst_awqos_reg0.val, 4, 4},
                {"awqos.dma2d", &AXI_ICM.mst_awqos_reg0.val, 8, 4},
        };

        struct QosMaster {
                        const char *name;
                        uint8_t     id; // axi_icm_ll_master_id_t
        };
        constexpr QosMaster QosMasters[] = {{"cpu", AXI_ICM_MASTER_CPU},          {"cache", AXI_ICM_MASTER_CACHE},
                                            {"gdma1", AXI_ICM_MASTER_DW_GDMA_M0}, {"gdma2", AXI_ICM_MASTER_DW_GDMA_M1},
                                            {"pdma", AXI_ICM_MASTER_GDMA},        {"dma2d", AXI_ICM_MASTER_DMA2D}};

        // One regulator command: 0 the burstiness (data[23:16], data[0] on), 1 the rates.
        void qosCommand(uint32_t master, uint32_t write, uint32_t cmd, uint32_t data) {
                while (AXI_ICM_QOS.cmd.reg_axi_cmd_en) {}
                AXI_ICM_QOS.data.val = data;
                AXI_ICM_QOS.cmd.reg_axi_rd_wr_cmd = 1;
                AXI_ICM_QOS.cmd.reg_rd_wr_chan = write;
                AXI_ICM_QOS.cmd.reg_axi_master_port = master;
                AXI_ICM_QOS.cmd.reg_axi_cmd = cmd;
                AXI_ICM_QOS.cmd.reg_axi_cmd_en = 1;
                while (AXI_ICM_QOS.cmd.reg_axi_cmd_en) {}
                return;
        }

        // display qos | qos FIELD VALUE | qos limit MASTER r|w BURST PEAK RATE | qos limit MASTER r|w off
        int runQos(CommandContext &ctx, Args args) {
                if (args.size() == 2) {
                        for (const QosField &f : QosFields)
                                ctx.printf("%-12s %u\n", f.name,
                                           static_cast<unsigned>((*f.reg >> f.shift) & ((1u << f.width) - 1)));
                        return Shell::ExitSuccess;
                }
                if (args.size() == 4) {
                        for (const QosField &f : QosFields) {
                                if (std::strcmp(args[2], f.name) != 0) continue;
                                const auto     v = static_cast<uint32_t>(std::atoi(args[3]));
                                const uint32_t mask = ((1u << f.width) - 1) << f.shift;
                                *f.reg = (*f.reg & ~mask) | ((v << f.shift) & mask);
                                return Shell::ExitSuccess;
                        }
                        return ctx.usage();
                }
                if (args.size() >= 6 && std::strcmp(args[2], "limit") == 0) {
                        const QosMaster *m = nullptr;
                        for (const QosMaster &q : QosMasters)
                                if (std::strcmp(args[3], q.name) == 0) m = &q;
                        const uint32_t write = std::strcmp(args[4], "w") == 0 ? 1 : 0;
                        if (m == nullptr || (write == 0 && std::strcmp(args[4], "r") != 0)) return ctx.usage();
                        if (args.size() == 6 && std::strcmp(args[5], "off") == 0) {
                                qosCommand(m->id, write, 0, 0); // the regulator off
                                return Shell::ExitSuccess;
                        }
                        if (args.size() != 8) return ctx.usage();
                        const int burst = std::atoi(args[5]), peak = std::atoi(args[6]), rate = std::atoi(args[7]);
                        if (burst < 1 || burst > 256 || peak < 0 || peak >= rate || rate > 11) return ctx.usage();
                        // Levels: 0 is 1/2 of the cycles, 11 is 1/4096 (axi_icm_ll.h).
                        qosCommand(m->id, write, 1, (0x80000000u >> peak) + (0x8000u >> rate));
                        qosCommand(m->id, write, 0, uint32_t(burst - 1) << 16 | 1);
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

        int runDisplay(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showStatus(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "edid") == 0) return showEdid(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "scan") == 0) return scanBus(ctx);
                if (args.size() >= 3 && std::strcmp(args[1], "test") == 0) return runTest(ctx, args);
                if (args.size() == 2 && std::strcmp(args[1], "stats") == 0) return runStats(ctx);
                if (args.size() == 3 && std::strcmp(args[1], "resync") == 0 && scanoutObject.isCreated()) {
                        scanout().requestResync(std::atoi(args[2]));
                        return Shell::ExitSuccess;
                }
                if (args.size() == 3 && std::strcmp(args[1], "stress") == 0) return runStress(ctx, args[2]);
                if (args.size() == 2 && std::strcmp(args[1], "check") == 0) return runCheck(ctx);
                if (args.size() >= 2 && std::strcmp(args[1], "qos") == 0) return runQos(ctx, args);
                if (args.size() == 2 && std::strcmp(args[1], "verify") == 0) return runVerify(ctx);
                return ctx.usage();
        }

} // namespace

Error DisplayTest::init() {
#if CONFIG_BMDS_DISPLAY_TEST_AT_BOOT
        auto [b, err] = attachBridge();
        if (err == Error::NotSupported) return Error::Ok; // no display on this board
        if (err.isError()) return err;
        err = startTest(*b, TestModes[0].timing, Scanout::Format::Rgb888, 8, 3, false);
        if (err.isOk()) bmdsInfo("Display: 720p60 test picture on HDMI ('display test off' stops it)");
        return err;
#else
        return Error::Ok;
#endif
}

BMDS_SHELL_COMMAND(display, runDisplay,
                   "[edid|scan|check|stats|test 720|1080|WxH@HZ [cpu|pal|565|dma|fb|fb565] [lines] [blocks] "
                   "[strict]|test off|stress "
                   "psram|write|internal|qos [FIELD N|limit MASTER r|w BURST PEAK RATE|off]]",
                   "Show the HDMI bridge and the monitor; 'edid' dumps the monitor's EDID, 'scan' lists the bus, "
                   "'test' starts the test picture (bring-up), 'stats' shows the scanout's counters")

BMDS_NAMESPACE_END
