/**
 * @file      usbcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `usb` command: the devices on the USB host (devplan/input.md). Its
 * output format is a userspace interface once shipped: keep the columns and
 * their order, add new ones only at the end.
 */

#include <cstdio>
#include <cstring>
#include <bmds/command.h>
#include <bmds/shell.h>
#include <bmds/usbhost.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A class code's short name (USB-IF base classes).
        const char *className(uint8_t cls, uint8_t subclass, uint8_t protocol) {
                switch (cls) {
                        case 0x01: return "audio";
                        case 0x02: return "cdc";
                        case 0x03:
                                if (subclass == 1 && protocol == 1) return "keyboard";
                                if (subclass == 1 && protocol == 2) return "mouse";
                                return "hid";
                        case 0x06: return "image";
                        case 0x07: return "printer";
                        case 0x08: return "storage";
                        case 0x09: return "hub";
                        case 0x0a: return "cdc-data";
                        case 0x0b: return "smartcard";
                        case 0x0e: return "video";
                        case 0xe0: return "wireless";
                        case 0xef: return "misc";
                        case 0xfe: return "app";
                        case 0xff: return "vendor";
                        default: return "?";
                }
        }

        int showDevices(CommandContext &ctx, UsbHost &host) {
                UsbHost::DeviceInfo devices[UsbHost::MaxDevices];
                const size_t        n = host.devices(Span<UsbHost::DeviceInfo>(devices, UsbHost::MaxDevices));
                ctx.printf("ADDR PORT  SPEED VID:PID   CLASS             NAME\n");
                for (size_t i = 0; i < n; ++i) {
                        const UsbHost::DeviceInfo &d = devices[i];
                        char                       port[8] = "root";
                        if (d.parent != 0) std::snprintf(port, sizeof(port), "%u.%u", d.parent, d.port);
                        // The device's class, else its interfaces' (comma separated).
                        char classes[48] = {};
                        if (d.cls != 0 || d.interfaceCount == 0) {
                                std::snprintf(classes, sizeof(classes), "%s", className(d.cls, 0, 0));
                        } else {
                                size_t used = 0;
                                for (size_t k = 0; k < d.interfaceCount && used < sizeof(classes); ++k) {
                                        const UsbHost::Interface &f = d.interfaces[k];
                                        const int w = std::snprintf(classes + used, sizeof(classes) - used, "%s%s",
                                                                    k == 0 ? "" : ",",
                                                                    className(f.cls, f.subclass, f.protocol));
                                        if (w < 0) break;
                                        used += static_cast<size_t>(w);
                                }
                        }
                        ctx.printf("%-4u %-5s %-5s %04x:%04x %-17s %s%s%s\n", d.address, port,
                                   UsbHost::speedName(d.speed), d.vendor, d.product, classes, d.manufacturer,
                                   d.manufacturer[0] != '\0' && d.name[0] != '\0' ? " " : "", d.name);
                }
                return Shell::ExitSuccess;
        }

        int showStats(CommandContext &ctx, UsbHost &host) {
                const UsbHost::Stats s = host.stats();
                ctx.printf("Devices: %lu added, %lu gone\n", static_cast<unsigned long>(s.added),
                           static_cast<unsigned long>(s.gone));
                ctx.printf("HID: %lu keyboards, %lu mice, %lu reports\n", static_cast<unsigned long>(s.keyboards),
                           static_cast<unsigned long>(s.mice), static_cast<unsigned long>(s.hidReports));
                // Each interface: waiting for a report (armed) or not, and since its last one. A
                // device that stays silent while used, armed, lost its transfer below the driver.
                UsbHost::HidState hids[8];
                const size_t      n = host.hids(Span<UsbHost::HidState>(hids, 8));
                for (size_t i = 0; i < n; ++i)
                        ctx.printf("  %u.%u %-8s %s, %lu reports, the last %lu ms ago\n", hids[i].address,
                                   hids[i].instance, hids[i].keyboard ? "keyboard" : "mouse",
                                   hids[i].armed ? "waiting" : "not waiting",
                                   static_cast<unsigned long>(hids[i].reports),
                                   static_cast<unsigned long>(hids[i].sinceMs));
                ctx.printf("Retried: %lu HID requests refused, %lu controller halts of no known cause",
                           static_cast<unsigned long>(s.rearmFailures), static_cast<unsigned long>(s.unhandledHalts));
                if (s.unhandledHalts != 0)
                        ctx.printf(" (the last: hcint 0x%08lx)", static_cast<unsigned long>(s.unhandledHcint));
                ctx.printf("\n");
                // A channel taken with its interrupt off is a transfer nobody will finish.
                const uint32_t lost = s.channelsTaken & ~s.channelInterrupts;
                ctx.printf("Channels: 0x%04lx taken, 0x%04lx interrupting", static_cast<unsigned long>(s.channelsTaken),
                           static_cast<unsigned long>(s.channelInterrupts));
                if (lost != 0) ctx.printf(" (0x%04lx taken but not heard: stuck)", static_cast<unsigned long>(lost));
                ctx.printf("\n");
                return Shell::ExitSuccess;
        }

        int runUsb(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                auto [host, err] = UsbHost::host(0);
                if (err.isError()) {
                        ctx.eprintf("usb: %s\n",
                                    err == Error::NotSupported ? "this board has no USB host" : err.desc());
                        return Shell::ExitFailure;
                }
                if (!host->isRunning()) {
                        ctx.eprintf("usb: host 0 is not running\n");
                        return Shell::ExitFailure;
                }
                if (args.size() == 1) return showDevices(ctx, *host);
                if (args.size() == 2 && std::strcmp(args[1], "stats") == 0) return showStats(ctx, *host);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(usb, runUsb, "[stats]",
                   "List the devices on the USB host (address, hub port, speed, IDs, class, name); 'stats' counts "
                   "them")

BMDS_NAMESPACE_END
