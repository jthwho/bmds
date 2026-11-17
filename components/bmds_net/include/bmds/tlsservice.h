/**
 * @file      tlsservice.h
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
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

class Socket;

/**
 * @brief The TLS sockets' handshakes (devplan/network.md §35c): a kernel thread, `tls`, that steps
 *        every handshaking socket's session as its socket allows, so a handshake (0.3-1.2 s of
 *        CPU) never runs in a program's non-blocking call, and one slow server holds up nobody
 *        else's. Started at the first handshake; it ends 30 s after the last.
 *
 * It also sends the rest of a record a non-blocking write left partly sent, as the kernel would
 * push buffered bytes. It waits on a semaphore of its own in each such socket's readiness list,
 * so an arrival or room to send wakes it; a handshake past its time (`Settings::handshakeTimeoutMs`) fails with
 * `Timeout`. When one ends, the socket's waiters are woken.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace TlsService {

        /** @brief The settings (`net/tls/...`), read at each start. */
        struct Settings {
                        size_t       maxSessions = 8;            ///< `net/tls/sessions`: at most this many TLS sockets.
                        unsigned int handshakeTimeoutMs = 30000; ///< `net/tls/handshake-timeout` (seconds there).
        };

        /** @brief How many sessions there are now, and since boot. */
        struct Counts {
                        uint32_t sessions = 0; ///< Open now (handshaking or not).
                        uint32_t handshaking = 0;
                        uint32_t handshakes = 0; ///< Finished well since boot.
                        uint32_t failures = 0;   ///< Failed since boot.
                        uint64_t bytesIn = 0;    ///< Plaintext received since boot.
                        uint64_t bytesOut = 0;   ///< Plaintext sent since boot.
        };

        /**
         * @brief Declares the settings and the statistics (`net.tls`). The firmware's
         *        (tlssettings.cpp); the host tests' stands in.
         * @return `Ok`, or what failed.
         */
        Error init();

        /** @brief Returns the settings now (the firmware's read sysconf). */
        Settings settings();

        /**
         * @brief Reads the system's extra trusted roots (`net/tls/ca`, PEM files) into @p out, one
         *        after the other; nothing when the setting is empty.
         * @return `Ok`, or the file's error (@p why says which file).
         */
        Error systemRoots(String &out, String &why);

        /** @brief The folder of the board's own certificate and key (`net/tls/server-keys`). */
        Error serverKeysFolder(String &out);

        /**
         * @brief Counts a session for @p socket and steps its handshake from now on.
         * @return `Ok`, `Full` (`maxSessions` reached), `NoMem` (no thread).
         */
        Error add(Socket &socket);

        /**
         * @brief Sends the rest of @p socket's record once it has room (a non-blocking write
         *        left it partly sent); nothing if it is already being stepped.
         */
        void flushLater(Socket &socket);

        /**
         * @brief Stops stepping @p socket's handshake, if it is, and waits for a step in progress
         *        to end; then the socket may free its session.
         * @blocking
         */
        void remove(Socket &socket);

        /** @brief Counts what a session sent (@p out) and received (@p in). */
        void count(uint64_t in, uint64_t out);

        /** @brief Returns the counts. */
        Counts counts();

} // namespace TlsService

BMDS_NAMESPACE_END
