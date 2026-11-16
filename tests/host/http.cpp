/**
 * @file      http.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * HTTP for fetching (devplan/packages.md §23): URLs parsed and resolved (RFC 3986's examples),
 * the request, and responses read whole, a byte at a time and in pieces: by length, by chunks, to
 * the end, interim responses, ranges; what is refused.
 */

#include <doctest.h>

#include <string>
#include <bmds/http.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::StringView;
namespace Http = bmds::Http;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        std::string resolved(const char *base, const char *ref) {
                auto [out, err] = Http::resolve(base, ref);
                return err.isError() ? std::string("<") + err.name() + ">" : str(out.view());
        }

        // Feeds @p response in pieces of @p step bytes (0: whole); returns the body, or the error's name.
        std::string body(const std::string &response, size_t step, Http::ResponseParser &p, bool ended = true) {
                std::string out;
                size_t      at = 0;
                while (at < response.size() && !p.isDone()) {
                        const size_t n = step == 0 ? response.size() - at : std::min(step, response.size() - at);
                        ByteSpan     in(reinterpret_cast<const uint8_t *>(response.data()) + at, n);
                        while (!in.isEmpty() && !p.isDone()) {
                                size_t      used = 0;
                                ByteSpan    piece;
                                const Error err = p.feed(in, used, piece);
                                if (err.isError()) return std::string("<") + err.name() + ">";
                                out.append(reinterpret_cast<const char *>(piece.data()), piece.size());
                                in = in.mid(used);
                                at += used;
                        }
                }
                if (ended && p.end().isError()) return out + "<cut short>";
                return out;
        }

        std::string body(const std::string &response, size_t step = 0) {
                Http::ResponseParser p;
                return body(response, step, p);
        }

} // namespace

TEST_CASE("HTTP: URLs parsed") {
        Http::Url u;
        REQUIRE(Http::parseUrl("https://github.com/owner/repo/releases/download/v1/a.bmpk?x=1#frag", u).isOk());
        CHECK(u.tls);
        CHECK(str(u.host) == "github.com");
        CHECK(u.port == 443);
        CHECK(str(u.path) == "/owner/repo/releases/download/v1/a.bmpk?x=1");
        REQUIRE(Http::parseUrl("HTTP://10.0.0.2:8080", u).isOk());
        CHECK_FALSE(u.tls);
        CHECK(str(u.host) == "10.0.0.2");
        CHECK(u.port == 8080);
        CHECK(u.path.isEmpty());
        REQUIRE(Http::parseUrl("http://example.com?q", u).isOk());
        CHECK(str(u.path) == "?q");

        for (const char *bad :
             {"ftp://x/", "http://", "http:///x", "http://user@host/", "http://host:0/", "http://host:65536/",
              "http://ho st/", "http://host/a b", "http://[::1]/", "file:/x", "https://host:x/"}) {
                CHECK_MESSAGE(Http::parseUrl(bad, u).code() == Error::Invalid, bad);
        }
        CHECK(Http::parseUrl(("http://h/" + std::string(5000, 'a')).c_str(), u).code() == Error::Invalid);
}

TEST_CASE("HTTP: references resolved (RFC 3986 §5.4)") {
        const char *base = "http://a/b/c/d;p?q";
        CHECK(resolved(base, "g") == "http://a/b/c/g");
        CHECK(resolved(base, "./g") == "http://a/b/c/g");
        CHECK(resolved(base, "g/") == "http://a/b/c/g/");
        CHECK(resolved(base, "/g") == "http://a/g");
        CHECK(resolved(base, "//g") == "http://g");
        CHECK(resolved(base, "?y") == "http://a/b/c/d;p?y");
        CHECK(resolved(base, "g?y") == "http://a/b/c/g?y");
        CHECK(resolved(base, "#s") == "http://a/b/c/d;p?q");
        CHECK(resolved(base, "") == "http://a/b/c/d;p?q");
        CHECK(resolved(base, ".") == "http://a/b/c/");
        CHECK(resolved(base, "..") == "http://a/b/");
        CHECK(resolved(base, "../g") == "http://a/b/g");
        CHECK(resolved(base, "../..") == "http://a/");
        CHECK(resolved(base, "../../g") == "http://a/g");
        CHECK(resolved(base, "../../../g") == "http://a/g");
        CHECK(resolved(base, "/./g") == "http://a/g");
        CHECK(resolved(base, "g.") == "http://a/b/c/g.");
        CHECK(resolved(base, "./g/.") == "http://a/b/c/g/");
        CHECK(resolved(base, "g;x=1/../y") == "http://a/b/c/y");
        CHECK(resolved(base, "https://other:8443/x") == "https://other:8443/x");

        // What packages need: a list's File against its index, a redirect's Location.
        CHECK(resolved("https://o.github.io/bmds/packages/index.bmpi", "busybox_1.37.0-1.bmpk") ==
              "https://o.github.io/bmds/packages/busybox_1.37.0-1.bmpk");
        CHECK(resolved("http://host:8080/p/index.bmpi", "/dl/x.bmpk") == "http://host:8080/dl/x.bmpk");
        CHECK(resolved(base, "ftp://x/y") == "<Invalid>");
        CHECK(resolved("not a url", "g") == "<Invalid>");
}

TEST_CASE("HTTP: the request") {
        Http::Url    u;
        bmds::String out;
        REQUIRE(Http::parseUrl("https://example.com/a/b.bmpk", u).isOk());
        REQUIRE(Http::buildRequest(u, 0, "BMDS/1", out).isOk());
        CHECK(str(out.view()) == "GET /a/b.bmpk HTTP/1.1\r\nHost: example.com\r\nUser-Agent: BMDS/1\r\nAccept: */*\r\n"
                                 "Connection: close\r\n\r\n");
        REQUIRE(Http::parseUrl("http://example.com:8080?q", u).isOk());
        REQUIRE(Http::buildRequest(u, 1234, "BMDS/1", out).isOk());
        CHECK(str(out.view()) == "GET /?q HTTP/1.1\r\nHost: example.com:8080\r\nUser-Agent: BMDS/1\r\nAccept: */*\r\n"
                                 "Range: bytes=1234-\r\nConnection: close\r\n\r\n");
}

TEST_CASE("HTTP: responses read, whole and in pieces") {
        const std::string byLength = "HTTP/1.1 200 OK\r\nContent-Type: x\r\ncontent-length: 11\r\n\r\nhello world";
        const std::string chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n "
                                    "world\r\n0\r\nTrailer: x\r\n\r\n";
        const std::string toEnd = "HTTP/1.0 200 OK\nServer: old\n\nhello world";
        for (size_t step : {size_t(0), size_t(1), size_t(2), size_t(7)}) {
                CAPTURE(step);
                CHECK(body(byLength, step) == "hello world");
                CHECK(body(chunked, step) == "hello world");
                CHECK(body(toEnd, step) == "hello world");
        }

        Http::ResponseParser p;
        CHECK(body("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 206 Partial Content\r\nContent-Range: bytes 5-10/11\r\n"
                   "Content-Length: 6\r\n\r\n world",
                   3, p) == " world");
        CHECK(p.head().status == 206);
        CHECK(p.head().rangeStart == 5);
        CHECK(p.head().rangeTotal == 11);
        CHECK(p.head().length == 6);

        Http::ResponseParser r;
        CHECK(body("HTTP/1.1 302 Found\r\nLocation: https://objects.example.com/x?sig=abc\r\nContent-Length: 0\r\n\r\n",
                   0, r)
                      .empty());
        CHECK(r.head().status == 302);
        CHECK(str(r.head().location.view()) == "https://objects.example.com/x?sig=abc");
        CHECK(body("HTTP/1.1 204 No Content\r\n\r\n").empty());
        Http::ResponseParser whole;
        CHECK(body("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */11\r\nContent-Length: 0\r\n\r\n", 0,
                   whole)
                      .empty());
        CHECK(whole.head().rangeTotal == 11);

        // What follows a response whole isn't taken.
        Http::ResponseParser after;
        const std::string    two = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiEXTRA";
        CHECK(body(two, 0, after) == "hi");
}

TEST_CASE("HTTP: responses refused, or cut short") {
        CHECK(body("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello") == "hello<cut short>");
        CHECK(body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n") == "hello<cut short>");
        CHECK(body("HTTP/1.1 200 OK\r\nContent-Length: 1") == "<cut short>");
        CHECK(body("SSH-2.0-dropbear\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 2000 OK\r\n\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nno colon\r\n\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nX: 1\r\n folded\r\n\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n") == "<NotSupported>");
        CHECK(body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiX\r\n") == "<CorruptData>");
        CHECK(body("HTTP/1.1 200 OK\r\nX: " + std::string(Http::MaxLineBytes, 'x') + "\r\n\r\n") == "<CorruptData>");
        std::string many = "HTTP/1.1 200 OK\r\n";
        while (many.size() < Http::MaxHeadBytes) many += "X-Header: " + std::string(1000, 'y') + "\r\n";
        CHECK(body(many + "\r\n") == "<CorruptData>");
}

TEST_CASE("HTTP: requests with a method, headers and a body (libbmdshttp's)") {
        Http::Url url;
        REQUIRE(Http::parseUrl("https://api.example:8443/v1/things?x=1", url).isOk());
        Http::Request r;
        r.url = &url;
        r.method = "POST";
        r.userAgent = "test/1";
        r.headers = "Accept: application/json\r\nX-Token: a b\tc\r\n";
        r.hasBody = true;
        r.bodyLength = 17;
        bmds::String out;
        REQUIRE(Http::buildRequest(r, out).isOk());
        CHECK(str(out.view()) == "POST /v1/things?x=1 HTTP/1.1\r\nHost: api.example:8443\r\nUser-Agent: test/1\r\n"
                                 "Content-Length: 17\r\nAccept: application/json\r\nX-Token: a b\tc\r\n"
                                 "Connection: close\r\n\r\n");
        // GET with a range: as the fetcher's, with the Accept that takes anything.
        Http::Request g;
        g.url = &url;
        g.from = 100;
        REQUIRE(Http::buildRequest(g, out).isOk());
        CHECK(str(out.view()).find("GET /v1/things?x=1 HTTP/1.1\r\n") == 0);
        CHECK(str(out.view()).find("Accept: */*\r\nRange: bytes=100-\r\n") != std::string::npos);
        // Refused: methods, lines not well formed, control characters, the headers it writes.
        const char *const bad[] = {"X: y",
                                   "X: y\r\nZ",
                                   ": y\r\n",
                                   "X y\r\n",
                                   "X: a\rb\r\n",
                                   "X: a\nb\r\n",
                                   "X\x01: y\r\n",
                                   "Host: other\r\n",
                                   "content-length: 3\r\n",
                                   "Connection: keep-alive\r\n",
                                   "Range: bytes=0-\r\n",
                                   "User-Agent: x\r\n",
                                   "Transfer-Encoding: chunked\r\n"};
        for (const char *h : bad) {
                Http::Request b;
                b.url = &url;
                b.headers = h;
                CHECK_MESSAGE(Http::buildRequest(b, out) == Error::Invalid, h);
        }
        for (const char *m : {"", "get", "GE T", "AVERYLONGMETHODNAME"}) {
                Http::Request b;
                b.url = &url;
                b.method = m;
                CHECK_MESSAGE(Http::buildRequest(b, out) == Error::Invalid, m);
        }
        Http::Request none;
        CHECK(Http::buildRequest(none, out) == Error::Invalid);
}

TEST_CASE("HTTP: HEAD's answer has no body; every header kept") {
        const std::string response = "HTTP/1.1 100 Continue\r\nX-Interim: 1\r\n\r\n"
                                     "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nContent-Type:  text/plain \r\n"
                                     "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n";
        for (size_t step : {size_t(0), size_t(1), size_t(7)}) {
                Http::ResponseParser p(Http::ResponseParser::HeadRequest | Http::ResponseParser::KeepHeaders);
                CHECK(body(response, step, p) == "");
                CHECK(p.isDone());
                CHECK(p.head().status == 200);
                CHECK(p.head().length == 12);
                CHECK(str(p.headers()) ==
                      "Content-Length: 12\nContent-Type: text/plain\nSet-Cookie: a=1\nSet-Cookie: b=2\n");
        }
        // Without KeepHeaders nothing is kept; a GET's answer has its body.
        Http::ResponseParser plain;
        CHECK(body("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", 0, plain) == "abc");
        CHECK(plain.headers().isEmpty());
}

TEST_CASE("HTTP: what follows a head (redirects, ranges)") {
        auto head = [](int status) {
                Http::Head h;
                h.status = status;
                return h;
        };
        Http::Next n;
        // Redirects: resolved against the URL; 303 a GET (HEAD stays HEAD); 301/302 a GET after POST.
        Http::Head r = head(302);
        (void)r.location.append("/elsewhere?q");
        REQUIRE(Http::next(r, "https://a.example/x/y", "POST", 0, true, n).isOk());
        CHECK(n.kind == Http::Next::Kind::Redirect);
        CHECK(str(n.url.view()) == "https://a.example/elsewhere?q");
        CHECK(n.asGet);
        REQUIRE(Http::next(r, "https://a.example/x/y", "PUT", 0, true, n).isOk());
        CHECK(!n.asGet);
        r.status = 307;
        REQUIRE(Http::next(r, "https://a.example/x/y", "POST", 0, true, n).isOk());
        CHECK(!n.asGet);
        r.status = 303;
        REQUIRE(Http::next(r, "https://a.example/x/y", "HEAD", 0, true, n).isOk());
        CHECK(!n.asGet);
        REQUIRE(Http::next(r, "https://a.example/x/y", "DELETE", 0, true, n).isOk());
        CHECK(n.asGet);
        // Not followed: a body like any other.
        REQUIRE(Http::next(r, "https://a.example/x/y", "GET", 0, false, n).isOk());
        CHECK(n.kind == Http::Next::Kind::Body);
        CHECK(Http::next(head(301), "https://a.example/", "GET", 0, true, n) == Error::IoError);
        Http::Head odd = head(308);
        (void)odd.location.append("ftp://b.example/");
        CHECK(Http::next(odd, "https://a.example/", "GET", 0, true, n) == Error::Invalid);

        // Ranges: a 206 from the byte asked; a 200 that ignored it starts at 0; a 416 from the end.
        Http::Head part = head(206);
        part.rangeStart = 1000;
        part.rangeTotal = 5000;
        REQUIRE(Http::next(part, "http://a.example/f", "GET", 1000, true, n).isOk());
        CHECK((n.kind == Http::Next::Kind::Body && n.start == 1000 && n.total == 5000));
        CHECK(Http::next(part, "http://a.example/f", "GET", 999, true, n) == Error::CorruptData);
        Http::Head all = head(200);
        all.length = 5000;
        REQUIRE(Http::next(all, "http://a.example/f", "GET", 1000, true, n).isOk());
        CHECK((n.kind == Http::Next::Kind::Body && n.start == 0 && n.total == 5000));
        all.chunked = true;
        REQUIRE(Http::next(all, "http://a.example/f", "GET", 0, true, n).isOk());
        CHECK(n.total == Http::Unknown);
        Http::Head past = head(416);
        past.rangeTotal = 5000;
        REQUIRE(Http::next(past, "http://a.example/f", "GET", 5000, true, n).isOk());
        CHECK((n.kind == Http::Next::Kind::Whole && n.start == 5000 && n.total == 5000));
        REQUIRE(Http::next(past, "http://a.example/f", "GET", 10, true, n).isOk());
        CHECK(n.kind == Http::Next::Kind::Body); // a 416 not at the end: the caller's to judge
        // Any other status: a body for the caller.
        Http::Head missing = head(404);
        REQUIRE(Http::next(missing, "http://a.example/f", "GET", 0, true, n).isOk());
        CHECK(n.kind == Http::Next::Kind::Body);
}
