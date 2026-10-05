/**
 * @file      wincommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `win` command: the desktop's windows from the serial shell
 * (devplan/windows.md W11). For now the scan-time composition prototype
 * (§4b): `win proto ...`.
 */

#include <cstdlib>
#include <cstring>
#include <sdkconfig.h>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/scancompositor.h>
#include <bmds/shell.h>
#include <bmds/staticobject.h>
#include <bmds/task.h>
#include <bmds/videomodes.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr const char *Owner = "window prototype";
        constexpr uint64_t    CyclesPerUs = 360; // the CPU clock

        constinit StaticObject<ScanCompositor> compositorObject;
        Display                               *protoDisplay = nullptr;

        ScanCompositor &compositor() {
                if (!compositorObject.isCreated()) compositorObject.create();
                return *compositorObject;
        }

        bool running(CommandContext &ctx) {
                if (protoDisplay != nullptr) return true;
                ctx.eprintf("win: no prototype running ('win proto start')\n");
                return false;
        }

        void stopProto() {
                if (protoDisplay == nullptr) return;
                protoDisplay->stop();
                protoDisplay->release();
                protoDisplay = nullptr;
                compositor().stop();
                return;
        }

        // win proto start [N] [565|888|mixed] [WxH@HZ]
        int startProto(CommandContext &ctx, Args args) {
                auto [d, derr] = Display::display(0);
                if (derr.isError()) {
                        ctx.eprintf("win: display 0: %s\n", derr.desc());
                        return Shell::ExitFailure;
                }
                if (d->framebuffer().isStarted() && protoDisplay == nullptr) {
                        ctx.eprintf("win: the display shows a framebuffer test; 'display test off' first\n");
                        return Shell::ExitFailure;
                }
                const int   count = args.size() > 3 ? std::atoi(args[3]) : 4;
                const char *kind = args.size() > 4 ? args[4] : "mixed";
                const char *modeText = args.size() > 5 ? args[5] : "1280x720@60";
                if (count < 0 || count > int(ScanCompositor::MaxLayers) ||
                    (std::strcmp(kind, "565") != 0 && std::strcmp(kind, "888") != 0 && std::strcmp(kind, "mixed") != 0))
                        return ctx.usage();
                auto [mode, merr] = VideoModes::parse(modeText);
                if (merr.isError()) {
                        ctx.eprintf("win: %s: not a mode (WIDTHxHEIGHT@HZ)\n", modeText);
                        return Shell::ExitUsage;
                }
                stopProto();
                const Error cerr = d->claim(Owner);
                if (cerr.isError()) {
                        ctx.eprintf("win: display 0 is in use by the %s\n", d->owner());
                        return Shell::ExitFailure;
                }
                ScanCompositor &c = compositor();
                Error           err = c.start(mode.hActive, mode.vActive, CONFIG_BMDS_DESKTOP_COLOR);
                for (int i = 0; i < count && err.isOk(); ++i) {
                        const bool wide =
                                std::strcmp(kind, "565") == 0 || (std::strcmp(kind, "mixed") == 0 && i % 2 == 0);
                        err = c.addLayer(wide ? ScanCompositor::Format::Rgb565 : ScanCompositor::Format::Rgb888, 400,
                                         300, 64 + i * 112, 48 + i * 64, static_cast<uint8_t>(i))
                                      .error();
                }
                if (err.isOk()) err = d->start(mode, c);
                if (err.isError()) {
                        ctx.eprintf("win: proto: %s\n", err.desc());
                        d->stop();
                        d->release();
                        c.stop();
                        return Shell::ExitFailure;
                }
                protoDisplay = d;
                const VideoTiming &t = d->timing();
                const uint32_t     mhz = t.refreshMilliHz();
                ctx.printf("Proto:    %ux%u at %u.%03u Hz, %u layers (%s), blocks of %u lines\n", t.hActive, t.vActive,
                           static_cast<unsigned>(mhz / 1000), static_cast<unsigned>(mhz % 1000),
                           static_cast<unsigned>(count), kind, d->blockLines());
                return Shell::ExitSuccess;
        }

        unsigned avgUs(uint64_t cycles, uint32_t n) {
                return n == 0 ? 0 : static_cast<unsigned>(cycles / n / CyclesPerUs);
        }

        unsigned maxUs(uint32_t cycles) {
                return static_cast<unsigned>(cycles / CyclesPerUs);
        }

        int showStats(CommandContext &ctx) {
                const ScanCompositor::Stats s = compositor().stats();
                const Scanout::Stats        o = protoDisplay->scanout().stats();
                const uint32_t              b = s.blocks;
                ctx.printf("Compose:  %u blocks, %u pieces (%u.%u a block, %u at most), %u jobs; %u stale, %u errors\n",
                           static_cast<unsigned>(b), static_cast<unsigned>(s.pieces),
                           static_cast<unsigned>(b == 0 ? 0 : s.pieces / b),
                           static_cast<unsigned>(b == 0 ? 0 : s.pieces * 10 / b % 10),
                           static_cast<unsigned>(s.piecesMax), static_cast<unsigned>(s.jobs),
                           static_cast<unsigned>(s.planFull), static_cast<unsigned>(s.errors));
                ctx.printf("Block:    %u us on average, %u at most\n", avgUs(s.blockCycles, b), maxUs(s.blockMax));
                ctx.printf("Parts:    plan %u/%u, fill %u/%u, copy %u/%u, blend %u/%u us (average a block/most)\n",
                           avgUs(s.planCycles, b), maxUs(s.planMax), avgUs(s.fillCycles, b), maxUs(s.fillMax),
                           avgUs(s.copyCycles, b), maxUs(s.copyMax), avgUs(s.blendCycles, b), maxUs(s.blendMax));
                const uint32_t lines = protoDisplay->blockLines();
                const uint64_t lineNs =
                        uint64_t(protoDisplay->timing().hTotal()) * 1'000'000 / protoDisplay->timing().pixelKhz;
                ctx.printf("Budget:   %u us a block (%u lines)\n", static_cast<unsigned>(lineNs * lines / 1000),
                           static_cast<unsigned>(lines));
                ctx.printf("Scanout:  %u frames, %u blocks, %u skipped, %u late, %u underruns\n",
                           static_cast<unsigned>(o.frames), static_cast<unsigned>(o.blocks),
                           static_cast<unsigned>(o.skipped), static_cast<unsigned>(o.late),
                           static_cast<unsigned>(o.underruns));
                return Shell::ExitSuccess;
        }

        int runCheck(CommandContext &ctx) {
                ScanCompositor &c = compositor();
                c.check();
                for (int i = 0; i < 100 && !c.checkResult().done; ++i) Task::sleep(1);
                const ScanCompositor::Check r = c.checkResult();
                if (!r.done) {
                        ctx.eprintf("win: check: no block composed\n");
                        return Shell::ExitFailure;
                }
                ctx.printf("check:    block at line %u: %u of %u pixels wrong", static_cast<unsigned>(r.first),
                           static_cast<unsigned>(r.wrong), static_cast<unsigned>(r.checked));
                if (r.wrong > 0)
                        ctx.printf(" (first at %u,%u: %06lx, not %06lx)", static_cast<unsigned>(r.x),
                                   static_cast<unsigned>(r.y), static_cast<unsigned long>(r.got),
                                   static_cast<unsigned long>(r.expected));
                ctx.printf("\n");
                return r.wrong == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int runProto(CommandContext &ctx, Args args) {
                if (args.size() < 3) return ctx.usage();
                const char *what = args[2];
                if (std::strcmp(what, "start") == 0) return startProto(ctx, args);
                if (std::strcmp(what, "stop") == 0) {
                        stopProto();
                        return Shell::ExitSuccess;
                }
                if (!running(ctx)) return Shell::ExitFailure;
                ScanCompositor &c = compositor();
                if (std::strcmp(what, "bg") == 0 && args.size() == 4) {
                        if (std::strcmp(args[3], "fill") == 0) {
                                c.setBackground(ScanCompositor::Background::Fill);
                        } else if (std::strcmp(args[3], "surface") == 0) {
                                c.setBackground(ScanCompositor::Background::Surface);
                        } else {
                                return ctx.usage();
                        }
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "jobs") == 0 && args.size() == 4) {
                        if (std::strcmp(args[3], "each") == 0) {
                                c.setJobs(ScanCompositor::Jobs::Each);
                        } else if (std::strcmp(args[3], "chain") == 0) {
                                c.setJobs(ScanCompositor::Jobs::Chained);
                        } else {
                                return ctx.usage();
                        }
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "sprite") == 0 && args.size() == 4 && std::strcmp(args[3], "off") == 0) {
                        c.setSprite(false, 0, 0);
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "sprite") == 0 && args.size() == 5) {
                        c.setSprite(true, std::atoi(args[3]), std::atoi(args[4]));
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "move") == 0 && args.size() == 6) {
                        c.moveLayer(size_t(std::atoi(args[3])), std::atoi(args[4]), std::atoi(args[5]));
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "drag") == 0 && args.size() == 6) {
                        c.dragLayer(size_t(std::atoi(args[3])), std::atoi(args[4]), std::atoi(args[5]));
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(what, "check") == 0) return runCheck(ctx);
                if (std::strcmp(what, "stats") == 0) {
                        if (args.size() == 4 && std::strcmp(args[3], "reset") == 0) {
                                c.resetStats();
                                return Shell::ExitSuccess;
                        }
                        return showStats(ctx);
                }
                return ctx.usage();
        }

        int runWin(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() >= 2 && std::strcmp(args[1], "proto") == 0) return runProto(ctx, args);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(win, runWin,
                   "proto start [N] [565|888|mixed] [WxH@HZ] | proto stop | proto bg surface|fill | "
                   "proto jobs each|chain | proto sprite X Y|off | proto move L X Y | proto drag L DX DY | "
                   "proto check | proto stats [reset]",
                   "The desktop's windows (for now the scan-time composition prototype)")

BMDS_NAMESPACE_END
