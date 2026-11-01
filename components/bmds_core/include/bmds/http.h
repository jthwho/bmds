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
 * @brief HTTP/1.1 for a client that fetches (devplan/packages.md §23): URLs, the request, and the
 *        response read as it arrives. No connection here: `bmds_net`'s `Http::get()` has that.
 *
 * Only what fetching a file needs: `GET`, one request a connection (`Connection: close`), a
 * `Range` to resume, a body by `Content-Length`, by chunks, or to the connection's end.
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

        /** @brief A size not known. */
        constexpr uint64_t Unknown = UINT64_MAX;

        /** @brief What a response's head said, as far as a fetch cares. */
        struct Head {
                        int      status = 0;
                        uint64_t length = Unknown;     ///< `Content-Length`.
                        bool     chunked = false;      ///< `Transfer-Encoding: chunked`.
                        String   location;             ///< `Location`.
                        uint64_t rangeStart = 0;       ///< `Content-Range`'s first byte (206).
                        uint64_t rangeTotal = Unknown; ///< `Content-Range`'s whole size (206, and 416's `*/SIZE`).
        };

        /**
         * @brief Reads a response as its bytes arrive: the head, then the body handed out as views of
         *        what was fed. Interim (1xx) responses are skipped. A response to `GET`: 204 and 304
         *        have no body.
         */
        class ResponseParser {
                public:
                        ResponseParser();

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
                        size_t   _headBytes = 0;
                        uint64_t _left = 0; ///< Of the body or the chunk.
                        bool     _sawLength = false;
        };

} // namespace Http

BMDS_NAMESPACE_END
