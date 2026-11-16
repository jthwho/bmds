/**
 * @file      http.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief HTTP/1.1 for a client (devplan/packages.md §23, network.md §35d): URLs, the request, the
 *        response read as it arrives, and what follows a response's head (a redirect, a range).
 *        No connection here: `bmds_net`'s `Http::get()` has the kernel's, and the SDK's libbmdshttp
 *        (bmds/httplib.h) a program's, both on this one parser.
 *
 * One request a connection (`Connection: close`); any method, a body by `Content-Length`; a
 * `Range` to resume; a response's body by `Content-Length`, by chunks, or to the connection's end.
 *
 * @par Thread Safety
 * The functions are pure; a `ResponseParser` is used by one thread at a time.
 */
namespace Http {

        /** @brief The longest line of a response's head, and the longest head. */
        constexpr size_t MaxLineBytes = 16 * 1024;
        constexpr size_t MaxHeadBytes = 64 * 1024; ///< See `MaxLineBytes`.

        /** @brief The longest URL taken. */
        constexpr size_t MaxUrlBytes = 4096;

        /** @brief An `http:` or `https:` URL, parsed. Views into its text. */
        struct Url {
                        bool       tls = false; ///< `https:`.
                        StringView host;        ///< A name or a dotted quad (no IPv6 literal yet).
                        uint16_t   port = 0;    ///< 80 or 443 when it names none.
                        StringView path;        ///< From its `/`, with the query; never empty.
        };

        /**
         * @brief Parses @p text: `http://` or `https://` (any case), a host of letters, digits, `.`
         *        and `-`, an optional `:PORT` (1-65535), then the path (`/` when none; a fragment
         *        dropped). No user information. At most `MaxUrlBytes`, printable ASCII.
         * @return `Ok`, or `Invalid`.
         */
        Error parseUrl(StringView text, Url &out);

        /**
         * @brief Resolves @p reference against the absolute URL @p base (RFC 3986 §5.2): an absolute
         *        URL as it is, `//HOST...` with the base's scheme, `/PATH` on the base's host, a
         *        relative path against the base's folder (`.` and `..` removed), `?QUERY` on the
         *        base's path. @return The URL (it parses), or `Invalid`.
         */
        Result<String> resolve(StringView base, StringView reference);

        /**
         * @brief Writes the request for @p url to @p out: `GET`, `Host` (with a port that isn't the
         *        scheme's), @p userAgent, `Range: bytes=FROM-` when @p from isn't 0, `Accept` (anything),
         *        `Connection: close`. @return `Ok` or `NoMem`.
         */
        Error buildRequest(const Url &url, uint64_t from, StringView userAgent, String &out);

        /** @brief A request's parts, for `buildRequest()`. */
        struct Request {
                        StringView method = "GET"; ///< 1-16 capital letters.
                        const Url *url = nullptr;
                        StringView userAgent = "BMDS";
                        uint64_t   from = 0; ///< `Range: bytes=FROM-` when not 0.
                        /**
                         * More header lines, each `Name: value\r\n`. Not the ones this writes
                         * (`Host`, `User-Agent`, `Range`, `Content-Length`, `Transfer-Encoding`,
                         * `Connection`); an `Accept` here replaces the one that takes anything.
                         */
                        StringView headers;
                        bool       hasBody = false; ///< `Content-Length: bodyLength` (the body is sent after).
                        uint64_t   bodyLength = 0;
        };

        /**
         * @brief Writes @p request's head to @p out (the body, if any, goes after it as it is).
         * @return `Ok`, `Invalid` (the method, a header line: one not well formed, with a control
         *         character, or one this writes), `NoMem`.
         */
        Error buildRequest(const Request &request, String &out);

        /** @brief A size not known. */
        constexpr uint64_t Unknown = UINT64_MAX;

        /** @brief What a response's head said, as far as a fetch cares. */
        struct Head {
                        int      status = 0;
                        String   reason;               ///< The status line's words ("Not Found").
                        uint64_t length = Unknown;     ///< `Content-Length`.
                        bool     chunked = false;      ///< `Transfer-Encoding: chunked`.
                        String   location;             ///< `Location`.
                        uint64_t rangeStart = 0;       ///< `Content-Range`'s first byte (206).
                        uint64_t rangeTotal = Unknown; ///< `Content-Range`'s whole size (206, and 416's `*/SIZE`).
        };

        /**
         * @brief Reads a response as its bytes arrive: the head, then the body handed out as views of
         *        what was fed. Interim (1xx) responses are skipped. 204 and 304 have no body, nor does
         *        the answer to `HEAD` (`HeadRequest`).
         */
        class ResponseParser {
                public:
                        /** @brief The parser's options. */
                        enum Option : uint8_t {
                                HeadRequest = 1u << 0, ///< The request was `HEAD`: no body whatever the head says.
                                KeepHeaders = 1u << 1, ///< Every header line kept (`headers()`).
                        };

                        /** @brief A parser for one response, with `Option` bits. */
                        explicit ResponseParser(uint8_t options = 0);

                        /**
                         * @brief Takes what it can of @p in: @p used is how much; body bytes among
                         *        them, if any, are @p body (a view into @p in). Called again with
                         *        the rest until it is all used, or the response is done (`isDone()`:
                         *        what follows isn't this response's).
                         * @return `Ok`; `CorruptData` (not HTTP: a bad line, a length or chunk that
                         *         doesn't read, two lengths, a line or head too long); `NotSupported`
                         *         (a transfer coding other than chunked); `NoMem`.
                         */
                        Error feed(ByteSpan in, size_t &used, ByteSpan &body);

                        /**
                         * @brief The connection ended. @return `Ok` if the response is whole (a
                         *         body to the end ends here), else `CorruptData` (cut short).
                         */
                        Error end();

                        /** @brief Returns true once the head is read; `head()` then holds it. */
                        bool hasHead() const { return _state > State::Headers; }

                        /** @brief Returns true once the whole response is read. */
                        bool isDone() const { return _state == State::Done; }

                        /** @brief Returns the head (once `hasHead()`). */
                        const Head &head() const { return _head; }

                        /**
                         * @brief Returns the final response's header lines (`KeepHeaders`), each
                         *        `Name: value\n`, the value trimmed, in the order sent.
                         */
                        StringView headers() const { return _headers.view(); }

                private:
                        enum class State : uint8_t {
                                StatusLine,
                                Headers,
                                BodyLength,
                                ChunkSize,
                                ChunkData,
                                ChunkEnd,
                                Trailers,
                                BodyToEnd,
                                Done,
                        };

                        Error line(StringView text);
                        Error header(StringView text);
                        Error headDone();

                        State    _state = State::StatusLine;
                        Head     _head;
                        String   _line;
                        String   _headers; ///< `KeepHeaders`.
                        uint8_t  _options = 0;
                        size_t   _headBytes = 0;
                        uint64_t _left = 0; ///< Of the body or the chunk.
                        bool     _sawLength = false;
        };

        /** @brief Returns true for a status that redirects (301, 302, 303, 307, 308). */
        constexpr bool isRedirect(int status) {
                return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        }

        /** @brief What follows a response's head (`next()`). */
        struct Next {
                        enum class Kind : uint8_t {
                                Body,     ///< Its body is the answer (`start`, `total`).
                                Redirect, ///< Ask `url` instead.
                                Whole,    ///< A 416 to a `Range` from the end: nothing more to fetch.
                        };
                        Kind   kind = Kind::Body;
                        String url;           ///< `Redirect`: where.
                        bool   asGet = false; ///< `Redirect`: as `GET` without a body (303; 301 and 302 after `POST`).
                        uint64_t start = 0;   ///< `Body`: its first byte (a 206's; 0 when a `Range` was ignored).
                        uint64_t total = Unknown; ///< `Body`: the whole file's size, if said.
        };

        /**
         * @brief Decides what follows @p head, the answer to @p method on @p url asked from byte
         *        @p from (a `Range`; 0: all): the redirect resolved against @p url (when
         *        @p followRedirects), a 416 saying the file is whole, or the body and where it
         *        starts. Any other status is a `Body` for the caller to judge.
         * @return `Ok`; `IoError` (a redirect without `Location`); `Invalid` (a `Location` that
         *         doesn't resolve to an http: or https: URL); `CorruptData` (a 206 from another byte
         *         than asked); `NoMem`.
         */
        Error next(const Head &head, StringView url, StringView method, uint64_t from, bool followRedirects, Next &out);

} // namespace Http

BMDS_NAMESPACE_END
