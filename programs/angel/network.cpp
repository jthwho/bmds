/**
 * @file      network.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network (devplan/scripting.md step 6): `Socket` (a TCP connection, read and written as
 * a File is), `ServerSocket` (waits for connections), `UdpSocket` (datagrams, broadcast
 * allowed), `Http::get`, `post` and `request` (plain HTTP/1.1 over the same connections),
 * `localAddress()` and `lookup()`. IPv4. What fails raises an exception that says why, which
 * a catch block can handle. Waiting is poll(), so a script waiting costs nothing.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <new>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace angel {

        namespace {

                constexpr int ConnectTimeoutMs = 10000; // Socket(host, port)'s wait for the answer
                constexpr int HttpTimeoutMs = 30000;    // each of Http's waits
                constexpr int HttpRedirects = 5;        // followed before giving up
                constexpr int UdpLargest = 65536;       // a datagram's most (its buffer, made once)

                int64_t nowMs() {
                        timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
                }

                // Waits until @p fd is ready for @p events: 1, 0 when @p ms (< 0: no limit) passed,
                // -1 on an error (errno).
                int waitFd(int fd, short events, int ms) {
                        const int64_t end = nowMs() + ms;
                        for (;;) {
                                pollfd    p = {fd, events, 0};
                                const int r = poll(&p, 1, ms);
                                if (r >= 0 || errno != EINTR) return r;
                                if (ms > 0) {
                                        const int64_t left = end - nowMs();
                                        ms = left > 0 ? int(left) : 0;
                                }
                        }
                }

                // A host name or dotted address, and a port, as an IPv4 address; false (and an
                // exception) if there's none.
                bool resolve(const char *host, int port, sockaddr_in &out) {
                        if (port < 0 || port > 65535) {
                                raise("Port %d isn't 0 to 65535", port);
                                return false;
                        }
                        memset(&out, 0, sizeof(out));
                        out.sin_family = AF_INET;
                        out.sin_port = htons(uint16_t(port));
                        if (inet_pton(AF_INET, host, &out.sin_addr) == 1) return true;
                        addrinfo hints;
                        memset(&hints, 0, sizeof(hints));
                        hints.ai_family = AF_INET;
                        hints.ai_socktype = SOCK_STREAM;
                        addrinfo *list = nullptr;
                        const int r = getaddrinfo(host, nullptr, &hints, &list);
                        if (r != 0 || list == nullptr) {
                                raise("Can't find the host '%s': %s", host, r != 0 ? gai_strerror(r) : "no address");
                                return false;
                        }
                        out.sin_addr = reinterpret_cast<const sockaddr_in *>(list->ai_addr)->sin_addr;
                        freeaddrinfo(list);
                        return true;
                }

                // ---- What the three kinds of socket share: the fd, a timeout, a reference count.
                class Endpoint {
                        public:
                                explicit Endpoint(int fd) : _fd(fd) {}
                                virtual ~Endpoint() { close(); }
                                Endpoint(const Endpoint &) = delete;
                                Endpoint &operator=(const Endpoint &) = delete;

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                }
                                void close() {
                                        if (_fd >= 0) ::close(_fd);
                                        _fd = -1;
                                }
                                int  fd() const { return _fd; }
                                bool isOpen() const { return _fd >= 0; }
                                // How long a read or accept waits (ms); 0: as long as it takes.
                                int  timeoutMs() const { return _timeoutMs; }
                                void setTimeoutMs(int ms) { _timeoutMs = ms > 0 ? ms : 0; }

                                // The open fd, or an exception.
                                bool usable() const {
                                        if (_fd >= 0) return true;
                                        raise("The socket is closed");
                                        return false;
                                }
                                // Waits for @p events within the timeout: true, or false and an
                                // exception saying @p waitingFor timed out.
                                bool wait(short events, const char *waitingFor) const {
                                        const int r = waitFd(_fd, events, _timeoutMs != 0 ? _timeoutMs : -1);
                                        if (r > 0) return true;
                                        if (r == 0)
                                                raise("Timed out waiting for %s (%d ms)", waitingFor, _timeoutMs);
                                        else
                                                raise("Waiting for %s failed: %s", waitingFor, strerror(errno));
                                        return false;
                                }
                                // Something to read (or the end) within @p ms (0: only looks).
                                bool readable(int ms) const { return _fd >= 0 && waitFd(_fd, POLLIN, ms) > 0; }

                        private:
                                int _fd;
                                int _timeoutMs = 0;
                                int _refs = 1;
                };

                // ---- A TCP connection, its input buffered for readLine().
                class Socket : public Endpoint {
                        public:
                                explicit Socket(int fd) : Endpoint(fd) {
                                        sockaddr_in a;
                                        socklen_t   length = sizeof(a);
                                        if (getpeername(fd, reinterpret_cast<sockaddr *>(&a), &length) == 0) {
                                                inet_ntop(AF_INET, &a.sin_addr, _peer, sizeof(_peer));
                                                _peerPort = ntohs(a.sin_port);
                                        }
                                }

                                const char *peer() const { return _peer; }
                                int         peerPort() const { return _peerPort; }
                                bool        buffered() const { return _start < _end; }
                                bool        ended() const { return _ended; }

                                // Reads more into the buffer (which is empty): bytes read, 0 at
                                // the end, -1 with an exception.
                                int fill() {
                                        _start = _end = 0;
                                        if (_ended) return 0;
                                        if (!usable() || !wait(POLLIN, "data")) return -1;
                                        ssize_t n;
                                        do n = recv(fd(), _buffer, sizeof(_buffer), 0);
                                        while (n < 0 && errno == EINTR);
                                        if (n < 0) {
                                                raise("Reading from %s failed: %s", _peer, strerror(errno));
                                                return -1;
                                        }
                                        if (n == 0) _ended = true;
                                        _end = size_t(n);
                                        return int(n);
                                }
                                // Up to @p most bytes of what's buffered, taken.
                                void take(Text &out, size_t most) {
                                        const size_t n = _end - _start < most ? _end - _start : most;
                                        out.append(_buffer + _start, n);
                                        _start += n;
                                }
                                // A line, without its newline (or \r\n); false at the end with
                                // nothing read, or with an exception.
                                bool readLine(Text &out) {
                                        bool any = false;
                                        for (;;) {
                                                if (!buffered()) {
                                                        const int n = fill();
                                                        if (n < 0) return false;
                                                        if (n == 0) break;
                                                }
                                                any = true;
                                                const char *from = _buffer + _start;
                                                const char *nl =
                                                        static_cast<const char *>(memchr(from, '\n', _end - _start));
                                                if (nl == nullptr) {
                                                        take(out, _end - _start);
                                                        continue;
                                                }
                                                take(out, size_t(nl - from));
                                                ++_start;
                                                break;
                                        }
                                        if (out.length() != 0 && out.data()[out.length() - 1] == '\r') {
                                                Text trimmed;
                                                trimmed.append(out.data(), out.length() - 1);
                                                out.clear();
                                                out.append(trimmed.data(), trimmed.length());
                                        }
                                        return any;
                                }
                                // Exactly @p count bytes (fewer if it ends first); false with an
                                // exception.
                                bool readExactly(Text &out, size_t count) {
                                        while (count != 0) {
                                                if (!buffered()) {
                                                        const int n = fill();
                                                        if (n < 0) return false;
                                                        if (n == 0) return true;
                                                }
                                                const size_t before = out.length();
                                                take(out, count);
                                                if (out.length() == before) return outOfMemory();
                                                count -= out.length() - before;
                                        }
                                        return true;
                                }
                                // All the rest, to the end; false with an exception.
                                bool readAll(Text &out) {
                                        for (;;) {
                                                if (!buffered()) {
                                                        const int n = fill();
                                                        if (n < 0) return false;
                                                        if (n == 0) return true;
                                                }
                                                const size_t before = out.length();
                                                take(out, _end - _start);
                                                if (out.length() == before) return outOfMemory();
                                        }
                                }
                                bool send(const char *data, size_t length) {
                                        if (!usable()) return false;
                                        while (length != 0) {
                                                const ssize_t n = ::send(fd(), data, length, MSG_NOSIGNAL);
                                                if (n < 0 && errno == EINTR) continue;
                                                if (n <= 0) {
                                                        raise("Sending to %s failed: %s", _peer, strerror(errno));
                                                        return false;
                                                }
                                                data += n;
                                                length -= size_t(n);
                                        }
                                        return true;
                                }

                        private:
                                static bool outOfMemory() {
                                        raise("Out of memory reading from the network");
                                        return false;
                                }

                                char   _buffer[2048];
                                size_t _start = 0, _end = 0;
                                bool   _ended = false;
                                char   _peer[INET_ADDRSTRLEN] = "";
                                int    _peerPort = 0;
                };

                // A TCP connection to @p host, waiting at most @p timeoutMs: a Socket, or nullptr
                // with an exception.
                Socket *connectTo(const char *host, int port, int timeoutMs) {
                        sockaddr_in a;
                        if (!resolve(host, port, a)) return nullptr;
                        const int fd = socket(AF_INET, SOCK_STREAM, 0);
                        if (fd < 0) {
                                raise("Can't make a socket: %s", strerror(errno));
                                return nullptr;
                        }
                        // Not blocking while it connects, so the wait has a limit.
                        const int flags = fcntl(fd, F_GETFL);
                        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                        int r = connect(fd, reinterpret_cast<const sockaddr *>(&a), sizeof(a));
                        int err = r == 0 ? 0 : errno;
                        if (err == EINPROGRESS) {
                                r = waitFd(fd, POLLOUT, timeoutMs);
                                if (r == 0) {
                                        err = ETIMEDOUT;
                                } else if (r < 0) {
                                        err = errno;
                                } else {
                                        socklen_t length = sizeof(err);
                                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) != 0) err = errno;
                                }
                        }
                        if (err != 0) {
                                ::close(fd);
                                raise("Can't connect to %s port %d: %s", host, port, strerror(err));
                                return nullptr;
                        }
                        fcntl(fd, F_SETFL, flags);
                        return new Socket(fd);
                }

                // ---- Waiting for TCP connections.
                class ServerSocket : public Endpoint {
                        public:
                                ServerSocket(int fd, int port) : Endpoint(fd), _port(port) {}
                                int port() const { return _port; }

                        private:
                                int _port;
                };

                // ---- Datagrams.
                class UdpSocket : public Endpoint {
                        public:
                                UdpSocket(int fd, int port) : Endpoint(fd), _port(port) {}
                                ~UdpSocket() override { free(_buffer); }
                                int   port() const { return _port; }
                                char *buffer() {
                                        if (_buffer == nullptr) _buffer = static_cast<char *>(malloc(UdpLargest));
                                        return _buffer;
                                }
                                char sender[INET_ADDRSTRLEN] = "";
                                int  senderPort = 0;

                        private:
                                int   _port;
                                char *_buffer = nullptr;
                };

                // The port a socket was given (0 asks for any free one).
                int boundPort(int fd) {
                        sockaddr_in a;
                        socklen_t   length = sizeof(a);
                        if (getsockname(fd, reinterpret_cast<sockaddr *>(&a), &length) != 0) return 0;
                        return ntohs(a.sin_port);
                }

                // A socket of @p type bound to @p port on every address: its fd, or -1 with an
                // exception.
                int boundSocket(int type, int port) {
                        if (port < 0 || port > 65535) {
                                raise("Port %d isn't 0 to 65535", port);
                                return -1;
                        }
                        const int fd = socket(AF_INET, type, 0);
                        if (fd < 0) {
                                raise("Can't make a socket: %s", strerror(errno));
                                return -1;
                        }
                        const int on = 1;
                        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                        if (type == SOCK_DGRAM) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
                        sockaddr_in a;
                        memset(&a, 0, sizeof(a));
                        a.sin_family = AF_INET;
                        a.sin_port = htons(uint16_t(port));
                        a.sin_addr.s_addr = htonl(INADDR_ANY);
                        if (bind(fd, reinterpret_cast<const sockaddr *>(&a), sizeof(a)) != 0) {
                                const int err = errno;
                                ::close(fd);
                                raise("Can't use port %d: %s", port, strerror(err));
                                return -1;
                        }
                        return fd;
                }

                // ---- Generic-convention helpers
                const String &argString(asIScriptGeneric *gen, asUINT n) {
                        return *static_cast<const String *>(gen->GetArgAddress(n));
                }
                int argInt(asIScriptGeneric *gen, asUINT n) {
                        return int(gen->GetArgDWord(n));
                }
                void returnString(asIScriptGeneric *gen, const char *data, size_t length) {
                        new (gen->GetAddressOfReturnLocation()) String(data, length);
                }
                // Hands @p text's buffer to the returned string (no copy).
                void returnText(asIScriptGeneric *gen, Text &text) {
                        String      *s = new (gen->GetAddressOfReturnLocation()) String();
                        const size_t length = text.length();
                        char        *data = text.release();
                        if (data != nullptr) s->adopt(data, length);
                }
                template <typename T> T *self(asIScriptGeneric *gen) {
                        return static_cast<T *>(gen->GetObject());
                }
                template <typename T> void addRefOf(asIScriptGeneric *gen) {
                        self<T>(gen)->addRef();
                }
                template <typename T> void releaseOf(asIScriptGeneric *gen) {
                        self<T>(gen)->release();
                }
                template <typename T> void closeOf(asIScriptGeneric *gen) {
                        self<T>(gen)->close();
                }
                template <typename T> void isOpenOf(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self<T>(gen)->isOpen());
                }
                template <typename T> void setTimeoutOf(asIScriptGeneric *gen) {
                        self<T>(gen)->setTimeoutMs(argInt(gen, 0));
                }
                template <typename T> void readyOf(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self<T>(gen)->readable(argInt(gen, 0)));
                }

                // ---- Socket

                void socketConnect(asIScriptGeneric *gen) {
                        gen->SetReturnAddress(connectTo(argString(gen, 0).cStr(), argInt(gen, 1), ConnectTimeoutMs));
                }
                void socketRead(asIScriptGeneric *gen) {
                        Socket   *s = self<Socket>(gen);
                        const int most = argInt(gen, 0);
                        Text      text;
                        if (most > 0 && (s->buffered() || s->fill() > 0)) s->take(text, size_t(most));
                        returnText(gen, text);
                }
                void socketReadLine(asIScriptGeneric *gen) {
                        Text line;
                        self<Socket>(gen)->readLine(line);
                        returnText(gen, line);
                }
                void socketReadAll(asIScriptGeneric *gen) {
                        Text text;
                        self<Socket>(gen)->readAll(text);
                        returnText(gen, text);
                }
                void socketEof(asIScriptGeneric *gen) {
                        Socket *s = self<Socket>(gen);
                        gen->SetReturnByte(!s->buffered() && s->fill() <= 0 && s->ended());
                }
                void socketReady(asIScriptGeneric *gen) {
                        Socket *s = self<Socket>(gen);
                        gen->SetReturnByte(s->buffered() || s->ended() || s->readable(argInt(gen, 0)));
                }
                void socketWriteValue(asIScriptGeneric *gen, bool newline) {
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        if (newline) text.append("\n");
                        self<Socket>(gen)->send(text.data(), text.length());
                }
                void socketWrite(asIScriptGeneric *gen) {
                        socketWriteValue(gen, false);
                }
                void socketWriteLine(asIScriptGeneric *gen) {
                        socketWriteValue(gen, true);
                }
                void socketAddress(asIScriptGeneric *gen) {
                        const char *peer = self<Socket>(gen)->peer();
                        returnString(gen, peer, strlen(peer));
                }
                void socketPort(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self<Socket>(gen)->peerPort()));
                }

                // ---- ServerSocket

                void serverListen(asIScriptGeneric *gen) {
                        const int fd = boundSocket(SOCK_STREAM, argInt(gen, 0));
                        if (fd < 0) return;
                        if (listen(fd, 8) != 0) {
                                raise("Can't listen on port %d: %s", argInt(gen, 0), strerror(errno));
                                ::close(fd);
                                return;
                        }
                        gen->SetReturnAddress(new ServerSocket(fd, boundPort(fd)));
                }
                void serverAccept(asIScriptGeneric *gen) {
                        ServerSocket *s = self<ServerSocket>(gen);
                        if (!s->usable() || !s->wait(POLLIN, "a connection")) return;
                        int fd;
                        do fd = accept(s->fd(), nullptr, nullptr);
                        while (fd < 0 && errno == EINTR);
                        if (fd < 0) {
                                raise("Accepting a connection failed: %s", strerror(errno));
                                return;
                        }
                        gen->SetReturnAddress(new Socket(fd));
                }
                void serverPort(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self<ServerSocket>(gen)->port()));
                }

                // ---- UdpSocket

                void udpOpen(asIScriptGeneric *gen) {
                        const int fd = boundSocket(SOCK_DGRAM, argInt(gen, 0));
                        if (fd >= 0) gen->SetReturnAddress(new UdpSocket(fd, boundPort(fd)));
                }
                void udpSendTo(asIScriptGeneric *gen) {
                        UdpSocket    *u = self<UdpSocket>(gen);
                        const char   *host = argString(gen, 0).cStr();
                        const int     port = argInt(gen, 1);
                        const String &data = argString(gen, 2);
                        sockaddr_in   a;
                        if (!u->usable() || !resolve(host, port, a)) return;
                        ssize_t n;
                        do
                                n = sendto(u->fd(), data.data(), data.length(), 0,
                                           reinterpret_cast<const sockaddr *>(&a), sizeof(a));
                        while (n < 0 && errno == EINTR);
                        if (n < 0) raise("Sending to %s port %d failed: %s", host, port, strerror(errno));
                }
                void udpReceive(asIScriptGeneric *gen) {
                        UdpSocket *u = self<UdpSocket>(gen);
                        char      *buffer = u->buffer();
                        if (buffer == nullptr) {
                                raise("Out of memory for a datagram");
                                return;
                        }
                        if (!u->usable() || !u->wait(POLLIN, "a datagram")) {
                                returnString(gen, "", 0);
                                return;
                        }
                        sockaddr_in a;
                        socklen_t   length = sizeof(a);
                        ssize_t     n;
                        do n = recvfrom(u->fd(), buffer, UdpLargest, 0, reinterpret_cast<sockaddr *>(&a), &length);
                        while (n < 0 && errno == EINTR);
                        if (n < 0) {
                                raise("Receiving a datagram failed: %s", strerror(errno));
                                n = 0;
                        } else {
                                inet_ntop(AF_INET, &a.sin_addr, u->sender, sizeof(u->sender));
                                u->senderPort = ntohs(a.sin_port);
                        }
                        returnString(gen, buffer, size_t(n));
                }
                void udpSender(asIScriptGeneric *gen) {
                        const char *sender = self<UdpSocket>(gen)->sender;
                        returnString(gen, sender, strlen(sender));
                }
                void udpSenderPort(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self<UdpSocket>(gen)->senderPort));
                }
                void udpPort(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self<UdpSocket>(gen)->port()));
                }

                // ---- Addresses

                void localAddress(asIScriptGeneric *gen) {
                        char text[INET_ADDRSTRLEN] = "";
#ifdef ANGEL_BMDS
                        // The first interface with an address that isn't loopback.
                        for (uint32_t i = 0;; ++i) {
                                bmds_net_info_t info;
                                memset(&info, 0, sizeof(info));
                                info.size = sizeof(info);
                                if (bmds_net_info(i, &info) < 0) break;
                                if (info.address == 0 || strcmp(info.name, "lo") == 0) continue;
                                in_addr a;
                                a.s_addr = info.address;
                                inet_ntop(AF_INET, &a, text, sizeof(text));
                                break;
                        }
#else
                        ifaddrs *list = nullptr;
                        if (getifaddrs(&list) == 0) {
                                for (const ifaddrs *i = list; i != nullptr; i = i->ifa_next) {
                                        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
                                        if ((i->ifa_flags & IFF_LOOPBACK) != 0) continue;
                                        inet_ntop(AF_INET,
                                                  &reinterpret_cast<const sockaddr_in *>(i->ifa_addr)->sin_addr, text,
                                                  sizeof(text));
                                        break;
                                }
                                freeifaddrs(list);
                        }
#endif
                        returnString(gen, text, strlen(text));
                }
                void lookup(asIScriptGeneric *gen) {
                        sockaddr_in a;
                        char        text[INET_ADDRSTRLEN] = "";
                        if (resolve(argString(gen, 0).cStr(), 0, a))
                                inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
                        returnString(gen, text, strlen(text));
                }

                // ---- HTTP

                // An answer: its status, reason, headers ("Name: value" a line) and body.
                class HttpResponse {
                        public:
                                void addRef() { ++refs; }
                                void release() {
                                        if (--refs == 0) delete this;
                                }

                                int    status = 0;
                                String reason;
                                String body;
                                String url; // where it came from (after any redirects)
                                Text   headers;
                                int    refs = 1;
                };

                // A header's value from "Name: value" lines (the first of that name), or nullptr.
                const char *findHeader(const Text &headers, const char *name, size_t &length) {
                        const size_t nameLength = strlen(name);
                        const char  *p = headers.data(), *end = p + headers.length();
                        while (p < end) {
                                const char *eol = static_cast<const char *>(memchr(p, '\n', size_t(end - p)));
                                if (eol == nullptr) eol = end;
                                if (size_t(eol - p) > nameLength && strncasecmp(p, name, nameLength) == 0 &&
                                    p[nameLength] == ':') {
                                        const char *v = p + nameLength + 1;
                                        while (v < eol && (*v == ' ' || *v == '\t')) ++v;
                                        const char *e = eol;
                                        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) --e;
                                        length = size_t(e - v);
                                        return v;
                                }
                                p = eol + 1;
                        }
                        return nullptr;
                }

                // An http:// address taken apart.
                struct Url {
                                char host[256] = "";
                                int  port = 80;
                                Text path; // with the query; "/" at least
                };

                bool parseUrl(const char *url, Url &u) {
                        const char *p = url;
                        if (strncasecmp(p, "https://", 8) == 0) {
                                raise("HTTPS isn't supported yet: '%s' (only http:// addresses)", url);
                                return false;
                        }
                        if (strncasecmp(p, "http://", 7) == 0) {
                                p += 7;
                        } else if (strstr(p, "://") != nullptr) {
                                raise("'%s' isn't an http:// address", url);
                                return false;
                        }
                        const char *hostEnd = p + strcspn(p, "/?#");
                        const char *at = static_cast<const char *>(memchr(p, '@', size_t(hostEnd - p)));
                        if (at != nullptr) p = at + 1; // user:password@ isn't sent
                        const char *colon = static_cast<const char *>(memchr(p, ':', size_t(hostEnd - p)));
                        const char *nameEnd = colon != nullptr ? colon : hostEnd;
                        if (nameEnd == p || size_t(nameEnd - p) >= sizeof(u.host)) {
                                raise("'%s' has no host name", url);
                                return false;
                        }
                        memcpy(u.host, p, size_t(nameEnd - p));
                        u.host[nameEnd - p] = 0;
                        u.port = 80;
                        if (colon != nullptr) {
                                char      *end = nullptr;
                                const long port = strtol(colon + 1, &end, 10);
                                if (end != hostEnd || port <= 0 || port > 65535) {
                                        raise("'%s' has a bad port", url);
                                        return false;
                                }
                                u.port = int(port);
                        }
                        const char  *rest = hostEnd;
                        const size_t restLength = strcspn(rest, "#");
                        u.path.clear();
                        if (restLength == 0 || rest[0] != '/') u.path.append("/");
                        u.path.append(rest, restLength);
                        return true;
                }

                // One request and its answer, over a connection of its own: false (and an
                // exception) when the network failed, not for an HTTP error status.
                bool exchange(const char *method, const Url &u, const String &body, CScriptArray *headers,
                              HttpResponse *r) {
                        Socket *s = connectTo(u.host, u.port, HttpTimeoutMs);
                        if (s == nullptr) return false;
                        s->setTimeoutMs(HttpTimeoutMs);
                        Text request;
                        request.appendf("%s %s HTTP/1.1\r\nHost: %s", method, u.path.data(), u.host);
                        if (u.port != 80) request.appendf(":%d", u.port);
                        request.append("\r\nUser-Agent: angel\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
                        const bool sendsBody =
                                body.length() != 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;
                        bool hasType = false;
                        if (headers != nullptr)
                                for (asUINT i = 0; i < headers->GetSize(); ++i) {
                                        const String *h = static_cast<const String *>(headers->At(i));
                                        if (h->length() == 0) continue;
                                        if (strncasecmp(h->cStr(), "Content-Type:", 13) == 0) {
                                                if (!sendsBody) continue; // a POST redirected to a GET
                                                hasType = true;
                                        }
                                        request.append(h->data(), h->length());
                                        request.append("\r\n");
                                }
                        if (sendsBody) {
                                if (!hasType) request.append("Content-Type: application/x-www-form-urlencoded\r\n");
                                request.appendf("Content-Length: %u\r\n", unsigned(body.length()));
                        }
                        request.append("\r\n");
                        bool ok = s->send(request.data(), request.length()) && s->send(body.data(), body.length());
                        // The status line (1xx answers skipped) and the headers.
                        Text line;
                        while (ok) {
                                line.clear();
                                if (!s->readLine(line)) {
                                        if (s->ended()) raise("%s closed the connection without answering", u.host);
                                        ok = false;
                                        break;
                                }
                                int  status = 0, used = 0;
                                char version[4];
                                if (sscanf(line.data(), "HTTP/%3[0-9.] %d%n", version, &status, &used) < 2 ||
                                    status < 100) {
                                        raise("%s didn't answer in HTTP", u.host);
                                        ok = false;
                                        break;
                                }
                                const char *reason = line.data() + used;
                                while (*reason == ' ') ++reason;
                                r->status = status;
                                r->reason.assign(reason, strlen(reason));
                                r->headers.clear();
                                for (;;) {
                                        line.clear();
                                        if (!s->readLine(line)) {
                                                if (s->ended()) raise("%s's answer ended in its headers", u.host);
                                                ok = false;
                                                break;
                                        }
                                        if (line.length() == 0) break;
                                        r->headers.append(line.data(), line.length());
                                        r->headers.append("\n");
                                }
                                if (status >= 200) break;
                        }
                        // The body: chunked, so long, or to the end.
                        Text        content;
                        size_t      length = 0;
                        const char *te = ok ? findHeader(r->headers, "Transfer-Encoding", length) : nullptr;
                        const bool  noBody = strcmp(method, "HEAD") == 0 || r->status == 204 || r->status == 304;
                        if (!ok || noBody) {
                        } else if (te != nullptr && length >= 7 && strncasecmp(te + length - 7, "chunked", 7) == 0) {
                                for (;;) {
                                        line.clear();
                                        if (!s->readLine(line)) {
                                                if (s->ended()) raise("%s's answer ended early", u.host);
                                                ok = false;
                                                break;
                                        }
                                        char               *end = nullptr;
                                        const unsigned long size = strtoul(line.data(), &end, 16);
                                        if (end == line.data()) {
                                                raise("%s sent a bad chunk", u.host);
                                                ok = false;
                                                break;
                                        }
                                        if (size == 0) { // the trailer, to its empty line
                                                do line.clear();
                                                while (s->readLine(line) && line.length() != 0);
                                                break;
                                        }
                                        if (!s->readExactly(content, size)) {
                                                ok = false;
                                                break;
                                        }
                                        line.clear();
                                        s->readLine(line);
                                }
                        } else if (const char *cl = findHeader(r->headers, "Content-Length", length); cl != nullptr) {
                                ok = s->readExactly(content, strtoul(cl, nullptr, 10));
                        } else {
                                ok = s->readAll(content);
                        }
                        s->release();
                        const size_t contentLength = content.length();
                        char        *data = content.release();
                        if (data != nullptr) r->body.adopt(data, contentLength);
                        return ok;
                }

                // A request, following redirects: the answer, or nullptr with an exception.
                HttpResponse *request(const char *method, const char *url, const String &body, CScriptArray *headers) {
                        Url           u;
                        HttpResponse *r = new HttpResponse();
                        Text          where;
                        where.append(url);
                        const char *m = method;
                        bool        withBody = true;
                        for (int hop = 0;; ++hop) {
                                if (!parseUrl(where.data(), u) ||
                                    !exchange(m, u, withBody ? body : String(), headers, r)) {
                                        r->release();
                                        return nullptr;
                                }
                                const int   s = r->status;
                                size_t      length = 0;
                                const char *location = findHeader(r->headers, "Location", length);
                                if ((s != 301 && s != 302 && s != 303 && s != 307 && s != 308) || location == nullptr)
                                        break;
                                if (hop == HttpRedirects) {
                                        raise("Too many redirects from '%s'", url);
                                        r->release();
                                        return nullptr;
                                }
                                Text       next;
                                const bool absolute = length > 0 && location[0] == '/';
                                if (absolute || memchr(location, ':', length) == nullptr) {
                                        next.appendf("http://%s", u.host);
                                        if (u.port != 80) next.appendf(":%d", u.port);
                                }
                                if (!absolute && memchr(location, ':', length) == nullptr) {
                                        // Relative to the directory of the path.
                                        const char *path = u.path.data();
                                        next.append(path, size_t(strrchr(path, '/') - path) + 1);
                                }
                                next.append(location, length);
                                where.clear();
                                where.append(next.data(), next.length());
                                // 303, and 301/302 after a POST as browsers do: a GET without the body.
                                if (s == 303 || ((s == 301 || s == 302) && strcmp(m, "POST") == 0)) {
                                        m = "GET";
                                        withBody = false;
                                }
                                r->body = String();
                        }
                        r->url.assign(where.data(), where.length());
                        return r;
                }

                // The body of a successful answer, or an exception naming the status.
                void bodyOf(asIScriptGeneric *gen, HttpResponse *r, const char *url) {
                        if (r == nullptr) {
                                returnString(gen, "", 0);
                                return;
                        }
                        if (r->status < 200 || r->status >= 300)
                                raise("HTTP %d %s: %s", r->status, r->reason.cStr(), url);
                        new (gen->GetAddressOfReturnLocation()) String(r->body);
                        r->release();
                }
                void httpGet(asIScriptGeneric *gen) {
                        const char *url = argString(gen, 0).cStr();
                        bodyOf(gen, request("GET", url, String(), nullptr), url);
                }
                void httpPost(asIScriptGeneric *gen) {
                        const char   *url = argString(gen, 0).cStr();
                        const String &type = argString(gen, 2);
                        String        header("Content-Type: ");
                        header.append(type.data(), type.length());
                        CScriptArray *headers = CScriptArray::Create(stringArrayType(gen->GetEngine()));
                        if (headers != nullptr) headers->InsertLast(&header);
                        bodyOf(gen, request("POST", url, argString(gen, 1), headers), url);
                        if (headers != nullptr) headers->Release();
                }
                void httpRequest(asIScriptGeneric *gen) {
                        CScriptArray *headers = static_cast<CScriptArray *>(gen->GetArgObject(3));
                        String        method(argString(gen, 0));
                        for (uint32_t i = 0; i < method.length(); ++i)
                                if (method.mutableData()[i] >= 'a' && method.mutableData()[i] <= 'z')
                                        method.mutableData()[i] = char(method.mutableData()[i] - 'a' + 'A');
                        gen->SetReturnAddress(
                                request(method.cStr(), argString(gen, 1).cStr(), argString(gen, 2), headers));
                        if (headers != nullptr) headers->Release();
                }
                void httpEncode(asIScriptGeneric *gen) {
                        const String &in = argString(gen, 0);
                        Text          out;
                        for (uint32_t i = 0; i < in.length(); ++i) {
                                const unsigned char c = static_cast<unsigned char>(in.data()[i]);
                                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                    c == '-' || c == '.' || c == '_' || c == '~') {
                                        const char ch = char(c);
                                        out.append(&ch, 1);
                                } else {
                                        out.appendf("%%%02X", c);
                                }
                        }
                        returnText(gen, out);
                }
                void responseHeader(asIScriptGeneric *gen) {
                        size_t      length = 0;
                        const char *v = findHeader(self<HttpResponse>(gen)->headers, argString(gen, 0).cStr(), length);
                        returnString(gen, v != nullptr ? v : "", v != nullptr ? length : 0);
                }
                void responseHeaders(asIScriptGeneric *gen) {
                        const Text &h = self<HttpResponse>(gen)->headers;
                        returnString(gen, h.data(), h.length());
                }

                // The methods every socket kind has.
                template <typename T> void registerEndpoint(asIScriptEngine *engine, const char *type) {
                        behaviour(engine, type, asBEHAVE_ADDREF, "void f()", addRefOf<T>, nullptr);
                        behaviour(engine, type, asBEHAVE_RELEASE, "void f()", releaseOf<T>, nullptr);
                        method(engine, type, "void close()", closeOf<T>,
                               "Closes it (also done when it's no longer used).");
                        method(engine, type, "bool isOpen() const", isOpenOf<T>, "It's open (not closed yet).");
                        method(engine, type, "void setTimeout(int ms)", setTimeoutOf<T>,
                               "How long waiting may take before an exception says it timed out; 0 (the start): no "
                               "limit.");
                        return;
                }

        } // namespace

        void registerNetwork(asIScriptEngine *engine) {
                check(engine->RegisterObjectType("Socket", 0, asOBJ_REF), "Socket");
                behaviour(engine, "Socket", asBEHAVE_FACTORY, "Socket@ f(const string &in host, int port)",
                          socketConnect, "Connects to a port of a host (a name or an address), TCP.");
                registerEndpoint<Socket>(engine, "Socket");
                method(engine, "Socket", "string read(int most = 4096)", socketRead,
                       "What has arrived, up to most bytes (waiting for some); \"\" at the end.");
                method(engine, "Socket", "string readLine()", socketReadLine, "The next line, without its newline.");
                method(engine, "Socket", "string readAll()", socketReadAll,
                       "Everything until the other end closes the connection.");
                method(engine, "Socket", "bool eof()", socketEof,
                       "The other end closed the connection and everything was read (waits to know).");
                method(engine, "Socket", "bool ready(int ms = 0)", socketReady,
                       "Something can be read now (or the end), waiting up to ms for it; 0 only looks.");
                method(engine, "Socket", "void write(const ?&in)", socketWrite, "Sends a value, as print() shows it.");
                method(engine, "Socket", "void writeLine(const ?&in)", socketWriteLine,
                       "Sends a value, then a newline.");
                method(engine, "Socket", "string address() const", socketAddress, "The other end's address.");
                method(engine, "Socket", "int port() const", socketPort, "The other end's port.");

                check(engine->RegisterObjectType("ServerSocket", 0, asOBJ_REF), "ServerSocket");
                behaviour(engine, "ServerSocket", asBEHAVE_FACTORY, "ServerSocket@ f(int port)", serverListen,
                          "Waits for TCP connections to a port (0: any free one, see port()).");
                registerEndpoint<ServerSocket>(engine, "ServerSocket");
                method(engine, "ServerSocket", "Socket@ accept()", serverAccept,
                       "Waits for a connection, and takes it.");
                method(engine, "ServerSocket", "bool ready(int ms = 0)", readyOf<ServerSocket>,
                       "A connection is waiting, waiting up to ms for one; 0 only looks.");
                method(engine, "ServerSocket", "int port() const", serverPort, "The port it listens on.");

                check(engine->RegisterObjectType("UdpSocket", 0, asOBJ_REF), "UdpSocket");
                behaviour(engine, "UdpSocket", asBEHAVE_FACTORY, "UdpSocket@ f(int port = 0)", udpOpen,
                          "Sends and receives datagrams on a port (0: any free one), broadcast too.");
                registerEndpoint<UdpSocket>(engine, "UdpSocket");
                method(engine, "UdpSocket", "void sendTo(const string &in host, int port, const string &in data)",
                       udpSendTo, "Sends a datagram (to 255.255.255.255: everyone on the network).");
                method(engine, "UdpSocket", "string receive()", udpReceive,
                       "Waits for a datagram and returns it; sender() says who sent it.");
                method(engine, "UdpSocket", "string sender() const", udpSender,
                       "The address the last datagram came from.");
                method(engine, "UdpSocket", "int senderPort() const", udpSenderPort,
                       "The port the last datagram came from.");
                method(engine, "UdpSocket", "bool ready(int ms = 0)", readyOf<UdpSocket>,
                       "A datagram is waiting, waiting up to ms for one; 0 only looks.");
                method(engine, "UdpSocket", "int port() const", udpPort, "The port it uses.");

                def(engine, "string localAddress()", localAddress,
                    "This computer's address on the network (\"\": none).");
                def(engine, "string lookup(const string &in host)", lookup, "A host name's address.");

                check(engine->RegisterObjectType("HttpResponse", 0, asOBJ_REF), "HttpResponse");
                behaviour(engine, "HttpResponse", asBEHAVE_ADDREF, "void f()", addRefOf<HttpResponse>, nullptr);
                behaviour(engine, "HttpResponse", asBEHAVE_RELEASE, "void f()", releaseOf<HttpResponse>, nullptr);
                check(engine->RegisterObjectProperty("HttpResponse", "const int status",
                                                     asOFFSET(HttpResponse, status)),
                      "HttpResponse::status");
                check(engine->RegisterObjectProperty("HttpResponse", "const string reason",
                                                     asOFFSET(HttpResponse, reason)),
                      "HttpResponse::reason");
                check(engine->RegisterObjectProperty("HttpResponse", "const string body", asOFFSET(HttpResponse, body)),
                      "HttpResponse::body");
                check(engine->RegisterObjectProperty("HttpResponse", "const string url", asOFFSET(HttpResponse, url)),
                      "HttpResponse::url");
                method(engine, "HttpResponse", "string header(const string &in name) const", responseHeader,
                       "A header's value (\"\": none); the name in any case.");
                method(engine, "HttpResponse", "string headers() const", responseHeaders,
                       "All the headers, \"Name: value\" a line.");

                check(engine->SetDefaultNamespace("Http"), "namespace Http");
                def(engine, "string get(const string &in url)", httpGet,
                    "The page at an http:// address (an exception if it can't be had).");
                def(engine,
                    "string post(const string &in url, const string &in body, const string &in type = "
                    "\"application/x-www-form-urlencoded\")",
                    httpPost, "Sends a form (or other data) to an http:// address; the answer's body.");
                def(engine,
                    "HttpResponse@ request(const string &in method, const string &in url, const string &in body = "
                    "\"\", "
                    "const array<string>@ headers = null)",
                    httpRequest, "Any request, with headers (\"Name: value\"); the answer, whatever its status.");
                def(engine, "string encode(const string &in text)", httpEncode,
                    "Text made safe for an address or a form (%-escaped).");
                check(engine->SetDefaultNamespace(""), "namespace");
                return;
        }

} // namespace angel
