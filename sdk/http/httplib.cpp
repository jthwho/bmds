/**
 * @file      httplib.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdshttp (bmds/httplib.h, devplan/network.md §35d): the C face of bmds_core's `Http` (the
 * URL, the request, the response parser, what follows a head: the kernel's own, compiled into
 * the library with libstdc++'s headers) over POSIX sockets and the kernel's TLS. bmds_core's
 * `Memory` is the program's heap (memory.cpp). Built with BMDS_HTTP_NO_TLS (the host's angel),
 * `https:` fails: there are no TLS sockets.
 */

#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <new>
#include <utility>
#include <sys/socket.h>
#include <sys/time.h>
#include <bmds/errno.h>
#include <bmds/http.h>
#include <bmds/httplib.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::String;
using bmds::StringView;
namespace Http = bmds::Http;

namespace {

        // A receive's buffer: what the parser is fed from, the body handed out of.
        constexpr size_t BufferBytes = 4096;

} // namespace

struct bmds_http {
                int                  fd = -1;
                bool                 tls = false;
                int32_t              status = 0;
                int64_t              length = -1;
                String               reason;
                String               url; // the one answering
                String               why;
                Http::ResponseParser parser;
                char                *headers = nullptr; // "name\0value\0" pairs
                const char         **pairs = nullptr;   // name, value, ...
                uint32_t             headerCount = 0;
                uint8_t              buf[BufferBytes];
                size_t               at = 0;   // the first byte not yet fed
                size_t               have = 0; // bytes in buf
                ByteSpan             pending;  // body bytes fed, not yet read
                bool                 ended = false;

                bmds_http() = default;
                ~bmds_http() {
                        if (fd >= 0) close(fd);
                        ::free(headers);
                        ::free(pairs);
                }
};

namespace {

        // Says what went wrong (with the URL) and returns -@p err.
        int32_t fail(bmds_http_t *h, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
        int32_t fail(bmds_http_t *h, int err, const char *fmt, ...) {
                h->why.clear();
                (void)h->why.appendf("%s: ", h->url.cStr());
                va_list args;
                va_start(args, fmt);
                (void)h->why.vappendf(fmt, args);
                va_end(args);
                return -err;
        }

        void closeConnection(bmds_http_t *h) {
                if (h->fd >= 0) close(h->fd);
                h->fd = -1;
                h->tls = false;
                h->at = h->have = 0;
                h->pending = ByteSpan();
                h->ended = false;
                return;
        }

        int32_t connectTo(bmds_http_t *h, const Http::Url &u, int32_t timeoutMs) {
                char host[256], port[8];
                if (u.host.size() >= sizeof(host)) return fail(h, EINVAL, "a host name too long");
                memcpy(host, u.host.data(), u.host.size());
                host[u.host.size()] = '\0';
                snprintf(port, sizeof(port), "%u", unsigned(u.port));
                struct addrinfo  hints = {};
                struct addrinfo *list = nullptr;
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                const int g = getaddrinfo(host, port, &hints, &list);
                if (g != 0) return fail(h, ENOENT, "%s: %s", host, gai_strerror(g));
                struct timeval tv = {};
                if (timeoutMs > 0) {
                        tv.tv_sec = timeoutMs / 1000;
                        tv.tv_usec = (timeoutMs % 1000) * 1000;
                }
                int err = 0;
                for (struct addrinfo *a = list; a != nullptr && h->fd < 0; a = a->ai_next) {
                        const int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
                        if (fd < 0) {
                                err = errno;
                                continue;
                        }
                        // Each wait, the connect's too (its send timeout).
                        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
                                h->fd = fd;
                        } else {
                                err = errno == EAGAIN ? ETIMEDOUT : errno;
                                close(fd);
                        }
                }
                freeaddrinfo(list);
                if (h->fd < 0) return fail(h, err, "%s port %s: %s", host, port, strerror(err));
                h->tls = u.tls; // startTls() next
                return 0;
        }

        int32_t startTls(bmds_http_t *h, const Http::Url &u, const bmds_tls_config_t *extra, bool insecure) {
#ifdef BMDS_HTTP_NO_TLS
                (void)u;
                (void)extra;
                (void)insecure;
                return fail(h, ENOSYS, "TLS isn't available on this system");
#else
                char host[256];
                memcpy(host, u.host.data(), u.host.size());
                host[u.host.size()] = '\0';
                bmds_tls_config_t c = {};
                if (extra != nullptr) {
                        const uint32_t n = extra->size < sizeof(c) ? extra->size : uint32_t(sizeof(c));
                        if (n < sizeof(uint32_t)) return fail(h, EINVAL, "a TLS configuration of no size");
                        memcpy(&c, extra, n);
                }
                c.size = sizeof(c);
                c.server_name = host;
                if (insecure) c.flags |= BMDS_TLS_NO_VERIFY;
                int r = bmds_tls_ctl(h->fd, BMDS_TLS_START, &c);
                if (r == 0) r = bmds_tls_ctl(h->fd, BMDS_TLS_HANDSHAKE, nullptr);
                if (r == 0) return 0;
                if (r == -BMDS_EINTR) (void)getppid(); // the handler runs as a libc call returns (linux.c)
                if (r == -BMDS_EAGAIN) return fail(h, ETIMEDOUT, "the TLS handshake didn't end in time");
                if (r != -BMDS_EKEYREJECTED) return fail(h, -r, "TLS: %s", strerror(-r));
                bmds_tls_info_t info = {};
                info.size = sizeof(info);
                const char *why = "its certificate was refused";
                if (bmds_tls_ctl(h->fd, BMDS_TLS_INFO, &info) == 0) {
                        if (info.verify & BMDS_TLS_VERIFY_CLOCK)
                                why = "the clock isn't set, so a certificate's dates can't be checked";
                        else if (info.verify & BMDS_TLS_VERIFY_EXPIRED)
                                why = "its certificate has expired";
                        else if (info.verify & BMDS_TLS_VERIFY_NOT_YET_VALID)
                                why = "its certificate isn't valid yet";
                        else if (info.verify & BMDS_TLS_VERIFY_NAME)
                                why = "its certificate is for another name";
                        else if (info.verify & BMDS_TLS_VERIFY_UNTRUSTED)
                                why = "its certificate isn't from an authority this system trusts";
                }
                return fail(h, EKEYREJECTED, "%s", why);
#endif
        }

        // @p headers without its Content-Type lines (a POST redirected to a GET has no body).
        int32_t withoutType(bmds_http_t *h, StringView headers, String &out) {
                out.clear();
                while (!headers.isEmpty()) {
                        size_t end = headers.indexOf("\r\n");
                        end = end == StringView::NPos ? headers.size() : end + 2;
                        const StringView line = headers.left(end);
                        headers = headers.mid(end);
                        if (line.size() >= 13 && line.left(13).equalsIgnoreCase("Content-Type:")) continue;
                        if (out.append(line).isError()) return fail(h, ENOMEM, "out of memory");
                }
                return 0;
        }

        int32_t sendAll(bmds_http_t *h, const void *data, size_t n) {
                const char *p = static_cast<const char *>(data);
                while (n > 0) {
                        const ssize_t w = send(h->fd, p, n, MSG_NOSIGNAL);
                        if (w < 0) {
                                const int e = errno == EAGAIN ? ETIMEDOUT : errno;
                                return fail(h, e, "sending the request: %s", strerror(e));
                        }
                        p += w;
                        n -= size_t(w);
                }
                return 0;
        }

        // Feeds what is received until the parser has the head, or the body bytes are @p wanted.
        // 0, or -errno.
        int32_t pump(bmds_http_t *h, bool head) {
                for (;;) {
                        if (head ? h->parser.hasHead() : (!h->pending.isEmpty() || h->parser.isDone())) return 0;
                        if (h->at == h->have) {
                                if (h->ended) return fail(h, EPROTO, "the connection ended early");
                                const ssize_t n = recv(h->fd, h->buf, sizeof(h->buf), 0);
                                if (n < 0) {
                                        const int e = errno == EAGAIN ? ETIMEDOUT : errno;
                                        return fail(h, e, "%s", e == ETIMEDOUT ? "no answer in time" : strerror(e));
                                }
                                h->at = 0;
                                h->have = size_t(n);
                                if (n == 0) {
                                        h->ended = true;
                                        if (h->parser.end().isError())
                                                return fail(h, EPROTO, "the connection ended early");
                                        continue;
                                }
                        }
                        size_t      used = 0;
                        ByteSpan    body;
                        const Error err = h->parser.feed(ByteSpan(h->buf + h->at, h->have - h->at), used, body);
                        if (err.isError())
                                return fail(h, err == Error::NoMem ? ENOMEM : EPROTO, "%s",
                                            err == Error::NotSupported ? "a transfer coding this can't read"
                                                                       : "not an HTTP response");
                        h->at += used;
                        h->pending = body;
                }
        }

        // The head's header lines ("Name: value\n") as name and value strings.
        int32_t keepHeaders(bmds_http_t *h) {
                const StringView lines = h->parser.headers();
                uint32_t         count = 0;
                for (char c : lines)
                        if (c == '\n') ++count;
                h->headers = static_cast<char *>(malloc(lines.size() + 1));
                h->pairs = static_cast<const char **>(malloc(sizeof(char *) * 2 * (count + 1)));
                if (h->headers == nullptr || h->pairs == nullptr) return fail(h, ENOMEM, "out of memory");
                memcpy(h->headers, lines.data(), lines.size());
                h->headers[lines.size()] = '\0';
                char *p = h->headers;
                for (uint32_t i = 0; i < count; ++i) {
                        char *colon = strstr(p, ": ");
                        char *end = strchr(p, '\n');
                        if (colon == nullptr || end == nullptr || colon > end) return fail(h, EPROTO, "a header");
                        *colon = '\0';
                        *end = '\0';
                        h->pairs[2 * i] = p;
                        h->pairs[2 * i + 1] = colon + 2;
                        p = end + 1;
                }
                h->headerCount = count;
                return 0;
        }

        int32_t open(bmds_http_t *h, const bmds_http_request_t &r) {
                if ((r.flags & ~uint32_t(BMDS_HTTP_NO_REDIRECTS | BMDS_HTTP_INSECURE)) != 0)
                        return fail(h, EINVAL, "flags 0x%x", unsigned(r.flags));
                if (r.url == nullptr) return fail(h, EINVAL, "no URL");
                if (h->url.append(r.url).isError()) return fail(h, ENOMEM, "out of memory");
                String method;
                if (method.append(r.method != nullptr ? r.method : "GET").isError())
                        return fail(h, ENOMEM, "out of memory");
                const void    *body = r.body;
                uint32_t       bodySize = r.body != nullptr ? r.body_size : 0;
                String         extra; // the caller's header lines, as they go
                if (r.headers != nullptr && extra.append(r.headers).isError()) return fail(h, ENOMEM, "out of memory");
                const bool     follow = (r.flags & BMDS_HTTP_NO_REDIRECTS) == 0;
                const uint32_t most = r.max_redirects != 0 ? r.max_redirects : BMDS_HTTP_DEFAULT_REDIRECTS;
                const int32_t  timeoutMs = r.timeout_ms == 0 ? BMDS_HTTP_DEFAULT_TIMEOUT_MS : r.timeout_ms;
                for (uint32_t hop = 0;; ++hop) {
                        Http::Url u;
                        if (Http::parseUrl(h->url.view(), u).isError())
                                return fail(h, EINVAL, "not an http: or https: URL");
                        closeConnection(h);
                        if (const int32_t e = connectTo(h, u, timeoutMs); e < 0) return e;
                        if (h->tls) {
                                if (const int32_t e = startTls(h, u, r.tls, (r.flags & BMDS_HTTP_INSECURE) != 0); e < 0)
                                        return e;
                        }
                        Http::Request q;
                        q.method = method.view();
                        q.url = &u;
                        q.userAgent = r.user_agent != nullptr ? StringView(r.user_agent) : StringView("BMDS");
                        q.from = r.range_from;
                        q.headers = extra.view();
                        // A body, or a method that carries one (an empty POST says so).
                        q.hasBody = body != nullptr || method.view() == StringView("POST") ||
                                    method.view() == StringView("PUT") || method.view() == StringView("PATCH");
                        q.bodyLength = bodySize;
                        String      head;
                        const Error berr = Http::buildRequest(q, head);
                        if (berr.isError())
                                return fail(h, berr == Error::NoMem ? ENOMEM : EINVAL, "%s",
                                            berr == Error::NoMem ? "out of memory" : "the method or a header line");
                        if (const int32_t e = sendAll(h, head.data(), head.size()); e < 0) return e;
                        if (bodySize > 0) {
                                if (const int32_t e = sendAll(h, body, bodySize); e < 0) return e;
                        }
                        uint8_t options = Http::ResponseParser::KeepHeaders;
                        if (method.view() == StringView("HEAD")) options |= Http::ResponseParser::HeadRequest;
                        h->parser = Http::ResponseParser(options);
                        if (const int32_t e = pump(h, true); e < 0) return e;
                        const Http::Head &hd = h->parser.head();
                        Http::Next        next;
                        const Error nerr = Http::next(hd, h->url.view(), method.view(), r.range_from, follow, next);
                        if (nerr == Error::IoError) return fail(h, EPROTO, "a redirect without a Location");
                        if (nerr == Error::Invalid)
                                return fail(h, EPROTO, "redirected to %s, not an http: or https: URL",
                                            hd.location.cStr());
                        if (nerr == Error::CorruptData)
                                return fail(h, EPROTO, "asked from byte %llu, sent from %llu",
                                            static_cast<unsigned long long>(r.range_from),
                                            static_cast<unsigned long long>(hd.rangeStart));
                        if (nerr.isError()) return fail(h, ENOMEM, "out of memory");
                        if (next.kind != Http::Next::Kind::Redirect) {
                                h->status = hd.status;
                                if (h->reason.append(hd.reason.view()).isError())
                                        return fail(h, ENOMEM, "out of memory");
                                h->length = hd.chunked || hd.length == Http::Unknown ? -1 : int64_t(hd.length);
                                return keepHeaders(h);
                        }
                        if (hop + 1 > most) return fail(h, EPROTO, "more than %u redirects", unsigned(most));
                        h->url = std::move(next.url);
                        if (next.asGet) {
                                method.clear();
                                (void)method.append("GET");
                                body = nullptr;
                                bodySize = 0;
                                String kept;
                                if (const int32_t e = withoutType(h, extra.view(), kept); e < 0) return e;
                                extra = std::move(kept);
                        }
                }
        }

} // namespace

extern "C" {

int32_t bmds_http_open(const bmds_http_request_t *request, bmds_http_t **out) {
        if (out == nullptr) return -BMDS_EINVAL;
        *out = nullptr;
        void *block = malloc(sizeof(bmds_http_t));
        if (block == nullptr) return -BMDS_ENOMEM;
        bmds_http_t *h = new (block) bmds_http_t();
        *out = h;
        if (request == nullptr || request->size < sizeof(uint32_t)) return fail(h, EINVAL, "no request");
        bmds_http_request_t r = {};
        memcpy(&r, request, request->size < sizeof(r) ? request->size : sizeof(r));
        const int32_t e = open(h, r);
        if (e < 0) {
                closeConnection(h);
                h->status = 0;
        }
        return e;
}

int32_t bmds_http_status(const bmds_http_t *http) {
        return http != nullptr ? http->status : 0;
}

const char *bmds_http_reason(const bmds_http_t *http) {
        return http != nullptr ? http->reason.cStr() : "";
}

const char *bmds_http_url(const bmds_http_t *http) {
        return http != nullptr ? http->url.cStr() : "";
}

const char *bmds_http_header(const bmds_http_t *http, const char *name) {
        if (http == nullptr || name == nullptr) return nullptr;
        for (uint32_t i = 0; i < http->headerCount; ++i)
                if (strcasecmp(http->pairs[2 * i], name) == 0) return http->pairs[2 * i + 1];
        return nullptr;
}

int32_t bmds_http_header_at(const bmds_http_t *http, uint32_t index, const char **name, const char **value) {
        if (http == nullptr || index >= http->headerCount) return -BMDS_ENOENT;
        if (name != nullptr) *name = http->pairs[2 * index];
        if (value != nullptr) *value = http->pairs[2 * index + 1];
        return 0;
}

int64_t bmds_http_length(const bmds_http_t *http) {
        return http != nullptr ? http->length : -1;
}

int32_t bmds_http_read(bmds_http_t *http, void *buf, uint32_t size) {
        if (http == nullptr || (buf == nullptr && size > 0)) return -BMDS_EINVAL;
        if (http->status == 0) return -BMDS_EINVAL;
        if (size == 0) return 0;
        if (const int32_t e = pump(http, false); e < 0) return e;
        if (http->pending.isEmpty()) return 0; // the body's end
        const size_t n = http->pending.size() < size ? http->pending.size() : size_t(size);
        memcpy(buf, http->pending.data(), n);
        http->pending = http->pending.mid(n);
        return int32_t(n);
}

int32_t bmds_http_tls_info(const bmds_http_t *http, bmds_tls_info_t *info) {
        if (http == nullptr || !http->tls || http->fd < 0) return -BMDS_EINVAL;
#ifdef BMDS_HTTP_NO_TLS
        (void)info;
        return -BMDS_EINVAL;
#else
        return bmds_tls_ctl(http->fd, BMDS_TLS_INFO, info);
#endif
}

const char *bmds_http_error(const bmds_http_t *http) {
        return http != nullptr ? http->why.cStr() : "out of memory";
}

void bmds_http_close(bmds_http_t *http) {
        if (http == nullptr) return;
        http->~bmds_http();
        free(http);
        return;
}

} // extern "C"
