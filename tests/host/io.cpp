/**
 * @file      io.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The I/O model (devplan/io.md §3.1-§3.4): the ESP32-P4's generated table, the
 * registry of holders, and a pad state's words.
 */

#include <doctest.h>

#include <cstring>
#include <bmds/iochip.h>
#include <bmds/iopad.h>
#include <bmds/iopin.h>
#include <bmds/ioregistry.h>

using bmds::Error;
using bmds::IoBoardUse;
using bmds::IoChip;
using bmds::IoClaim;
using bmds::IoClass;
using bmds::IoHolder;
using bmds::IoKind;
using bmds::IoOwnerKind;
using bmds::IoPadInfo;
using bmds::IoPadState;
using bmds::IoPinBoard;
using bmds::IoPinDetail;
using bmds::IoPinField;
using bmds::IoPinRow;
using bmds::IoRegistry;
using bmds::IoResource;
using bmds::IoUse;
using bmds::Span;

namespace {

        IoResource resource(IoKind kind, unsigned n) {
                auto [r, err] = bmds::ioChip().resource(kind, n);
                REQUIRE(err.isOk());
                return r;
        }

        IoHolder holderOf(const IoRegistry &registry, IoResource r) {
                IoHolder h;
                CHECK(registry.holder(r, h).isOk());
                return h;
        }

} // namespace

TEST_CASE("IoChip: the ESP32-P4's table") {
        const IoChip &chip = bmds::ioChip();

        SUBCASE("pads and their IO_MUX functions") {
                CHECK(std::strcmp(chip.name, "ESP32-P4") == 0);
                CHECK(chip.padCount() == 55);
                CHECK(chip.gpioFunction == 1);
                for (size_t pad = 0; pad < chip.padCount(); ++pad) {
                        INFO("GPIO" << pad);
                        REQUIRE(chip.functionName(pad, chip.gpioFunction) != nullptr);
                        CHECK(std::strcmp(chip.functionName(pad, chip.gpioFunction), "GPIO") == 0);
                }
                CHECK(std::strcmp(chip.functionName(37, 0), "UART0_TXD") == 0);
                CHECK(std::strcmp(chip.functionName(38, 0), "UART0_RXD") == 0);
                CHECK(std::strcmp(chip.functionName(37, 2), "SPI2_IO7") == 0);
                CHECK(chip.functionName(37, 9) == nullptr);
        }

        SUBCASE("pad facts from ESP-IDF's GPIO table") {
                for (unsigned pad = 34; pad <= 38; ++pad) CHECK(chip.pads[pad].is(IoPadInfo::Strapping));
                CHECK_FALSE(chip.pads[33].is(IoPadInfo::Strapping));
                CHECK(chip.pads[0].lpIo == 0);
                CHECK(chip.pads[15].lpIo == 15);
                CHECK(chip.pads[16].lpIo == -1);
                CHECK(std::strcmp(chip.pads[20].analog, "ADC1_CH4") == 0);
                CHECK(std::strstr(chip.pads[24].note, "USB-JTAG") != nullptr);
        }

        SUBCASE("matrix signals") {
                CHECK(chip.gpioOutputSignal == 256);
                CHECK(std::strcmp(chip.outputName(256), "GPIO") == 0);
                CHECK(std::strcmp(chip.inputName(10), "UART0_RXD") == 0);
                CHECK(std::strcmp(chip.outputName(10), "UART0_TXD") == 0);
                CHECK(std::strcmp(chip.inputName(68), "I2C0_SCL") == 0);
                CHECK(chip.findOutput("uart0_txd").value() == 10);
                CHECK(chip.findInput("I2C0_SDA").value() == 69);
                CHECK(chip.findInput("NO_SUCH").error() == Error::NotExist);
                CHECK(chip.constZeroInput == 0x3e);
                CHECK(chip.constOneInput == 0x3f);
                CHECK(std::strcmp(chip.groups[chip.outputGroups[10]], "UART0") == 0);
                CHECK(std::strcmp(chip.groups[chip.inputGroups[68]], "I2C0") == 0);
        }

        SUBCASE("resources: pads first, then each kind's instances") {
                CHECK(resource(IoKind::Pad, 7) == 7);
                const IoResource uart0 = resource(IoKind::Uart, 0);
                CHECK(uart0 == chip.padCount());
                CHECK(chip.resource(IoKind::Uart, 5).error() == Error::NotExist);
                // SPI is numbered as the chip does: SPI2 and SPI3.
                CHECK(chip.resource(IoKind::Spi, 1).error() == Error::NotExist);
                const IoResource spi3 = resource(IoKind::Spi, 3);
                char             name[24];
                CHECK(chip.resourceName(spi3, name, sizeof(name)) == 4);
                CHECK(std::strcmp(name, "spi3") == 0);
                CHECK(chip.resourceName(5, name, sizeof(name)) == 5);
                CHECK(std::strcmp(name, "GPIO5") == 0);
                CHECK(chip.findResource("spi3").value() == spi3);
                CHECK(chip.findResource("gpio5").value() == 5);
                CHECK(chip.findResource("GPIO54").value() == 54);
                CHECK(chip.findResource("GPIO55").error() == Error::NotExist);
                CHECK(chip.findResource("ledc-timer3").value() == resource(IoKind::LedcTimer, 3));
                CHECK(chip.resourceName(static_cast<IoResource>(chip.resourceCount()), name, sizeof(name)) == 0);
                unsigned number = 0;
                CHECK(chip.kindOf(resource(IoKind::Rmt, 6), &number).value().kind == IoKind::Rmt);
                CHECK(number == 6);
                CHECK(chip.resourceCount() <= IoRegistry::MaxResources);
        }
}

TEST_CASE("IoRegistry: claims") {
        IoRegistry registry(bmds::ioChip());
        CHECK(registry.setBoardUse(0, IoBoardUse::Fixed).isOk());
        CHECK(registry.setBoardUse(35, IoBoardUse::Device).isOk());
        CHECK(registry.setBoardUse(37, IoBoardUse::Free).isOk());
        CHECK(registry.setBoardUse(55, IoBoardUse::Free) == Error::Invalid);
        const IoResource i2c0 = resource(IoKind::I2c, 0);

        SUBCASE("classes before any claim") {
                CHECK(registry.resourceClass(0) == IoClass::Fixed);
                CHECK(registry.resourceClass(35) == IoClass::System);
                CHECK(registry.resourceClass(37) == IoClass::Free);
                CHECK(registry.resourceClass(14) == IoClass::Free);
                CHECK(registry.resourceClass(i2c0) == IoClass::Free);
        }

        SUBCASE("a driver claims board-wired pads; another can't take them") {
                const IoUse eth[] = {{35, "RMII TXD1"}, {34, "RMII TXD0"}};
                IoResource  blocked = bmds::NoIoResource;
                auto [claim, err] = registry.claim("eth0", IoOwnerKind::System, eth, 0, &blocked);
                REQUIRE(err.isOk());
                CHECK(claim.isValid());
                IoHolder h = holderOf(registry, 35);
                CHECK(h.resourceClass == IoClass::System);
                CHECK(std::strcmp(h.owner, "eth0") == 0);
                CHECK(std::strcmp(h.role, "RMII TXD1") == 0);

                const IoUse other[] = {{33, "x"}, {34, "y"}};
                auto [second, err2] = registry.claim("other", IoOwnerKind::System, other, 0, &blocked);
                CHECK(err2 == Error::Busy);
                CHECK(blocked == 34);
                // All or nothing: 33 wasn't taken either.
                CHECK_FALSE(holderOf(registry, 33).isHeld());
        }

        SUBCASE("a user needs force for board-wired pads, and for held ones") {
                const IoUse fixed[] = {{0, "LED"}};
                IoResource  blocked = bmds::NoIoResource;
                CHECK(registry.claim("io", IoOwnerKind::User, fixed, 0, &blocked).error() == Error::Busy);
                CHECK(blocked == 0);
                const IoUse wired[] = {{35, "button"}};
                CHECK(registry.claim("io", IoOwnerKind::User, wired, 0, nullptr).error() == Error::Busy);
                const IoUse free[] = {{37, "TX"}, {i2c0, ""}};
                auto [claim, err] = registry.claim("io", IoOwnerKind::User, free, 0, nullptr);
                CHECK(err.isOk());
                CHECK(registry.resourceClass(37) == IoClass::User);
                CHECK(registry.resourceClass(i2c0) == IoClass::User);
                auto [forced, err2] = registry.claim("io2", IoOwnerKind::User, wired, IoRegistry::Force, nullptr);
                CHECK(err2.isOk());
                CHECK(registry.resourceClass(35) == IoClass::User);
        }

        SUBCASE("a claim releases on destruction") {
                {
                        const IoUse uses[] = {{14, "A"}};
                        auto [claim, err] = registry.claim("tmp", IoOwnerKind::User, uses, 0, nullptr);
                        CHECK(err.isOk());
                        CHECK(holderOf(registry, 14).isHeld());
                }
                CHECK_FALSE(holderOf(registry, 14).isHeld());
                CHECK(registry.resourceClass(14) == IoClass::Free);
        }

        SUBCASE("adding to a claim; releasing one; moving a claim") {
                auto [claim, err] = registry.claim("dsp0", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr);
                REQUIRE(err.isOk());
                const IoUse more[] = {{9, "DOUT"}, {10, "WS"}};
                CHECK(claim.add(more, 0, nullptr).isOk());
                CHECK(claim.add(more, 0, nullptr) == Error::Exists);
                CHECK(claim.release(9).isOk());
                CHECK(claim.release(9) == Error::NotExist);
                CHECK_FALSE(holderOf(registry, 9).isHeld());
                IoClaim moved(std::move(claim));
                CHECK_FALSE(claim.isValid());
                CHECK(moved.isValid());
                CHECK(holderOf(registry, 10).isHeld());
                moved.releaseAll();
                CHECK_FALSE(holderOf(registry, 10).isHeld());
                CHECK(moved.add(more, 0, nullptr).isOk());
        }

        SUBCASE("a forced resource is lent, and goes back") {
                const IoUse eth[] = {{35, "RMII TXD1"}};
                auto [owner, err] = registry.claim("eth0", IoOwnerKind::System, eth, 0, nullptr);
                REQUIRE(err.isOk());
                const IoUse    take[] = {{35, "LED"}};
                const uint32_t before = registry.generation();
                {
                        auto [borrower, err2] =
                                registry.claim("io", IoOwnerKind::User, take, IoRegistry::Force, nullptr);
                        REQUIRE(err2.isOk());
                        CHECK(registry.generation() != before);
                        IoHolder h = holderOf(registry, 35);
                        CHECK(std::strcmp(h.owner, "io") == 0);
                        CHECK(h.isForced());
                        CHECK(std::strcmp(h.lender, "eth0") == 0);
                        CHECK(std::strcmp(h.lenderRole, "RMII TXD1") == 0);
                        // One borrower at a time: the first's saved state would be lost.
                        CHECK(registry.claim("io2", IoOwnerKind::User, take, IoRegistry::Force, nullptr).error() ==
                              Error::Busy);
                }
                IoHolder h = holderOf(registry, 35);
                CHECK(std::strcmp(h.owner, "eth0") == 0);
                CHECK(std::strcmp(h.role, "RMII TXD1") == 0);
                CHECK_FALSE(h.isForced());
        }

        SUBCASE("the lender ending while its resource is borrowed") {
                const IoUse eth[] = {{35, "RMII TXD1"}};
                auto [owner, err] = registry.claim("eth0", IoOwnerKind::System, eth, 0, nullptr);
                REQUIRE(err.isOk());
                const IoUse take[] = {{35, "LED"}};
                auto [borrower, err2] = registry.claim("io", IoOwnerKind::User, take, IoRegistry::Force, nullptr);
                REQUIRE(err2.isOk());
                owner.reset();
                IoHolder h = holderOf(registry, 35);
                CHECK(std::strcmp(h.owner, "io") == 0);
                CHECK_FALSE(h.isForced());
                borrower.reset();
                CHECK_FALSE(holderOf(registry, 35).isHeld());
        }

        SUBCASE("bad claims") {
                const IoUse twice[] = {{14, "a"}, {14, "b"}};
                CHECK(registry.claim("x", IoOwnerKind::System, twice, 0, nullptr).error() == Error::Invalid);
                const IoUse past[] = {{static_cast<IoResource>(bmds::ioChip().resourceCount()), "a"}};
                CHECK(registry.claim("x", IoOwnerKind::System, past, 0, nullptr).error() == Error::Invalid);
                CHECK(registry.claim("", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr).error() ==
                      Error::Invalid);
                CHECK(registry.claim("x", IoOwnerKind::System, Span<const IoUse>(), 0x80, nullptr).error() ==
                      Error::Invalid);
                IoClaim empty;
                CHECK(empty.add(twice, 0, nullptr) == Error::Invalid);
        }

        SUBCASE("owners are a fixed pool") {
                IoClaim claims[IoRegistry::MaxOwners];
                for (IoClaim &c : claims) {
                        auto [claim, err] = registry.claim("o", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr);
                        REQUIRE(err.isOk());
                        c = std::move(claim);
                }
                CHECK(registry.claim("o", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr).error() == Error::Full);
                claims[3].reset();
                CHECK(registry.claim("o", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr).isOk());
        }

        SUBCASE("long owner names are cut to fit") {
                auto [claim, err] =
                        registry.claim("a-very-long-owner-name", IoOwnerKind::System, Span<const IoUse>(), 0, nullptr);
                REQUIRE(err.isOk());
                const IoUse uses[] = {{14, ""}};
                CHECK(claim.add(uses, 0, nullptr).isOk());
                CHECK(std::strlen(holderOf(registry, 14).owner) == IoHolder::NameBytes - 1);
        }
}

TEST_CASE("IoPadState: words") {
        const IoChip &chip = bmds::ioChip();
        char          text[bmds::IoRouteTextBytes];

        SUBCASE("plain GPIO") {
                IoPadState s;
                s.function = chip.gpioFunction;
                s.outputSignal = chip.gpioOutputSignal;
                s.outputEnabled = true;
                ioRouteText(chip, 2, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "GPIO") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 2, s), "out") == 0);
                s.outputEnabled = false;
                s.inputEnabled = true;
                s.pullUp = true;
                ioRouteText(chip, 2, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "GPIO") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 2, s), "in") == 0);
                CHECK(std::strcmp(ioPullText(s), "up") == 0);
                s.inputEnabled = false;
                ioRouteText(chip, 2, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "-") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 2, s), "-") == 0);
        }

        SUBCASE("an IO_MUX function") {
                IoPadState s;
                s.function = 0;
                s.inputEnabled = true;
                ioRouteText(chip, 37, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "UART0_TXD (IO_MUX)") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 37, s), "periph") == 0);
        }

        SUBCASE("a pad's own IO_MUX function (its reset value) drives nothing") {
                IoPadState s;
                s.function = 0; // GPIO14_0
                ioRouteText(chip, 14, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "-") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 14, s), "-") == 0);
                s.inputEnabled = true;
                ioRouteText(chip, 14, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "GPIO14_0 (IO_MUX)") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 14, s), "in") == 0);
                CHECK_FALSE(chip.function(14, 0)->peripheral);
                CHECK(chip.function(37, 0)->peripheral);
                CHECK_FALSE(chip.function(37, 1)->peripheral);
        }

        SUBCASE("matrix signals, a bidirectional one named once") {
                IoPadState s;
                s.function = chip.gpioFunction;
                s.outputSignal = 69; // I2C0_SDA
                s.outputByPeripheral = true;
                s.inputEnabled = true;
                s.openDrain = true;
                s.pullUp = true;
                const uint16_t inputs[] = {69};
                ioRouteText(chip, 7, s, inputs, text, sizeof(text));
                CHECK(std::strcmp(text, "I2C0_SDA") == 0);
                CHECK(std::strcmp(ioDirectionText(chip, 7, s), "periph,od") == 0);
                const uint16_t two[] = {10, 13}; // UART0_RXD, UART1_RXD
                s.outputSignal = chip.gpioOutputSignal;
                s.outputByPeripheral = false;
                s.openDrain = false;
                ioRouteText(chip, 38, s, two, text, sizeof(text));
                CHECK(std::strcmp(text, "UART0_RXD,UART1_RXD") == 0);
        }

        SUBCASE("the LP IO domain, and truncation") {
                IoPadState s;
                s.lpIo = true;
                ioRouteText(chip, 1, s, Span<const uint16_t>(), text, sizeof(text));
                CHECK(std::strcmp(text, "LP IO") == 0);
                char small[6];
                s.lpIo = false;
                s.function = 0;
                CHECK(ioRouteText(chip, 37, s, Span<const uint16_t>(), small, sizeof(small)) == 5);
                CHECK(std::strcmp(small, "UART0") == 0);
        }
}

TEST_CASE("IoPinRow, IoPinDetail: a pad as people are shown it") {
        const IoChip &chip = bmds::ioChip();
        IoRegistry    registry(chip);
        REQUIRE(registry.setBoardUse(35, IoBoardUse::Device).isOk());
        const IoUse uses[] = {{35, "RMII TXD1"}};
        auto [claim, err] = registry.claim("eth0", IoOwnerKind::System, uses, 0, nullptr);
        REQUIRE(err.isOk());
        IoHolder holder;
        REQUIRE(registry.holder(35, holder).isOk());
        IoPinBoard board;
        board.label = "RMII TXD1";
        board.note = "also BOOT";
        std::strcpy(board.where, "EXT1.9");

        SUBCASE("levels: the input's, else the latch's, else none") {
                IoPadState s;
                CHECK(bmds::ioLevelChar(s) == '-');
                s.outputEnabled = true;
                s.outputLevel = true;
                CHECK(bmds::ioLevelChar(s) == '1');
                s.inputEnabled = true;
                CHECK(bmds::ioLevelChar(s) == '0');
                s.lpIo = true;
                CHECK(bmds::ioLevelChar(s) == '?');
        }

        SUBCASE("the inputs a pad feeds") {
                const int16_t sources[] = {-1, 7, 3, 7, 0x3e};
                uint16_t      out[bmds::IoMaxPadInputs];
                CHECK(bmds::ioPadInputs(sources, 7, out) == 2);
                CHECK(out[0] == 1);
                CHECK(out[1] == 3);
                CHECK(bmds::ioPadInputs(sources, 9, out) == 0);
                uint16_t one[1];
                CHECK(bmds::ioPadInputs(sources, 7, one) == 1);
        }

        SUBCASE("a held pad's row: the holder's role over the board's label") {
                IoPadState s;
                s.function = chip.gpioFunction;
                s.outputSignal = chip.gpioOutputSignal;
                s.outputEnabled = true;
                IoPinRow r;
                bmds::ioPinRow(chip, 35, s, Span<const uint16_t>(), holder, board, r);
                CHECK(r.pad == 35);
                CHECK(std::strcmp(r.name, "GPIO35") == 0);
                CHECK(r.resourceClass == IoClass::System);
                CHECK(r.level == '0');
                CHECK(std::strcmp(r.owner, "eth0") == 0);
                CHECK(std::strcmp(r.function, "RMII TXD1") == 0);
                CHECK(std::strcmp(r.route, "GPIO") == 0);
                CHECK(std::strcmp(r.direction, "out") == 0);
                CHECK(std::strcmp(r.where, "EXT1.9") == 0);
        }

        SUBCASE("a free pad's row: the board's label") {
                IoHolder   free;
                IoPinBoard b;
                b.label = "status LED";
                IoPinRow r;
                bmds::ioPinRow(chip, 14, IoPadState(), Span<const uint16_t>(), free, b, r);
                CHECK(r.owner[0] == '\0');
                CHECK(std::strcmp(r.function, "status LED") == 0);
                CHECK(r.where[0] == '\0');
        }

        SUBCASE("details") {
                IoPadState s;
                s.function = chip.gpioFunction;
                s.outputSignal = chip.gpioOutputSignal;
                s.inputEnabled = true;
                s.pullUp = true;
                s.drive = 2;
                const uint16_t inputs[] = {10}; // UART0_RXD
                IoPinDetail    d;
                bmds::ioPinDetail(chip, 35, s, inputs, holder, board, d);
                CHECK(std::strcmp(d.name, "GPIO35") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Board), "RMII TXD1 (EXT1.9)") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Note), "also BOOT") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Class), "system") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Owner), "eth0 as RMII TXD1") == 0);
                CHECK(d.at(IoPinField::Forced)[0] == '\0');
                CHECK(std::strstr(d.at(IoPinField::Chip), "strapping") != nullptr);
                CHECK(std::strstr(d.at(IoPinField::Functions), "*1 GPIO 2 ") != nullptr);
                CHECK(std::strcmp(d.at(IoPinField::Route), "UART0_RXD") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Direction), "in") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Input), "enable 1, level 0, signals UART0_RXD") == 0);
                CHECK(std::strncmp(d.at(IoPinField::Output), "signal 256 (", 12) == 0);
                CHECK(std::strcmp(d.at(IoPinField::Pull), "up") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Drive), "2") == 0);
                CHECK(std::strcmp(d.at(IoPinField::LpIo), "no") == 0);
                CHECK(std::strcmp(bmds::ioPinFieldName(IoPinField::LpIo), "LP IO") == 0);
        }

        SUBCASE("a forced pad's details name the lender") {
                const IoUse take[] = {{35, "probe"}};
                auto [user, uerr] = registry.claim("io", IoOwnerKind::User, take, IoRegistry::Force, nullptr);
                REQUIRE(uerr.isOk());
                IoHolder h;
                REQUIRE(registry.holder(35, h).isOk());
                IoPinDetail d;
                bmds::ioPinDetail(chip, 35, IoPadState(), Span<const uint16_t>(), h, board, d);
                CHECK(std::strcmp(d.at(IoPinField::Owner), "io as probe") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Forced), "from eth0 (RMII TXD1)") == 0);
                CHECK(std::strcmp(d.at(IoPinField::Class), "user") == 0);
        }
}
