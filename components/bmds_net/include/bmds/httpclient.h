/**
 * @file      httpclient.h
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
#include <bmds/http.h>
#include <bmds/result.h>
#include <bmds/socket.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace Http {

        /**
         * @brief A connection's bytes through TLS (`openTls()`): what `get()` reads and writes
         *        instead of the socket's.
         */
        struct Tls {
                        void *state = nullptr;
                        /** @brief Sends all of @p data. @return Bytes sent, or the error. */
                        Result<size_t> (*send)(void *state, ByteSpan data) = nullptr;
                        /** @brief Receives into @p into. @return Bytes (0 at the end), or the error. */
                        Result<size_t> (*receive)(void *state, WritableByteSpan into) = nullptr;
                        /** @brief Ends the session and frees its state. */
                        void (*close)(void *state) = nullptr;
        };

        /** @brief What `get()` needs from where it runs: the firmware's network, or a test's. */
        struct Environment {
                        /** @brief Finds @p host's address (network order): `NetStack::resolve()`. */
                        Error (*resolve)(StringView host, uint32_t &ipv4, unsigned int timeoutMs) = nullptr;
                        /** @brief Readies a socket before it connects (a test binds its interface); may be null. */
                        void (*prepare)(Socket &socket) = nullptr;
                        /** @brief Opens TLS over @p socket for @p host (`openTls()`); null: no `https:`. */
                        Error (*openTls)(Socket &socket, StringView host, String &why, Tls &out) = nullptr;
                        StringView   userAgent = "BMDS";
                        unsigned int timeoutMs = 30000; ///< To connect, and for each read.
        };

        /** @brief Where a body goes. */
        struct Sink {
                        /**
                         * @brief The body starts at byte @p start of a file of @p total bytes
                         *        (`Unknown` if not said), before any of it: 0 when a server ignored a
                         *        `Range` and sends it all again. @return `Ok`, or an error to stop.
                         */
                        Error (*begin)(void *arg, uint64_t start, uint64_t total) = nullptr;
                        /** @brief The body's next bytes. @return `Ok`, or an error to stop. */
                        Error (*body)(void *arg, ByteSpan bytes) = nullptr;
                        void *arg = nullptr;
        };

        /** @brief What `get()` fetched. */
        struct Fetched {
                        int      status = 0;      ///< 200 or 206.
                        String   url;             ///< After redirects.
                        uint64_t start = 0;       ///< Where the body started.
                        uint64_t total = Unknown; ///< The whole file's size, if said.
        };

        /** @brief The most redirects followed. */
        constexpr unsigned MaxRedirects = 5;

        /**
         * @brief Fetches @p url from byte @p from (a `Range`; 0: all), following redirects, its body
         *        to @p sink. One request a connection.
         * @return `Ok` (a 200, or a 206 starting at @p from; a 416 when @p from is the whole file's
         *         size, its body empty); `NotExist` (no such host, or 404 and 410); `Invalid` (not a
         *         URL); `NotSupported` (`https:` without TLS); `Timeout`; `Interrupted`;
         *         `CorruptData` (not HTTP, or cut short); `PermissionDenied` (a certificate refused);
         *         `IoError` (another status, too many redirects); the network's errors. @p why says
         *         what, with the URL.
         * @blocking
         */
        Error get(StringView url, uint64_t from, const Environment &environment, const Sink &sink, Fetched &out,
                  String &why);

        /**
         * @brief TLS over a connected @p socket for @p host (the firmware's, `httptls.cpp`): mbedTLS,
         *        TLS 1.2, the server's certificate checked against IDF's bundle and @p host, the
         *        handshake done. Refused while the clock isn't set.
         * @return `Ok`; `NotReady` (the clock); `PermissionDenied` (the certificate); `CorruptData`
         *         (the handshake); `Timeout`; `Interrupted`; `NoMem`.
         * @blocking
         */
        Error openTls(Socket &socket, StringView host, String &why, Tls &out);

} // namespace Http

BMDS_NAMESPACE_END
