/**
 * @file      i2ccommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `i2c` (devplan/io.md §11.3): the system's I²C buses, scanned, a device's registers read and
 * written. Its forms and output are a userspace interface: append only. An address a BMDS driver
 * owns needs --force, as /dev/i2c-N's I2C_SLAVE_FORCE.
 */

#include <cstdlib>
#include <cstring>
#include <bmds/command.h>
#include <bmds/i2cbus.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int TimeoutMs = 1000;
        constexpr size_t       MaxCount = 64; // `read`'s and `write`'s bytes

        bool number(const char *text, unsigned long max, unsigned long &out) {
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 0); // 0x48, 72
                if (end == text || *end != '\0' || v > max) return false;
                out = v;
                return true;
        }

        const char *errorText(Error err) {
                return err == Error::NotFound ? "no answer (nothing acknowledged)" : err.desc();
        }

        // i2cdetect's grid: an address that answers, `UU` for a driver's (not probed), `--` none.
        int scan(CommandContext &ctx, I2cBus &bus) {
                ctx.printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
                for (unsigned row = 0; row < 0x80; row += 0x10) {
                        ctx.printf("%02x:", row);
                        for (unsigned a = row; a < row + 0x10; ++a) {
                                if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                                // The reserved ranges, as i2cdetect leaves them.
                                if (a < 0x08 || a > 0x77) {
                                        ctx.printf("   ");
                                        continue;
                                }
                                if (bus.isOwned(uint8_t(a))) {
                                        ctx.printf(" UU");
                                        continue;
                                }
                                // A write of the address alone: a device there acknowledges it.
                                I2cMessage m;
                                m.address = uint16_t(a);
                                const Error err = bus.transfer(Span<const I2cMessage>(&m, 1), TimeoutMs);
                                if (err.isOk())
                                        ctx.printf(" %02x", a);
                                else
                                        ctx.printf(" --");
                        }
                        ctx.printf("\n");
                }
                return Shell::ExitSuccess;
        }

        int runI2c(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) {
                        for (size_t i = 0; i < I2cBus::count(); ++i) {
                                I2cBus *bus = I2cBus::bus(i);
                                ctx.printf("i2c-%u  %u Hz  /dev/i2c-%u  drivers at:", unsigned(i),
                                           unsigned(bus->clockHz()), unsigned(i));
                                bool any = false;
                                for (unsigned a = 0; a < 0x80; ++a)
                                        if (bus->isOwned(uint8_t(a))) {
                                                ctx.printf(" 0x%02x", a);
                                                any = true;
                                        }
                                ctx.printf("%s\n", any ? "" : " none");
                        }
                        return Shell::ExitSuccess;
                }
                // The words, --force anywhere.
                bool        force = false;
                const char *words[3 + MaxCount] = {};
                size_t      n = 0;
                for (size_t i = 1; i < args.size(); ++i) {
                        if (std::strcmp(args[i], "--force") == 0)
                                force = true;
                        else if (n < sizeof(words) / sizeof(words[0]))
                                words[n++] = args[i];
                        else
                                return ctx.usage();
                }
                if (n < 2) return ctx.usage();
                unsigned long busNumber = 0;
                if (!number(words[1], 255, busNumber) || I2cBus::bus(busNumber) == nullptr) {
                        ctx.eprintf("i2c: no bus '%s' ('i2c' lists them)\n", words[1]);
                        return Shell::ExitFailure;
                }
                I2cBus          &bus = *I2cBus::bus(busNumber);
                const StringView verb(words[0]);
                if (verb == "scan" && n == 2) return scan(ctx, bus);
                if ((verb != "read" && verb != "write") || n < 4) return ctx.usage();
                unsigned long address = 0, reg = 0;
                if (!number(words[2], 0x7f, address) || !number(words[3], 0xff, reg)) return ctx.usage();
                if (bus.isOwned(uint8_t(address)) && !force) {
                        ctx.eprintf("i2c: 0x%02lx on i2c-%lu is a driver's device: changing what it sees will "
                                    "probably break it; use --force to reach it anyway\n",
                                    address, busNumber);
                        return Shell::ExitFailure;
                }
                uint8_t regByte = uint8_t(reg);
                if (verb == "read") {
                        unsigned long count = 1;
                        if (n > 5 || (n == 5 && (!number(words[4], MaxCount, count) || count == 0))) return ctx.usage();
                        uint8_t          bytes[MaxCount];
                        const I2cMessage messages[] = {
                                {uint16_t(address), false, &regByte, 1},
                                {uint16_t(address), true, bytes, count},
                        };
                        const Error err = bus.transfer(messages, TimeoutMs);
                        if (err.isError()) {
                                ctx.eprintf("i2c: 0x%02lx: %s\n", address, errorText(err));
                                return Shell::ExitFailure;
                        }
                        for (size_t i = 0; i < count; ++i)
                                ctx.printf("%02x%s", bytes[i], (i + 1) % 16 == 0 || i + 1 == count ? "\n" : " ");
                        return Shell::ExitSuccess;
                }
                // write: the register, then its bytes, one message.
                if (n < 5) return ctx.usage();
                uint8_t bytes[1 + MaxCount];
                bytes[0] = regByte;
                size_t length = 1;
                for (size_t i = 4; i < n; ++i) {
                        unsigned long v = 0;
                        if (!number(words[i], 0xff, v)) return ctx.usage();
                        bytes[length++] = uint8_t(v);
                }
                const I2cMessage m{uint16_t(address), false, bytes, length};
                const Error      err = bus.transfer(Span<const I2cMessage>(&m, 1), TimeoutMs);
                if (err.isError()) {
                        ctx.eprintf("i2c: 0x%02lx: %s\n", address, errorText(err));
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(i2c, runI2c, "[scan N | read N ADDR REG [COUNT] | write N ADDR REG BYTE...] [--force]",
                   "The system's I2C buses: scanned, a device's registers read and written (devplan/io.md)")

BMDS_NAMESPACE_END
