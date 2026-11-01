/**
 * @file      httpclient.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Http::get() (devplan/packages.md §23) over lwipwire's two interfaces against a server thread
 * that answers from a script: a body, redirects, ranges kept and ignored, a file fetched whole
 * already, the statuses refused, a body cut short, no TLS, a connection refused.
 */

#include <doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <bmds/httpclient.h>

#include "lwipwire.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::Socket;
using bmds::SocketAddress;
using bmds::Span;
using bmds::StringView;
using bmds::WritableByteSpan;
namespace Http = bmds::Http;

namespace {

        using namespace LwipWire;

        // A server on 10.0.0.2:8080: each connection's request recorded, answered from the script.
        class Server {
                public:
                        explicit Server(std::vector<std::string> script) : _script(std::move(script)) {
                                auto [h, err] = Socket::create(Socket::Type::Stream, 0);
                                REQUIRE(err.isOk());
                                _handle = std::move(h);
                                _socket = _handle->stream().socket();
                                REQUIRE(_socket->setOption(Socket::Option::ReuseAddress, 1).isOk());
                                REQUIRE(_socket->bind({addr("10.0.0.2").addr, 8080}).isOk());
                                REQUIRE(_socket->listen(4).isOk());
                                _thread = std::thread([this] { serve(); });
                        }

                        ~Server() {
                                _thread.join();
                                _handle.reset();
                        }

                        std::vector<std::string> requests;

                private:
                        void serve() {
                                for (const std::string &answer : _script) {
                                        auto [h, err] = _socket->accept(nullptr, 0);
                                        if (err.isError()) return;
                                        Socket     *c = h->stream().socket();
                                        std::string request;
                                        char        buf[1024];
                                        while (request.find("\r\n\r\n") == std::string::npos) {
                                                WritableByteSpan piece(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
                                                auto [r, rerr] = c->receive(Span<const WritableByteSpan>(&piece, 1), 0);
                                                if (rerr.isError() || r.bytes == 0) break;
                                                request.append(buf, r.bytes);
                                        }
                                        requests.push_back(request);
                                        ByteSpan out(reinterpret_cast<const uint8_t *>(answer.data()), answer.size());
                                        while (!out.isEmpty()) {
                                                auto [n, serr] = c->send(Span<const ByteSpan>(&out, 1), nullptr, 0);
                                                if (serr.isError()) break;
                                                out = out.mid(n);
                                        }
                                        (void)c->shutdown(false, true);
                                        // The client closes first: what it hasn't read isn't reset away.
                                        WritableByteSpan rest(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
                                        while (c->receive(Span<const WritableByteSpan>(&rest, 1), 0).value().bytes >
                                               0) {}
                                }
                        }

                        std::vector<std::string> _script;
                        bmds::FileHandle::Ptr    _handle;
                        Socket                  *_socket = nullptr;
                        std::thread              _thread;
        };

        Error resolve(StringView host, uint32_t &ipv4, unsigned int) {
                if (host != StringView("10.0.0.2")) return Error::NotExist;
                ipv4 = addr("10.0.0.2").addr;
                return Error::Ok;
        }

        // Out through interface a, so the packets cross the wire.
        void prepare(Socket &s) {
                const std::string name = std::string{a.netif.name[0], a.netif.name[1]} + std::to_string(a.netif.num);
                (void)s.bindToDevice(name.c_str());
                return;
        }

        Http::Environment env() {
                Http::Environment e;
                e.resolve = resolve;
                e.prepare = prepare;
                e.userAgent = "BMDS-test";
                e.timeoutMs = 5000;
                return e;
        }

        struct Got {
                        std::string body;
                        uint64_t    start = 99;
                        uint64_t    total = 99;
                        int         begins = 0;
        };

        Http::Sink sinkOf(Got &got) {
                Http::Sink s;
                s.begin = [](void *arg, uint64_t start, uint64_t total) {
                        Got &g = *static_cast<Got *>(arg);
                        g.start = start;
                        g.total = total;
                        ++g.begins;
                        return Error(Error::Ok);
                };
                s.body = [](void *arg, ByteSpan bytes) {
                        static_cast<Got *>(arg)->body.append(reinterpret_cast<const char *>(bytes.data()),
                                                             bytes.size());
                        return Error(Error::Ok);
                };
                s.arg = &got;
                return s;
        }

        void finish(size_t used) {
                for (int i = 0; i < 1300; ++i) idle(100);
                CHECK(heapUsed() == used);
                return;
        }

} // namespace

TEST_CASE("Http::get: a body, redirects, ranges") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread   net;
                std::string big(100000, 'x');
                for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + (i * 7) % 26);
                Server server({"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big,
                               "HTTP/1.1 302 Found\r\nLocation: /real/file.bmpk\r\nContent-Length: 0\r\n\r\n",
                               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nreal\r\n0\r\n\r\n",
                               "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-10/11\r\nContent-Length: "
                               "6\r\n\r\n world",
                               "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world",
                               "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */11\r\nContent-Length: "
                               "0\r\n\r\n"});
                Http::Fetched f;
                bmds::String  why;
                Got           g1;
                REQUIRE(Http::get("http://10.0.0.2:8080/packages/a.bmpk", 0, env(), sinkOf(g1), f, why).isOk());
                CHECK(g1.body == big);
                CHECK(g1.start == 0);
                CHECK(g1.total == big.size());
                CHECK(f.status == 200);

                // A redirect, relative, followed; the URL after it said.
                Got g2;
                REQUIRE(Http::get("http://10.0.0.2:8080/packages/b.bmpk", 0, env(), sinkOf(g2), f, why).isOk());
                CHECK(g2.body == "real");
                CHECK(std::string(f.url.cStr()) == "http://10.0.0.2:8080/real/file.bmpk");
                CHECK(g2.total == Http::Unknown);

                // Resumed from byte 5; then a server that ignores the range starts again from 0.
                Got g3;
                REQUIRE(Http::get("http://10.0.0.2:8080/c", 5, env(), sinkOf(g3), f, why).isOk());
                CHECK(g3.body == " world");
                CHECK(g3.start == 5);
                CHECK(g3.total == 11);
                Got g4;
                REQUIRE(Http::get("http://10.0.0.2:8080/c", 5, env(), sinkOf(g4), f, why).isOk());
                CHECK(g4.body == "hello world");
                CHECK(g4.start == 0);

                // Asked from its end: whole already.
                Got g5;
                REQUIRE(Http::get("http://10.0.0.2:8080/c", 11, env(), sinkOf(g5), f, why).isOk());
                CHECK(g5.body.empty());
                CHECK(g5.begins == 1);
                CHECK(g5.start == 11);

                REQUIRE(server.requests.size() == 6);
                CHECK(server.requests[0] ==
                      "GET /packages/a.bmpk HTTP/1.1\r\nHost: 10.0.0.2:8080\r\nUser-Agent: BMDS-test\r\n"
                      "Accept: */*\r\nConnection: close\r\n\r\n");
                CHECK(server.requests[2].rfind("GET /real/file.bmpk HTTP/1.1\r\n", 0) == 0);
                CHECK(server.requests[3].find("Range: bytes=5-\r\n") != std::string::npos);
        }
        finish(used);
}

TEST_CASE("Http::get: what is refused") {
        setUp();
        const size_t used = heapUsed();
        {
                NetThread                net;
                std::vector<std::string> script = {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
                                                   "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nonly this",
                                                   "SSH-2.0-dropbear\r\n"};
                for (unsigned i = 0; i <= Http::MaxRedirects; ++i)
                        script.push_back("HTTP/1.1 301 Moved\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n");
                Server        server(script);
                Http::Fetched f;
                bmds::String  why;
                Got           g;
                CHECK(Http::get("http://10.0.0.2:8080/missing", 0, env(), sinkOf(g), f, why) == Error::NotExist);
                CHECK(std::string(why.cStr()) == "http://10.0.0.2:8080/missing: the server answered 404 (not found)");
                CHECK(Http::get("http://10.0.0.2:8080/short", 0, env(), sinkOf(g), f, why) == Error::CorruptData);
                CHECK(std::string(why.cStr()) == "http://10.0.0.2:8080/short: the connection ended early");
                CHECK(Http::get("http://10.0.0.2:8080/ssh", 0, env(), sinkOf(g), f, why) == Error::CorruptData);
                CHECK(Http::get("http://10.0.0.2:8080/loop", 0, env(), sinkOf(g), f, why) == Error::IoError);
                CHECK(std::string(why.cStr()) == "http://10.0.0.2:8080/loop: more than 5 redirects");

                // Not reaching a server at all.
                CHECK(Http::get("https://10.0.0.2/x", 0, env(), sinkOf(g), f, why) == Error::NotSupported);
                CHECK(Http::get("http://nowhere.example/x", 0, env(), sinkOf(g), f, why) == Error::NotExist);
                CHECK(std::string(why.cStr()) == "nowhere.example: no such host");
                CHECK(Http::get("http://10.0.0.2:8081/x", 0, env(), sinkOf(g), f, why) == Error::ConnectionRefused);
                CHECK(Http::get("ftp://10.0.0.2/x", 0, env(), sinkOf(g), f, why) == Error::Invalid);
        }
        finish(used);
}
