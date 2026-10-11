/**
 * @file      pingcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `ping` command (devplan/network.md §15): ICMP echo on a raw lwIP PCB,
 * printed and summed up as iputils' ping does.
 */

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <lwip/inet_chksum.h>
#include <lwip/ip4_addr.h>
#include <lwip/prot/icmp.h>
#include <lwip/prot/ip4.h>
#include <lwip/raw.h>
#include <bmds/command.h>
#include <bmds/deadline.h>
#include <bmds/ethernet.h>
#include <bmds/interrupt.h>
#include <bmds/netstack.h>
#include <bmds/random.h>
#include <bmds/semaphore.h>
#include <bmds/shell.h>
#include <bmds/spinlock.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t       EchoHeaderBytes = 8;
        constexpr unsigned int DefaultSize = 56;
        constexpr unsigned int MaxSize = 8192;
        constexpr unsigned int DefaultIntervalMs = 1000;
        constexpr unsigned int DefaultWaitMs = 1000;
        constexpr unsigned int ResolveTimeoutMs = 10'000; // lwIP's resolver gives up sooner
        constexpr unsigned int FloodWaitMs = 10; // flood: the next one goes after a reply, or this long
        constexpr size_t       Window = 64;      // send times kept, by sequence number

        struct Reply {
                        uint16_t seq = 0;
                        uint8_t  ttl = 0;
                        uint16_t bytes = 0; // the ICMP message's
                        uint64_t atUs = 0;
                        uint32_t from = 0;
        };

        // What the raw PCB's callback (the network task, core lock held) hands the command.
        struct Pinger {
                        uint16_t  id = 0;
                        Semaphore arrived{1, 0};
                        SpinLock  lock;
                        Reply     replies[8];
                        size_t    count = 0;
                        uint32_t  lost = 0; // replies that didn't fit

                        static u8_t receive(void *arg, struct raw_pcb *, struct pbuf *p, const ip_addr_t *addr) {
                                auto          &self = *static_cast<Pinger *>(arg);
                                const uint64_t now = System::uptimeUs();
                                if (p->tot_len < IP_HLEN) return 0;
                                uint8_t ip[IP_HLEN];
                                (void)pbuf_copy_partial(p, ip, IP_HLEN, 0);
                                const uint16_t ihl = static_cast<uint16_t>((ip[0] & 0x0f) * 4);
                                uint8_t        icmp[EchoHeaderBytes];
                                if (p->tot_len < ihl + EchoHeaderBytes ||
                                    pbuf_copy_partial(p, icmp, EchoHeaderBytes, ihl) != EchoHeaderBytes)
                                        return 0;
                                if (icmp[0] != ICMP_ER || Ethernet::read16(icmp + 4) != self.id) return 0;
                                Reply r;
                                r.seq = Ethernet::read16(icmp + 6);
                                r.ttl = ip[8];
                                r.bytes = static_cast<uint16_t>(p->tot_len - ihl);
                                r.atUs = now;
                                r.from = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));
                                {
                                        SpinLock::Locker l(self.lock);
                                        if (self.count < sizeof(self.replies) / sizeof(self.replies[0]))
                                                self.replies[self.count++] = r;
                                        else
                                                ++self.lost;
                                }
                                self.arrived.release();
                                pbuf_free(p);
                                return 1;
                        }

                        bool take(Reply &out) {
                                SpinLock::Locker l(lock);
                                if (count == 0) return false;
                                out = replies[0];
                                for (size_t i = 1; i < count; ++i) replies[i - 1] = replies[i];
                                --count;
                                return true;
                        }
        };

        bool parseNumber(const char *text, unsigned long min, unsigned long max, unsigned long &out) {
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 10);
                if (end == text || *end != '\0' || v < min || v > max) return false;
                out = v;
                return true;
        }

        struct Options {
                        unsigned long count = 0; // 0: until Ctrl-C
                        unsigned long intervalMs = DefaultIntervalMs;
                        unsigned long size = DefaultSize;
                        unsigned long waitMs = DefaultWaitMs;
                        bool          flood = false;
                        bool          quiet = false;
                        const char   *host = nullptr; // a name or an address
                        uint32_t      target = 0;     // its address
        };

        bool parseOptions(Span<char *const> args, Options &o) {
                bool haveTarget = false;
                for (size_t i = 1; i < args.size(); ++i) {
                        const char *a = args[i];
                        const bool  hasValue = i + 1 < args.size();
                        if (std::strcmp(a, "-c") == 0 && hasValue) {
                                if (!parseNumber(args[++i], 1, 1'000'000, o.count)) return false;
                        } else if (std::strcmp(a, "-i") == 0 && hasValue) {
                                if (!parseNumber(args[++i], 1, 3'600'000, o.intervalMs)) return false;
                        } else if (std::strcmp(a, "-s") == 0 && hasValue) {
                                if (!parseNumber(args[++i], 0, MaxSize, o.size)) return false;
                        } else if (std::strcmp(a, "-W") == 0 && hasValue) {
                                if (!parseNumber(args[++i], 1, 60'000, o.waitMs)) return false;
                        } else if (std::strcmp(a, "-f") == 0) {
                                o.flood = true;
                        } else if (std::strcmp(a, "-q") == 0) {
                                o.quiet = true;
                        } else if (a[0] != '-' && !haveTarget) {
                                o.host = a;
                                haveTarget = true;
                        } else {
                                return false;
                        }
                }
                return haveTarget;
        }

        // Sends echo request @p seq (core lock held). Its first 8 bytes of data are the send time.
        err_t sendEcho(struct raw_pcb *pcb, const Options &o, uint16_t id, uint16_t seq, uint64_t nowUs) {
                const u16_t  bytes = static_cast<u16_t>(EchoHeaderBytes + o.size);
                struct pbuf *p = pbuf_alloc(PBUF_IP, bytes, PBUF_RAM);
                if (p == nullptr) return ERR_MEM;
                auto *m = static_cast<uint8_t *>(p->payload); // PBUF_RAM: one piece
                m[0] = ICMP_ECHO;
                m[1] = 0;
                Ethernet::write16(m + 2, 0);
                Ethernet::write16(m + 4, id);
                Ethernet::write16(m + 6, seq);
                for (size_t i = 0; i < o.size; ++i) m[EchoHeaderBytes + i] = static_cast<uint8_t>(i);
                if (o.size >= 8) std::memcpy(m + EchoHeaderBytes, &nowUs, 8);
                const u16_t sum = inet_chksum(m, bytes);
                std::memcpy(m + 2, &sum, 2); // already in network order
                const ip_addr_t to = IPADDR4_INIT(lwip_htonl(o.target));
                const err_t     err = raw_sendto(pcb, p, &to);
                pbuf_free(p);
                return err;
        }

        int runPing(CommandContext &ctx) {
                Options o;
                if (!parseOptions(ctx.args(), o)) return ctx.usage();
                if (!NetStack::isReady()) {
                        ctx.eprintf("ping: no network\n");
                        return Shell::ExitFailure;
                }
                if (o.flood) o.intervalMs = FloodWaitMs;
                uint32_t    found = 0;
                const Error rerr = NetStack::resolve(StringView(o.host), found, ResolveTimeoutMs);
                if (rerr.isError()) {
                        const char *why = rerr == Error::NotExist      ? "Name or service not known"
                                          : rerr == Error::NotReady    ? "No DNS server"
                                          : rerr == Error::Timeout     ? "Temporary failure in name resolution"
                                          : rerr == Error::Interrupted ? "Interrupted"
                                                                       : rerr.desc();
                        ctx.eprintf("ping: %s: %s\n", o.host, why);
                        return Shell::ExitFailure;
                }
                o.target = lwip_ntohl(found);

                Pinger pinger;
                Random::fill(&pinger.id, sizeof(pinger.id));
                struct raw_pcb *pcb;
                {
                        NetStack::Locker lock;
                        pcb = raw_new(IP_PROTO_ICMP);
                        if (pcb != nullptr) raw_recv(pcb, &Pinger::receive, &pinger);
                }
                if (pcb == nullptr) {
                        ctx.eprintf("ping: out of memory\n");
                        return Shell::ExitFailure;
                }

                char target[Ipv4::TextBytes + 1];
                Ipv4::format(o.target, target);
                ctx.printf("PING %s (%s) %lu(%lu) bytes of data.\n", o.host, target, o.size,
                           o.size + EchoHeaderBytes + IP_HLEN);

                uint64_t       sentAt[Window] = {};
                uint32_t       transmitted = 0, received = 0, errors = 0;
                uint64_t       minUs = UINT64_MAX, maxUs = 0;
                double         sumUs = 0, sumSquares = 0;
                const uint64_t startUs = System::uptimeUs();
                bool           interrupted = false;
                uint16_t       seq = 0;

                while (!interrupted && (o.count == 0 || transmitted < o.count)) {
                        ++seq;
                        const uint64_t now = System::uptimeUs();
                        err_t          err;
                        {
                                NetStack::Locker lock;
                                err = sendEcho(pcb, o, pinger.id, seq, now);
                        }
                        sentAt[seq % Window] = now;
                        ++transmitted;
                        if (err != ERR_OK) {
                                ++errors;
                                if (!o.quiet && !o.flood)
                                        ctx.printf("ping: sendmsg: %s\n", err == ERR_RTE ? "Network is unreachable"
                                                                                         : "No buffer space available");
                        } else if (o.flood && !o.quiet) {
                                ctx.printf(".");
                        }

                        // Replies until the next send (after the last one: until the wait runs out).
                        const bool     last = o.count != 0 && transmitted == o.count;
                        const Deadline until(last ? static_cast<unsigned int>(o.waitMs)
                                                  : static_cast<unsigned int>(o.intervalMs));
                        for (;;) {
                                Reply r;
                                bool  gotOurs = false;
                                while (pinger.take(r)) {
                                        const uint64_t sent = sentAt[r.seq % Window];
                                        if (sent == 0 || static_cast<uint16_t>(seq - r.seq) >= Window) continue;
                                        const uint64_t rtt = r.atUs - sent;
                                        ++received;
                                        minUs = rtt < minUs ? rtt : minUs;
                                        maxUs = rtt > maxUs ? rtt : maxUs;
                                        sumUs += static_cast<double>(rtt);
                                        sumSquares += static_cast<double>(rtt) * static_cast<double>(rtt);
                                        sentAt[r.seq % Window] = 0; // a duplicate isn't counted twice
                                        if (r.seq == seq) gotOurs = true;
                                        if (o.quiet) {
                                        } else if (o.flood) {
                                                ctx.printf("\b");
                                        } else {
                                                char from[Ipv4::TextBytes + 1];
                                                Ipv4::format(r.from, from);
                                                ctx.printf("%u bytes from %s: icmp_seq=%u ttl=%u time=%" PRIu64
                                                           ".%03" PRIu64 " ms\n",
                                                           r.bytes, from, r.seq, r.ttl, rtt / 1000, rtt % 1000);
                                        }
                                }
                                if ((o.flood || last) && gotOurs) break;
                                const Error w = Interrupt::waitCurrent(pinger.arrived, until);
                                if (w == Error::Interrupted || ctx.isInterrupted()) {
                                        interrupted = true;
                                        break;
                                }
                                if (w == Error::Timeout) break;
                        }
                }

                {
                        NetStack::Locker lock;
                        raw_remove(pcb);
                }
                const uint64_t elapsedMs = (System::uptimeUs() - startUs) / 1000;
                if (o.flood && !o.quiet) ctx.printf("\n");
                ctx.printf("\n--- %s ping statistics ---\n", target);
                ctx.printf("%" PRIu32 " packets transmitted, %" PRIu32 " received", transmitted, received);
                if (errors != 0) ctx.printf(", +%" PRIu32 " errors", errors);
                const unsigned loss =
                        transmitted == 0 ? 0 : static_cast<unsigned>((transmitted - received) * 100ull / transmitted);
                ctx.printf(", %u%% packet loss, time %" PRIu64 "ms\n", loss, elapsedMs);
                if (received != 0) {
                        const double avg = sumUs / received;
                        const double var = sumSquares / received - avg * avg;
                        const double mdev = var > 0 ? std::sqrt(var) : 0;
                        ctx.printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", static_cast<double>(minUs) / 1000,
                                   avg / 1000, static_cast<double>(maxUs) / 1000, mdev / 1000);
                }
                if (interrupted && received != 0) return Shell::ExitSuccess;
                return received != 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

} // namespace

BMDS_SHELL_COMMAND(ping, runPing, "[-c COUNT] [-i MS] [-s BYTES] [-W MS] [-f] [-q] HOST",
                   "Send ICMP echo requests to a host (a name or IPv4 address) and show the replies (-f: flood)")

BMDS_NAMESPACE_END
