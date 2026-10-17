/**
 * @file      displayregs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `display dsi` and `display chip` (devplan/display.md §5e): every register of the DSI host and
 * the DSI bridge, field by field (named from IDF's register headers), and of the HDMI bridge
 * chip (the LT8912B: the registers its driver writes by name, then its three pages raw); and
 * writing any of them, for experiments. Test aids: a write can stop the picture.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/command.h>
#include <bmds/display.h>
#include <bmds/lt8912b.h>
#include <bmds/shell.h>
#include "regtable.h"

BMDS_NAMESPACE_BEGIN

namespace detail {

        int runDisplayDsi(CommandContext &ctx, Span<char *const> args);
        int runDisplayChip(CommandContext &ctx, Span<char *const> args);

} // namespace detail

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int TimeoutMs = 100;

        // Display 0 with its link up, or null with a message.
        Display *linked(CommandContext &ctx) {
                auto [d, err] = Display::display(0);
                if (err.isError()) {
                        ctx.eprintf("display: %s\n", err.desc());
                        return nullptr;
                }
                if (!d->isRunning() || d->scanout().hostRegisters() == nullptr) {
                        ctx.eprintf("display: not started ('display start'): the DSI clocks are off\n");
                        return nullptr;
                }
                return d;
        }

        bool parseNumber(const char *text, uint32_t &value) {
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 0);
                if (end == text || *end != '\0') return false;
                value = static_cast<uint32_t>(v);
                return true;
        }

        uint32_t fieldMask(const detail::RegField &f) {
                return f.width >= 32 ? 0xffffffffu : ((1u << f.width) - 1u) << f.lo;
        }

        // ---- The DSI host and bridge

        struct Block {
                        const char             *name;
                        const detail::RegTable &table;
        };
        const Block Blocks[] = {{"host", detail::DsiHostRegs}, {"bridge", detail::DsiBrgRegs}};

        volatile uint32_t *baseOf(Display &d, size_t block) {
                return block == 0 ? d.scanout().hostRegisters() : d.scanout().bridgeRegisters();
        }

        void printField(CommandContext &ctx, const detail::RegField &f, uint32_t value) {
                const uint32_t v = (value & fieldMask(f)) >> f.lo;
                char           bits[8];
                if (f.width == 1)
                        std::snprintf(bits, sizeof(bits), "[%u]", static_cast<unsigned>(f.lo));
                else
                        std::snprintf(bits, sizeof(bits), "[%u:%u]", static_cast<unsigned>(f.lo + f.width - 1),
                                      static_cast<unsigned>(f.lo));
                static const char *const access[] = {"ro", "rw", "wt", "  "};
                if (f.width >= 4)
                        ctx.printf("            %-28s %-7s %s %u (0x%x)\n", f.name, bits, access[f.access & 3],
                                   static_cast<unsigned>(v), static_cast<unsigned>(v));
                else
                        ctx.printf("            %-28s %-7s %s %u\n", f.name, bits, access[f.access & 3],
                                   static_cast<unsigned>(v));
                return;
        }

        // The registers of @p block whose name, or a field's, contains @p pattern (null: all).
        void dumpBlock(CommandContext &ctx, Display &d, size_t block, const char *pattern) {
                const detail::RegTable  &t = Blocks[block].table;
                volatile uint32_t *const base = baseOf(d, block);
                if (base == nullptr) return;
                const Scanout::Stats s = d.scanout().stats();
                for (size_t r = 0; r < t.count; ++r) {
                        const detail::RegInfo &reg = t.regs[r];
                        const bool             all = pattern == nullptr || std::strstr(reg.name, pattern) != nullptr;
                        bool                   any = all;
                        for (uint8_t i = 0; i < reg.count && !any; ++i)
                                any = std::strstr(t.fields[reg.first + i].name, pattern) != nullptr;
                        if (!any) continue;
                        uint32_t value = 0;
                        if (!reg.noRead) {
                                value = base[reg.offset / 4];
                                ctx.printf("%-6s 0x%03x %-30s 0x%08x\n", Blocks[block].name,
                                           static_cast<unsigned>(reg.offset), reg.name, static_cast<unsigned>(value));
                        } else if (block == 0 && std::strcmp(reg.name, "int_st0") == 0) {
                                value = s.linkErrors0; // cleared by reading: the scan's own read, a frame
                                ctx.printf("%-6s 0x%03x %-30s 0x%08x (the flags seen since the start)\n",
                                           Blocks[block].name, static_cast<unsigned>(reg.offset), reg.name,
                                           static_cast<unsigned>(value));
                        } else if (block == 0 && std::strcmp(reg.name, "int_st1") == 0) {
                                value = s.linkErrors1;
                                ctx.printf("%-6s 0x%03x %-30s 0x%08x (the flags seen since the start)\n",
                                           Blocks[block].name, static_cast<unsigned>(reg.offset), reg.name,
                                           static_cast<unsigned>(value));
                        } else {
                                ctx.printf("%-6s 0x%03x %-30s (not read: reading changes it)\n", Blocks[block].name,
                                           static_cast<unsigned>(reg.offset), reg.name);
                                continue;
                        }
                        for (uint8_t i = 0; i < reg.count; ++i) {
                                const detail::RegField &f = t.fields[reg.first + i];
                                if (all || std::strstr(f.name, pattern) != nullptr) printField(ctx, f, value);
                        }
                }
                return;
        }

        // `display dsi set host|bridge REG[.FIELD] VALUE`
        int setDsi(CommandContext &ctx, Display &d, Args args) {
                if (args.size() != 6) return ctx.usage();
                size_t block = 0;
                while (block < 2 && std::strcmp(args[3], Blocks[block].name) != 0) ++block;
                uint32_t value = 0;
                if (block == 2 || !parseNumber(args[5], value)) return ctx.usage();
                const detail::RegTable &t = Blocks[block].table;
                const char             *dot = std::strchr(args[4], '.');
                const StringView        regName(args[4], dot != nullptr ? size_t(dot - args[4]) : std::strlen(args[4]));
                const detail::RegInfo  *reg = nullptr;
                for (size_t r = 0; r < t.count && reg == nullptr; ++r)
                        if (regName == StringView(t.regs[r].name)) reg = &t.regs[r];
                if (reg == nullptr) {
                        ctx.eprintf("display: %s has no register %s\n", Blocks[block].name, args[4]);
                        return Shell::ExitFailure;
                }
                const detail::RegField *field = nullptr;
                if (dot != nullptr) {
                        for (uint8_t i = 0; i < reg->count && field == nullptr; ++i)
                                if (std::strcmp(t.fields[reg->first + i].name, dot + 1) == 0)
                                        field = &t.fields[reg->first + i];
                        if (field == nullptr) {
                                ctx.eprintf("display: %s has no field %s\n", reg->name, dot + 1);
                                return Shell::ExitFailure;
                        }
                        if (field->access == 0) {
                                ctx.eprintf("display: %s.%s is read-only\n", reg->name, field->name);
                                return Shell::ExitFailure;
                        }
                        if (field->width < 32 && value >> field->width != 0) {
                                ctx.eprintf("display: %s.%s has %u bits\n", reg->name, field->name,
                                            static_cast<unsigned>(field->width));
                                return Shell::ExitFailure;
                        }
                }
                volatile uint32_t *const base = baseOf(d, block);
                if (base == nullptr) return Shell::ExitFailure;
                volatile uint32_t &word = base[reg->offset / 4];
                const uint32_t     before = reg->noRead ? 0 : word;
                const uint32_t after = field == nullptr ? value : (before & ~fieldMask(*field)) | (value << field->lo);
                word = after;
                ctx.printf("%s %s: 0x%08x -> 0x%08x (reads 0x%08x)\n", Blocks[block].name, reg->name,
                           static_cast<unsigned>(before), static_cast<unsigned>(after),
                           static_cast<unsigned>(reg->noRead ? 0 : word));
                return Shell::ExitSuccess;
        }

        // ---- The HDMI bridge chip (LT8912B)

        constexpr const char *Pages[] = {"main", "dsi", "avi"};

        // The registers its driver writes or reads (lt8912b.cpp), several bytes low first.
        struct ChipReg {
                        uint8_t     page;
                        uint8_t     reg;
                        uint8_t     bytes;
                        const char *name;
        };
        constexpr ChipReg ChipRegs[] = {
                {0, 0x00, 1, "chip id high"},
                {0, 0x01, 1, "chip id low"},
                {0, 0x02, 1, "reset and clocks"},
                {0, 0x03, 1, "receiver reset (0x7f pulses it)"},
                {0, 0x05, 1, "logic reset (0xfb pulses it)"},
                {0, 0x08, 1, "digital clocks 1"},
                {0, 0x09, 1, "digital clocks 2"},
                {0, 0x0a, 1, "digital clocks 3"},
                {0, 0x0b, 1, "digital clocks 4"},
                {0, 0x0c, 1, "digital clocks 5"},
                {0, 0x31, 1, "HDMI transmitter 1"},
                {0, 0x32, 1, "HDMI transmitter 2"},
                {0, 0x33, 1, "HDMI output (0x0e on, 0x0c off)"},
                {0, 0x37, 1, "CBUS"},
                {0, 0x38, 1, "HDMI PLL"},
                {0, 0x39, 1, "HDMI PLL 2"},
                {0, 0x3e, 1, "DSI receiver (P/N swap)"},
                {0, 0x3f, 1, "DSI receiver 2"},
                {0, 0x41, 1, "DSI receiver equalizer"},
                {0, 0x44, 1, "LVDS PLL"},
                {0, 0x50, 1, "output PLL 1"},
                {0, 0x51, 1, "output PLL 2"},
                {0, 0x52, 1, "output PLL 3"},
                {0, 0x60, 1, "analog 60"},
                {0, 0x69, 1, "scaler bypass 1"},
                {0, 0x6a, 1, "scaler bypass 2"},
                {0, 0x6b, 1, "scaler bypass 3"},
                {0, 0x6c, 1, "scaler bypass 4"},
                {0, 0x7f, 1, "LVDS off"},
                {0, 0x9c, 2, "measured 0x9c-9d"},
                {0, 0x9e, 2, "measured lines a frame"},
                {0, 0xa8, 1, "output path"},
                {0, 0xab, 1, "sync polarity (bit 1 h, bit 0 v)"},
                {0, 0xb2, 1, "HDMI mode (1) or DVI (0)"},
                {0, 0xc1, 1, "HDMI status (bit 7 hot-plug)"},
                {1, 0x10, 1, "DSI termination"},
                {1, 0x11, 1, "DSI settle"},
                {1, 0x13, 1, "DSI lanes (0: 4)"},
                {1, 0x18, 1, "hsync"},
                {1, 0x19, 1, "vsync"},
                {1, 0x1a, 1, "DSI receiver 1a"},
                {1, 0x1b, 1, "DSI receiver 1b"},
                {1, 0x1c, 2, "h active"},
                {1, 0x2f, 1, "FIFO length"},
                {1, 0x34, 2, "h total"},
                {1, 0x36, 2, "v total"},
                {1, 0x38, 2, "v back porch"},
                {1, 0x3a, 2, "v front porch"},
                {1, 0x3c, 2, "h back porch"},
                {1, 0x3e, 2, "h front porch"},
                {1, 0x4e, 3, "DDS frequency word"},
                {1, 0x51, 1, "DDS control"},
                {2, 0x3c, 1, "packets (null packets on)"},
                {2, 0x43, 1, "AVI PB0 (checksum)"},
                {2, 0x44, 1, "AVI PB1 (RGB)"},
                {2, 0x45, 1, "AVI PB2 (aspect)"},
                {2, 0x46, 1, "AVI PB3"},
                {2, 0x47, 1, "AVI PB4 (VIC)"},
        };

        Lt8912b *chip(CommandContext &ctx) {
                auto [d, err] = Display::display(0);
                if (err.isError()) {
                        ctx.eprintf("display: %s\n", err.desc());
                        return nullptr;
                }
                return &d->bridge();
        }

        bool pageOf(const char *name, uint8_t &page) {
                for (page = 0; page < 3; ++page)
                        if (std::strcmp(name, Pages[page]) == 0) return true;
                return false;
        }

        void dumpPage(CommandContext &ctx, Lt8912b &b, uint8_t page) {
                ctx.printf("%s page:\n", Pages[page]);
                for (unsigned row = 0; row < 256; row += 16) {
                        ctx.printf("  %02x:", row);
                        for (unsigned i = 0; i < 16; ++i) {
                                auto [v, err] = b.readRegister(page, static_cast<uint8_t>(row + i));
                                if (err.isError())
                                        ctx.printf(" --");
                                else
                                        ctx.printf(" %02x", static_cast<unsigned>(v));
                        }
                        ctx.printf("\n");
                }
                return;
        }

        void dumpNamed(CommandContext &ctx, Lt8912b &b) {
                for (const ChipReg &r : ChipRegs) {
                        uint32_t value = 0;
                        bool     ok = true;
                        for (uint8_t i = 0; i < r.bytes && ok; ++i) {
                                auto [v, err] = b.readRegister(r.page, static_cast<uint8_t>(r.reg + i));
                                ok = err.isOk();
                                value |= uint32_t(v) << (8 * i);
                        }
                        if (!ok)
                                ctx.printf("%-4s 0x%02x %-36s --\n", Pages[r.page], static_cast<unsigned>(r.reg),
                                           r.name);
                        else if (r.bytes == 1)
                                ctx.printf("%-4s 0x%02x %-36s 0x%02x\n", Pages[r.page], static_cast<unsigned>(r.reg),
                                           r.name, static_cast<unsigned>(value));
                        else
                                ctx.printf("%-4s 0x%02x %-36s %u (0x%x)\n", Pages[r.page], static_cast<unsigned>(r.reg),
                                           r.name, static_cast<unsigned>(value), static_cast<unsigned>(value));
                }
                return;
        }

} // namespace

namespace detail {

        // display dsi [host|bridge] [PATTERN] | set host|bridge REG[.FIELD] VALUE
        int runDisplayDsi(CommandContext &ctx, Args args) {
                Display *d = linked(ctx);
                if (d == nullptr) return Shell::ExitFailure;
                if (args.size() >= 3 && std::strcmp(args[2], "set") == 0) return setDsi(ctx, *d, args);
                size_t      from = 0, to = 2;
                const char *pattern = nullptr;
                size_t      i = 2;
                if (i < args.size()) {
                        for (size_t b = 0; b < 2; ++b)
                                if (std::strcmp(args[i], Blocks[b].name) == 0) {
                                        from = b;
                                        to = b + 1;
                                        ++i;
                                        break;
                                }
                }
                if (i < args.size()) pattern = args[i++];
                if (i != args.size()) return ctx.usage();
                for (size_t b = from; b < to; ++b) dumpBlock(ctx, *d, b, pattern);
                return Shell::ExitSuccess;
        }

        // display chip [main|dsi|avi] | set PAGE REG VALUE | reset | configure
        int runDisplayChip(CommandContext &ctx, Args args) {
                Lt8912b *b = chip(ctx);
                if (b == nullptr) return Shell::ExitFailure;
                uint8_t page = 0;
                if (args.size() == 2) {
                        dumpNamed(ctx, *b);
                        for (page = 0; page < 3; ++page) dumpPage(ctx, *b, page);
                        return Shell::ExitSuccess;
                }
                if (args.size() == 3 && pageOf(args[2], page)) {
                        dumpPage(ctx, *b, page);
                        return Shell::ExitSuccess;
                }
                if (args.size() == 3 && std::strcmp(args[2], "reset") == 0) {
                        const Error err = b->resetReceiver();
                        if (err.isError()) ctx.eprintf("display: reset: %s\n", err.desc());
                        return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
                }
                if (args.size() == 3 && std::strcmp(args[2], "configure") == 0) {
                        // As after a rebuild of the link: the whole set-up for the timing sent.
                        auto [d, derr] = Display::display(0);
                        if (derr.isError() || !d->isRunning()) {
                                ctx.eprintf("display: not started ('display start')\n");
                                return Shell::ExitFailure;
                        }
                        Error err = b->configure(d->scanout().timing(), d->scanout().lanes(), true);
                        if (err.isOk()) err = b->resetReceiver();
                        if (err.isError()) ctx.eprintf("display: configure: %s\n", err.desc());
                        return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
                }
                uint32_t reg = 0, value = 0;
                if (args.size() == 6 && std::strcmp(args[2], "set") == 0 && pageOf(args[3], page) &&
                    parseNumber(args[4], reg) && parseNumber(args[5], value) && reg <= 0xff && value <= 0xff) {
                        auto [before, rerr] = b->readRegister(page, static_cast<uint8_t>(reg));
                        Error err = b->writeRegister(page, static_cast<uint8_t>(reg), static_cast<uint8_t>(value));
                        auto [after, aerr] = b->readRegister(page, static_cast<uint8_t>(reg));
                        if (err.isError()) {
                                ctx.eprintf("display: set: %s\n", err.desc());
                                return Shell::ExitFailure;
                        }
                        ctx.printf("%s 0x%02x: 0x%02x -> 0x%02x (reads 0x%02x)\n", Pages[page],
                                   static_cast<unsigned>(reg), rerr.isOk() ? unsigned(before) : 0u,
                                   static_cast<unsigned>(value), aerr.isOk() ? unsigned(after) : 0u);
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

} // namespace detail

BMDS_NAMESPACE_END
