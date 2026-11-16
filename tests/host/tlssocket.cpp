/**
 * @file      tlssocket.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS sockets (devplan/network.md §35) over lwIP's two wired interfaces, with the toy session of
 * faketls.cpp and a server speaking it by hand on a plain socket: the handshake on the tls thread,
 * blocking and not; readiness that half a record doesn't wake; refusals, NoVerify, the handshake's
 * time, a failed record; close_notify both ways; the session limit; a close during a handshake.
 */

#include <doctest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <bmds/socket.h>
#include <bmds/tlsservice.h>

#include "faketls.h"
#include "lwipwire.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::Socket;
using bmds::SocketAddress;
using bmds::Span;
using bmds::Stream;
using bmds::String;
using bmds::TlsSession;
using bmds::WritableByteSpan;
namespace TlsService = bmds::TlsService;

namespace {

        using namespace LwipWire;

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

        uint32_t ip(const char *text) {
                return addr(text).addr;
        }

        void ensureService() {
                static const bool started = TlsService::init().isOk();
                REQUIRE(started);
                bmds::FakeTls::settings = TlsService::Settings{};
                return;
        }

        // A connected pair: the client (through interface a) and the server's side.
        struct Pair {
                        Sock listener;
                        Sock client;
                        Sock server;
        };

        Pair connectPair(uint16_t port) {
                Pair p;
                auto [l, lerr] = Socket::create(Socket::Type::Stream, 0);
                REQUIRE(lerr.isOk());
                p.listener = adopt(std::move(l));
                REQUIRE(p.listener->bind({ip("10.0.0.2"), port}).isOk());
                REQUIRE(p.listener->listen(4).isOk());
                auto [c, cerr] = Socket::create(Socket::Type::Stream, 0);
                REQUIRE(cerr.isOk());
                p.client = adopt(std::move(c));
                const std::string name = std::string{a.netif.name[0], a.netif.name[1]} + std::to_string(a.netif.num);
                REQUIRE(p.client->bindToDevice(name.c_str()).isOk());
                REQUIRE(p.client->connect({ip("10.0.0.2"), port}, 0).isOk());
                auto [h, aerr] = p.listener->accept(nullptr, 0);
                REQUIRE(aerr.isOk());
                p.server = adopt(std::move(h));
                return p;
        }

        void put(Socket &s, const void *data, size_t n) {
                const ByteSpan piece(static_cast<const uint8_t *>(data), n);
                REQUIRE(s.send(Span<const ByteSpan>(&piece, 1), nullptr, 0).value() == n);
                return;
        }

        void put(Socket &s, const std::string &text) {
                put(s, text.data(), text.size());
                return;
        }

        // A record of the toy protocol: its length, then the bytes.
        std::string record(const std::string &text) {
                return std::string(1, char(text.size())) + text;
        }

        // Exactly @p n bytes from a plain socket.
        std::string get(Socket &s, size_t n) {
                std::string            out(n, '\0');
                const WritableByteSpan piece(reinterpret_cast<uint8_t *>(out.data()), n);
                auto [r, err] = s.receive(Span<const WritableByteSpan>(&piece, 1), Socket::WaitAll);
                REQUIRE(err.isOk());
                out.resize(r.bytes);
                return out;
        }

        bmds::Result<Socket::Received> receive(Socket &s, void *buf, size_t n, uint32_t flags = 0) {
                const WritableByteSpan piece(static_cast<uint8_t *>(buf), n);
                return s.receive(Span<const WritableByteSpan>(&piece, 1), flags);
        }

        bmds::Result<size_t> send(Socket &s, const std::string &text, uint32_t flags = 0) {
                const ByteSpan piece(reinterpret_cast<const uint8_t *>(text.data()), text.size());
                return s.send(Span<const ByteSpan>(&piece, 1), nullptr, flags);
        }

        std::string text(Socket &s, uint32_t flags = 0) {
                char buf[256];
                auto [r, err] = receive(s, buf, sizeof(buf), flags);
                if (err.isError()) return err.name();
                return std::string(buf, r.bytes);
        }

        Error start(Socket &s, uint32_t flags = 0, const char *alpn = "") {
                TlsSession::Config config;
                config.serverName = "test.example";
                config.flags = flags;
                config.alpn = alpn;
                String why;
                return s.startTls(config, why);
        }

        template <typename Fn> bool eventually(Fn &&ready) {
                for (int i = 0; i < 2000; ++i) {
                        if (ready()) return true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
        }

} // namespace

TEST_CASE("TLS socket: a blocking handshake, records both ways, the session's record") {
        setUp();
        ensureService();
        NetThread net;
        Pair      p = connectPair(7101);
        REQUIRE(start(*p.client, 0, "irc,http/1.1").isOk());
        CHECK(p.client->isTls());
        CHECK(start(*p.client) == Error::AlreadyInProgress);
        CHECK(get(*p.server, 2) == "HI");
        put(*p.server, "OK");
        CHECK(p.client->waitTls(0).isOk());

        TlsSession::Info info;
        REQUIRE(p.client->tlsInfo(info).isOk());
        CHECK(info.state == TlsSession::State::Open);
        CHECK(info.version == 0x0303);
        CHECK(std::string(info.cipherName) == "FAKE");
        CHECK(std::string(info.alpn) == "irc");
        CHECK(std::string(info.peer.subject) == "CN=test.example");
        CHECK(TlsService::counts().sessions == 1);
        CHECK(TlsService::counts().handshaking == 0);

        // A write is one record (the stand-in's are at most 255 bytes); a read gets its plaintext.
        CHECK(send(*p.client, "hello").value() == 5);
        CHECK(get(*p.server, 6) == record("hello"));
        put(*p.server, record("welcome"));
        CHECK(text(*p.client) == "welcome");
        const std::string big(600, 'x');
        CHECK(send(*p.client, big).value() == 600);
        CHECK(get(*p.server, 256) == record(std::string(255, 'x')));
        CHECK(get(*p.server, 256) == record(std::string(255, 'x')));
        CHECK(get(*p.server, 91) == record(std::string(90, 'x')));

        // Peeking isn't offered.
        char c;
        CHECK(receive(*p.client, &c, 1, Socket::Peek).error() == Error::NotSupported);

        // The client's close_notify, then its FIN; the server's close_notify ends the client's reads.
        REQUIRE(p.client->shutdown(false, true).isOk());
        CHECK(get(*p.server, 1) == std::string(1, '\0'));
        put(*p.server, std::string(1, '\0'));
        CHECK(text(*p.client) == "EndOfFile");
        REQUIRE(p.client->tlsInfo(info).isOk());
        CHECK(info.cleanClose);
        p.client.close();
        CHECK(eventually([] { return TlsService::counts().sessions == 0; }));
}

TEST_CASE("TLS socket: readiness waits for a whole record; not waiting") {
        setUp();
        ensureService();
        NetThread net;
        Pair      p = connectPair(7102);
        REQUIRE(start(*p.client).isOk());
        // Handshaking: nothing to do yet, and not waiting says so.
        CHECK(p.client->readiness() == 0);
        CHECK(text(*p.client, Socket::DontWait) == "WouldBlock");
        CHECK(send(*p.client, "early", Socket::DontWait).error() == Error::WouldBlock);
        CHECK(p.client->waitTls(Socket::DontWait) == Error::WouldBlock);
        CHECK(get(*p.server, 2) == "HI");
        put(*p.server, "OK");
        CHECK(eventually([&] { return (p.client->readiness() & Stream::Writable) != 0; }));
        CHECK(p.client->waitTls(Socket::DontWait).isOk());
        CHECK((p.client->readiness() & Stream::Readable) == 0);

        // Half a record: not readable (a read would only wait), and reading says so.
        const std::string r = record("0123456789");
        put(*p.server, r.substr(0, 6));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(text(*p.client, Socket::DontWait) == "WouldBlock"); // the header taken: 10 wanted
        put(*p.server, r.substr(6, 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK((p.client->readiness() & Stream::Readable) == 0);
        put(*p.server, r.substr(9));
        CHECK(eventually([&] { return (p.client->readiness() & Stream::Readable) != 0; }));
        // Its plaintext in two reads: the rest waits in the session, and stays readable.
        char buf[4];
        CHECK(receive(*p.client, buf, 4, Socket::DontWait).value().bytes == 4);
        CHECK((p.client->readiness() & Stream::Readable) != 0);
        CHECK(text(*p.client, Socket::DontWait) == "456789");
        CHECK((p.client->readiness() & Stream::Readable) == 0);
}

TEST_CASE("TLS socket: refusals, NoVerify, a bad record, the handshake's time") {
        setUp();
        ensureService();
        NetThread net;
        SUBCASE("refused") {
                Pair p = connectPair(7103);
                REQUIRE(start(*p.client).isOk());
                CHECK(get(*p.server, 2) == "HI");
                put(*p.server, "NO");
                CHECK(p.client->waitTls(0) == Error::KeyRejected);
                CHECK(std::string(p.client->tlsWhy().cStr()).find("authority") != std::string::npos);
                CHECK(p.client->takeError() == Error::KeyRejected); // SO_ERROR, once
                CHECK(p.client->takeError().isOk());
                CHECK((p.client->readiness() & Stream::Broken) != 0);
                CHECK(text(*p.client) == "KeyRejected");
                CHECK(send(*p.client, "x").error() == Error::KeyRejected);
                TlsSession::Info info;
                REQUIRE(p.client->tlsInfo(info).isOk());
                CHECK(info.verify == TlsSession::VerifyUntrusted);
        }
        SUBCASE("NoVerify lets it pass and says what it let pass") {
                Pair p = connectPair(7104);
                REQUIRE(start(*p.client, TlsSession::NoVerify).isOk());
                CHECK(get(*p.server, 2) == "HI");
                put(*p.server, "NO");
                CHECK(p.client->waitTls(0).isOk());
                TlsSession::Info info;
                REQUIRE(p.client->tlsInfo(info).isOk());
                CHECK(info.verify == TlsSession::VerifyUntrusted);
        }
        SUBCASE("a record that fails its check ends the session") {
                Pair p = connectPair(7105);
                REQUIRE(start(*p.client).isOk());
                CHECK(get(*p.server, 2) == "HI");
                put(*p.server, "OK");
                REQUIRE(p.client->waitTls(0).isOk());
                put(*p.server, record("!BAD record"));
                CHECK(text(*p.client) == "BadMessage");
                CHECK(text(*p.client) == "BadMessage");
        }
        SUBCASE("a server that never answers") {
                bmds::FakeTls::settings.handshakeTimeoutMs = 200;
                Pair p = connectPair(7106);
                REQUIRE(start(*p.client).isOk());
                const auto t0 = std::chrono::steady_clock::now();
                CHECK(p.client->waitTls(0) == Error::Timeout);
                const auto ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
                CHECK(ms.count() >= 150);
                CHECK(ms.count() < 1500);
                CHECK(std::string(p.client->tlsWhy().cStr()).find("too long") != std::string::npos);
        }
        CHECK(eventually([] { return TlsService::counts().sessions == 0; }));
}

TEST_CASE("TLS socket: the session limit; closed while handshaking; not a TCP connection") {
        setUp();
        ensureService();
        NetThread net;
        bmds::FakeTls::settings.maxSessions = 1;
        Pair p = connectPair(7107);
        Pair q = connectPair(7108);
        REQUIRE(start(*p.client).isOk());
        CHECK(start(*q.client) == Error::Full);
        CHECK(!q.client->isTls());
        // Closed in its handshake: the tls thread lets go of it, and the count with it.
        p.client.close();
        CHECK(eventually([] { return TlsService::counts().sessions == 0; }));
        REQUIRE(start(*q.client).isOk());
        q.client.close();
        CHECK(eventually([] { return TlsService::counts().sessions == 0; }));

        auto [u, uerr] = Socket::create(Socket::Type::Datagram, 0);
        REQUIRE(uerr.isOk());
        CHECK(start(*u->stream().socket()) == Error::NotSupported);
        auto [t, terr] = Socket::create(Socket::Type::Stream, 0);
        REQUIRE(terr.isOk());
        CHECK(start(*t->stream().socket()) == Error::NotConnected);
        CHECK(p.listener->isTls() == false);
        CHECK(start(*p.listener) == Error::NotConnected);
}
