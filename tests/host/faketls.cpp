/**
 * @file      faketls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The host's stand-in for `TlsSession` (mbedTLS is the firmware's) and TlsService's settings:
 * a toy protocol with TLS's shape, so the tests exercise the socket layer, the tls thread and
 * readiness over lwIP without cryptography.
 *
 * - The handshake: the client sends "HI"; the server answers "OK" (open), "NO" (an untrusted
 *   certificate: `KeyRejected`, or open with `NoVerify`) or anything else (`ProtocolError`).
 * - Records: a length byte (1-255) and that many bytes; a record starting "!BAD" fails its
 *   check (`BadMessage`). A length of 0 is close_notify.
 * - The record being read is asked for as mbedTLS asks: the header, then exactly the rest.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/socket.h>
#include <bmds/tlsservice.h>
#include <bmds/tlssession.h>

#include "faketls.h"
#include "tlssettings.h"

BMDS_NAMESPACE_BEGIN

namespace FakeTls {

        TlsService::Settings settings;

} // namespace FakeTls

namespace TlsService {

        Error declareSettings() {
                return Error::Ok;
        }

        Settings settings() {
                return FakeTls::settings;
        }

        Error systemRoots(String &out, String &why) {
                out.clear();
                why.clear();
                return Error::Ok;
        }

} // namespace TlsService

struct TlsSession::Impl {
                enum class Phase : uint8_t {
                        Hello,
                        Reply,
                        Open
                };

                TlsSession session;
                Socket    *socket = nullptr;
                Phase      phase = Phase::Hello;
                uint8_t    out[256] = {}; // a record being sent
                size_t     outLength = 0;
                size_t     outSent = 0;
                char       reply[2] = {};
                size_t     replyHave = 0;
                bool       headerRead = false;
                uint8_t    record[255] = {};
                size_t     recordLength = 0;
                size_t     recordHave = 0;
                size_t     plainLength = 0; // the last record's payload, delivered from plainAt
                size_t     plainAt = 0;
                bool       ended = false;
                char       alpn[MaxAlpnName + 1] = {};

                uint32_t ioFlags(uint32_t flags) const { return (flags & Socket::DontWait) | Socket::Underlying; }

                // The record in `out`, what is left of it. `WouldBlock`: some is still left.
                Error flush(uint32_t flags) {
                        while (outSent < outLength) {
                                const ByteSpan piece(out + outSent, outLength - outSent);
                                auto [n, err] = socket->send(Span<const ByteSpan>(&piece, 1), nullptr, ioFlags(flags));
                                if (err.isError()) return err;
                                outSent += n;
                        }
                        return Error::Ok;
                }

                // Up to @p n bytes into @p into. `EndOfFile` at the end.
                Result<size_t> take(uint8_t *into, size_t n, uint32_t flags) {
                        const WritableByteSpan piece(into, n);
                        auto [r, err] = socket->receive(Span<const WritableByteSpan>(&piece, 1), ioFlags(flags));
                        if (err.isError()) {
                                if (err == Error::WouldBlock) session._bytesWanted = n;
                                return Result<size_t>(size_t(0), err);
                        }
                        return Result<size_t>(r.bytes, Error::Ok);
                }
};

Result<TlsSession *> TlsSession::create(Socket &socket, const Config &config, String &why) {
        why.clear();
        const bool noVerify = (config.flags & NoVerify) != 0;
        if ((config.flags & ~uint32_t(NoVerify)) != 0 || (config.serverName.isEmpty() && !noVerify) ||
            config.cert.isEmpty() != config.key.isEmpty())
                return Result<TlsSession *>(nullptr, Error::Invalid);
        void *block = Memory::alloc(MemSpace::Psram, sizeof(Impl));
        if (block == nullptr) return Result<TlsSession *>(nullptr, Error::NoMem);
        Impl *m = new (block) Impl();
        m->session._impl = m;
        m->session._flags = config.flags;
        m->socket = &socket;
        (void)m->session._name.append(config.serverName);
        // The first protocol offered is the one "chosen".
        size_t n = 0;
        while (n < config.alpn.size() && config.alpn.data()[n] != ',' && n < MaxAlpnName) ++n;
        std::memcpy(m->alpn, config.alpn.data(), n);
        return Result<TlsSession *>(&m->session, Error::Ok);
}

void TlsSession::destroy(TlsSession *session) {
        if (session == nullptr) return;
        Impl *m = session->_impl;
        m->~Impl();
        Memory::free(m);
        return;
}

Error TlsSession::fail(Error err) {
        _state = State::Failed;
        _failure = err;
        _want = Want::Nothing;
        return err;
}

Error TlsSession::ioError(int ret) const {
        (void)ret;
        return Error::ProtocolError;
}

void TlsSession::abandon(Error err, const char *reason) {
        if (_state != State::Handshaking) return;
        _why.clear();
        (void)_why.appendf("%s: %s", _name.cStr(), reason);
        (void)fail(err);
        return;
}

Error TlsSession::handshake(uint32_t flags) {
        if (_state == State::Open) return Error::Ok;
        if (_state == State::Failed) return _failure;
        Impl &m = *_impl;
        _want = Want::Nothing;
        for (;;) {
                switch (m.phase) {
                        case Impl::Phase::Hello: {
                                if (m.outLength == 0) {
                                        std::memcpy(m.out, "HI", 2);
                                        m.outLength = 2;
                                        m.outSent = 0;
                                }
                                const Error err = m.flush(flags);
                                if (err == Error::WouldBlock) {
                                        _want = Want::Write;
                                        return err;
                                }
                                if (err.isError()) return fail(err);
                                m.outLength = 0;
                                m.phase = Impl::Phase::Reply;
                                break;
                        }
                        case Impl::Phase::Reply: {
                                auto [n, err] = m.take(reinterpret_cast<uint8_t *>(m.reply) + m.replyHave,
                                                       2 - m.replyHave, flags);
                                if (err == Error::WouldBlock) {
                                        _want = Want::Read;
                                        return err;
                                }
                                if (err == Error::EndOfFile) {
                                        _why.clear();
                                        (void)_why.appendf("%s: closed in the handshake", _name.cStr());
                                        return fail(Error::ProtocolError);
                                }
                                if (err.isError()) return fail(err);
                                m.replyHave += n;
                                if (m.replyHave < 2) break;
                                _why.clear();
                                if (std::memcmp(m.reply, "OK", 2) == 0 || std::memcmp(m.reply, "NO", 2) == 0) {
                                        _peer.isKnown = true;
                                        (void)std::snprintf(_peer.subject, sizeof(_peer.subject), "CN=%s",
                                                            _name.cStr());
                                        (void)std::snprintf(_peer.issuer, sizeof(_peer.issuer), "CN=Fake CA");
                                        _peer.sha256[0] = 0xfa;
                                        _peer.notAfter = 2000000000;
                                }
                                if (std::memcmp(m.reply, "NO", 2) == 0) {
                                        _verify = VerifyUntrusted;
                                        if ((_flags & NoVerify) == 0) {
                                                (void)_why.appendf("%s: its certificate isn't from an authority this "
                                                                   "system trusts",
                                                                   _name.cStr());
                                                return fail(Error::KeyRejected);
                                        }
                                } else if (std::memcmp(m.reply, "OK", 2) != 0) {
                                        (void)_why.appendf("%s: the TLS handshake failed", _name.cStr());
                                        return fail(Error::ProtocolError);
                                }
                                m.phase = Impl::Phase::Open;
                                _state = State::Open;
                                _bytesWanted = 1;
                                return Error::Ok;
                        }
                        case Impl::Phase::Open: return Error::Ok;
                }
        }
}

Result<size_t> TlsSession::send(ByteSpan data, uint32_t flags) {
        if (Error err = handshake(flags); err.isError()) return Result<size_t>(size_t(0), err);
        Impl &m = *_impl;
        _want = Want::Nothing;
        if (_flushPending) {
                const Error err = m.flush(flags);
                if (err == Error::WouldBlock) {
                        _want = Want::Write;
                        return Result<size_t>(size_t(0), err);
                }
                if (err.isError()) return Result<size_t>(size_t(0), err);
                _flushPending = false;
        }
        if (data.isEmpty()) return Result<size_t>(size_t(0), Error::Ok);
        const size_t n = data.size() < 255 ? data.size() : 255;
        m.out[0] = uint8_t(n);
        std::memcpy(m.out + 1, data.data(), n);
        m.outLength = n + 1;
        m.outSent = 0;
        const Error err = m.flush(flags);
        if (err == Error::WouldBlock) {
                _flushPending = true;
                _want = Want::Write;
                return Result<size_t>(n, Error::Ok);
        }
        if (err.isError()) return Result<size_t>(size_t(0), err);
        return Result<size_t>(n, Error::Ok);
}

Result<size_t> TlsSession::receive(WritableByteSpan into, uint32_t flags) {
        if (Error err = handshake(flags); err.isError()) return Result<size_t>(size_t(0), err);
        Impl &m = *_impl;
        _want = Want::Nothing;
        for (;;) {
                if (m.plainAt < m.plainLength) {
                        size_t n = m.plainLength - m.plainAt;
                        if (n > into.size()) n = into.size();
                        std::memcpy(into.data(), m.record + m.plainAt, n);
                        m.plainAt += n;
                        return Result<size_t>(n, Error::Ok);
                }
                if (m.ended) return Result<size_t>(size_t(0), Error::Ok);
                if (!m.headerRead) {
                        uint8_t length = 0;
                        auto [n, err] = m.take(&length, 1, flags);
                        if (err == Error::EndOfFile) {
                                m.ended = true;
                                continue;
                        }
                        if (err == Error::WouldBlock) {
                                _want = Want::Read;
                                return Result<size_t>(size_t(0), err);
                        }
                        if (err.isError()) return Result<size_t>(size_t(0), err);
                        (void)n;
                        if (length == 0) {
                                _cleanClose = true;
                                m.ended = true;
                                continue;
                        }
                        m.headerRead = true;
                        m.recordLength = length;
                        m.recordHave = 0;
                }
                auto [n, err] = m.take(m.record + m.recordHave, m.recordLength - m.recordHave, flags);
                if (err == Error::EndOfFile) {
                        m.ended = true;
                        m.headerRead = false;
                        continue;
                }
                if (err == Error::WouldBlock) {
                        _want = Want::Read;
                        return Result<size_t>(size_t(0), err);
                }
                if (err.isError()) return Result<size_t>(size_t(0), err);
                m.recordHave += n;
                if (m.recordHave < m.recordLength) continue;
                m.headerRead = false;
                _bytesWanted = 1;
                if (m.recordLength >= 4 && std::memcmp(m.record, "!BAD", 4) == 0)
                        return Result<size_t>(size_t(0), fail(Error::BadMessage));
                m.plainLength = m.recordLength;
                m.plainAt = 0;
        }
}

void TlsSession::closeNotify() {
        if (_state != State::Open || _flushPending) return;
        Impl &m = *_impl;
        m.out[0] = 0;
        m.outLength = 1;
        m.outSent = 0;
        (void)m.flush(Socket::DontWait);
        return;
}

size_t TlsSession::pendingBytes() const {
        return _impl->plainLength - _impl->plainAt;
}

uint16_t TlsSession::version() const {
        return _state == State::Open ? 0x0303 : 0;
}

uint16_t TlsSession::cipher() const {
        return _state == State::Open ? 0xfffe : 0;
}

const char *TlsSession::cipherName() const {
        return _state == State::Open ? "FAKE" : "";
}

void TlsSession::info(Info &out) const {
        out = Info{};
        out.state = _state;
        out.failure = _failure;
        out.verify = _verify;
        out.version = version();
        out.cipher = cipher();
        (void)std::snprintf(out.cipherName, sizeof(out.cipherName), "%s", cipherName());
        if (_state == State::Open) (void)std::snprintf(out.alpn, sizeof(out.alpn), "%s", _impl->alpn);
        out.peer = _peer;
        out.cleanClose = _cleanClose;
        out.ended = _impl->ended;
        return;
}

BMDS_NAMESPACE_END
