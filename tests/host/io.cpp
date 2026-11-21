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
#include <bmds/ioline.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/pwmtiming.h>

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

TEST_CASE("IoRegistry: the change hook, after the lock") {
        IoRegistry registry(bmds::ioChip());
        struct Seen {
                        IoRegistry *registry = nullptr;
                        int         calls = 0;
                        IoClass     classOf14 = IoClass::Free; // read inside the hook: the lock is let go
        } seen;
        seen.registry = &registry;
        registry.setChangeHook(
                [](void *arg) {
                        auto *s = static_cast<Seen *>(arg);
                        ++s->calls;
                        s->classOf14 = s->registry->resourceClass(14);
                },
                &seen);
        const IoUse line[] = {{14, "line"}};
        {
                auto [claim, err] = registry.claim("gpioset", IoOwnerKind::User, line, 0, nullptr);
                REQUIRE(err.isOk());
                CHECK(seen.calls == 1);
                CHECK(seen.classOf14 == IoClass::User);
                auto [again, busy] = registry.claim("other", IoOwnerKind::User, line, 0, nullptr);
                CHECK(busy == Error::Busy);
                CHECK(seen.calls == 2); // a failed claim is told too
                CHECK(claim.release(14).isOk());
                CHECK(seen.calls == 3);
                CHECK(seen.classOf14 == IoClass::Free);
        }
        CHECK(seen.calls == 4); // the claim's end: its owner ended
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

TEST_CASE("IoGpioSetup: io/gpio's value") {
        using bmds::IoGpioSetup;
        using bmds::IoPinSetup;
        using bmds::StringView;
        const IoChip &chip = bmds::ioChip();
        IoGpioSetup   setup;
        char          why[IoGpioSetup::ReasonBytes];

        SUBCASE("every word") {
                REQUIRE(setup.parse(chip,
                                    "14=out,high 15=in,pull-up GPIO16=od,low,drive=3 17=off,pull-down 35=in,force", why)
                                .isOk());
                CHECK(why[0] == '\0');
                REQUIRE(setup.count() == 5);
                const IoPinSetup *p = setup.find(14);
                REQUIRE(p != nullptr);
                CHECK(p->mode == IoPinSetup::Mode::Output);
                CHECK(p->high);
                CHECK(p->isOutput());
                CHECK(p->function == -1);
                CHECK(p->outSignal == IoPinSetup::NoSignal);
                CHECK(setup.find(15)->pullUp);
                CHECK(setup.find(15)->mode == IoPinSetup::Mode::Input);
                CHECK(setup.find(16)->mode == IoPinSetup::Mode::OpenDrain);
                CHECK(setup.find(16)->drive == 3);
                CHECK_FALSE(setup.find(16)->high);
                CHECK(setup.find(17)->mode == IoPinSetup::Mode::Off);
                CHECK(setup.find(17)->pullDown);
                CHECK(setup.find(35)->force);
                CHECK(setup.find(18) == nullptr);
        }

        SUBCASE("an empty value, and spaces") {
                CHECK(setup.parse(chip, "", why).isOk());
                CHECK(setup.count() == 0);
                CHECK(setup.parse(chip, "  14=out   15=in ", why).isOk());
                CHECK(setup.count() == 2);
        }

        SUBCASE("IO_MUX functions by number or name, the GPIO's as none") {
                REQUIRE(setup.parse(chip, "37=func=0 38=func=uart0_rxd 2=func=1", why).isOk());
                CHECK(setup.find(37)->function == 0);
                CHECK(setup.find(38)->function == 0); // UART0_RXD is 38's function 0
                CHECK(setup.find(2)->function == -1); // the GPIO
                CHECK(setup.parse(chip, "14=func=UART0_TXD", why) == Error::Invalid);
                CHECK(std::strstr(why, "GPIO14 has no IO_MUX function") != nullptr);
        }

        SUBCASE("matrix signals") {
                REQUIRE(setup.parse(chip, "14=out,out:UART1_TXD 15=in:uart1_rxd,in:I2C0_SDA,pull-up", why).isOk());
                CHECK(setup.find(14)->outSignal == chip.findOutput("UART1_TXD").value());
                CHECK(setup.find(15)->inputCount == 2);
                CHECK(setup.find(15)->inputs[0] == chip.findInput("UART1_RXD").value());
                CHECK(setup.parse(chip, "14=in,out:UART1_TXD", why) == Error::Invalid); // a signal needs out or od
                CHECK(setup.parse(chip, "14=out,out:NOPE", why) == Error::Invalid);
                CHECK(std::strstr(why, "no output signal 'NOPE'") != nullptr);
                CHECK(setup.parse(chip, "14=in:UART1_RXD 15=in:UART1_RXD", why) == Error::Exists);
                CHECK(setup.parse(chip, "37=func=0,out,out:UART1_TXD", why) == Error::Invalid);
                CHECK(setup.parse(chip, "14=in:A,in:B,in:C,in:D,in:E", why) == Error::Invalid); // A isn't a signal
        }

        SUBCASE("refused, with the reason") {
                CHECK(setup.parse(chip, "99=out", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'99' isn't a pin of the chip's") == 0);
                CHECK(setup.parse(chip, "x=out", why) == Error::Invalid);
                CHECK(setup.parse(chip, "14=sideways", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO14: 'sideways' isn't a word io/gpio knows") == 0);
                CHECK(setup.parse(chip, "14=out GPIO14=in", why) == Error::Exists);
                CHECK(std::strcmp(why, "GPIO14 is set up twice") == 0);
                CHECK(setup.parse(chip, "14=drive=4", why) == Error::Invalid);
                CHECK(setup.count() == 0); // a refused value leaves nothing
        }

        SUBCASE("edges: which way, debounce, only with the input on") {
                REQUIRE(setup.parse(chip, "14=in,edges,debounce=2000 15=in,rising 16=od,falling 17=in:UART1_RXD,edges",
                                    why)
                                .isOk());
                CHECK(setup.find(14)->edges == (IoPinSetup::Rising | IoPinSetup::Falling));
                CHECK(setup.find(14)->debounceUs == 2000);
                CHECK(setup.find(15)->edges == IoPinSetup::Rising);
                CHECK(setup.find(16)->edges == IoPinSetup::Falling);
                CHECK(setup.find(17)->edges != 0);
                CHECK(setup.find(17)->readsInput());
                CHECK(setup.parse(chip, "14=in,rising,falling", why).isOk());
                CHECK(setup.find(14)->edges == (IoPinSetup::Rising | IoPinSetup::Falling));
                CHECK(setup.parse(chip, "14=out,edges", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO14: edges need its input on (in, od, in:SIGNAL)") == 0);
                CHECK(setup.parse(chip, "14=off,edges", why) == Error::Invalid);
                CHECK(setup.parse(chip, "14=in,debounce=10", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO14: debounce is for edges (edges, rising, falling)") == 0);
                CHECK(setup.parse(chip, "14=in,edges,debounce=1000001", why) == Error::Invalid);
                CHECK(setup.parse(chip, "14=in,edges,debounce=1000000", why).isOk());
                IoGpioSetup other;
                REQUIRE(other.parse(chip, "14=in,edges,debounce=999999", why).isOk());
                CHECK_FALSE(*setup.find(14) == *other.find(14));
        }

        SUBCASE("two setups compared, force included") {
                IoGpioSetup other;
                REQUIRE(setup.parse(chip, "14=out,high 15=in:UART1_RXD", why).isOk());
                REQUIRE(other.parse(chip, "15=in:UART1_RXD 14=out,high", why).isOk());
                CHECK(*setup.find(14) == *other.find(14));
                CHECK(*setup.find(15) == *other.find(15));
                REQUIRE(other.parse(chip, "14=out,high,force", why).isOk());
                CHECK_FALSE(*setup.find(14) == *other.find(14));
        }
}

TEST_CASE("IoUartSetup: io/uartN's value") {
        using bmds::IoUartSetup;
        const IoChip &chip = bmds::ioChip();
        IoUartSetup   u;
        char          why[IoUartSetup::ReasonBytes];

        SUBCASE("the defaults, and every word") {
                REQUIRE(u.parse(chip, "tx=37 rx=GPIO38", why).isOk());
                CHECK(u.tx == 37);
                CHECK(u.rx == 38);
                CHECK(u.baud == 115200);
                CHECK(u.dataBits == 8);
                CHECK(u.parity == 'n');
                CHECK(u.stopBits == 1);
                CHECK_FALSE(u.rtsCts);
                REQUIRE(u.parse(chip, "tx=14 rx=15 rts=16 cts=17 flow=rtscts baud=9600 format=7e2 loopback force", why)
                                .isOk());
                CHECK(u.rts == 16);
                CHECK(u.cts == 17);
                CHECK(u.rtsCts);
                CHECK(u.baud == 9600);
                CHECK(u.dataBits == 7);
                CHECK(u.parity == 'e');
                CHECK(u.stopBits == 2);
                CHECK(u.loopback);
                CHECK(u.force);
                REQUIRE(u.parse(chip, "rx=38", why).isOk()); // receive only
                CHECK(u.tx == -1);
        }

        SUBCASE("refused, with the reason") {
                CHECK(u.parse(chip, "baud=9600", why) == Error::Invalid);
                CHECK(std::strcmp(why, "a UART port needs tx=, rx= or both") == 0);
                CHECK(u.parse(chip, "tx=99", why) == Error::Invalid);
                CHECK(std::strcmp(why, "tx: '99' isn't a pin of the chip's") == 0);
                CHECK(u.parse(chip, "tx=14 rx=14", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO14 is given twice") == 0);
                CHECK(u.parse(chip, "tx=14 baud=100", why) == Error::Invalid);
                CHECK(u.parse(chip, "tx=14 baud=5000001", why) == Error::Invalid);
                CHECK(u.parse(chip, "tx=14 format=9n1", why) == Error::Invalid);
                CHECK(u.parse(chip, "tx=14 format=8x1", why) == Error::Invalid);
                CHECK(u.parse(chip, "tx=14 rts=15", why) == Error::Invalid);             // without flow=rtscts
                CHECK(u.parse(chip, "tx=14 flow=rtscts rts=15", why) == Error::Invalid); // no cts
                CHECK(u.parse(chip, "tx=14 flow=xon", why) == Error::Invalid);
                CHECK(u.parse(chip, "tx=14 parity=e", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'parity=e' isn't a word a UART port knows") == 0);
        }

        SUBCASE("compared") {
                IoUartSetup other;
                REQUIRE(u.parse(chip, "tx=37 rx=38", why).isOk());
                REQUIRE(other.parse(chip, "rx=38 tx=37 baud=115200 format=8n1", why).isOk());
                CHECK(u == other);
                REQUIRE(other.parse(chip, "rx=38 tx=37 baud=9600", why).isOk());
                CHECK_FALSE(u == other);
        }
}

TEST_CASE("IoSpiSetup: io/spiN's value") {
        using bmds::IoSpiSetup;
        const IoChip &chip = bmds::ioChip();
        IoSpiSetup    p;
        char          why[IoSpiSetup::ReasonBytes];

        SUBCASE("the defaults, and every word") {
                REQUIRE(p.parse(chip, "sck=4 mosi=53 miso=54 cs=5", why).isOk());
                CHECK(p.sck == 4);
                CHECK(p.mosi == 53);
                CHECK(p.miso == 54);
                CHECK(p.csCount == 1);
                CHECK(p.cs[0] == 5);
                CHECK(p.mode == 0);
                CHECK(p.clock == 1000000);
                CHECK(p.bits == 8);
                CHECK_FALSE(p.lsbFirst);
                REQUIRE(p.parse(chip, "sck=4 mosi=53 cs=5,6,GPIO7 mode=3 clock=500k bits=16 lsb cs-high loopback force",
                                why)
                                .isOk());
                CHECK(p.csCount == 3);
                CHECK(p.cs[2] == 7);
                CHECK(p.mode == 3);
                CHECK(p.clock == 500000);
                CHECK(p.bits == 16);
                CHECK(p.lsbFirst);
                CHECK(p.csHigh);
                CHECK(p.loopback);
                CHECK(p.force);
                REQUIRE(p.parse(chip, "sck=4 mosi=53 3wire clock=40m", why).isOk()); // no chip select
                CHECK(p.csCount == 0);
                CHECK(p.threeWire);
                CHECK(p.clock == 40000000);
                REQUIRE(p.parse(chip, "sck=4 miso=54 clock=12345", why).isOk()); // receive only
                CHECK(p.clock == 12345);
                CHECK_FALSE(p.dma);
                REQUIRE(p.parse(chip, "sck=4 mosi=53 dma", why).isOk());
                CHECK(p.dma);
        }

        SUBCASE("refused, with the reason") {
                CHECK(p.parse(chip, "mosi=53", why) == Error::Invalid);
                CHECK(std::strcmp(why, "an SPI port needs sck=") == 0);
                CHECK(p.parse(chip, "sck=4", why) == Error::Invalid);
                CHECK(std::strcmp(why, "an SPI port needs mosi=, miso= or both") == 0);
                CHECK(p.parse(chip, "sck=4 mosi=4", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO4 is given twice") == 0);
                CHECK(p.parse(chip, "sck=4 mosi=53 cs=5,53", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 cs=", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 cs=5,6,7,8,9,10,11", why) == Error::Invalid);
                CHECK(std::strcmp(why, "cs takes 6 pins at most") == 0);
                CHECK(p.parse(chip, "sck=4 mosi=53 cs=99", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 mode=4", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 clock=999", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 clock=81m", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 clock=fast", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 bits=9", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 miso=54 loopback", why) == Error::Invalid);
                CHECK(std::strcmp(why, "loopback uses MOSI for what comes in: no miso=") == 0);
                CHECK(p.parse(chip, "sck=4 miso=54 3wire", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 3wire loopback", why) == Error::Invalid);
                CHECK(p.parse(chip, "sck=4 mosi=53 speed=1m", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'speed=1m' isn't a word an SPI port knows") == 0);
        }

        SUBCASE("compared") {
                IoSpiSetup other;
                REQUIRE(p.parse(chip, "sck=4 mosi=53 cs=5", why).isOk());
                REQUIRE(other.parse(chip, "cs=5 mosi=53 sck=4 mode=0 clock=1m", why).isOk());
                CHECK(p == other);
                REQUIRE(other.parse(chip, "sck=4 mosi=53 cs=6", why).isOk());
                CHECK_FALSE(p == other);
        }
}

TEST_CASE("IoLine: a line request's config, Linux's rules") {
        using bmds::IoLineConfig;
        using bmds::IoLineSettings;
        using C = IoLineConfig;
        namespace L = bmds::IoLine;

        SUBCASE("flags checked as Linux checks them") {
                CHECK(L::checkFlags(0).isOk());
                CHECK(L::checkFlags(C::Input | C::EdgeRising | C::EdgeFalling | C::PullUp).isOk());
                CHECK(L::checkFlags(C::Output | C::OpenDrain | C::ActiveLow).isOk());
                CHECK(L::checkFlags(C::Input | C::ClockRealtime).isOk());
                CHECK(L::checkFlags(C::Used) == Error::Invalid);
                CHECK(L::checkFlags(1ull << 20) == Error::Invalid);
                CHECK(L::checkFlags(C::Input | C::Output) == Error::Invalid);
                CHECK(L::checkFlags(C::EdgeRising) == Error::Invalid); // edges need input
                CHECK(L::checkFlags(C::Output | C::EdgeRising) == Error::Invalid);
                CHECK(L::checkFlags(C::Input | C::OpenDrain) == Error::Invalid); // a drive needs output
                CHECK(L::checkFlags(C::Output | C::OpenDrain | C::OpenSource) == Error::Invalid);
                CHECK(L::checkFlags(C::PullUp) == Error::Invalid); // a bias needs a direction
                CHECK(L::checkFlags(C::Input | C::PullUp | C::PullDown) == Error::Invalid);
                CHECK(L::checkFlags(C::Input | C::ClockRealtime | C::ClockHte) == Error::Invalid);
                CHECK(L::checkFlags(C::Input | C::ClockHte) == Error::NotSupported);
        }

        SUBCASE("each line's settings: the config's, then the first attribute by mask") {
                IoLineConfig c;
                c.flags = C::Input | C::PullUp | C::EdgeFalling;
                c.attrCount = 4;
                c.attrs[0].id = C::AttrFlags;
                c.attrs[0].value = C::Output | C::OpenDrain | C::ActiveLow;
                c.attrs[0].mask = 0b0110;
                c.attrs[1].id = C::AttrOutputValues;
                c.attrs[1].value = 0b0100;
                c.attrs[1].mask = 0b0110;
                c.attrs[2].id = C::AttrFlags; // line 1 is the first attribute's already
                c.attrs[2].value = C::Input;
                c.attrs[2].mask = 0b0010;
                c.attrs[3].id = C::AttrDebounce;
                c.attrs[3].value = 5000;
                c.attrs[3].mask = 0b1001;
                IoLineSettings s[4];
                REQUIRE(L::resolve(c, 4, s).isOk());
                CHECK(s[0].direction == IoLineSettings::Direction::Input);
                CHECK(s[0].bias == IoLineSettings::Bias::PullUp);
                CHECK(s[0].edges == IoLineSettings::Falling);
                CHECK(s[0].debounceUs == 5000);
                CHECK(s[1].direction == IoLineSettings::Direction::Output);
                CHECK(s[1].drive == IoLineSettings::Drive::OpenDrain);
                CHECK(s[1].activeLow);
                CHECK_FALSE(s[1].value);
                CHECK(s[2].value);
                CHECK(s[2].debounceUs == 0); // an output's debounce means nothing
                CHECK(s[3].debounceUs == 5000);
                CHECK(s[3].direction == IoLineSettings::Direction::Input);

                IoLineConfig asIs;
                REQUIRE(L::resolve(asIs, 1, s).isOk());
                CHECK(s[0].direction == IoLineSettings::Direction::AsIs);
                CHECK(s[0].bias == IoLineSettings::Bias::AsIs);
        }

        SUBCASE("refused") {
                IoLineConfig   c;
                IoLineSettings s[2];
                CHECK(L::resolve(c, 0, s) == Error::Invalid);
                CHECK(L::resolve(c, 65, s) == Error::Invalid);
                c.attrCount = 11;
                CHECK(L::resolve(c, 1, s) == Error::Invalid);
                c.attrCount = 1;
                c.attrs[0].id = 9;
                CHECK(L::resolve(c, 1, s) == Error::Invalid);
                c.attrs[0].id = C::AttrFlags;
                c.attrs[0].value = C::Input | C::Output;
                CHECK(L::resolve(c, 1, s) == Error::Invalid); // even for no line of the request
                c.attrs[0].value = 0;
                c.attrs[0].padding = 1;
                CHECK(L::resolve(c, 1, s) == Error::Invalid);
                c.attrs[0].padding = 0;
                c.padding[2] = 1;
                CHECK(L::resolve(c, 1, s) == Error::Invalid);
                c.padding[2] = 0;
                c.attrs[0].id = C::AttrDebounce;
                c.attrs[0].value = 1000001;
                CHECK(L::resolve(c, 1, s) == Error::Invalid);
        }
}

TEST_CASE("IoRmtSetup: io/rmtN's value") {
        using bmds::IoRmtSetup;
        const IoChip &chip = bmds::ioChip();
        IoRmtSetup    r;
        char          why[IoRmtSetup::ReasonBytes];

        SUBCASE("the defaults, and every word") {
                REQUIRE(r.parse(chip, "pin=46 dir=tx", why).isOk());
                CHECK(r.pin == 46);
                CHECK_FALSE(r.receive);
                CHECK(r.resolution == 1000000);
                CHECK(r.buffer == 1024);
                CHECK(r.maxTicks() == 32767);
                REQUIRE(r.parse(chip, "pin=GPIO47 dir=tx resolution=10m idle=high invert continuous buffer=4096 force",
                                why)
                                .isOk());
                CHECK(r.resolution == 10000000);
                CHECK(r.idleHigh);
                CHECK(r.invert);
                CHECK(r.continuous);
                CHECK(r.buffer == 4096);
                CHECK(r.pulsePin == -1);
                REQUIRE(r.parse(chip, "pin=46 dir=tx pulse=47 pulse-width=25 pulse-low", why).isOk());
                CHECK(r.pulsePin == 47);
                CHECK(r.pulseWidthUs == 25);
                CHECK(r.pulseLow);
                CHECK(r.dma == 0);
                REQUIRE(r.parse(chip, "pin=46 dir=tx dma", why).isOk());
                CHECK(r.dma == 1024);
                CHECK(r.buffer == 2048); // a ring of two DMA buffers unless asked
                CHECK(r.parse(chip, "pin=46 dir=tx dma=1024 buffer=512", why) == Error::Invalid);
                CHECK(std::strcmp(why, "buffer is at least dma's (1024 symbols) for DMA") == 0);
                REQUIRE(r.parse(chip, "pin=46 dir=tx dma=2046", why).isOk());
                CHECK(r.dma == 2046);
                CHECK(r.parse(chip, "pin=46 dir=tx dma=2048", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx dma=101", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=rx dma", why) == Error::Invalid);
                REQUIRE(r.parse(chip, "pin=46 dir=tx resolution=10m idle=high invert continuous buffer=4096 force", why)
                                .isOk());
                CHECK(r.force);
                REQUIRE(r.parse(chip, "pin=46 dir=rx min=500 max=20000000 monitor", why).isOk());
                CHECK(r.receive);
                CHECK(r.minNs == 500);
                CHECK(r.maxTicks() == 20000); // 20 ms at 1 MHz
                CHECK(r.monitor);
        }

        SUBCASE("refused, with the reason") {
                CHECK(r.parse(chip, "dir=tx", why) == Error::Invalid);
                CHECK(std::strcmp(why, "an RMT port needs pin=") == 0);
                CHECK(r.parse(chip, "pin=46", why) == Error::Invalid);
                CHECK(std::strcmp(why, "an RMT port needs dir=tx or dir=rx") == 0);
                CHECK(r.parse(chip, "pin=46 dir=both", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=99 dir=tx", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx resolution=5k", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx resolution=81m", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx buffer=10", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx idle=middle", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=rx continuous", why) == Error::Invalid);
                CHECK(std::strcmp(why, "idle=, continuous, pulse= and dma are a sender's (dir=tx)") == 0);
                CHECK(r.parse(chip, "pin=46 dir=rx pulse=47", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx pulse=46", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO46 is given twice") == 0);
                CHECK(r.parse(chip, "pin=46 dir=tx pulse=47 pulse-width=0", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx pulse=47 pulse-width=100001", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=tx monitor", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=rx min=5000", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=rx monitor force", why) == Error::Invalid);
                CHECK(r.parse(chip, "pin=46 dir=rx max=40000000", why) == Error::Invalid); // 40000 ticks
                CHECK(std::strcmp(why, "max is at most 32767 ticks: 32767000 ns at this resolution") == 0);
                CHECK(r.parse(chip, "pin=46 dir=tx carrier=38k", why) == Error::Invalid);
        }

        SUBCASE("compared") {
                IoRmtSetup other;
                REQUIRE(r.parse(chip, "pin=46 dir=tx", why).isOk());
                REQUIRE(other.parse(chip, "dir=tx pin=46 resolution=1m buffer=1024", why).isOk());
                CHECK(r == other);
                REQUIRE(other.parse(chip, "dir=tx pin=46 continuous", why).isOk());
                CHECK_FALSE(r == other);
        }
}

TEST_CASE("IoPwmSetup: io/pwmN's value") {
        using bmds::IoPwmSetup;
        const IoChip &chip = bmds::ioChip();
        IoPwmSetup    p;
        char          why[IoPwmSetup::ReasonBytes];

        SUBCASE("times, shares and the defaults") {
                REQUIRE(p.parse(chip, "pin=21", why).isOk());
                CHECK(p.pin == 21);
                CHECK(p.periodNs == 0);
                CHECK(p.dutyNs() == 0);
                REQUIRE(p.parse(chip, "pin=GPIO21 period=1ms duty=25% offset=100us force", why).isOk());
                CHECK(p.periodNs == 1000000);
                CHECK(p.dutyNs() == 250000);
                CHECK(p.offsetNs() == 100000);
                CHECK(p.force);
                REQUIRE(p.parse(chip, "pin=21 period=1.5ms duty=12.5% offset=0.5%", why).isOk());
                CHECK(p.periodNs == 1500000);
                CHECK(p.dutyNs() == 187500);
                CHECK(p.offsetNs() == 7500);
                REQUIRE(p.parse(chip, "pin=21 period=20000 duty=2.5us", why).isOk());
                CHECK(p.periodNs == 20000);
                CHECK(p.dutyNs() == 2500);
                REQUIRE(p.parse(chip, "pin=21 period=13s duty=100%", why).isOk());
                CHECK(p.periodNs == 13000000000ull);
                CHECK(p.dutyNs() == 13000000000ull);
                REQUIRE(p.parse(chip, "pin=21 period=1ms duty=33.33%", why).isOk());
                CHECK(p.dutyNs() == 333300);
        }

        SUBCASE("refusals, in words") {
                CHECK(p.parse(chip, "period=1ms", why) == Error::Invalid);
                CHECK(std::strcmp(why, "a PWM channel needs pin=") == 0);
                CHECK(p.parse(chip, "pin=21 period=1ms duty=2ms", why) == Error::Invalid);
                CHECK(std::strcmp(why, "duty is longer than the period") == 0);
                CHECK(p.parse(chip, "pin=21 period=1ms offset=100%", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 offset=1us", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 period=61s", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 period=1ks", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 period=1. duty=1", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 period=1ms duty=101%", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 period=1ms duty=1.234%", why) == Error::Invalid);
                CHECK(p.parse(chip, "pin=21 freq=1k", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'freq=1k' isn't a word a PWM channel knows") == 0);
                CHECK(p.parse(chip, "pin=99", why) == Error::Invalid);
        }

        SUBCASE("equality") {
                IoPwmSetup a, b;
                REQUIRE(a.parse(chip, "pin=21 period=1ms duty=25%", why).isOk());
                REQUIRE(b.parse(chip, "pin=21 period=1ms duty=250us", why).isOk());
                CHECK_FALSE(a == b); // the same waveform, said differently: a change all the same
                REQUIRE(b.parse(chip, "duty=25% period=1ms pin=21", why).isOk());
                CHECK(a == b);
        }
}

TEST_CASE("LedcTiming: Linux's rounding onto LEDC") {
        using bmds::LedcTiming;
        using bmds::PwmWaveform;
        constexpr uint32_t Hz = 80000000; // 12.5 ns

        SUBCASE("common periods exact, with the most bits that keep them so") {
                LedcTiming t = LedcTiming::round({1000000, 250000, 0}, Hz); // 1 kHz, 25 %
                CHECK(t.ticks == 80000);
                CHECK(t.bits == 15);
                CHECK(t.div256 == 625); // 2.44
                CHECK(t.duty == 8192);
                CHECK(t.waveform(Hz) == PwmWaveform{1000000, 250000, 0});
                t = LedcTiming::round({20000000, 1500000, 0}, Hz); // a servo's 50 Hz, 1.5 ms
                CHECK(t.bits == 17);
                CHECK(t.waveform(Hz).periodNs == 20000000);
                // Counts of 152.6 ns: 1.5 ms is 9830.4 of them, so down to 9830.
                CHECK(t.duty == 9830);
                CHECK(t.waveform(Hz).dutyNs == 1499938);
                t = LedcTiming::round({40000, 20000, 10000}, Hz); // a fan's 25 kHz
                CHECK(t.bits == 11);
                CHECK(t.waveform(Hz) == PwmWaveform{40000, 20000, 10000});
        }

        SUBCASE("down to the next the hardware has, sub-ns dropped") {
                // 1 ms + 5 ns: 80000 ticks and a 5 ns remainder, below a tick.
                LedcTiming t = LedcTiming::round({1000005, 0, 0}, Hz);
                CHECK(t.ticks == 80000);
                // 12.5 ns steps: 37 ns is two ticks (25 ns) and 37.5 would be three.
                t = LedcTiming::round({37, 0, 0}, Hz);
                CHECK(t.ticks == 2);
                CHECK(t.waveform(Hz).periodNs == 25);
                // An odd tick count is kept, at the bits that can (8: a fractional divider).
                t = LedcTiming::round({1000011, 0, 0}, Hz);
                CHECK(t.ticks == 80000);
                t = LedcTiming::round({1000012, 0, 0}, Hz); // 80001 ticks: 1000012.5 ns, its .5 dropped
                CHECK(t.ticks == 80001);
                CHECK(t.bits == 8);
                CHECK(t.waveform(Hz).periodNs == 1000012);
                // A duty between counts: down.
                t = LedcTiming::round({1000000, 250020, 0}, Hz); // counts of 30.5 ns
                CHECK(t.duty == 8192);
        }

        SUBCASE("up only when nothing is below; the edges") {
                LedcTiming t = LedcTiming::round({1, 0, 0}, Hz);
                CHECK(t.ticks == 2);
                CHECK(t.bits == 1);
                CHECK(t.div256 == 256);
                t = LedcTiming::round({0, 0, 0}, Hz);
                CHECK(t.ticks == 0);
                CHECK(t.waveform(Hz) == PwmWaveform{});
                // The longest: a divider of 1023.996 at 20 bits, 13.4 s.
                t = LedcTiming::round({60000000000ull, 60000000000ull, 0}, Hz);
                CHECK(t.bits == 20);
                CHECK(t.div256 == LedcTiming::MaxDiv256);
                CHECK(t.duty == (1u << 20));
                CHECK(t.waveform(Hz).periodNs == 13421721600ull); // 262143 × 4096 ticks
                // Full duty, and the last offset.
                t = LedcTiming::round({1000000, 1000000, 999999}, Hz);
                CHECK(t.duty == (1u << t.bits));
                CHECK(t.hpoint == (1u << t.bits) - 1);
        }

        SUBCASE("a timer shared by the same timing") {
                const LedcTiming a = LedcTiming::round({1000000, 100000, 0}, Hz);
                const LedcTiming b = LedcTiming::round({1000000, 900000, 500000}, Hz);
                const LedcTiming c = LedcTiming::round({500000, 100000, 0}, Hz);
                CHECK(a.sharesTimer(b));
                CHECK_FALSE(a.sharesTimer(c));
        }
}

TEST_CASE("IoPcntSetup: io/pcntN's value") {
        using bmds::IoPcntSetup;
        using F = IoPcntSetup::Function;
        using A = IoPcntSetup::Action;
        const IoChip &chip = bmds::ioChip();
        IoPcntSetup   c;
        char          why[IoPcntSetup::ReasonBytes];

        SUBCASE("the defaults, and every word") {
                REQUIRE(c.parse(chip, "a=46", why).isOk());
                CHECK(c.a == 46);
                CHECK(c.b == -1);
                CHECK(c.function == F::Increase);
                CHECK(c.action == A::Rising);
                CHECK(c.filterNs == 0);
                REQUIRE(c.parse(chip, "a=46 function=decrease action=both filter=1.5us pull-up force", why).isOk());
                CHECK(c.function == F::Decrease);
                CHECK(c.action == A::Both);
                CHECK(c.filterNs == 1500);
                CHECK(c.pullUp);
                CHECK(c.force);
                REQUIRE(c.parse(chip, "a=GPIO46 b=47 function=quadrature-x4 monitor", why).isOk());
                CHECK(c.function == F::QuadratureX4);
                CHECK(c.readsB());
                CHECK(c.monitor);
                REQUIRE(c.parse(chip, "a=46 b=47 function=pulse-direction pull-down", why).isOk());
                CHECK(c.function == F::PulseDirection);
                for (int f = 0; f <= int(F::QuadratureX4); ++f) CHECK(std::strlen(IoPcntSetup::functionName(F(f))) > 0);
                CHECK(std::strcmp(IoPcntSetup::functionName(F::QuadratureX2B), "quadrature-x2-b") == 0);
        }

        SUBCASE("refusals, in words") {
                CHECK(c.parse(chip, "b=47", why) == Error::Invalid);
                CHECK(std::strcmp(why, "a counter needs a=") == 0);
                CHECK(c.parse(chip, "a=46 function=quadrature-x2-a", why) == Error::Invalid);
                CHECK(std::strcmp(why, "quadrature-x2-a needs b=") == 0);
                CHECK(c.parse(chip, "a=46 b=47", why) == Error::Invalid);
                CHECK(std::strcmp(why, "increase reads only a=") == 0);
                CHECK(c.parse(chip, "a=46 b=46 function=quadrature-x4", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 b=47 function=quadrature-x4 action=both", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 function=quadrature-x8", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 action=up", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 filter=11us", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 pull-up pull-down", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 monitor force", why) == Error::Invalid);
                CHECK(c.parse(chip, "a=46 ceiling=100", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'ceiling=100' isn't a word a counter knows") == 0);
        }

        SUBCASE("equality") {
                IoPcntSetup x, y;
                REQUIRE(x.parse(chip, "a=46 b=47 function=quadrature-x4", why).isOk());
                REQUIRE(y.parse(chip, "function=quadrature-x4 b=47 a=46", why).isOk());
                CHECK(x == y);
                REQUIRE(y.parse(chip, "function=quadrature-x2-a b=47 a=46", why).isOk());
                CHECK_FALSE(x == y);
        }
}

TEST_CASE("IoParlioSetup: io/parlioN's value") {
        using bmds::IoParlioSetup;
        const IoChip &chip = bmds::ioChip();
        IoParlioSetup p;
        char          why[IoParlioSetup::ReasonBytes];

        SUBCASE("the defaults, and every word") {
                REQUIRE(p.parse(chip, "dir=tx data=46", why).isOk());
                CHECK_FALSE(p.receive);
                CHECK(p.dataCount == 1);
                CHECK(p.width() == 1);
                CHECK(p.clock == 1000000);
                CHECK_FALSE(p.rising);
                REQUIRE(p.parse(chip, "dir=tx data=46 edge=rising", why).isOk());
                CHECK(p.rising);
                REQUIRE(p.parse(chip, "dir=tx data=46,47,48 clock=10m clk=17 edge=falling msb idle=5 force", why)
                                .isOk());
                CHECK(p.dataCount == 3);
                CHECK(p.data[2] == 48);
                CHECK(p.width() == 4);
                CHECK(p.clock == 10000000);
                CHECK(p.clk == 17);
                CHECK_FALSE(p.rising);
                CHECK(p.msb);
                CHECK(p.idle == 5);
                CHECK(p.force);
                CHECK(p.drive == 2);
                REQUIRE(p.parse(chip, "dir=tx data=46 drive=0", why).isOk());
                CHECK(p.drive == 0);
                CHECK(p.parse(chip, "dir=tx data=46 drive=4", why) == Error::Invalid);
                REQUIRE(p.parse(chip, "dir=rx data=46,47,48,16,17 clk-in=18 clock=40m buffer=32768 monitor", why)
                                .isOk());
                CHECK(p.receive);
                CHECK(p.width() == 8);
                CHECK(p.clkIn == 18);
                CHECK(p.buffer == 32768);
                CHECK(p.monitor);
                REQUIRE(p.parse(chip, "dir=rx data=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15", why).isOk());
                CHECK(p.width() == 16);
                CHECK(p.buffer == IoParlioSetup::DefaultBuffer);
        }

        SUBCASE("refusals, in words") {
                CHECK(p.parse(chip, "data=46", why) == Error::Invalid);
                CHECK(std::strcmp(why, "a PARLIO port needs dir=tx or dir=rx") == 0);
                CHECK(p.parse(chip, "dir=tx", why) == Error::Invalid);
                CHECK(std::strcmp(why, "a PARLIO port needs data=") == 0);
                CHECK(p.parse(chip, "dir=tx data=46,46", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO46 is given twice") == 0);
                CHECK(p.parse(chip, "dir=tx data=46 clk=46", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=tx data=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=tx data=46 buffer=8192", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=tx data=46 monitor", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 idle=1", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 clk=17 clk-in=18", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=tx data=46,47 idle=4", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 buffer=4100", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 buffer=65536", why) == Error::Invalid);
                REQUIRE(p.parse(chip, "dir=rx data=46 buffer=65472", why).isOk());
                CHECK(p.parse(chip, "dir=rx data=46 clock=90m", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 monitor force", why) == Error::Invalid);
                CHECK(p.parse(chip, "dir=rx data=46 valid=47", why) == Error::Invalid);
                CHECK(std::strcmp(why, "'valid=47' isn't a word a PARLIO port knows") == 0);
        }

        SUBCASE("equality") {
                IoParlioSetup a, b;
                REQUIRE(a.parse(chip, "dir=tx data=46,47 clock=1m", why).isOk());
                REQUIRE(b.parse(chip, "clock=1m data=46,47 dir=tx", why).isOk());
                CHECK(a == b);
                REQUIRE(b.parse(chip, "dir=tx data=47,46 clock=1m", why).isOk());
                CHECK_FALSE(a == b);
        }
}

TEST_CASE("IoAdcSetup: io/adc0's value") {
        using bmds::IoAdcSetup;
        using A = IoAdcSetup::Atten;
        const IoChip &chip = bmds::ioChip();
        IoAdcSetup    a;
        char          why[IoAdcSetup::ReasonBytes];

        SUBCASE("pins, their channels, attenuations, streaming") {
                REQUIRE(a.parse(chip, "pins=20", why).isOk());
                CHECK(a.pinCount == 1);
                CHECK(a.unit[0] == 0);
                CHECK(a.channel[0] == 4);
                CHECK(a.atten[0] == A::Db12);
                CHECK(a.rate == 0);
                REQUIRE(a.parse(chip, "pins=20,16:6,49:2.5db atten=0 rate=10k buffer=8192 force", why).isOk());
                CHECK(a.pinCount == 3);
                CHECK(a.atten[0] == A::Db0);
                CHECK(a.atten[1] == A::Db6);
                CHECK(a.unit[2] == 1);
                CHECK(a.channel[2] == 0);
                CHECK(a.atten[2] == A::Db2_5);
                CHECK(a.rate == 10000);
                CHECK(a.buffer == 8192);
                CHECK(a.force);
                CHECK(a.indexOf(49) == 2);
                CHECK(a.indexOf(17) == -1);
                CHECK(IoAdcSetup::centiDb(A::Db2_5) == 250);
                // ADC2_CH2's table entry carries the comparator's functions after it.
                uint8_t u = 0, c = 0;
                REQUIRE(IoAdcSetup::channelOf(chip, 51, u, c));
                CHECK((u == 1 && c == 2));
                CHECK_FALSE(IoAdcSetup::channelOf(chip, 46, u, c));
        }

        SUBCASE("refusals, in words") {
                CHECK(a.parse(chip, "rate=10k", why) == Error::Invalid);
                CHECK(std::strcmp(why, "the ADC needs pins=") == 0);
                CHECK(a.parse(chip, "pins=46", why) == Error::Invalid);
                CHECK(std::strcmp(why, "GPIO46 has no ADC channel") == 0);
                CHECK(a.parse(chip, "pins=20,20", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20:3", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20 atten=11", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20 rate=100", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20 rate=100k", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20 buffer=4096", why) == Error::Invalid);
                CHECK(std::strcmp(why, "buffer= is a stream's: it needs rate=") == 0);
                CHECK(a.parse(chip, "pins=20 rate=1k buffer=100", why) == Error::Invalid);
                CHECK(a.parse(chip, "pins=20 bits=12", why) == Error::Invalid);
        }

        SUBCASE("equality") {
                IoAdcSetup x, y;
                REQUIRE(x.parse(chip, "pins=20,16 atten=6", why).isOk());
                REQUIRE(y.parse(chip, "atten=6 pins=20,16", why).isOk());
                CHECK(x == y);
                REQUIRE(y.parse(chip, "pins=20,16:12 atten=6", why).isOk());
                CHECK_FALSE(x == y);
        }
}
