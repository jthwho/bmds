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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sdkconfig.h>
#include <cstring>
#include <esp_cache.h>
#include <esp_rom_sys.h>
#include <hal/axi_icm_ll.h>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/display.h>
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
#include <bmds/task.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr uint8_t      EdidAddress = 0x50;
        constexpr uint32_t     DdcHz = 100'000; // DDC's standard clock
        constexpr unsigned int TimeoutMs = 100;

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
                        // The frame count the stripe's place follows (4 pixels a frame).
                        void setFrame(uint32_t frame) {
                                _frame = frame;
                                return;
                        }

                        BMDS_HOT_ATTR void frame(uint8_t *fb, size_t stride) override {
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
                        uint16_t _width = 0;
                        uint16_t _height = 0;
                        uint32_t _frame = 0;
        };

        void                            stopAnim(); // `display anim`, below
        TestPattern                     testPattern;
        uint64_t                        testStartUs = 0;

        // The board's bridge, attached on first use.
        Result<Lt8912b *> attachBridge() {
                auto [d, err] = Display::display(0);
                if (err.isError()) return makeError<Lt8912b *>(err);
                return makeResult(&d->bridge());
        }

        // True once display 0 (its bridge) has answered; its parts are then there.
        bool hasDisplay() {
                return Display::display(0).isOk();
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

        // Display 0's parts: only once `hasDisplay()`.
        Framebuffer &frames() {
                return Display::display(0).value()->framebuffer();
        }

        Scanout &scanout() {
                return Display::display(0).value()->scanout();
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

        Error startTest(Lt8912b &b, const VideoTiming &timing, Scanout::Format format, uint16_t lines, uint8_t blocks,
                        bool framebuffer, bool fb565 = false) {
                stopAnim();
                scanout().stop();
                frames().stop();
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
                if (err.isOk() && fb565) {
                        // Both buffers drawn once, the stripe in a different place in each
                        // (`display present` shows the swaps), and written back in pieces
                        // (a cache operation runs with interrupts off).
                        Framebuffer &f = frames();
                        err = f.start(c.timing.hActive, c.timing.vActive);
                        const size_t bytes = f.stride() * f.height();
                        for (int i = 0; i < 2 && err.isOk(); ++i) {
                                uint8_t *buffer = i == 0 ? f.front() : f.back();
                                testPattern.setFrame(i == 0 ? 0 : 100);
                                testPattern.render(0, f.height(), buffer, f.stride());
                                for (size_t off = 0; off < bytes; off += 64 * 1024)
                                        (void)esp_cache_msync(buffer + off,
                                                              bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                                              ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                        }
                        if (err.isOk()) err = scanout().start(c, f);
                } else if (err.isOk()) {
                        err = scanout().start(c, testPattern);
                }
                if (err.isError()) return err;
                testStartUs = System::uptimeUs();
                return b.resetReceiver();
        }

        // The fewest lines a block worth taking by default: every block costs a
        // fixed amount besides its lines (devplan/display.md §8).
        constexpr uint16_t MinBlockLines = 4;

        // display test 720|1080|WxH@Hz [cpu|pal|565|dma|fb|fb565] [lines per block] [blocks] | off
        int runTest(CommandContext &ctx, Args args) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr || isClaimed(ctx)) return Shell::ExitFailure;
                if (std::strcmp(args[2], "off") == 0) {
                        stopAnim();
                        scanout().stop();
                        frames().stop();
                        (void)b->setOutput(false);
                        return Shell::ExitSuccess;
                }
                // Knobs anywhere after the mode: pack=N, lane24, strict (the ring stops for a late block).
                testPacking = 0;
                testFullLane = false;
                testFree = true;
                size_t positional = args.size();
                for (size_t i = 3; i < args.size(); ++i) {
                        const bool pack = std::strncmp(args[i], "pack=", 5) == 0;
                        if (pack) testPacking = static_cast<uint8_t>(std::atoi(args[i] + 5));
                        const bool lane24 = std::strcmp(args[i], "lane24") == 0;
                        const bool strict = std::strcmp(args[i], "strict") == 0;
                        if (lane24) testFullLane = true;
                        if (strict) testFree = false;
                        if ((pack || lane24 || strict) && i < positional) positional = i;
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
                // Lines a block, unless given: the most up to 24 for the framebuffer's
                // ring (devplan/display.md V11), 8 otherwise, that divide the height.
                uint16_t lines = 0;
                if (args.size() > 4) {
                        lines = static_cast<uint16_t>(std::atoi(args[4]));
                } else {
                        const uint16_t most = fb565 ? 24 : 8;
                        for (lines = most; lines > 0 && timing.vActive % lines != 0; --lines) {}
                        if (lines < MinBlockLines) {
                                ctx.eprintf("display: no block of %u to %u lines divides %u lines; give one\n",
                                            MinBlockLines, most, timing.vActive);
                                return Shell::ExitFailure;
                        }
                }
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
                if (!hasDisplay() || !scanout().isRunning()) {
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
        // widened from (`display test MODE fb565`): every line one of the
        // picture's two kinds, each RGB888 pixel's top bits its RGB565 pixel's.
        int runVerify(CommandContext &ctx) {
                if (!hasDisplay() || !frames().isStarted() || !scanout().isRunning()) {
                        ctx.eprintf("display: verify needs `display test MODE fb565`\n");
                        return Shell::ExitFailure;
                }
                const VideoTiming &t = runningTiming;
                const size_t       ringBytes = scanout().ringBytes();
                const uint8_t     *ring = scanout().ring();
                (void)esp_cache_msync(const_cast<uint8_t *>(ring), ringBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                const size_t   stride888 = size_t(t.hActive) * 3;
                const uint8_t *front = frames().front();
                const uint8_t *kinds[2] = {front, front + size_t(t.vActive - 1) * t.hActive * 2};
                uint32_t       lowBits = 0;
                // One of the picture's two kinds of line, each pixel's top bits right.
                auto matches = [&](const uint8_t *out) {
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
                                if (x == t.hActive) return true;
                        }
                        return false;
                };
                size_t lines = 0, bad = 0, inFlight = 0;
                for (size_t off = 0; off + stride888 <= ringBytes; off += stride888, ++lines) {
                        const uint8_t *out = ring + off;
                        if (matches(out)) continue;
                        // The line the 2D-DMA is writing is part old, part new: looked at
                        // again once it's done (a line takes about 11 us), it's right.
                        esp_rom_delay_us(50);
                        (void)esp_cache_msync(const_cast<uint8_t *>(out), stride888, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        if (matches(out)) {
                                ++inFlight;
                                continue;
                        }
                        if (bad == 0)
                                ctx.printf("first bad line %u: out %02x %02x %02x\n", static_cast<unsigned>(lines),
                                           out[0], out[1], out[2]);
                        ++bad;
                }
                ctx.printf("verify: %u of %u ring lines wrong (%u more being written, right 50 us later); low bits "
                           "seen %06lx\n",
                           static_cast<unsigned>(bad), static_cast<unsigned>(lines), static_cast<unsigned>(inFlight),
                           static_cast<unsigned long>(lowBits));
                return bad == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        // Bring-up: `display present [N]` swaps the test framebuffer N times,
        // timing each present (it waits for the next frame's start).
        int runPresent(CommandContext &ctx, const char *count) {
                if (isClaimed(ctx)) return Shell::ExitFailure;
                if (!hasDisplay() || !frames().isStarted() || !scanout().isRunning()) {
                        ctx.eprintf("display: present needs `display test MODE fb565`\n");
                        return Shell::ExitFailure;
                }
                const int n = count == nullptr ? 1 : std::atoi(count);
                uint64_t  total = 0, longest = 0;
                for (int i = 0; i < n && !ctx.isInterrupted(); ++i) {
                        const uint64_t start = System::uptimeUs();
                        const Error    err = frames().present(100);
                        const uint64_t us = System::uptimeUs() - start;
                        if (err.isError()) {
                                ctx.eprintf("display: present: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                        total += us;
                        if (us > longest) longest = us;
                }
                ctx.printf("present: %d swaps, %llu us each on average, %llu at most (%u swaps in all)\n", n,
                           static_cast<unsigned long long>(n > 0 ? total / n : 0),
                           static_cast<unsigned long long>(longest), static_cast<unsigned>(frames().swaps()));
                return Shell::ExitSuccess;
        }


        // Bring-up: a moving picture drawn the planned way (devplan/display.md §5b
        // step 2): squares bouncing over the bars and ramp, each frame's changes
        // drawn into a small strip in internal RAM, blitted into the back
        // buffer, then presented. With `copy`, the back buffer is first brought
        // up to date from the front in full (a full-frame copy's cost).
        class AnimTest {
                public:
                        static constexpr int MaxSquares = 32;

                        Error start(Framebuffer &f, int squares, bool copy) {
                                if (_running) return Error::InvalidState;
                                _f = &f;
                                _count = squares < 1 ? 1 : squares > MaxSquares ? MaxSquares : squares;
                                _copy = copy;
                                const size_t w = f.width();
                                _templates = static_cast<uint16_t *>(Memory::alloc(MemSpace::Internal, 2 * w * 2));
                                _strip = static_cast<uint16_t *>(
                                        Memory::allocAligned(MemSpace::InternalDma, StripBytes, 64));
                                if (_templates == nullptr || _strip == nullptr) {
                                        freeAll();
                                        return Error::NoMem;
                                }
                                static constexpr uint32_t Bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                                     0xff00ff, 0xff0000, 0x0000ff, 0x000000};
                                for (size_t x = 0; x < w; ++x) {
                                        _templates[x] = rgb565(Bars[x * 8 / w]);
                                        _templates[w + x] = rgb565(static_cast<uint32_t>(x * 256 / w) * 0x010101u);
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        q.x = static_cast<int>((i * 157) % (f.width() - Size)) & ~1;
                                        q.y = static_cast<int>((i * 89) % (f.height() - Size));
                                        q.dx = (i % 2 ? 4 : -2) * (1 + i % 3 / 2);
                                        q.dy = i % 3 ? 3 : -2;
                                        q.color = rgb565(Colors[i % 6]);
                                        q.px[0] = q.px[1] = q.x;
                                        q.py[0] = q.py[1] = q.y;
                                }
                                _stats = Stats();
                                _stopping.store(false);
                                _exited.store(false);
                                // Both buffers to the plain background first.
                                for (int b = 0; b < 2; ++b) {
                                        const Error err = background();
                                        if (err.isError()) {
                                                freeAll();
                                                return err;
                                        }
                                        (void)f.present(100);
                                }
                                Task::Config tc;
                                tc.name = "anim";
                                tc.stackBytes = 4096;
                                tc.priority = 5;
                                tc.core = 0;
                                const Error err = _task.start(tc, entry, this);
                                if (err.isError()) {
                                        freeAll();
                                        return err;
                                }
                                _running = true;
                                return Error::Ok;
                        }

                        void stop() {
                                halt();
                                freeAll();
                                return;
                        }

                        // Stops drawing (after the frame under way), keeping what a check needs.
                        void halt() {
                                if (!_running) return;
                                _stopping.store(true);
                                for (int i = 0; i < 100 && !_exited.load(); ++i) Task::sleep(5);
                                (void)_task.stop();
                                _running = false;
                                return;
                        }

                        // After `halt()`: every pixel of the front buffer against the picture
                        // last presented (the background, the squares where they were drawn).
                        void check(CommandContext &ctx) const {
                                if (_templates == nullptr) return;
                                uint8_t     *front = _f->front();
                                const size_t bytes = _f->stride() * _f->height();
                                for (size_t off = 0; off < bytes; off += 64 * 1024)
                                        (void)esp_cache_msync(front + off,
                                                              bytes - off < 64 * 1024 ? bytes - off : 64 * 1024,
                                                              ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                                const auto *px = reinterpret_cast<const uint16_t *>(front);
                                size_t      wrong = 0;
                                for (int y = 0; y < _f->height(); ++y) {
                                        for (int x = 0; x < _f->width(); ++x) {
                                                uint16_t want = line(y)[x];
                                                for (int j = 0; j < _count; ++j) {
                                                        const Square &o = _squares[j];
                                                        if (x >= o.px[1] && x < o.px[1] + Size && y >= o.py[1] &&
                                                            y < o.py[1] + Size)
                                                                want = o.color;
                                                }
                                                const uint16_t got = px[size_t(y) * _f->width() + x];
                                                if (got == want) continue;
                                                if (wrong == 0)
                                                        ctx.printf("check:    first wrong pixel (%d, %d): %04x, not "
                                                                   "%04x\n",
                                                                   x, y, got, want);
                                                ++wrong;
                                        }
                                }
                                ctx.printf("check:    %u of %u pixels wrong\n", static_cast<unsigned>(wrong),
                                           static_cast<unsigned>(size_t(_f->width()) * _f->height()));
                                return;
                        }

                        bool isRunning() const { return _running; }

                        void report(CommandContext &ctx) const {
                                const Stats   &s = _stats;
                                const uint64_t us = System::uptimeUs() - s.startUs;
                                ctx.printf(
                                        "anim:     %d squares%s, %u frames in %llu ms (%llu.%01llu fps), %u errors\n",
                                        _count, _copy ? ", full copy each frame" : "", static_cast<unsigned>(s.frames),
                                        static_cast<unsigned long long>(us / 1000),
                                        static_cast<unsigned long long>(us ? uint64_t(s.frames) * 1000000 / us : 0),
                                        static_cast<unsigned long long>(us ? uint64_t(s.frames) * 10000000 / us % 10
                                                                           : 0),
                                        static_cast<unsigned>(s.errors));
                                const unsigned f = s.frames ? s.frames : 1;
                                ctx.printf(
                                        "draw:     %llu us a frame on average (%llu blitting %u bytes), %llu at most\n",
                                        static_cast<unsigned long long>(s.drawUs / f),
                                        static_cast<unsigned long long>(s.blitUs / f),
                                        static_cast<unsigned>(s.blitBytes / f),
                                        static_cast<unsigned long long>(s.drawUsMax));
                                if (_copy)
                                        ctx.printf("copy:     %llu us a frame on average, %llu at most (%u MB/s)\n",
                                                   static_cast<unsigned long long>(s.copyUs / f),
                                                   static_cast<unsigned long long>(s.copyUsMax),
                                                   static_cast<unsigned>(s.copyUs ? uint64_t(_f->stride()) *
                                                                                            _f->height() * s.frames /
                                                                                            s.copyUs
                                                                                  : 0));
                                ctx.printf("present:  %llu us waiting on average, %llu at most\n",
                                           static_cast<unsigned long long>(s.presentUs / f),
                                           static_cast<unsigned long long>(s.presentUsMax));
                                return;
                        }

                private:
                        static constexpr int      Size = 64; // a square's side
                        static constexpr size_t   StripBytes = 1280 * 16 * 2;
                        static constexpr uint32_t Colors[6] = {0xff4040, 0x40ff40, 0x4040ff,
                                                               0xffff40, 0x40ffff, 0xff40ff};

                        struct Square {
                                        int      x = 0, y = 0, dx = 0, dy = 0;
                                        int      px[2] = {}, py[2] = {}; // the last two frames' places
                                        uint16_t color = 0;
                        };

                        struct Stats {
                                        uint64_t startUs = System::uptimeUs();
                                        uint32_t frames = 0;
                                        uint32_t errors = 0;
                                        uint64_t drawUs = 0, blitUs = 0, copyUs = 0, presentUs = 0;
                                        uint64_t drawUsMax = 0, copyUsMax = 0, presentUsMax = 0;
                                        uint64_t blitBytes = 0;
                        };

                        static uint16_t rgb565(uint32_t rgb) {
                                return static_cast<uint16_t>((rgb >> 19 & 31) << 11 | (rgb >> 10 & 63) << 5 |
                                                             (rgb >> 3 & 31));
                        }

                        static void entry(void *arg) {
                                auto *self = static_cast<AnimTest *>(arg);
                                while (!self->_stopping.load()) self->frame();
                                self->_exited.store(true);
                                for (;;) Task::sleep(1000);
                        }

                        // The plain background into the back buffer, 16 lines a blit.
                        Error background() {
                                const uint16_t w = _f->width();
                                for (uint16_t y = 0; y < _f->height(); y += 16) {
                                        const uint16_t h =
                                                static_cast<uint16_t>(_f->height() - y < 16 ? _f->height() - y : 16);
                                        for (uint16_t r = 0; r < h; ++r)
                                                std::memcpy(_strip + size_t(r) * w, line(y + r), size_t(w) * 2);
                                        Rect to;
                                        to.y = y;
                                        to.width = w;
                                        to.height = h;
                                        const Error err =
                                                _f->blit(to, reinterpret_cast<uint8_t *>(_strip), size_t(w) * 2, 100);
                                        if (err.isError()) return err;
                                }
                                return Error::Ok;
                        }

                        const uint16_t *line(int y) const {
                                return _templates + (y < _f->height() * 3 / 4 ? 0 : _f->width());
                        }

                        // One frame: move the squares; in the back buffer (two frames old)
                        // redraw each square's area from then to now; present.
                        void frame() {
                                const uint64_t start = System::uptimeUs();
                                if (_copy) {
                                        Rect all;
                                        all.width = _f->width();
                                        all.height = _f->height();
                                        if (_f->copyFromFront(all, 100).isError()) ++_stats.errors;
                                        const uint64_t c = System::uptimeUs() - start;
                                        _stats.copyUs += c;
                                        if (c > _stats.copyUsMax) _stats.copyUsMax = c;
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        q.x += q.dx;
                                        q.y += q.dy;
                                        if (q.x < 0 || q.x > _f->width() - Size) {
                                                q.dx = -q.dx;
                                                q.x += 2 * q.dx;
                                        }
                                        if (q.y < 0 || q.y > _f->height() - Size) {
                                                q.dy = -q.dy;
                                                q.y += 2 * q.dy;
                                        }
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        // The area: where the square was two frames ago (what the back
                                        // buffer shows) and where it is now; x and width even.
                                        const int x0 = (q.x < q.px[0] ? q.x : q.px[0]) & ~1;
                                        const int y0 = q.y < q.py[0] ? q.y : q.py[0];
                                        int       x1 = (q.x > q.px[0] ? q.x : q.px[0]) + Size;
                                        const int y1 = (q.y > q.py[0] ? q.y : q.py[0]) + Size;
                                        x1 = (x1 + 1) & ~1;
                                        const int w = x1 - x0, h = y1 - y0;
                                        if (size_t(w) * h * 2 > StripBytes) continue;
                                        for (int r = 0; r < h; ++r) {
                                                uint16_t *row = _strip + size_t(r) * w;
                                                std::memcpy(row, line(y0 + r) + x0, size_t(w) * 2);
                                                for (int j = 0; j < _count; ++j) {
                                                        const Square &o = _squares[j];
                                                        if (y0 + r < o.y || y0 + r >= o.y + Size) continue;
                                                        const int a = o.x > x0 ? o.x : x0;
                                                        const int b = o.x + Size < x1 ? o.x + Size : x1;
                                                        for (int x = a; x < b; ++x) row[x - x0] = o.color;
                                                }
                                        }
                                        Rect to;
                                        to.x = static_cast<uint16_t>(x0);
                                        to.y = static_cast<uint16_t>(y0);
                                        to.width = static_cast<uint16_t>(w);
                                        to.height = static_cast<uint16_t>(h);
                                        const uint64_t b0 = System::uptimeUs();
                                        if (_f->blit(to, reinterpret_cast<uint8_t *>(_strip), size_t(w) * 2, 100)
                                                    .isError())
                                                ++_stats.errors;
                                        _stats.blitUs += System::uptimeUs() - b0;
                                        _stats.blitBytes += size_t(w) * h * 2;
                                }
                                for (int i = 0; i < _count; ++i) {
                                        Square &q = _squares[i];
                                        q.px[0] = q.px[1];
                                        q.py[0] = q.py[1];
                                        q.px[1] = q.x;
                                        q.py[1] = q.y;
                                }
                                const uint64_t drawn = System::uptimeUs();
                                const uint64_t d = drawn - start;
                                _stats.drawUs += d;
                                if (d > _stats.drawUsMax) _stats.drawUsMax = d;
                                if (_f->present(100).isError()) ++_stats.errors;
                                const uint64_t p = System::uptimeUs() - drawn;
                                _stats.presentUs += p;
                                if (p > _stats.presentUsMax) _stats.presentUsMax = p;
                                ++_stats.frames;
                                return;
                        }

                        void freeAll() {
                                Memory::free(_templates);
                                Memory::free(_strip);
                                _templates = nullptr;
                                _strip = nullptr;
                                return;
                        }

                        Framebuffer      *_f = nullptr;
                        Square            _squares[MaxSquares];
                        int               _count = 0;
                        bool              _copy = false;
                        bool              _running = false;
                        uint16_t         *_templates = nullptr;
                        uint16_t         *_strip = nullptr;
                        Stats             _stats;
                        std::atomic<bool> _stopping{false};
                        std::atomic<bool> _exited{false};
                        Task              _task;
        };

        constinit StaticObject<AnimTest> animObject;

        AnimTest &anim() {
                if (!animObject.isCreated()) animObject.create();
                return *animObject;
        }

        void stopAnim() {
                if (animObject.isCreated()) anim().stop();
                return;
        }

        // display anim start [N] [copy] | anim stop | anim
        int runAnim(CommandContext &ctx, Args args) {
                if (args.size() >= 3 && std::strcmp(args[2], "stop") == 0) {
                        if (animObject.isCreated() && anim().isRunning()) {
                                anim().report(ctx);
                                anim().halt();
                                anim().check(ctx);
                        }
                        if (animObject.isCreated()) anim().stop();
                        return Shell::ExitSuccess;
                }
                if (args.size() >= 3 && std::strcmp(args[2], "start") == 0) {
                        if (isClaimed(ctx)) return Shell::ExitFailure;
                        if (!hasDisplay() || !frames().isStarted() || !scanout().isRunning()) {
                                ctx.eprintf("display: anim needs `display test MODE fb565`\n");
                                return Shell::ExitFailure;
                        }
                        const int   n = args.size() > 3 ? std::atoi(args[3]) : 8;
                        const bool  copy = args.size() > 4 && std::strcmp(args[4], "copy") == 0;
                        const Error err = anim().start(frames(), n, copy);
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
                if (args.size() == 2 && std::strcmp(args[1], "edid") == 0) return showEdid(ctx, false);
                if (args.size() == 3 && std::strcmp(args[1], "edid") == 0 && std::strcmp(args[2], "raw") == 0)
                        return showEdid(ctx, true);
                if (args.size() == 2 && std::strcmp(args[1], "scan") == 0) return scanBus(ctx);
                if (args.size() >= 3 && std::strcmp(args[1], "test") == 0) return runTest(ctx, args);
                if (args.size() == 2 && std::strcmp(args[1], "stats") == 0) return runStats(ctx);
                if (args.size() == 3 && std::strcmp(args[1], "resync") == 0 && hasDisplay()) {
                        scanout().requestResync(std::atoi(args[2]));
                        return Shell::ExitSuccess;
                }
                if (args.size() == 3 && std::strcmp(args[1], "stress") == 0) return runStress(ctx, args[2]);
                if (args.size() == 2 && std::strcmp(args[1], "check") == 0) return runCheck(ctx);
                if (args.size() >= 2 && std::strcmp(args[1], "qos") == 0) return runQos(ctx, args);
                if (args.size() == 2 && std::strcmp(args[1], "verify") == 0) return runVerify(ctx);
                if (args.size() >= 2 && std::strcmp(args[1], "anim") == 0) return runAnim(ctx, args);
                if (args.size() <= 3 && args.size() >= 2 && std::strcmp(args[1], "present") == 0)
                        return runPresent(ctx, args.size() == 3 ? args[2] : nullptr);
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
                   "[edid [raw]|scan|check|stats|test 720|1080|WxH@HZ [cpu|pal|565|dma|fb|fb565] [lines] [blocks] "
                   "[strict]|test off|present [N]|verify|anim [start [N] [copy]|stop]|stress "
                   "psram|write|internal|qos [FIELD N|limit MASTER r|w BURST PEAK RATE|off]]",
                   "Show the HDMI bridge and the monitor; 'edid' decodes the monitor's EDID ('raw': its bytes), 'scan' "
                   "lists the bus, "
                   "'test' starts the test picture (bring-up), 'stats' shows the scanout's counters")

BMDS_NAMESPACE_END
