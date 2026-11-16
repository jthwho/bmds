/**
 * @file      tlssession.h
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

class Socket;

/**
 * @brief A TLS client session over a connected TCP `Socket` (devplan/network.md §35): mbedTLS,
 *        TLS 1.2, the server's certificate checked against IDF's bundle, its dates and the server's
 *        name. The firmware's one TLS implementation: `pkg`'s fetching (`Http::openTls()`) and,
 *        from §35's step 2, programs' TLS sockets.
 *
 * The handshake can be stepped without waiting (`Socket::DontWait`): it then answers `WouldBlock`
 * and says what it waits for (`want()`, `bytesWanted()`), so a caller can wait on the socket's
 * readiness and step it again. The session and mbedTLS's buffers (16 KB in, 4 KB out) are in
 * `MemSpace::Psram`.
 *
 * @par Thread Safety
 * Not thread-safe: one caller at a time (step 2's socket holds a lock around each call).
 */
class TlsSession {
        public:
                /** @brief `Config::flags`. */
                enum Flag : uint32_t {
                        NoVerify = 1u << 0, ///< The chain, dates and name aren't checked (`verifyFlags()` still says).
                };

                /** @brief Why a certificate was refused (`verifyFlags()`), or what `NoVerify` let pass. */
                enum Verify : uint32_t {
                        VerifyExpired = 1u << 0,     ///< Past its end.
                        VerifyNotYetValid = 1u << 1, ///< Before its start.
                        VerifyName = 1u << 2,        ///< For another name.
                        VerifyUntrusted = 1u << 3,   ///< Not from an authority this system trusts.
                        VerifyClock = 1u << 4,       ///< The clock isn't set: dates can't be checked.
                        VerifyOther = 1u << 5,       ///< Another of mbedTLS's reasons.
                };

                /** @brief What a session is waiting for, after `WouldBlock`. */
                enum class Want : uint8_t {
                        Nothing, ///< Not waiting.
                        Read,    ///< `bytesWanted()` more bytes from the socket.
                        Write,   ///< Room in the socket's send buffer.
                };

                /** @brief How to make a session. */
                struct Config {
                                StringView serverName; ///< SNI and the name checked; empty only with `NoVerify`.
                                uint32_t   flags = 0;  ///< `Flag` bits.
                };

                /** @brief The server's own certificate, as seen in the handshake. */
                struct Peer {
                                uint8_t sha256[32] = {}; ///< Of its DER (IRC's CertFP fingerprint).
                                char    subject[128] = {};
                                char    issuer[128] = {};
                                int64_t notBefore = 0; ///< Unix seconds.
                                int64_t notAfter = 0;  ///< Unix seconds.
                                bool    isKnown = false;
                };

                /**
                 * @brief Makes a session over @p socket (which must outlive it). Nothing is sent
                 *        until `handshake()`.
                 * @return The session (`destroy()` it), or `Invalid` (no name without `NoVerify`),
                 *         `NotReady` (the clock isn't set and certificates are checked: @p why says
                 *         so, `VerifyClock`), `NoMem`.
                 */
                static Result<TlsSession *> create(Socket &socket, const Config &config, String &why);

                /** @brief Frees @p session (null: nothing), without a `close_notify`. */
                static void destroy(TlsSession *session);

                /**
                 * @brief Runs the handshake: to its end, or with `Socket::DontWait` as far as the
                 *        socket allows.
                 * @return `Ok` (done, or done before); `WouldBlock` (`DontWait`: see `want()`);
                 *         `PermissionDenied` (the certificate: `verifyFlags()`); `CorruptData` (the
                 *         handshake: an alert, nothing in common); `Timeout` (the socket's);
                 *         `Interrupted`; `NoMem`; the socket's errors. A failure is kept: each call
                 *         after answers it again. `why()` says what, with the server's name.
                 * @blocking
                 */
                Error handshake(uint32_t flags);

                /**
                 * @brief Sends @p data, the handshake first if it isn't done.
                 * @param flags `Socket::DontWait`.
                 * @return Bytes taken (some of @p data), or `WouldBlock`, `Timeout`, `CorruptData`, the
                 *         socket's errors.
                 * @blocking
                 */
                Result<size_t> send(ByteSpan data, uint32_t flags);

                /**
                 * @brief Receives into @p into, the handshake first if it isn't done.
                 * @param flags `Socket::DontWait`.
                 * @return Bytes (0 at the end: the server's `close_notify`, or the connection's end
                 *         without one, `isCleanClose()` telling which), or `WouldBlock`, `Timeout`,
                 *         `CorruptData` (a record that failed its check), the socket's errors.
                 * @blocking
                 */
                Result<size_t> receive(WritableByteSpan into, uint32_t flags);

                /** @brief Sends `close_notify`, best effort (never waits for room). */
                void closeNotify();

                /** @brief Returns true once the handshake is done. */
                bool isOpen() const { return _state == State::Open; }

                /** @brief Returns what a `WouldBlock` waits for. */
                Want want() const { return _want; }

                /** @brief Returns the bytes the record being read still lacks (`Want::Read`). */
                size_t bytesWanted() const { return _bytesWanted; }

                /** @brief Returns `Verify` bits: why the certificate was refused, or what `NoVerify` let pass. */
                uint32_t verifyFlags() const { return _verify; }

                /** @brief Returns the server's certificate (`isKnown` once it was seen). */
                const Peer &peer() const { return _peer; }

                /** @brief Returns true if the server ended with `close_notify`. */
                bool isCleanClose() const { return _cleanClose; }

                /** @brief Returns the protocol's number (`0x0303` for TLS 1.2), 0 before the handshake. */
                uint16_t version() const;

                /** @brief Returns the cipher suite's IANA number, 0 before the handshake. */
                uint16_t cipher() const;

                /** @brief Returns the cipher suite's name ("" before the handshake). */
                const char *cipherName() const;

                /** @brief Returns what the last failure was, in words, with the server's name. */
                StringView why() const { return _why.view(); }

                /** @brief The state, for `Socket` (step 2). */
                enum class State : uint8_t {
                        Handshaking,
                        Open,
                        Failed
                };

        private:
                struct Impl;

                TlsSession() = default;
                ~TlsSession() = default;
                TlsSession(const TlsSession &) = delete;
                TlsSession &operator=(const TlsSession &) = delete;

                Error fail(Error err);
                Error ioError(int ret) const;

                Impl    *_impl = nullptr;
                String   _name; ///< The server's, for the messages.
                String   _why;
                Peer     _peer;
                Error    _failure;
                uint32_t _verify = 0;
                uint32_t _flags = 0;
                size_t   _bytesWanted = 0;
                State    _state = State::Handshaking;
                Want     _want = Want::Nothing;
                bool     _cleanClose = false;
};

BMDS_NAMESPACE_END
