/**
 * @file      netcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `net` command (devplan/network.md §4, step 1): the Ethernet devices'
 * state and counters, their PHY's registers, and an ARP probe that sends a
 * frame and waits for the answer, with both hardware timestamps, before
 * there is an IP stack.
 */

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/ethernetdevice.h>
#include <bmds/semaphore.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr unsigned int ArpWaitMs = 1000;
        constexpr uint8_t      PhyRegisters = 32;

        Result<EthernetDevice *> pick(CommandContext &ctx, const char *name) {
                size_t n = 0;
                if (name != nullptr) {
                        if (std::strncmp(name, "eth", 3) != 0 || name[3] < '0' || name[3] > '9' || name[4] != '\0') {
                                ctx.eprintf("net: %s: not an interface name (eth0)\n", name);
                                return makeError<EthernetDevice *>(Error::Invalid);
                        }
                        n = static_cast<size_t>(name[3] - '0');
                }
                auto r = EthernetDevice::device(n);
                if (r.error().isError()) ctx.eprintf("net: eth%u: no such interface\n", static_cast<unsigned>(n));
                return r;
        }

        void showDevice(CommandContext &ctx, EthernetDevice &d) {
                char mac[MacAddress::TextBytes + 1];
                d.address().toString(mac);
                const EthernetDevice::Link l = d.link();
                if (l.up)
                        ctx.printf("%s: %s, link up, %u Mbit/s %s duplex\n", d.name(), mac, l.speedMbps,
                                   l.fullDuplex ? "full" : "half");
                else
                        ctx.printf("%s: %s, link down\n", d.name(), mac);
                const EthernetDevice::Features &f = d.features();
                ctx.printf("  MAC version %08" PRIx32 ", features %08" PRIx32
                           ": %u rx + %u tx DMA channels%s%s%s%s%s%s%s%s\n",
                           f.version, f.raw, f.rxChannels, f.txChannels, f.avb ? ", AV" : "",
                           f.timestampV2 ? ", 1588-2008" : "", f.txChecksum ? ", tx checksum" : "",
                           f.rxChecksum ? ", rx checksum" : "", f.extraAddresses ? ", address filters" : "",
                           f.hashFilter ? ", hash filter" : "", f.l3l4Filter ? ", L3/L4 filter" : "",
                           f.flexiblePps ? ", flexible PPS" : "");
                const EthernetDevice::Counters c = d.counters();
                ctx.printf("  rx %" PRIu64 " frames %" PRIu64 " bytes, %" PRIu32 " errors, %" PRIu32
                           " no buffer, %" PRIu32 " missed, %" PRIu32 " overflow, %" PRIu32 " unclaimed\n",
                           c.rxFrames, c.rxBytes, c.rxErrors, c.rxNoBuffer, c.rxMissed, c.rxOverflow, c.rxUnclaimed);
                ctx.printf("  tx %" PRIu64 " frames %" PRIu64 " bytes, %" PRIu32 " errors, %" PRIu32
                           " waited; most queued %" PRIu32 "\n",
                           c.txFrames, c.txBytes, c.txErrors, c.txFull, c.txQueuedMax);
                ctx.printf("  buffers held %" PRIu32 " (most %" PRIu32 "), %" PRIu32 " interrupts, %" PRIu32
                           " link changes\n",
                           c.rxHeld, c.rxHeldMax, c.interrupts, c.linkChanges);
                const PtpTime t = d.ptpTime();
                if (Clock::isSet())
                        ctx.printf("  1588 clock %" PRIu64 ".%09" PRIu32 " (%+" PRId64 " us from the system clock)\n",
                                   t.seconds, t.nanoseconds, (t.toNs() - Clock::now().ns) / 1000);
                else
                        ctx.printf("  1588 clock %" PRIu64 ".%09" PRIu32 " (the system clock isn't set)\n", t.seconds,
                                   t.nanoseconds);
                return;
        }

        int runStatus(CommandContext &ctx, const char *name) {
                if (name == nullptr && EthernetDevice::count() == 0) {
                        ctx.printf("net: no Ethernet interfaces\n");
                        return Shell::ExitSuccess;
                }
                if (name != nullptr) {
                        auto [d, err] = pick(ctx, name);
                        if (err.isError()) return Shell::ExitFailure;
                        showDevice(ctx, *d);
                        return Shell::ExitSuccess;
                }
                for (size_t i = 0; i < EthernetDevice::count(); ++i)
                        showDevice(ctx, *EthernetDevice::device(i).value());
                return Shell::ExitSuccess;
        }

        bool parseNumber(const char *text, unsigned long max, unsigned long &out) {
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 0);
                if (end == text || *end != '\0' || v > max) return false;
                out = v;
                return true;
        }

        int runPhy(CommandContext &ctx, Args args) {
                auto [d, err] = pick(ctx, nullptr);
                if (err.isError()) return Shell::ExitFailure;
                if (args.size() == 2) {
                        for (uint8_t r = 0; r < PhyRegisters; ++r) {
                                auto [v, e] = d->readPhy(r);
                                if (e.isError()) {
                                        ctx.eprintf("net: PHY register %u: %s\n", r, e.desc());
                                        return Shell::ExitFailure;
                                }
                                ctx.printf("%2u: %04x%s", r, v, (r % 8 == 7) ? "\n" : "  ");
                        }
                        return Shell::ExitSuccess;
                }
                unsigned long reg = 0, value = 0;
                if (!parseNumber(args[2], PhyRegisters - 1, reg)) return ctx.usage();
                if (args.size() == 4) {
                        if (!parseNumber(args[3], 0xffff, value)) return ctx.usage();
                        const Error e = d->writePhy(static_cast<uint8_t>(reg), static_cast<uint16_t>(value));
                        if (e.isError()) {
                                ctx.eprintf("net: PHY register %lu: %s\n", reg, e.desc());
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                auto [v, e] = d->readPhy(static_cast<uint8_t>(reg));
                if (e.isError()) {
                        ctx.eprintf("net: PHY register %lu: %s\n", reg, e.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("%04x\n", v);
                return Shell::ExitSuccess;
        }

        // The probe's receiver: the first ARP reply from the address asked about.
        struct Probe {
                        uint32_t   target = 0;
                        Semaphore  answered{1, 0};
                        MacAddress mac;
                        PtpTime    stamp;
                        bool       stamped = false;
                        bool       done = false;

                        static void receive(void *arg, EthernetDevice &device, const EthernetDevice::RxFrame &frame) {
                                auto                     &p = *static_cast<Probe *>(arg);
                                Ethernet::Header          h;
                                Arp::Packet               a;
                                const Span<const uint8_t> bytes(frame.data, frame.length);
                                if (!p.done && Ethernet::parseHeader(bytes, h) &&
                                    h.etherType == static_cast<uint16_t>(Ethernet::EtherType::Arp) &&
                                    Arp::parse(bytes.mid(h.payloadOffset), a) && a.op == Arp::Op::Reply &&
                                    a.senderIp == p.target) {
                                        p.mac = a.senderMac;
                                        p.stamp = frame.stamp;
                                        p.stamped = frame.stamped;
                                        p.done = true;
                                        p.answered.release();
                                }
                                device.release(frame);
                                return;
                        }
        };

        int runArp(CommandContext &ctx, Args args) {
                if (args.size() < 3) return ctx.usage();
                uint32_t target = 0, source = 0;
                if (!Arp::parseIpv4(StringView(args[2]), target)) return ctx.usage();
                if (args.size() == 5 && std::strcmp(args[3], "-s") == 0) {
                        if (!Arp::parseIpv4(StringView(args[4]), source)) return ctx.usage();
                } else if (args.size() != 3) {
                        return ctx.usage();
                }
                auto [d, err] = pick(ctx, nullptr);
                if (err.isError()) return Shell::ExitFailure;

                Arp::Packet request;
                request.op = Arp::Op::Request;
                request.senderMac = d->address();
                request.senderIp = source; // 0.0.0.0: a probe (RFC 5227), as there is no address yet
                request.targetIp = target;
                uint8_t      frame[Ethernet::MinFrameBytes];
                const size_t length = Arp::writeFrame(request, Span<uint8_t>(frame, sizeof(frame)));

                Probe probe;
                probe.target = target;
                d->setReceiver(&Probe::receive, &probe);
                auto [ticket, sendErr] =
                        d->transmit(Span<const uint8_t>(frame, length), EthernetDevice::TxStamp, ArpWaitMs);
                Error waitErr = sendErr;
                if (sendErr.isOk()) waitErr = probe.answered.acquire(ArpWaitMs);
                d->setReceiver(nullptr, nullptr);
                if (sendErr.isError()) {
                        ctx.eprintf("net: send: %s\n", sendErr.desc());
                        return Shell::ExitFailure;
                }
                char ip[Arp::Ipv4TextBytes + 1];
                Arp::formatIpv4(target, ip);
                if (waitErr.isError()) {
                        ctx.printf("%s: no answer\n", ip);
                        return Shell::ExitFailure;
                }
                char mac[MacAddress::TextBytes + 1];
                probe.mac.toString(mac);
                auto [sent, stampErr] = d->waitTxStamp(ticket, ArpWaitMs);
                if (stampErr.isOk() && probe.stamped)
                        ctx.printf("%s is at %s, %" PRId64 " ns (hardware stamps)\n", ip, mac,
                                   probe.stamp.toNs() - sent.toNs());
                else
                        ctx.printf("%s is at %s (no stamps: %s)\n", ip, mac,
                                   stampErr.isError() ? stampErr.desc() : "receive");
                return Shell::ExitSuccess;
        }

        int runRegs(CommandContext &ctx) {
                auto [d, err] = pick(ctx, nullptr);
                if (err.isError()) return Shell::ExitFailure;
                const EthernetDevice::Registers r = d->registers();
                ctx.printf("MAC config %08" PRIx32 ", frame filter %08" PRIx32 ", debug %08" PRIx32 "\n", r.macConfig,
                           r.frameFilter, r.macDebug);
                ctx.printf("DMA status %08" PRIx32 " (rx state %" PRIu32 ", tx state %" PRIu32 "), mode %08" PRIx32
                           "\n",
                           r.dmaStatus, (r.dmaStatus >> 17) & 7, (r.dmaStatus >> 20) & 7, r.dmaMode);
                ctx.printf("GMAC control %08" PRIx32 ", clocks %08" PRIx32 " %08" PRIx32 "\n", r.gmacCtrl, r.clkCtrl0,
                           r.clkCtrl1);
                ctx.printf("rx ring %08" PRIx32 ", DMA at %08" PRIx32 " (descriptor %" PRIu32 "), driver at %" PRIu32
                           ", %" PRIu32 " owned by the DMA, %" PRIu32 " empty, %" PRIu32 " buffers free\n",
                           r.rxList, r.rxCurrent, (r.rxCurrent - r.rxList) / 64, r.rxNext, r.rxOwnedByDma, r.rxEmpty,
                           r.rxFree);
                return Shell::ExitSuccess;
        }

        int runNet(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() < 2) return runStatus(ctx, nullptr);
                const char *sub = args[1];
                if (std::strncmp(sub, "eth", 3) == 0 && args.size() == 2) return runStatus(ctx, sub);
                if (std::strcmp(sub, "phy") == 0 && args.size() <= 4) return runPhy(ctx, args);
                if (std::strcmp(sub, "arp") == 0) return runArp(ctx, args);
                if (std::strcmp(sub, "regs") == 0 && args.size() == 2) return runRegs(ctx);
                if (std::strcmp(sub, "phyreset") == 0 && args.size() == 2) {
                        auto [d, err] = pick(ctx, nullptr);
                        if (err.isError()) return Shell::ExitFailure;
                        d->restartPhy();
                        return Shell::ExitSuccess;
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(net, runNet, "[ethN] | phy [REG [VALUE]] | phyreset | arp ADDRESS [-s FROM] | regs",
                   "Show the network interfaces; read, write or reset the PHY; ask who has an address (ARP); show the "
                   "MAC's registers")

BMDS_NAMESPACE_END
