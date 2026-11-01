/**
 * @file      socket.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The kernel's sockets (devplan/network.md §18) on two interfaces wired
 * back to back (lwipwire.h), lwIP in a network thread, the sockets used
 * from the test's threads: TCP both ways with half closes, refusals,
 * resets, not waiting, timeouts, interrupts; UDP with its queue and
 * multicast; raw ICMP; lwIP's heap back where it was after each.
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <lwip/inet_chksum.h>
#include <lwip/opt.h>
#include <bmds/clock.h>
#include <bmds/interrupt.h>
#include <bmds/netstack.h>
#include <bmds/socket.h>

#include "lwipwire.h"
#include "rxcopies.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::Socket;
using bmds::SocketAddress;
using bmds::Span;
using bmds::WritableByteSpan;

namespace {

        using namespace LwipWire;
        using Option = Socket::Option;

        uint32_t ip(const char *text) {
                return addr(text).addr;
        }

        // A socket and the handle that keeps it.
        struct Sock {
                        FileHandle::Ptr handle;
                        Socket         *socket = nullptr;

                        Socket *operator->() const { return socket; }
                        Socket &operator*() const { return *socket; }
                        void    close() {
                                handle.reset();
                                socket = nullptr;
                        }
        };

        Sock adopt(FileHandle::Ptr handle) {
                Socket *s = handle->stream().socket();
                REQUIRE(s != nullptr);
                return Sock{std::move(handle), s};
        }

        Sock make(Socket::Type type, uint8_t protocol = 0) {
                auto [handle, err] = Socket::create(type, protocol);
                REQUIRE(err.isOk());
                return adopt(std::move(handle));
        }

        bmds::Result<size_t> send(Socket &s, const void *data, size_t n, const SocketAddress *to = nullptr,
                                  uint32_t flags = 0) {
                const ByteSpan piece(static_cast<const uint8_t *>(data), n);
                return s.send(Span<const ByteSpan>(&piece, 1), to, flags);
        }

        bmds::Result<size_t> send(Socket &s, const char *text, const SocketAddress *to = nullptr) {
                return send(s, text, std::strlen(text), to);
        }

        bmds::Result<Socket::Received> receive(Socket &s, void *buf, size_t n, uint32_t flags = 0) {
                const WritableByteSpan piece(static_cast<uint8_t *>(buf), n);
                return s.receive(Span<const WritableByteSpan>(&piece, 1), flags);
        }

        // Receives text (up to 255 bytes), or the error's name.
        std::string text(Socket &s, uint32_t flags = 0) {
                char buf[256];
                auto [r, err] = receive(s, buf, sizeof(buf) - 1, flags);
                if (err.isError()) return err.name();
                return std::string(buf, r.bytes);
        }

        // Waits (up to 2 s) for @p ready.
        template <typename Fn> bool eventually(Fn &&ready) {
                for (int i = 0; i < 2000; ++i) {
                        if (ready()) return true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
        }

        // The closes finish and TIME-WAIT ends, with the thread gone: lwIP's made-up time passes,
        // the wire delivering as it does.
        void finish(size_t used) {
                for (int i = 0; i < 1300; ++i) idle(100);
                CHECK(heapUsed() == used);
                return;
        }

        // Sends through interface a, onto the wire (both interfaces are one stack's: a socket
        // left to the routes takes b's, and a packet to b's own address loops back).
        void onA(Sock &s) {
                const std::string name = std::string{a.netif.name[0], a.netif.name[1]} + std::to_string(a.netif.num);
                REQUIRE(s->bindToDevice(name.c_str()).isOk());
                return;
        }

} // namespace

TEST_CASE("Socket: TCP connect, accept, both ways, half close") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      l = make(Socket::Type::Stream);
                REQUIRE(l->bind({ip("10.0.0.2"), 7000}).isOk());
                REQUIRE(l->listen(4).isOk());
                CHECK(l->option(Option::Listening).value() == 1);
                CHECK(l->address().port == 7000);

                Sock c = make(Socket::Type::Stream);
                onA(c);
                CHECK(c->peer().error() == Error::NotConnected);
                REQUIRE(c->connect({ip("10.0.0.2"), 7000}, 0).isOk());
                CHECK(c->connect({ip("10.0.0.2"), 7000}, 0) == Error::AlreadyConnected);
                SocketAddress peer;
                auto [h, aerr] = l->accept(&peer, 0);
                REQUIRE(aerr.isOk());
                Sock s = adopt(std::move(h));
                CHECK(peer.ipv4 == ip("10.0.0.1"));
                CHECK(peer.port == c->address().port);
                CHECK(c->peer().value().ipv4 == ip("10.0.0.2"));
                CHECK(c->peer().value().port == 7000);
                CHECK(c->address().ipv4 == ip("10.0.0.1"));

                // 200 KB one way, from another thread: the sender waits for the window.
                std::vector<uint8_t> out(200'000);
                for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(i * 7 + (i >> 9));
                std::atomic<size_t>  sent{0};
                std::thread          sender([&] {
                        auto [n, err] = send(*c, out.data(), out.size());
                        sent.store(err.isOk() ? n : 0);
                });
                std::vector<uint8_t> in;
                uint8_t              buf[3000];
                while (in.size() < out.size()) {
                        auto [r, err] = receive(*s, buf, sizeof(buf));
                        REQUIRE(err.isOk());
                        REQUIRE(r.bytes > 0);
                        in.insert(in.end(), buf, buf + r.bytes);
                }
                sender.join();
                CHECK(sent.load() == out.size());
                CHECK(in == out);

                // The other way, gathered from pieces.
                const char    *hello = "hel";
                const char    *rest = "lo";
                const ByteSpan pieces[2] = {ByteSpan(reinterpret_cast<const uint8_t *>(hello), 3),
                                            ByteSpan(reinterpret_cast<const uint8_t *>(rest), 2)};
                CHECK(s->send(Span<const ByteSpan>(pieces, 2), nullptr, 0).value() == 5);
                char got[5];
                CHECK(receive(*c, got, 5, Socket::WaitAll).value().bytes == 5);
                CHECK(std::string(got, 5) == "hello");

                // Half closes: the client's FIN ends the server's reads; it still sends.
                REQUIRE(c->shutdown(false, true).isOk());
                CHECK(text(*s) == "EndOfFile");
                CHECK((s->readiness() & bmds::Stream::Readable) != 0);
                CHECK(send(*c, "late").error() == Error::BrokenPipe);
                CHECK(send(*s, "bye").value() == 3);
                CHECK(text(*c) == "bye");
                s.close();
                CHECK(text(*c) == "EndOfFile");
                CHECK((c->readiness() & bmds::Stream::HungUp) != 0);
        }
        finish(used);
}

TEST_CASE("Socket: TCP's scaled window: more than 64 KB unread, read in odd pieces") {
        // devplan/network.md §32: the window is 251 KB, what it holds the received copies' (PSRAM)
        // slots, and a socket's unread data passes a pbuf chain's 16-bit length.
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      l = make(Socket::Type::Stream);
                REQUIRE(l->bind({ip("10.0.0.2"), 7010}).isOk());
                REQUIRE(l->listen(1).isOk());
                Sock c = make(Socket::Type::Stream);
                onA(c);
                REQUIRE(c->connect({ip("10.0.0.2"), 7010}, 0).isOk());
                auto [h, aerr] = l->accept(nullptr, 0);
                REQUIRE(aerr.isOk());
                Sock s = adopt(std::move(h));
                CHECK(s->option(Option::ReceiveBuffer).value() == TCP_WND);

                std::vector<uint8_t> out(600'000);
                for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(i * 13 + (i >> 11));
                std::atomic<bool> done{false};
                std::thread       sender([&] {
                        (void)send(*c, out.data(), out.size());
                        done.store(true);
                });

                // Nothing read: the sender stops once the window is full, well past 64 KB.
                const size_t frames = (TCP_WND - TCP_MSS) / TCP_MSS; // a segment's worth of slack
                CHECK(eventually([&] {
                        bmds::NetStack::Locker lock;
                        return bmds::RxCopies::usage().used >= frames;
                }));
                CHECK_FALSE(done.load());

                // Read in pieces that cross the delivered chains' ends, with a look first now and
                // then.
                const size_t         sizes[] = {1, 7, 1500, 1461, 70'000, 3, 65'536, 129'000};
                std::vector<uint8_t> in;
                std::vector<uint8_t> buf(129'000);
                for (size_t k = 0; in.size() < out.size(); ++k) {
                        const size_t want = sizes[k % (sizeof(sizes) / sizeof(sizes[0]))];
                        if (k % 5 == 0) {
                                uint8_t peeked[10];
                                auto [p, perr] = receive(*s, peeked, sizeof(peeked), Socket::Peek);
                                REQUIRE(perr.isOk());
                                REQUIRE(p.bytes > 0);
                                CHECK(std::memcmp(peeked, out.data() + in.size(), p.bytes) == 0);
                        }
                        auto [r, err] = receive(*s, buf.data(), want);
                        REQUIRE(err.isOk());
                        REQUIRE(r.bytes > 0);
                        REQUIRE(r.bytes <= want);
                        in.insert(in.end(), buf.data(), buf.data() + r.bytes);
                }
                sender.join();
                CHECK(in == out);

                // Some unread when it closes: the queue is freed whole.
                CHECK(send(*c, out.data(), 100'000).value() == 100'000);
                CHECK(eventually([&] {
                        bmds::NetStack::Locker lock;
                        return bmds::RxCopies::usage().used >= 100'000 / TCP_MSS;
                }));
                s.close();
                c.close();
        }
        finish(used);
        bmds::NetStack::Locker lock;
        CHECK(bmds::RxCopies::usage().used == 0);
}

TEST_CASE("Socket: TCP refused, not waiting, timeouts") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      c = make(Socket::Type::Stream);
                CHECK(c->connect({ip("10.0.0.2"), 7001}, 0) == Error::ConnectionRefused);

                Sock l = make(Socket::Type::Stream);
                REQUIRE(l->listen(2).isOk()); // an ephemeral port
                const uint16_t port = l->address().port;
                CHECK(port != 0);
                CHECK(l->accept(nullptr, Socket::DontWait).error() == Error::WouldBlock);

                Sock d = make(Socket::Type::Stream);
                CHECK(d->connect({ip("10.0.0.1"), port}, Socket::DontWait) == Error::InProgress);
                const Error again = d->connect({ip("10.0.0.1"), port}, Socket::DontWait);
                CHECK((again == Error::AlreadyInProgress || again == Error::AlreadyConnected)); // it may be done
                CHECK(eventually([&] { return (d->readiness() & bmds::Stream::Writable) != 0; }));
                CHECK(d->takeError().isOk());
                CHECK(eventually([&] { return (l->readiness() & bmds::Stream::Readable) != 0; }));
                auto [h, aerr] = l->accept(nullptr, Socket::DontWait);
                REQUIRE(aerr.isOk());
                Sock s = adopt(std::move(h));

                char buf[8];
                CHECK(receive(*d, buf, sizeof(buf), Socket::DontWait).error() == Error::WouldBlock);
                REQUIRE(d->setOption(Option::ReceiveTimeoutMs, 50).isOk());
                const auto start = std::chrono::steady_clock::now();
                CHECK(receive(*d, buf, sizeof(buf)).error() == Error::WouldBlock);
                CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(45));

                // Peeking leaves the data.
                CHECK(send(*s, "peek").value() == 4);
                CHECK(eventually([&] { return (d->readiness() & bmds::Stream::Readable) != 0; }));
                CHECK(text(*d, Socket::Peek) == "peek");
                CHECK(text(*d) == "peek");

                // Refused without waiting: the error comes with the readiness.
                Sock e = make(Socket::Type::Stream);
                CHECK(e->connect({ip("10.0.0.2"), 7001}, Socket::DontWait) == Error::InProgress);
                CHECK(eventually([&] { return (e->readiness() & bmds::Stream::Broken) != 0; }));
                CHECK(e->takeError() == Error::ConnectionRefused);
                CHECK(e->takeError().isOk());
        }
        finish(used);
}

TEST_CASE("Socket: TCP resets") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      l = make(Socket::Type::Stream);
                REQUIRE(l->bind({0, 7002}).isOk());
                REQUIRE(l->listen(2).isOk());

                // A zero linger: the close resets.
                Sock c = make(Socket::Type::Stream);
                REQUIRE(c->connect({ip("10.0.0.2"), 7002}, 0).isOk());
                Sock s = adopt(l->accept(nullptr, 0).value());
                REQUIRE(s->setOption(Option::Linger, 0).isOk());
                s.close();
                CHECK(text(*c) == "ConnectionReset");
                CHECK(text(*c) == "EndOfFile");
                CHECK(send(*c, "x").error() == Error::BrokenPipe);
                const uint8_t all =
                        bmds::Stream::Readable | bmds::Stream::Writable | bmds::Stream::HungUp | bmds::Stream::Broken;
                CHECK(c->readiness() == all);
                c.close();

                // Closed with data unread: a reset too.
                c = make(Socket::Type::Stream);
                REQUIRE(c->connect({ip("10.0.0.2"), 7002}, 0).isOk());
                s = adopt(l->accept(nullptr, 0).value());
                CHECK(send(*c, "unread").value() == 6);
                CHECK(eventually([&] { return (s->readiness() & bmds::Stream::Readable) != 0; }));
                s.close();
                CHECK(text(*c) == "ConnectionReset");
        }
        finish(used);
}

TEST_CASE("Socket: a waiting receive is interrupted") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread         net;
                Sock              u = make(Socket::Type::Datagram);
                bmds::Interrupt   interrupt;
                std::atomic<int>  result{-1};
                std::atomic<bool> waiting{false};
                std::thread       t([&] {
                        bmds::Interrupt::Scope scope(&interrupt);
                        char                   buf[8];
                        waiting.store(true);
                        result.store(receive(*u, buf, sizeof(buf)).error().code());
                });
                CHECK(eventually([&] { return waiting.load(); }));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                interrupt.raise();
                t.join();
                CHECK(result.load() == Error::Interrupted);
        }
        finish(used);
}

TEST_CASE("Socket: UDP datagrams, their queue, addresses") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      u = make(Socket::Type::Datagram);
                REQUIRE(u->bind({ip("10.0.0.2"), 9000}).isOk());
                CHECK(u->bind({ip("10.0.0.2"), 9001}) == Error::Invalid); // bound already
                Sock other = make(Socket::Type::Datagram);
                CHECK(other->bind({ip("10.0.0.2"), 9000}) == Error::AddressInUse);
                CHECK(other->bind({ip("10.9.9.9"), 9000}) == Error::AddressNotAvailable);
                CHECK(Socket::create(Socket::Type::Datagram, 6).error() == Error::NotSupported);
                CHECK(Socket::create(Socket::Type::Raw, 0).error() == Error::NotSupported);

                Sock s = make(Socket::Type::Datagram);
                onA(s);
                const SocketAddress to = {ip("10.0.0.2"), 9000};
                CHECK(send(*s, "nowhere").error() == Error::NotConnected);
                CHECK(send(*s, "hello", &to).value() == 5);
                char buf[200];
                auto [r, err] = receive(*u, buf, sizeof(buf));
                REQUIRE(err.isOk());
                CHECK(std::string(buf, r.bytes) == "hello");
                CHECK(r.from.ipv4 == ip("10.0.0.1"));
                CHECK(r.from.port == s->address().port);

                // Peeked, then taken; cut to fit, the rest dropped.
                CHECK(send(*s, "abc", &to).value() == 3);
                CHECK(text(*u, Socket::Peek) == "abc");
                CHECK(text(*u) == "abc");
                std::string big(100, 'z');
                CHECK(send(*s, big.c_str(), &to).value() == 100);
                auto [cut, cerr] = receive(*u, buf, 10);
                REQUIRE(cerr.isOk());
                CHECK(cut.bytes == 10);
                CHECK(cut.length == 100);
                CHECK(receive(*u, buf, sizeof(buf), Socket::DontWait).error() == Error::WouldBlock);

                // A full queue drops what comes.
                for (int i = 0; i < 20; ++i) CHECK(send(*s, "q", &to).isOk());
                CHECK(eventually([&] { return u->dropped() == 4; }));
                for (size_t i = 0; i < Socket::MaxDatagrams; ++i) CHECK(text(*u, Socket::DontWait) == "q");
                CHECK(text(*u, Socket::DontWait) == "WouldBlock");

                // Connected: no address needed, and only the peer's come in.
                REQUIRE(s->connect(to, 0).isOk());
                CHECK(s->peer().value().port == 9000);
                CHECK(send(*s, "connected").value() == 9);
                CHECK(text(*u) == "connected");
                REQUIRE(u->connect({ip("10.0.0.1"), s->address().port}, 0).isOk());
                CHECK(send(*u, "back").value() == 4);
                CHECK(text(*s) == "back");
                REQUIRE(s->connect({0, 0}, 0).isOk()); // forgotten
                CHECK(send(*s, "x").error() == Error::NotConnected);
                REQUIRE(u->shutdown(true, false).isOk());
                CHECK(text(*u) == "EndOfFile");
        }
        pump();
        CHECK(heapUsed() == used);
}

TEST_CASE("Socket: UDP multicast") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread      net;
                const uint32_t group = ip("239.1.2.3");
                Sock           r = make(Socket::Type::Datagram);
                REQUIRE(r->bind({0, 9100}).isOk());
                CHECK(r->joinGroup(ip("10.1.1.1"), 0) == Error::Invalid);
                CHECK(r->joinGroup(group, ip("10.9.9.9")) == Error::AddressNotAvailable);
                REQUIRE(r->joinGroup(group, ip("10.0.0.2")).isOk());
                CHECK(r->joinGroup(group, ip("10.0.0.2")) == Error::Exists);

                Sock s = make(Socket::Type::Datagram);
                REQUIRE(s->setMulticastInterface(ip("10.0.0.1")).isOk());
                CHECK(s->multicastInterface() == ip("10.0.0.1"));
                CHECK(s->option(Option::MulticastTtl).value() == 1);
                const SocketAddress to = {group, 9100};
                CHECK(send(*s, "group", &to).value() == 5);
                char buf[32];
                auto [got, err] = receive(*r, buf, sizeof(buf));
                REQUIRE(err.isOk());
                CHECK(std::string(buf, got.bytes) == "group");
                CHECK(got.from.ipv4 == ip("10.0.0.1"));

                // Looped back to the sender's own interface, unless asked not to.
                const uint32_t loopGroup = ip("239.9.9.9");
                Sock           l = make(Socket::Type::Datagram);
                REQUIRE(l->bind({0, 9200}).isOk());
                REQUIRE(l->joinGroup(loopGroup, ip("10.0.0.1")).isOk());
                const SocketAddress loopTo = {loopGroup, 9200};
                CHECK(send(*s, "looped", &loopTo).value() == 6);
                CHECK(text(*l) == "looped");
                REQUIRE(s->setOption(Option::MulticastLoop, 0).isOk());
                CHECK(send(*s, "not looped", &loopTo).value() == 10);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                CHECK(text(*l, Socket::DontWait) == "WouldBlock");

                REQUIRE(r->leaveGroup(group, ip("10.0.0.2")).isOk());
                CHECK(r->leaveGroup(group, ip("10.0.0.2")) == Error::NotExist);
                CHECK(send(*s, "left", &to).value() == 4);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                CHECK(text(*r, Socket::DontWait) == "WouldBlock");
                CHECK(Socket::create(Socket::Type::Stream, 0).value()->stream().socket()->joinGroup(group, 0) ==
                      Error::NotSupported);
        }
        pump();
        CHECK(heapUsed() == used);
}

TEST_CASE("Socket: raw ICMP echo, with the IP header") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      r = make(Socket::Type::Raw, 1);
                onA(r);
                uint8_t echo[8 + 16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1};
                for (size_t i = 8; i < sizeof(echo); ++i) echo[i] = static_cast<uint8_t>(i);
                const u16_t sum = inet_chksum(echo, sizeof(echo));
                std::memcpy(echo + 2, &sum, 2);
                const SocketAddress to = {ip("10.0.0.2"), 0};
                CHECK(send(*r, echo, sizeof(echo), &to).value() == sizeof(echo));
                // Every ICMP packet the stack receives: the request at b, then the reply at a.
                uint8_t          buf[128];
                Socket::Received got;
                size_t           ihl = 0;
                for (int i = 0; i < 2; ++i) {
                        auto [g, err] = receive(*r, buf, sizeof(buf));
                        REQUIRE(err.isOk());
                        REQUIRE(g.bytes >= 20 + sizeof(echo));
                        got = g;
                        ihl = (buf[0] & 0x0f) * 4u;
                        if (buf[ihl] == 0) break;
                        CHECK(buf[ihl] == 8);
                        CHECK(g.from.ipv4 == ip("10.0.0.1"));
                }
                CHECK((buf[0] >> 4) == 4);
                CHECK(buf[ihl] == 0); // an echo reply
                CHECK(buf[ihl + 4] == 0x12);
                CHECK(buf[ihl + 5] == 0x34);
                CHECK(got.from.ipv4 == ip("10.0.0.2"));
        }
        pump();
        CHECK(heapUsed() == used);
}

TEST_CASE("Socket: real-time options, receive stamps, the error queue") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      u = make(Socket::Type::Datagram);
                // Priority: its own, and IP_TOS's as Linux gives it (EF 4, low delay 6).
                CHECK(u->setOption(Option::Priority, -1) == Error::Invalid);
                REQUIRE(u->setOption(Option::Priority, 7).isOk());
                CHECK(u->option(Option::Priority).value() == 7);
                REQUIRE(u->setOption(Option::Tos, 0xb8).isOk());
                CHECK(u->option(Option::Priority).value() == 4);
                REQUIRE(u->setOption(Option::Tos, 0x10).isOk());
                CHECK(u->option(Option::Priority).value() == 6);

                // Timestamping: Linux's flags only; a stream socket has none.
                CHECK(u->setOption(Option::Timestamping, 1 << 30) == Error::Invalid);
                Sock t = make(Socket::Type::Stream);
                CHECK(t->setOption(Option::Timestamping, Socket::TimestampRxSoftware) == Error::NotSupported);
                const int32_t flags = Socket::TimestampRxSoftware | Socket::TimestampSoftware |
                                      Socket::TimestampTxSoftware | Socket::TimestampOptTsOnly;
                REQUIRE(u->setOption(Option::Timestamping, flags).isOk());
                CHECK(u->option(Option::Timestamping).value() == flags);
                REQUIRE(u->setOption(Option::TimestampNs, 1).isOk());
                REQUIRE(u->setOption(Option::TimestampsNew, 1).isOk());
                const Socket::StampOptions o = u->stampOptions();
                CHECK((o.timestamping == uint32_t(flags) && o.ns && !o.us && o.wide));

                // A datagram's arrival on the system clock (no Ethernet here: no hardware).
                REQUIRE(u->bind({ip("10.0.0.2"), 9200}).isOk());
                Sock s = make(Socket::Type::Datagram);
                onA(s);
                const SocketAddress to = {ip("10.0.0.2"), 9200};
                const int64_t       before = bmds::Clock::now().ns;
                CHECK(send(*s, "stamped", &to).value() == 7);
                char buf[64];
                auto [r, err] = receive(*u, buf, sizeof(buf));
                REQUIRE(err.isOk());
                CHECK(r.hardwareNs == 0);
                CHECK(r.softwareNs >= before);
                CHECK(r.softwareNs <= bmds::Clock::now().ns);

                // The error queue: nothing waits, nothing is said; no fast path without a driver.
                bmds::WritableByteSpan piece(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
                CHECK(u->receiveTxReport(bmds::Span<const bmds::WritableByteSpan>(&piece, 1)).error() ==
                      Error::WouldBlock);
                CHECK((u->readiness() & (bmds::Stream::ErrorQueue | bmds::Stream::Urgent)) == 0);
                CHECK(u->setFastPath({true, 1000, 4}) == Error::NotSupported);
                CHECK(t->setFastPath({true, 1000, 4}) == Error::NotSupported);
                CHECK(u->setFastPath({true, 1000, 9}) == Error::Invalid);
                CHECK(!u->fastPath().on);
                REQUIRE(u->setOption(Option::Timestamping, 0).isOk());
        }
        pump();
        CHECK(heapUsed() == used);
}

TEST_CASE("Socket: options, inherited; a listener closed with connections waiting") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread net;
                Sock      l = make(Socket::Type::Stream);
                CHECK(l->setOption(Option::KeepIdle, 0) == Error::Invalid);
                CHECK(l->setOption(Option::MulticastTtl, 4) == Error::NotSupported);
                CHECK(l->setOption(Option::Ttl, 0) == Error::Invalid);
                REQUIRE(l->setOption(Option::NoDelay, 1).isOk());
                REQUIRE(l->setOption(Option::KeepAlive, 1).isOk());
                REQUIRE(l->setOption(Option::KeepIdle, 30).isOk());
                REQUIRE(l->setOption(Option::Ttl, 9).isOk());
                CHECK(l->option(Option::ReceiveBuffer).value() == TCP_WND);
                CHECK(l->option(Option::SendBuffer).value() == TCP_SND_BUF);
                CHECK(l->bindToDevice("nope") == Error::NotExist);
                CHECK(l->bindToDevice("").isOk());
                REQUIRE(l->bind({0, 7003}).isOk());
                REQUIRE(l->listen(4).isOk());
                CHECK(l->connect({ip("10.0.0.1"), 7003}, 0) == Error::Invalid);

                Sock c = make(Socket::Type::Stream);
                REQUIRE(c->connect({ip("10.0.0.1"), 7003}, 0).isOk());
                Sock s = adopt(l->accept(nullptr, 0).value());
                CHECK(s->option(Option::NoDelay).value() == 1);
                CHECK(s->option(Option::KeepAlive).value() == 1);
                CHECK(s->option(Option::KeepIdle).value() == 30);
                CHECK(s->option(Option::Ttl).value() == 9);
                CHECK(s->option(Option::Listening).value() == 0);

                // Two more connect, nobody accepts them; the listener goes.
                Sock w1 = make(Socket::Type::Stream);
                Sock w2 = make(Socket::Type::Stream);
                REQUIRE(w1->connect({ip("10.0.0.1"), 7003}, 0).isOk());
                REQUIRE(w2->connect({ip("10.0.0.1"), 7003}, 0).isOk());
                CHECK(send(*w2, "never read").value() == 10);
                CHECK(eventually([&] { return (l->readiness() & bmds::Stream::Readable) != 0; }));
                l.close();
                CHECK(text(*w1) == "EndOfFile");
                CHECK(text(*w2) == "ConnectionReset"); // its data unread
        }
        finish(used);
}
