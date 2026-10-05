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

        int runDisplay(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return showStatus(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "edid") == 0) return showEdid(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "scan") == 0) return scanBus(ctx);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(
        display, runDisplay, "[edid|scan]",
        "Show the HDMI bridge and the monitor; 'edid' dumps the monitor's EDID, 'scan' lists the bus's devices")

BMDS_NAMESPACE_END
