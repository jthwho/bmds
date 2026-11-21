/**
 * @file      ioports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ports: a peripheral started on the pins the user chose (devplan/io.md §11). The UARTs:
 * `io/uartN`, checked, started and stopped as it changes, `/dev/ttySN` while one runs.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"
#include "uartconsole.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned UartCount = 5; // the P4's HP UARTs: io/uart0 .. io/uart4

        // The UART's pins in a port: what a pad does for it.
        enum class Role : uint8_t {
                Tx,
                Rx,
                Rts,
                Cts
        };

        constexpr const char *RoleNames[] = {"TX", "RX", "RTS", "CTS"};
        constexpr const char *SignalNames[] = {"TXD", "RXD", "RTS", "CTS"};

        struct PortPin {
                        int8_t           pad = -1;
                        Role             role = Role::Tx;
                        uint16_t         inputSignal = 0;
                        uint32_t         inputWord = 0; // its input signal's old selector (RX, CTS)
                        IoChipOps::Saved saved;
        };

        struct UartPort {
                        char         owner[8] = {}; // `ttyS1`
                        char         key[12] = {};  // `io/uart1`
                        IoUartSetup  setup;         // as running
                        bool         running = false;
                        IoClaim      claim;
                        PortPin      pins[4];
                        size_t       pinCount = 0;
                        UartConsole *console = nullptr; // made when first started, kept
                        bool         fileAdded = false; // /dev/ttySN
                        SysConf::Id  id = SysConf::NoId;
        };

        struct State {
                        Mutex            lock;
                        SysConf::Watcher watcher;
                        UartPort         uarts[UartCount];
        };

        constinit PsramObject<State> state;

        void addTtyFile(UartPort &p);

        // The chip's signal for a UART's pin: `UART1_TXD`.
        Result<uint16_t> signalOf(unsigned uart, Role role) {
                char name[16];
                std::snprintf(name, sizeof(name), "UART%u_%s", uart, SignalNames[size_t(role)]);
                const bool input = role == Role::Rx || role == Role::Cts;
                return input ? ioChip().findInput(StringView(name)) : ioChip().findOutput(StringView(name));
        }

        // A value's pins, the roles they play.
        size_t pinsOf(const IoUartSetup &s, PortPin (&out)[4]) {
                size_t     n = 0;
                const auto add = [&](int8_t pad, Role role) {
                        if (pad < 0) return;
                        out[n] = PortPin();
                        out[n].pad = pad;
                        out[n].role = role;
                        ++n;
                };
                add(s.tx, Role::Tx);
                add(s.rx, Role::Rx);
                add(s.rts, Role::Rts);
                add(s.cts, Role::Cts);
                return n;
        }

        // io/uartN's check: the value parses, and without `force` takes no pin or UART another owner
        // holds or the board wires.
        template <unsigned N> Error checkUart(StringView value) {
                if (value.isEmpty()) return Error::Ok; // off
                if (!Io::isReady()) return Error::NotReady;
                IoUartSetup s;
                char        why[IoUartSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[8];
                std::snprintf(owner, sizeof(owner), "ttyS%u", N);
                const IoResource uart = Io::instance(IoKind::Uart, N);
                if (uart == NoIoResource) {
                        SysConf::refuse("this chip has no such UART");
                        return Error::Invalid;
                }
                if (s.force) return Error::Ok;
                if (IoSetups::takenElsewhere(uart, owner)) return IoSetups::refuseTaken(uart, nullptr);
                PortPin      pins[4];
                const size_t n = pinsOf(s, pins);
                for (size_t i = 0; i < n; ++i)
                        if (IoSetups::takenElsewhere(IoResource(pins[i].pad), owner))
                                return IoSetups::refuseTaken(IoResource(pins[i].pad), nullptr);
                return Error::Ok;
        }

        constexpr const char *UartHelp = "A UART port on chosen pins, /dev/ttySN while set: 'tx=37 rx=38 baud=115200 "
                                         "format=8n1'; rts=, cts= with flow=rtscts; loopback; force (devplan/io.md)";
        BMDS_SYSCONF(ioUart0, "io/uart0", Config::Type::String, "", UartHelp, 0, 0, nullptr, checkUart<0>);
        BMDS_SYSCONF(ioUart1, "io/uart1", Config::Type::String, "", UartHelp, 0, 0, nullptr, checkUart<1>);
        BMDS_SYSCONF(ioUart2, "io/uart2", Config::Type::String, "", UartHelp, 0, 0, nullptr, checkUart<2>);
        BMDS_SYSCONF(ioUart3, "io/uart3", Config::Type::String, "", UartHelp, 0, 0, nullptr, checkUart<3>);
        BMDS_SYSCONF(ioUart4, "io/uart4", Config::Type::String, "", UartHelp, 0, 0, nullptr, checkUart<4>);

        // Stops port @p p: the UART, its pins' registers and input signals as they were, the claim.
        // The lock held.
        void stopUart(UartPort &p) {
                if (!p.running) return;
                if (p.console != nullptr) p.console->stop();
                for (size_t i = p.pinCount; i-- > 0;) {
                        const PortPin &pin = p.pins[i];
                        if (pin.role == Role::Rx || pin.role == Role::Cts)
                                IoChipOps::setInputWord(pin.inputSignal, pin.inputWord);
                        IoChipOps::restorePad(unsigned(pin.pad), pin.saved);
                }
                p.claim.reset();
                p.pinCount = 0;
                p.running = false;
                bmdsInfo("io: %s stopped", p.owner);
                return;
        }

        // Starts port @p p as @p s says. The lock held.
        void startUart(unsigned n, UartPort &p, const IoUartSetup &s) {
                // Its pins and the UART, claimed for the port (forced if the value says so).
                PortPin      pins[4];
                const size_t count = pinsOf(s, pins);
                IoUse        uses[5];
                size_t       u = 0;
                for (size_t i = 0; i < count; ++i) {
                        uses[u++] = IoUse{IoResource(pins[i].pad), RoleNames[size_t(pins[i].role)]};
                }
                uses[u++] = IoUse{Io::instance(IoKind::Uart, n), "the port"};
                IoResource blocked = NoIoResource;
                auto [claim, err] =
                        Io::registry().claim(StringView(p.owner), IoOwnerKind::User, Span<const IoUse>(uses, u),
                                             s.force ? IoRegistry::Force : 0u, &blocked);
                if (err.isError()) {
                        char busy[160];
                        (void)Io::busyText(blocked, busy, sizeof(busy));
                        bmdsWarn("io: %s not started: %s", p.key, busy);
                        return;
                }
                // Its inputs routed first, while the UART is off (its start clears the FIFO, so the
                // switch to the new pad reads as nothing); the UART; then its outputs (idle high
                // from their first instant).
                const auto route = [&](bool outputs) {
                        for (size_t i = 0; i < count; ++i) {
                                PortPin   &pin = pins[i];
                                const bool output = pin.role == Role::Tx || pin.role == Role::Rts;
                                if (output != outputs) continue;
                                const auto [signal, serr] = signalOf(n, pin.role);
                                IoChipOps::savePad(unsigned(pin.pad), pin.saved);
                                IoPinSetup setup;
                                setup.pad = uint8_t(pin.pad);
                                if (output) {
                                        setup.mode = IoPinSetup::Mode::Output;
                                        setup.high = true; // idle
                                        setup.outSignal = serr.isOk() ? signal : IoPinSetup::NoSignal;
                                        IoChipOps::configurePad(unsigned(pin.pad), setup);
                                } else {
                                        setup.mode = IoPinSetup::Mode::Input;
                                        setup.pullUp = true; // a line nobody drives reads idle
                                        IoChipOps::configurePad(unsigned(pin.pad), setup);
                                        if (serr.isOk()) {
                                                pin.inputSignal = signal;
                                                pin.inputWord = IoChipOps::inputWord(signal);
                                                IoChipOps::routeInput(signal, unsigned(pin.pad));
                                        }
                                }
                        }
                };
                const auto unroute = [&](bool outputs) {
                        for (size_t i = count; i-- > 0;) {
                                const PortPin &pin = pins[i];
                                const bool     output = pin.role == Role::Tx || pin.role == Role::Rts;
                                if (output != outputs) continue;
                                if (!output) IoChipOps::setInputWord(pin.inputSignal, pin.inputWord);
                                IoChipOps::restorePad(unsigned(pin.pad), pin.saved);
                        }
                };
                if (p.console == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram,
                                                  sizeof(UartConsole) + UartConsole::TxBytes + UartConsole::RxBytes);
                        if (mem == nullptr) {
                                bmdsWarn("io: %s not started: no memory", p.key);
                                return;
                        }
                        uint8_t *rings = static_cast<uint8_t *>(mem) + sizeof(UartConsole);
                        p.console = ::new (mem)
                                UartConsole(n, WritableByteSpan(rings, UartConsole::TxBytes),
                                            WritableByteSpan(rings + UartConsole::TxBytes, UartConsole::RxBytes));
                }
                route(false);
                if (const Error started = p.console->start(s); started.isError()) {
                        unroute(false);
                        bmdsWarn("io: %s not started: the UART (%s)", p.key, started.desc());
                        return; // the claim ends with this scope
                }
                route(true);
                std::memcpy(p.pins, pins, sizeof(pins));
                p.pinCount = count;
                p.claim = std::move(claim);
                p.setup = s;
                p.running = true;
                addTtyFile(p);
                bmdsInfo("io: %s: /dev/%s at %u %u%c%u%s", p.key, p.owner, unsigned(s.baud), unsigned(s.dataBits),
                         s.parity, unsigned(s.stopBits), s.loopback ? ", looped back" : "");
                return;
        }

        // Applies io/uartN's value now. The lock held.
        void applyUart(unsigned n) {
                UartPort &p = state->uarts[n];
                String    value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                if (value.isEmpty()) {
                        stopUart(p);
                        return;
                }
                IoUartSetup s;
                char        why[IoUartSetup::ReasonBytes];
                if (s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (p.running && s == p.setup) return;
                stopUart(p);
                startUart(n, p, s);
                return;
        }

        void changed(void *arg) {
                (void)arg;
                Mutex::Locker lock(state->lock, WaitForever);
                SysConf::Id   id;
                bool          all = state->watcher.overflowed();
                bool          which[UartCount] = {};
                while (state->watcher.next(id))
                        for (unsigned n = 0; n < UartCount; ++n)
                                if (state->uarts[n].id == id) which[n] = true;
                for (unsigned n = 0; n < UartCount; ++n)
                        if (all || which[n]) applyUart(n);
                return;
        }

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4): every port's
        // pins put back first. No lock: the restart's task, the others about to stop.
        void stopAtRestart() {
                if (!state.isCreated()) return;
                for (UartPort &p : state->uarts) {
                        if (!p.running) continue;
                        for (size_t i = p.pinCount; i-- > 0;) {
                                const PortPin &pin = p.pins[i];
                                if (pin.role == Role::Rx || pin.role == Role::Cts)
                                        IoChipOps::setInputWord(pin.inputSignal, pin.inputWord);
                                IoChipOps::restorePad(unsigned(pin.pad), pin.saved);
                        }
                }
                return;
        }

        bool ttyPresent(void *arg);

        // /dev/ttySN, there while the port runs: added when the port first starts (the table of
        // /dev's files is small, Vfs::MaxDeviceFiles), kept for the boot.
        void addTtyFile(UartPort &p) {
                if (p.fileAdded) return;
                Vfs::DeviceFile file;
                file.name = p.owner;
                file.arg = &p;
                file.isDevice = true;
                file.present = ttyPresent;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        auto         *port = static_cast<UartPort *>(arg);
                        const uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                             ((flags & Vfs::Write) ? FileHandle::Write : 0);
                        if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                        if (!port->running || port->console == nullptr)
                                return makeError<FileHandle::Ptr>(Error::NotExist);
                        return FileHandle::open(*port->console, mode);
                };
                if (const Error err = Vfs::addDeviceFile(file); err.isError())
                        bmdsWarn("io: /dev/%s: %s", p.owner, err.desc());
                else
                        p.fileAdded = true;
                return;
        }

        bool ttyPresent(void *arg) {
                const auto *p = static_cast<const UartPort *>(arg);
                return p->running && p->console != nullptr;
        }

} // namespace

namespace IoSetups {

        Error startPorts() {
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < UartCount; ++n) {
                        UartPort &p = s->uarts[n];
                        std::snprintf(p.owner, sizeof(p.owner), "ttyS%u", n);
                        std::snprintf(p.key, sizeof(p.key), "io/uart%u", n);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(stopAtRestart).isError())
                        bmdsWarn("io: no restart hook: the ports' pins will stay set up across a restart");
                {
                        Mutex::Locker lock(s->lock, WaitForever);
                        for (unsigned n = 0; n < UartCount; ++n) applyUart(n);
                }
                if (const Error err = s->watcher.start(StringView("io"), changed, nullptr); err.isError()) return err;
                return startSpiPorts();
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
