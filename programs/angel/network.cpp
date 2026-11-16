/**
 * @file      network.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The network (devplan/scripting.md step 6): `Socket` (a TCP connection, read and written as
 * a File is; `startTls()` for STARTTLS), `TlsSocket` (one with TLS from the start: the kernel's
 * TLS sockets, devplan/network.md §35), `ServerSocket` (waits for connections), `UdpSocket`
 * (datagrams, broadcast allowed), `Http::get`, `post` and `request` (HTTP/1.1 and HTTPS through
 * libbmdshttp, bmds/httplib.h: the kernel's own parser), `localAddress()` and `lookup()`. IPv4.
 * What fails raises an exception that says why, which a catch block can handle. Waiting is
 * poll(), so a script waiting costs nothing.
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
#include <bmds/httplib.h>
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/errno.h>
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
                                bool        isTls() const { return _tls; }
                                void        setTls() { _tls = true; }
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
                                bool   _tls = false;
                };

                // TLS on @p s's connection (the kernel's: reads and writes stay as they were), the
                // server's certificate checked for @p name unless @p check is false: true, or false
                // with an exception saying why.
                bool startTls(Socket *s, const char *name, bool check) {
                        if (!s->usable()) return false;
                        if (s->isTls()) {
                                raise("The connection to %s has TLS already", name);
                                return false;
                        }
                        if (s->buffered()) {
                                raise("%s sent more before TLS started", name);
                                return false;
                        }
#ifdef ANGEL_BMDS
                        bmds_tls_config_t c;
                        memset(&c, 0, sizeof(c));
                        c.size = sizeof(c);
                        c.server_name = name;
                        c.flags = check ? 0u : BMDS_TLS_NO_VERIFY;
                        int r = bmds_tls_ctl(s->fd(), BMDS_TLS_START, &c);
                        if (r == 0) r = bmds_tls_ctl(s->fd(), BMDS_TLS_HANDSHAKE, nullptr);
                        if (r == -BMDS_EINTR) (void)getppid(); // its handler runs as a libc call returns
                        if (r == 0) {
                                s->setTls();
                                return true;
                        }
                        if (r == -BMDS_EKEYREJECTED) {
                                bmds_tls_info_t info;
                                memset(&info, 0, sizeof(info));
                                info.size = sizeof(info);
                                const char *why = "it was refused";
                                if (bmds_tls_ctl(s->fd(), BMDS_TLS_INFO, &info) == 0) {
                                        if (info.verify & BMDS_TLS_VERIFY_CLOCK)
                                                why = "the clock isn't set, so its dates can't be checked";
                                        else if (info.verify & BMDS_TLS_VERIFY_EXPIRED)
                                                why = "it has expired";
                                        else if (info.verify & BMDS_TLS_VERIFY_NOT_YET_VALID)
                                                why = "it isn't valid yet";
                                        else if (info.verify & BMDS_TLS_VERIFY_NAME)
                                                why = "it is for another name";
                                        else if (info.verify & BMDS_TLS_VERIFY_UNTRUSTED)
                                                why = "it isn't from an authority this system trusts";
                                }
                                raise("%s's certificate was refused: %s", name, why);
                        } else {
                                raise("TLS with %s failed: %s", name, strerror(-r));
                        }
                        return false;
#else
                        (void)check;
                        raise("TLS with %s: no TLS sockets on this system", name);
                        return false;
#endif
                }

                // What a TLS socket's session says of its server (empty fields if not TLS).
                struct TlsFacts {
                                char subject[128] = "";
                                char issuer[128] = "";
                                char fingerprint[7 + 64 + 1] = "";
                                char cipher[64] = "";
                };

                TlsFacts tlsFacts(Socket *s) {
                        TlsFacts f;
#ifdef ANGEL_BMDS
                        bmds_tls_info_t info;
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        if (!s->isTls() || bmds_tls_ctl(s->fd(), BMDS_TLS_INFO, &info) != 0) return f;
                        snprintf(f.subject, sizeof(f.subject), "%s", info.subject);
                        snprintf(f.issuer, sizeof(f.issuer), "%s", info.issuer);
                        snprintf(f.cipher, sizeof(f.cipher), "%s", info.cipher_name);
                        static const char hex[] = "0123456789abcdef";
                        memcpy(f.fingerprint, "SHA256:", 7);
                        for (int i = 0; i < 32; ++i) {
                                f.fingerprint[7 + 2 * i] = hex[info.peer_sha256[i] >> 4];
                                f.fingerprint[8 + 2 * i] = hex[info.peer_sha256[i] & 15];
                        }
                        f.fingerprint[7 + 64] = '\0';
#else
                        (void)s;
#endif
                        return f;
                }

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
                void socketStartTls(asIScriptGeneric *gen) {
                        (void)startTls(self<Socket>(gen), argString(gen, 0).cStr(), gen->GetArgByte(1) != 0);
                }
                void tlsConnect(asIScriptGeneric *gen) {
                        const char *host = argString(gen, 0).cStr();
                        Socket     *s = connectTo(host, argInt(gen, 1), ConnectTimeoutMs);
                        if (s == nullptr) return;
                        if (!startTls(s, host, gen->GetArgByte(2) != 0)) {
                                s->release();
                                return;
                        }
                        gen->SetReturnAddress(s);
                }
                void tlsSubject(asIScriptGeneric *gen) {
                        const TlsFacts f = tlsFacts(self<Socket>(gen));
                        returnString(gen, f.subject, strlen(f.subject));
                }
                void tlsIssuer(asIScriptGeneric *gen) {
                        const TlsFacts f = tlsFacts(self<Socket>(gen));
                        returnString(gen, f.issuer, strlen(f.issuer));
                }
                void tlsFingerprint(asIScriptGeneric *gen) {
                        const TlsFacts f = tlsFacts(self<Socket>(gen));
                        returnString(gen, f.fingerprint, strlen(f.fingerprint));
                }
                void tlsCipher(asIScriptGeneric *gen) {
                        const TlsFacts f = tlsFacts(self<Socket>(gen));
                        returnString(gen, f.cipher, strlen(f.cipher));
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

                // A request through libbmdshttp (redirects followed): the answer, or nullptr with an
                // exception saying why.
                HttpResponse *request(const char *method, const char *url, const String &body, CScriptArray *headers) {
                        const bool sendsBody =
                                body.length() != 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;
                        Text lines;
                        lines.append("Accept-Encoding: identity\r\n");
                        bool hasType = false;
                        if (headers != nullptr)
                                for (asUINT i = 0; i < headers->GetSize(); ++i) {
                                        const String *h = static_cast<const String *>(headers->At(i));
                                        if (h->length() == 0) continue;
                                        if (strncasecmp(h->cStr(), "Content-Type:", 13) == 0) hasType = true;
                                        lines.append(h->data(), h->length());
                                        lines.append("\r\n");
                                }
                        if (sendsBody && !hasType) lines.append("Content-Type: application/x-www-form-urlencoded\r\n");
                        bmds_http_request_t q;
                        memset(&q, 0, sizeof(q));
                        q.size = sizeof(q);
                        q.method = method;
                        q.url = url;
                        q.headers = lines.data();
                        q.body = sendsBody ? body.data() : nullptr;
                        q.body_size = body.length();
                        q.user_agent = "angel";
                        q.max_redirects = HttpRedirects;
                        q.timeout_ms = HttpTimeoutMs;
                        bmds_http_t *h = nullptr;
                        if (bmds_http_open(&q, &h) != 0) {
                                raise("%s", h != nullptr ? bmds_http_error(h) : "Out of memory");
                                bmds_http_close(h);
                                return nullptr;
                        }
                        HttpResponse *r = new HttpResponse();
                        r->status = bmds_http_status(h);
                        r->reason.assign(bmds_http_reason(h), strlen(bmds_http_reason(h)));
                        r->url.assign(bmds_http_url(h), strlen(bmds_http_url(h)));
                        const char *name, *value;
                        for (uint32_t i = 0; bmds_http_header_at(h, i, &name, &value) == 0; ++i)
                                r->headers.appendf("%s: %s\n", name, value);
                        Text    content;
                        char    buf[2048];
                        int32_t n;
                        while ((n = bmds_http_read(h, buf, sizeof(buf))) > 0) content.append(buf, size_t(n));
                        if (n < 0) {
                                raise("%s", bmds_http_error(h));
                                bmds_http_close(h);
                                r->release();
                                return nullptr;
                        }
                        bmds_http_close(h);
                        const size_t contentLength = content.length();
                        char        *data = content.release();
                        if (data != nullptr) r->body.adopt(data, contentLength);
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
                // A Socket, and a TlsSocket (the same connection, TLS from the start: its reads and
                // writes are the kernel's plaintext).
                check(engine->RegisterObjectType("Socket", 0, asOBJ_REF), "Socket");
                check(engine->RegisterObjectType("TlsSocket", 0, asOBJ_REF), "TlsSocket");
                behaviour(engine, "Socket", asBEHAVE_FACTORY, "Socket@ f(const string &in host, int port)",
                          socketConnect, "Connects to a port of a host (a name or an address), TCP.");
                behaviour(engine, "TlsSocket", asBEHAVE_FACTORY,
                          "TlsSocket@ f(const string &in host, int port, bool check = true)", tlsConnect,
                          "Connects to a port of a host with TLS; its certificate checked for the host's name "
                          "unless check is false.");
                static const char *const SocketTypes[] = {"Socket", "TlsSocket"};
                for (const char *type : SocketTypes) {
                        registerEndpoint<Socket>(engine, type);
                        method(engine, type, "string read(int most = 4096)", socketRead,
                               "What has arrived, up to most bytes (waiting for some); \"\" at the end.");
                        method(engine, type, "string readLine()", socketReadLine,
                               "The next line, without its newline.");
                        method(engine, type, "string readAll()", socketReadAll,
                               "Everything until the other end closes the connection.");
                        method(engine, type, "bool eof()", socketEof,
                               "The other end closed the connection and everything was read (waits to know).");
                        method(engine, type, "bool ready(int ms = 0)", socketReady,
                               "Something can be read now (or the end), waiting up to ms for it; 0 only looks.");
                        method(engine, type, "void write(const ?&in)", socketWrite,
                               "Sends a value, as print() shows it.");
                        method(engine, type, "void writeLine(const ?&in)", socketWriteLine,
                               "Sends a value, then a newline.");
                        method(engine, type, "string address() const", socketAddress, "The other end's address.");
                        method(engine, type, "int port() const", socketPort, "The other end's port.");
                }
                method(engine, "Socket", "void startTls(const string &in name, bool check = true)", socketStartTls,
                       "TLS from now on (after a protocol's STARTTLS), the certificate checked for name unless "
                       "check is false.");
                method(engine, "TlsSocket", "string subject() const", tlsSubject,
                       "The server's certificate's subject (\"CN=irc.libera.chat\").");
                method(engine, "TlsSocket", "string issuer() const", tlsIssuer, "Who issued the server's certificate.");
                method(engine, "TlsSocket", "string fingerprint() const", tlsFingerprint,
                       "The server's certificate's SHA-256 (\"SHA256:...\", IRC's CertFP).");
                method(engine, "TlsSocket", "string cipher() const", tlsCipher, "The cipher suite agreed on.");

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
