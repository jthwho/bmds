/**
 * @file      padcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `pad` command: the gamepads, live (devplan/controls.md §4c, §7a). Its output format is a
 * userspace interface once shipped: keep the columns and their order, add new ones only at the end.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/command.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/pads.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        const char *originName(Pads::Origin origin) {
                return origin == Pads::Origin::Database ? "database" : "standard";
        }

        // A pad's number from @p text (`0`, or `pad0`).
        bool padNumber(const char *text, uint8_t &out) {
                if (std::strncmp(text, "pad", 3) == 0) text += 3;
                char         *end = nullptr;
                unsigned long n = std::strtoul(text, &end, 10);
                if (end == text || *end != '\0' || n >= Pads::MaxPads) return false;
                out = static_cast<uint8_t>(n);
                return true;
        }

        int showPads(CommandContext &ctx) {
                Pads::Info   pads[Pads::MaxPads];
                const size_t n = Pads::list(Span<Pads::Info>(pads, Pads::MaxPads));
                ctx.printf("PAD  VID:PID   KIND MAPPING  AXES HATS BUTTONS NAME\n");
                for (size_t i = 0; i < n; ++i) {
                        const Pads::Info &p = pads[i];
                        ctx.printf("%-4u %04x:%04x %-4s %-8s %-4u %-4u %-7u %s\n", p.number, p.vendor, p.product, "hid",
                                   originName(p.origin), p.axes, p.hats, p.buttons, p.name);
                }
                return Shell::ExitSuccess;
        }

        // The pad's line for gamepads.txt: SDL's GUID for a USB device, its name, its mapping.
        int showMapping(CommandContext &ctx, uint8_t number) {
                Pads::Info info;
                PadMapping mapping;
                if (Pads::info(number, info).isError() || Pads::mapping(number, mapping).isError()) {
                        ctx.eprintf("pad: pad%u isn't attached\n", number);
                        return Shell::ExitFailure;
                }
                char name[Pads::MaxName];
                std::snprintf(name, sizeof(name), "%s", info.name[0] != '\0' ? info.name : "Gamepad");
                for (char &c : name)
                        if (c == ',') c = ' ';
                char fields[PadMapping::MaxText];
                (void)mapping.format(fields, sizeof(fields));
                ctx.printf("03000000%02x%02x0000%02x%02x000000000000,%s,%splatform:BMDS,\n", info.vendor & 0xffu,
                           info.vendor >> 8, info.product & 0xffu, info.product >> 8, name, fields);
                return Shell::ExitSuccess;
        }

        int showDescriptor(CommandContext &ctx, uint8_t number) {
                uint8_t bytes[Pads::MaxDescriptor];
                auto [n, err] = Pads::descriptor(number, Span<uint8_t>(bytes, sizeof(bytes)));
                if (err.isError()) {
                        ctx.eprintf("pad: pad%u isn't attached\n", number);
                        return Shell::ExitFailure;
                }
                for (size_t i = 0; i < n; i += 16) {
                        char   line[16 * 3 + 1];
                        size_t used = 0;
                        for (size_t k = i; k < n && k < i + 16; ++k)
                                used += size_t(std::snprintf(line + used, sizeof(line) - used, "%s%02x",
                                                             k == i ? "" : " ", bytes[k]));
                        ctx.printf("%04x  %s\n", unsigned(i), line);
                }
                return Shell::ExitSuccess;
        }

        const char *hatName(int32_t bits) {
                switch (bits) {
                        case RawPad::HatUp: return "up";
                        case RawPad::HatUp | RawPad::HatRight: return "up-right";
                        case RawPad::HatRight: return "right";
                        case RawPad::HatDown | RawPad::HatRight: return "down-right";
                        case RawPad::HatDown: return "down";
                        case RawPad::HatDown | RawPad::HatLeft: return "down-left";
                        case RawPad::HatLeft: return "left";
                        case RawPad::HatUp | RawPad::HatLeft: return "up-left";
                        default: return "centred";
                }
        }

        // An axis's value from −1.00 to 1.00 (a trigger's 0.00 to 1.00).
        void formatAxis(char (&out)[8], int32_t value) {
                const int32_t hundredths = (value * 100 + (value < 0 ? -16383 : 16383)) / 32767;
                const int32_t a = hundredths < 0 ? -hundredths : hundredths;
                std::snprintf(out, sizeof(out), "%s%ld.%02ld", hundredths < 0 ? "-" : " ", long(a / 100),
                              long(a % 100));
                return;
        }

        void printEvent(CommandContext &ctx, const Pads::Event &e) {
                using Type = Pads::Event::Type;
                char value[8];
                switch (e.type) {
                        case Type::Attached: ctx.printf("pad%u attached\n", e.pad); break;
                        case Type::Detached: ctx.printf("pad%u gone\n", e.pad); break;
                        case Type::RawAxis:
                                formatAxis(value, e.value);
                                ctx.printf("pad%u raw a%u %s\n", e.pad, e.index, value);
                                break;
                        case Type::RawHat: ctx.printf("pad%u raw h%u %s\n", e.pad, e.index, hatName(e.value)); break;
                        case Type::RawButton:
                                ctx.printf("pad%u raw b%u %s\n", e.pad, e.index, e.value != 0 ? "down" : "up");
                                break;
                        case Type::Axis:
                                formatAxis(value, e.value);
                                ctx.printf("pad%u     %s %s\n", e.pad, PadState::axisName(PadState::Axis(e.index)),
                                           value);
                                break;
                        case Type::Button:
                                ctx.printf("pad%u     %s %s\n", e.pad, PadState::buttonName(PadState::Button(e.index)),
                                           e.value != 0 ? "down" : "up");
                                break;
                }
                return;
        }

        // Each change as it comes, raw and standard, until Ctrl-C (or @p seconds, if not 0).
        int testPads(CommandContext &ctx, bool all, uint8_t number, unsigned seconds) {
                Pads::Info info;
                if (!all && Pads::info(number, info).isError()) {
                        ctx.eprintf("pad: pad%u isn't attached\n", number);
                        return Shell::ExitFailure;
                }
                Pads::Listener listener;
                if (Pads::listen(listener).isError()) {
                        ctx.eprintf("pad: too many listening\n");
                        return Shell::ExitFailure;
                }
                if (all) {
                        ctx.printf("All pads: press buttons, move sticks; Ctrl-C ends.\n");
                } else {
                        ctx.printf("pad%u: %s (%04x:%04x), mapping: %s. Press buttons, move sticks; Ctrl-C ends.\n",
                                   number, info.name[0] != '\0' ? info.name : "a gamepad", info.vendor, info.product,
                                   originName(info.origin));
                }
                const Deadline until(seconds != 0 ? seconds * 1000u : WaitForever);
                for (;;) {
                        Pads::Event e;
                        while (listener.take(e))
                                if (all || e.pad == number) printEvent(ctx, e);
                        if (ctx.isInterrupted()) break;
                        const Error err = Interrupt::waitCurrent(listener.ready(), until);
                        if (err == Error::Interrupted || err == Error::Timeout) break;
                }
                Pads::unlisten(listener);
                if (listener.dropped() != 0)
                        ctx.printf("(%lu changes came too fast to show)\n", (unsigned long)listener.dropped());
                return Shell::ExitSuccess;
        }

        int runPad(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() == 1) return showPads(ctx);
                const char *verb = args[1];
                uint8_t     number = 0;
                if (std::strcmp(verb, "test") == 0) {
                        size_t   at = 2;
                        unsigned seconds = 0;
                        if (at + 1 < args.size() && std::strcmp(args[at], "-t") == 0) {
                                char         *end = nullptr;
                                unsigned long n = std::strtoul(args[at + 1], &end, 10);
                                if (end == args[at + 1] || *end != '\0' || n == 0 || n > 86400) return ctx.usage();
                                seconds = static_cast<unsigned>(n);
                                at += 2;
                        }
                        if (at == args.size()) return testPads(ctx, true, 0, seconds);
                        if (at + 1 != args.size() || !padNumber(args[at], number)) return ctx.usage();
                        return testPads(ctx, false, number, seconds);
                }
                if (args.size() != 3 || !padNumber(args[2], number)) return ctx.usage();
                if (std::strcmp(verb, "map") == 0) return showMapping(ctx, number);
                if (std::strcmp(verb, "descriptor") == 0) return showDescriptor(ctx, number);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(pad, runPad, "[test [-t SECONDS] [N] | map N | descriptor N]",
                   "List the gamepads; 'test' shows their buttons and sticks live (until Ctrl-C, or for SECONDS), "
                   "'map' prints a pad's mapping "
                   "as a gamepads.txt line, 'descriptor' its HID report descriptor")

BMDS_NAMESPACE_END
