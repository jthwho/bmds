/**
 * @file      httpclient.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Fetching a file over HTTP(S) (devplan/packages.md §23): a request a connection over `Socket`,
 * redirects, a `Range` to resume. TLS comes from the environment (httptls.cpp on the target).
 */

#include <cstdarg>
#include <bmds/httpclient.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A receive's buffer: TLS records are up to 16 KB.
        constexpr size_t BufferBytes = 16 * 1024;

        Error fail(String &why, Error err, const char *fmt, ...) BMDS_PRINTF_FORMAT(3, 4);
        Error fail(String &why, Error err, const char *fmt, ...) {
                why.clear();
                va_list args;
                va_start(args, fmt);
                (void)why.vappendf(fmt, args);
                va_end(args);
                return err;
        }

        // One connection: the socket, and TLS over it when the URL asks.
        class Connection {
                public:
                        Connection() = default;
                        ~Connection() { close(); }
                        Connection(const Connection &) = delete;
                        Connection &operator=(const Connection &) = delete;

                        Error open(const Http::Url &url, const Http::Environment &env, String &why) {
                                const String host = hostOf(url);
                                if (url.tls && env.openTls == nullptr)
                                        return fail(why, Error::NotSupported, "%s: no HTTPS in this build",
                                                    host.cStr());
                                uint32_t ipv4 = 0;
                                Error    err = env.resolve != nullptr ? env.resolve(url.host, ipv4, env.timeoutMs)
                                                                      : Error::NotSupported;
                                if (err.isError()) {
                                        return fail(why, err, "%s: %s", host.cStr(),
                                                    err == Error::NotExist ? "no such host"
                                                    : err == Error::NotReady
                                                            ? "the network isn't up (no DNS server yet)"
                                                    : err == Error::Timeout ? "no answer from the DNS server"
                                                                            : err.desc());
                                }
                                auto [handle, cerr] = Socket::create(Socket::Type::Stream, 0);
                                if (cerr.isError()) return fail(why, cerr, "a socket: %s", cerr.desc());
                                _handle = std::move(handle);
                                _socket = _handle->stream().socket();
                                (void)_socket->setOption(Socket::Option::SendTimeoutMs, int32_t(env.timeoutMs));
                                (void)_socket->setOption(Socket::Option::ReceiveTimeoutMs, int32_t(env.timeoutMs));
                                (void)_socket->setOption(Socket::Option::NoDelay, 1);
                                if (env.prepare != nullptr) env.prepare(*_socket);
                                err = _socket->connect(SocketAddress{ipv4, url.port}, 0);
                                if (err == Error::WouldBlock) err = Error::Timeout;
                                if (err.isError()) {
                                        return fail(why, err, "%s port %u: %s", host.cStr(), unsigned(url.port),
                                                    err == Error::ConnectionRefused ? "refused" : err.desc());
                                }
                                if (url.tls) {
                                        err = env.openTls(*_socket, url.host, why, _tls);
                                        if (err.isError()) return err;
                                        _secure = true;
                                }
                                return Error::Ok;
                        }

                        Error send(ByteSpan data) {
                                while (!data.isEmpty()) {
                                        auto [n, err] =
                                                _secure ? _tls.send(_tls.state, data)
                                                        : _socket->send(Span<const ByteSpan>(&data, 1), nullptr, 0);
                                        if (err.isError()) return err == Error::WouldBlock ? Error::Timeout : err;
                                        if (n == 0) return Error::BrokenPipe;
                                        data = data.mid(n);
                                }
                                return Error::Ok;
                        }

                        // Bytes received, 0 at the end.
                        Result<size_t> receive(WritableByteSpan into) {
                                if (_secure) return _tls.receive(_tls.state, into);
                                auto [r, err] = _socket->receive(Span<const WritableByteSpan>(&into, 1), 0);
                                if (err == Error::EndOfFile) return Result<size_t>(size_t(0), Error::Ok);
                                if (err == Error::WouldBlock) return Result<size_t>(size_t(0), Error::Timeout);
                                return Result<size_t>(r.bytes, err);
                        }

                        void close() {
                                if (_secure && _tls.close != nullptr) _tls.close(_tls.state);
                                _secure = false;
                                _tls = Http::Tls{};
                                _socket = nullptr;
                                _handle = FileHandle::Ptr();
                                return;
                        }

                        static String hostOf(const Http::Url &url) {
                                String host;
                                (void)host.append(url.host);
                                return host;
                        }

                private:
                        FileHandle::Ptr _handle;
                        Socket         *_socket = nullptr;
                        Http::Tls       _tls;
                        bool            _secure = false;
        };

        class Buffer {
                public:
                        Buffer() : _p(static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, BufferBytes))) {}
                        ~Buffer() { Memory::free(_p); }
                        Buffer(const Buffer &) = delete;
                        Buffer  &operator=(const Buffer &) = delete;
                        uint8_t *data() const { return _p; }

                private:
                        uint8_t *_p;
        };

        bool isRedirect(int status) {
                return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        }

} // namespace

Error Http::get(StringView url, uint64_t from, const Environment &env, const Sink &sink, Fetched &out, String &why) {
        out = Fetched{};
        why.clear();
        Error err = out.url.append(url);
        if (err.isError()) return fail(why, err, "out of memory");
        Buffer buf;
        if (buf.data() == nullptr) return fail(why, Error::NoMem, "out of memory");
        for (unsigned hop = 0; hop <= MaxRedirects; ++hop) {
                Url u;
                if (parseUrl(out.url.view(), u).isError())
                        return fail(why, Error::Invalid, "%s: not an http: or https: URL", out.url.cStr());
                Connection c;
                err = c.open(u, env, why);
                if (err.isError()) return err;
                String request;
                err = buildRequest(u, from, env.userAgent, request);
                if (err.isOk())
                        err = c.send(ByteSpan(reinterpret_cast<const uint8_t *>(request.data()), request.size()));
                if (err.isError()) return fail(why, err, "%s: sending the request: %s", out.url.cStr(), err.desc());

                ResponseParser parser;
                bool           started = false, redirected = false;
                while (!parser.isDone() && !redirected) {
                        auto [n, rerr] = c.receive(WritableByteSpan(buf.data(), BufferBytes));
                        if (rerr.isError()) return fail(why, rerr, "%s: %s", out.url.cStr(), rerr.desc());
                        if (n == 0) {
                                if (parser.end().isError())
                                        return fail(why, Error::CorruptData, "%s: the connection ended early",
                                                    out.url.cStr());
                                break;
                        }
                        ByteSpan in(buf.data(), n);
                        while (!in.isEmpty() && !parser.isDone()) {
                                size_t   used = 0;
                                ByteSpan body;
                                err = parser.feed(in, used, body);
                                if (err.isError()) {
                                        return fail(why, err, "%s: %s", out.url.cStr(),
                                                    err == Error::NotSupported ? "a transfer coding this can't read"
                                                                               : "not an HTTP response");
                                }
                                in = in.mid(used);
                                if (!parser.hasHead()) continue;
                                const Head &h = parser.head();
                                if (!started) {
                                        started = true;
                                        out.status = h.status;
                                        if (isRedirect(h.status)) {
                                                if (h.location.isEmpty())
                                                        return fail(why, Error::IoError,
                                                                    "%s: a redirect without a Location",
                                                                    out.url.cStr());
                                                auto [next, lerr] = resolve(out.url.view(), h.location.view());
                                                if (lerr.isError())
                                                        return fail(why, Error::Invalid, "%s: redirected to %s",
                                                                    out.url.cStr(), h.location.cStr());
                                                out.url = std::move(next);
                                                redirected = true;
                                                break;
                                        }
                                        if (h.status == 416 && from != 0 && h.rangeTotal == from) {
                                                // Asked for what follows the end: it is all here already.
                                                out.start = from;
                                                out.total = from;
                                                err = sink.begin != nullptr ? sink.begin(sink.arg, from, from)
                                                                            : Error::Ok;
                                                return err.isError() ? fail(why, err, "%s", err.desc()) : Error::Ok;
                                        }
                                        if (h.status == 200) {
                                                out.start = 0;
                                                out.total = h.chunked ? Unknown : h.length;
                                        } else if (h.status == 206) {
                                                out.start = h.rangeStart;
                                                out.total = h.rangeTotal;
                                                if (out.start != from)
                                                        return fail(why, Error::CorruptData,
                                                                    "%s: asked from byte %llu, sent from %llu",
                                                                    out.url.cStr(),
                                                                    static_cast<unsigned long long>(from),
                                                                    static_cast<unsigned long long>(out.start));
                                        } else {
                                                const Error e = h.status == 404 || h.status == 410 ? Error::NotExist
                                                                                                   : Error::IoError;
                                                return fail(why, e, "%s: the server answered %d%s", out.url.cStr(),
                                                            h.status, h.status == 404 ? " (not found)" : "");
                                        }
                                        if (sink.begin != nullptr) {
                                                err = sink.begin(sink.arg, out.start, out.total);
                                                if (err.isError()) return fail(why, err, "%s", err.desc());
                                        }
                                }
                                if (!body.isEmpty() && sink.body != nullptr) {
                                        err = sink.body(sink.arg, body);
                                        if (err.isError()) return fail(why, err, "%s", err.desc());
                                }
                        }
                }
                if (!redirected) return Error::Ok;
        }
        return fail(why, Error::IoError, "%.*s: more than %u redirects", int(url.size()), url.data(), MaxRedirects);
}

BMDS_NAMESPACE_END
