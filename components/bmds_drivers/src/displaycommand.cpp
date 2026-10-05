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

#include <cstring>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/edid.h>
#include <bmds/i2cbus.h>
#include <bmds/lt8912b.h>
#include <bmds/shell.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr uint8_t      EdidAddress = 0x50;
        constexpr uint32_t     DdcHz = 100'000; // DDC's standard clock
        constexpr unsigned int TimeoutMs = 100;

        constinit StaticObject<Lt8912b> bridgeObject;

        // Step 1b's bring-up (devplan/display.md): IDF's DPI panel showing the
        // DSI controller's own bar pattern, to prove the bridge setup. The
        // line-buffer scanout replaces it.
        struct TestMode {
                        const char *name;
                        VideoTiming timing;
        };
        constexpr TestMode TestModes[] = {
                // Espressif's timings: pixel clocks the DPI clock divides from its PLL.
                {"720", {80'000, 1280, 408, 32, 80, 720, 3, 5, 13, true, false, false}},
                {"1080", {70'000, 1920, 48, 32, 80, 1080, 3, 5, 8, true, false, false}},
        };
        esp_ldo_channel_handle_t testLdo = nullptr;
        esp_lcd_dsi_bus_handle_t testBus = nullptr;
        esp_lcd_panel_handle_t   testPanel = nullptr;

        void stopTest() {
                if (testPanel != nullptr) (void)esp_lcd_panel_del(testPanel);
                if (testBus != nullptr) (void)esp_lcd_del_dsi_bus(testBus);
                if (testLdo != nullptr) (void)esp_ldo_release_channel(testLdo);
                testPanel = nullptr;
                testBus = nullptr;
                testLdo = nullptr;
                return;
        }

        // The board's bridge, attached on first use; null (with a message) if none.
        Lt8912b *bridge(CommandContext &ctx) {
                const BoardDisplay &d = board().display;
                if (d.bridge != BoardDisplayBridge::Lt8912b) {
                        ctx.eprintf("display: this board has no HDMI bridge\n");
                        return nullptr;
                }
                if (!bridgeObject.isCreated()) {
                        I2cBus *bus = I2cBus::bus(static_cast<size_t>(d.i2c));
                        if (bus == nullptr) {
                                ctx.eprintf("display: no I2C bus %u\n", static_cast<unsigned>(d.i2c));
                                return nullptr;
                        }
                        Lt8912b    &b = bridgeObject.create(*bus, d.i2cAddr);
                        const Error err = b.attach();
                        if (err.isError()) {
                                ctx.eprintf("display: %s\n", err.desc());
                                return nullptr;
                        }
                }
                return &*bridgeObject;
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

        int runTest(CommandContext &ctx, const char *which) {
                Lt8912b *b = bridge(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                stopTest();
                if (std::strcmp(which, "off") == 0) {
                        (void)b->setOutput(false);
                        return Shell::ExitSuccess;
                }
                const TestMode *mode = nullptr;
                for (const TestMode &m : TestModes) {
                        if (std::strcmp(m.name, which) == 0) mode = &m;
                }
                if (mode == nullptr) return ctx.usage();
                const VideoTiming       &t = mode->timing;
                const BoardDisplay      &d = board().display;
                const uint32_t           laneMbps = (t.pixelKhz * 24 / d.dsiLanes + 999) / 1000;
                esp_err_t                err = ESP_OK;
                const char              *step = "";
                esp_ldo_channel_config_t ldo = {};
                ldo.chan_id = d.phyLdo;
                ldo.voltage_mv = d.phyLdoMv;
                step = "LDO";
                err = esp_ldo_acquire_channel(&ldo, &testLdo);
                if (err == ESP_OK) {
                        esp_lcd_dsi_bus_config_t bus = {};
                        bus.bus_id = 0;
                        bus.num_data_lanes = d.dsiLanes;
                        // 0: the driver picks by chip revision (the crystal needs v3.0).
                        bus.phy_clk_src = static_cast<mipi_dsi_phy_pllref_clock_source_t>(0);
                        bus.lane_bit_rate_mbps = laneMbps;
                        step = "DSI bus";
                        err = esp_lcd_new_dsi_bus(&bus, &testBus);
                }
                if (err == ESP_OK) {
                        const Error berr = b->configure(t, d.dsiLanes, true);
                        if (berr.isError()) {
                                ctx.eprintf("display: bridge: %s\n", berr.desc());
                                stopTest();
                                return Shell::ExitFailure;
                        }
                        esp_lcd_dpi_panel_config_t panel = {};
                        panel.virtual_channel = 0;
                        panel.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
                        panel.dpi_clock_freq_mhz = t.pixelKhz / 1000;
                        panel.in_color_format = LCD_COLOR_FMT_RGB565;
                        panel.out_color_format = LCD_COLOR_FMT_RGB888;
                        panel.num_fbs = 1;
                        panel.video_timing.h_size = t.hActive;
                        panel.video_timing.v_size = t.vActive;
                        panel.video_timing.hsync_back_porch = t.hBack;
                        panel.video_timing.hsync_pulse_width = t.hSync;
                        panel.video_timing.hsync_front_porch = t.hFront;
                        panel.video_timing.vsync_back_porch = t.vBack;
                        panel.video_timing.vsync_pulse_width = t.vSync;
                        panel.video_timing.vsync_front_porch = t.vFront;
                        panel.flags.disable_lp = true;
                        step = "DPI panel";
                        err = esp_lcd_new_panel_dpi(testBus, &panel, &testPanel);
                }
                if (err == ESP_OK) {
                        step = "pattern";
                        err = esp_lcd_dpi_panel_set_pattern(testPanel, MIPI_DSI_PATTERN_BAR_VERTICAL);
                }
                if (err == ESP_OK) {
                        step = "start";
                        err = esp_lcd_panel_init(testPanel);
                }
                if (err != ESP_OK) {
                        ctx.eprintf("display: %s: %s\n", step, esp_err_to_name(err));
                        stopTest();
                        return Shell::ExitFailure;
                }
                (void)b->resetReceiver();
                ctx.printf("Test:     %ux%u, %u.%03u MHz, lanes at %u Mbps, vertical bars\n", t.hActive, t.vActive,
                           static_cast<unsigned>(t.pixelKhz / 1000), static_cast<unsigned>(t.pixelKhz % 1000),
                           static_cast<unsigned>(laneMbps));
                return Shell::ExitSuccess;
        }

        // Bring-up diagnostics: I2C read reliability at two clocks, and what the
        // bridge sees of the DSI input (main 0x9c-0x9f: the measured sync counts).
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

        int runDisplay(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showStatus(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "edid") == 0) return showEdid(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "scan") == 0) return scanBus(ctx);
                if (args.size() == 3 && std::strcmp(args[1], "test") == 0) return runTest(ctx, args[2]);
                if (args.size() == 2 && std::strcmp(args[1], "check") == 0) return runCheck(ctx);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(
        display, runDisplay, "[edid|scan]",
        "Show the HDMI bridge and the monitor; 'edid' dumps the monitor's EDID, 'scan' lists the bus's devices")

BMDS_NAMESPACE_END
