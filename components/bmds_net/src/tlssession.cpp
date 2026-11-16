/**
 * @file      tlssession.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS client sessions (devplan/network.md §35): mbedTLS over a connected `Socket` (its BIO), the
 * server's certificate checked against IDF's bundle, its dates and the server's name. And
 * mbedTLS's allocations, through `Memory` in PSRAM (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/memory.h>
#include <bmds/socket.h>
#include <bmds/tlssession.h>

// The bundle's own check of a chain's first untrusted link against its roots
// (esp_crt_bundle.c; not in its header). Ours wraps it to see the server's certificate.
extern "C" int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);

extern "C" void *esp_mbedtls_mem_calloc(size_t n, size_t size) {
        if (size != 0 && n > SIZE_MAX / size) return nullptr;
        void *p = bmds::Memory::alloc(bmds::MemSpace::Psram, n * size);
        if (p != nullptr) std::memset(p, 0, n * size);
        return p;
}

extern "C" void esp_mbedtls_mem_free(void *ptr) {
        bmds::Memory::free(ptr);
        return;
}

BMDS_NAMESPACE_BEGIN

struct TlsSession::Impl {
                TlsSession          session;
                mbedtls_ssl_context ssl;
                mbedtls_ssl_config  conf;
                Socket             *socket = nullptr;
                uint32_t            ioFlags = 0; ///< `Socket::DontWait` while stepping without waiting.
                Error               ioError;     ///< What the socket said, when mbedTLS failed through it.

                static int bioSend(void *arg, const unsigned char *data, size_t bytes);
                static int bioReceive(void *arg, unsigned char *into, size_t bytes);
};

namespace {

        int random(void *, unsigned char *out, size_t bytes) {
                esp_fill_random(out, bytes);
                return 0;
        }


        // What `NoVerify` and a refusal report: mbedTLS's flags as ours.
        uint32_t verifyBits(uint32_t flags) {
                uint32_t out = 0;
                if (flags & MBEDTLS_X509_BADCERT_EXPIRED) out |= TlsSession::VerifyExpired;
                if (flags & MBEDTLS_X509_BADCERT_FUTURE) out |= TlsSession::VerifyNotYetValid;
                if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) out |= TlsSession::VerifyName;
                if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) out |= TlsSession::VerifyUntrusted;
                if (flags & ~uint32_t(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE |
                                      MBEDTLS_X509_BADCERT_CN_MISMATCH | MBEDTLS_X509_BADCERT_NOT_TRUSTED))
                        out |= TlsSession::VerifyOther;
                return out;
        }

        // A refused certificate, in words (the first reason of `verifyBits()`'s).
        const char *verifyWords(uint32_t bits) {
                if (bits & TlsSession::VerifyClock)
                        return "the clock isn't set yet (no time from the network): a certificate's dates are "
                               "checked against it";
                if (bits & TlsSession::VerifyExpired) return "its certificate has expired";
                if (bits & TlsSession::VerifyNotYetValid)
                        return "its certificate isn't valid yet (is the clock right?)";
                if (bits & TlsSession::VerifyName) return "its certificate is for another name";
                if (bits & TlsSession::VerifyUntrusted)
                        return "its certificate isn't from an authority this system trusts";
                return "its certificate was refused";
        }

        int64_t unixSeconds(const mbedtls_x509_time &t) {
                return Calendar::daysFromCivil(t.year, unsigned(t.mon), unsigned(t.day)) * 86400 + t.hour * 3600 +
                       t.min * 60 + t.sec;
        }

        // Each certificate of the chain, the server's last (depth 0): the bundle's check, its
        // refusal kept in the flags rather than ending the handshake (so `NoVerify` can let it pass,
        // and a refusal says why), and the server's certificate recorded.
        int verify(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
                (void)esp_crt_verify_callback(nullptr, crt, depth, flags);
                if (depth != 0) return 0;
                TlsSession::Peer &p = *static_cast<TlsSession::Peer *>(arg);
                (void)mbedtls_sha256(crt->raw.p, crt->raw.len, p.sha256, 0);
                if (mbedtls_x509_dn_gets(p.subject, sizeof(p.subject), &crt->subject) < 0) p.subject[0] = '\0';
                if (mbedtls_x509_dn_gets(p.issuer, sizeof(p.issuer), &crt->issuer) < 0) p.issuer[0] = '\0';
                p.notBefore = unixSeconds(crt->valid_from);
                p.notAfter = unixSeconds(crt->valid_to);
                p.isKnown = true;
                return 0;
        }

        // What is offered, best first (devplan/network.md §33): AES-GCM, its counter mode on the
        // engine and its GHASH bmds's (tlsgcm.h), 6.2 MB/s a 16 KB record; ChaCha20-Poly1305 in
        // software 3.7. Ephemeral keys only.
        constexpr int Suites[] = {
                MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
                MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
                MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
                MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                0,
        };

} // namespace

int TlsSession::Impl::bioSend(void *arg, const unsigned char *data, size_t bytes) {
        Impl          &m = *static_cast<Impl *>(arg);
        const ByteSpan piece(data, bytes);
        auto [n, err] = m.socket->send(Span<const ByteSpan>(&piece, 1), nullptr, m.ioFlags);
        if (err == Error::WouldBlock && (m.ioFlags & Socket::DontWait) != 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (err.isError()) {
                m.ioError = err == Error::WouldBlock ? Error::Timeout : err;
                return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        return int(n);
}

int TlsSession::Impl::bioReceive(void *arg, unsigned char *into, size_t bytes) {
        Impl                  &m = *static_cast<Impl *>(arg);
        const WritableByteSpan piece(into, bytes);
        auto [r, err] = m.socket->receive(Span<const WritableByteSpan>(&piece, 1), m.ioFlags);
        if (err == Error::EndOfFile) return 0;
        if (err == Error::WouldBlock && (m.ioFlags & Socket::DontWait) != 0) {
                // mbedTLS asks for exactly what the record still lacks: what a reader's readiness
                // must wait for (§35b), so half a record wakes nobody.
                m.session._bytesWanted = bytes;
                return MBEDTLS_ERR_SSL_WANT_READ;
        }
        if (err.isError()) {
                m.ioError = err == Error::WouldBlock ? Error::Timeout : err;
                return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return int(r.bytes);
}

Result<TlsSession *> TlsSession::create(Socket &socket, const Config &config, String &why) {
        why.clear();
        const bool noVerify = (config.flags & NoVerify) != 0;
        if ((config.flags & ~uint32_t(NoVerify)) != 0 || (config.serverName.isEmpty() && !noVerify))
                return Result<TlsSession *>(nullptr, Error::Invalid);
        if (!noVerify && !Clock::isSet()) {
                (void)why.appendf("%.*s: %s", int(config.serverName.size()), config.serverName.data(),
                                  verifyWords(VerifyClock));
                return Result<TlsSession *>(nullptr, Error::NotReady);
        }
        void *block = Memory::alloc(MemSpace::Psram, sizeof(Impl));
        if (block == nullptr) return Result<TlsSession *>(nullptr, Error::NoMem);
        Impl *m = new (block) Impl();
        m->session._impl = m;
        m->session._flags = config.flags;
        m->socket = &socket;
        if (m->session._name.append(config.serverName).isError()) {
                destroy(&m->session);
                return Result<TlsSession *>(nullptr, Error::NoMem);
        }
        mbedtls_ssl_init(&m->ssl);
        mbedtls_ssl_config_init(&m->conf);
        // The bundle's log says what `why` says, on the console in the middle of a command's output.
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);
        int ret = mbedtls_ssl_config_defaults(&m->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0) {
                // `NoVerify`: the chain is still checked, so `verifyFlags()` says what passed.
                mbedtls_ssl_conf_authmode(&m->conf,
                                          noVerify ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_rng(&m->conf, random, nullptr);
                ret = esp_crt_bundle_attach(&m->conf) == ESP_OK ? 0 : MBEDTLS_ERR_SSL_ALLOC_FAILED;
        }
        if (ret == 0) {
                mbedtls_ssl_conf_verify(&m->conf, verify, &m->session._peer);
                mbedtls_ssl_conf_ciphersuites(&m->conf, Suites);
                ret = mbedtls_ssl_setup(&m->ssl, &m->conf);
        }
        if (ret == 0 && !m->session._name.isEmpty()) ret = mbedtls_ssl_set_hostname(&m->ssl, m->session._name.cStr());
        if (ret != 0) {
                destroy(&m->session);
                return Result<TlsSession *>(nullptr, Error::NoMem);
        }
        mbedtls_ssl_set_bio(&m->ssl, m, Impl::bioSend, Impl::bioReceive, nullptr);
        return Result<TlsSession *>(&m->session, Error::Ok);
}

void TlsSession::destroy(TlsSession *session) {
        if (session == nullptr) return;
        Impl *m = session->_impl;
        mbedtls_ssl_free(&m->ssl);
        mbedtls_ssl_config_free(&m->conf);
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

// An mbedTLS failure as an `Error`: the socket's own when it came from there.
Error TlsSession::ioError(int ret) const {
        if (ret == MBEDTLS_ERR_NET_SEND_FAILED || ret == MBEDTLS_ERR_NET_RECV_FAILED) return _impl->ioError;
        if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED) return Error::NoMem;
        return Error::CorruptData;
}

Error TlsSession::handshake(uint32_t flags) {
        if (_state == State::Open) return Error::Ok;
        if (_state == State::Failed) return _failure;
        Impl &m = *_impl;
        m.ioFlags = flags & Socket::DontWait;
        _want = Want::Nothing;
        int ret;
        do {
                ret = mbedtls_ssl_handshake(&m.ssl);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        if (m.ioFlags == 0) continue; // mbedTLS's own retry, not the socket's
                        _want = ret == MBEDTLS_ERR_SSL_WANT_READ ? Want::Read : Want::Write;
                        return Error::WouldBlock;
                }
                break;
        } while (true);

        const char    *name = _name.isEmpty() ? "the server" : _name.cStr();
        const uint32_t flagsSeen = mbedtls_ssl_get_verify_result(&m.ssl);
        _verify = flagsSeen == uint32_t(-1) ? 0 : verifyBits(flagsSeen);
        // Only `NoVerify` gets this far without the clock: its dates were checked against 1970.
        if (_peer.isKnown && !Clock::isSet()) _verify |= VerifyClock;
        _why.clear();
        if (ret == 0) {
                if (_verify != 0 && (_flags & NoVerify) == 0) {
                        (void)_why.appendf("%s: %s", name, verifyWords(_verify));
                        return fail(Error::PermissionDenied);
                }
                _state = State::Open;
                return Error::Ok;
        }
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && _verify != 0) {
                (void)_why.appendf("%s: %s", name, verifyWords(_verify));
                return fail(Error::PermissionDenied);
        }
        const Error err = ioError(ret);
        if (err == Error::CorruptData)
                (void)_why.appendf("%s: the TLS handshake failed (mbedTLS -0x%04x)", name, unsigned(-ret));
        else
                (void)_why.appendf("%s: %s", name, err.desc());
        return fail(err);
}

Result<size_t> TlsSession::send(ByteSpan data, uint32_t flags) {
        if (Error err = handshake(flags); err.isError()) return Result<size_t>(size_t(0), err);
        Impl &m = *_impl;
        m.ioFlags = flags & Socket::DontWait;
        _want = Want::Nothing;
        for (;;) {
                const int ret = mbedtls_ssl_write(&m.ssl, data.data(), data.size());
                if (ret >= 0) return Result<size_t>(size_t(ret), Error::Ok);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        if (m.ioFlags == 0) continue;
                        _want = ret == MBEDTLS_ERR_SSL_WANT_READ ? Want::Read : Want::Write;
                        return Result<size_t>(size_t(0), Error::WouldBlock);
                }
                return Result<size_t>(size_t(0), ioError(ret));
        }
}

Result<size_t> TlsSession::receive(WritableByteSpan into, uint32_t flags) {
        if (Error err = handshake(flags); err.isError()) return Result<size_t>(size_t(0), err);
        Impl &m = *_impl;
        m.ioFlags = flags & Socket::DontWait;
        _want = Want::Nothing;
        for (;;) {
                const int ret = mbedtls_ssl_read(&m.ssl, into.data(), into.size());
                if (ret >= 0) return Result<size_t>(size_t(ret), Error::Ok);
                if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                        _cleanClose = true;
                        return Result<size_t>(size_t(0), Error::Ok);
                }
                // The connection's end without `close_notify` (servers do: a body that ends by
                // closing must have sent its length to be trusted).
                if (ret == MBEDTLS_ERR_SSL_CONN_EOF) return Result<size_t>(size_t(0), Error::Ok);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        if (m.ioFlags == 0) continue;
                        _want = ret == MBEDTLS_ERR_SSL_WANT_READ ? Want::Read : Want::Write;
                        return Result<size_t>(size_t(0), Error::WouldBlock);
                }
                return Result<size_t>(size_t(0), ioError(ret));
        }
}

void TlsSession::closeNotify() {
        if (_state != State::Open) return;
        _impl->ioFlags = Socket::DontWait;
        (void)mbedtls_ssl_close_notify(&_impl->ssl);
        return;
}

uint16_t TlsSession::version() const {
        return _state == State::Open ? uint16_t(mbedtls_ssl_get_version_number(&_impl->ssl)) : 0;
}

uint16_t TlsSession::cipher() const {
        return _state == State::Open ? uint16_t(mbedtls_ssl_get_ciphersuite_id_from_ssl(&_impl->ssl)) : 0;
}

const char *TlsSession::cipherName() const {
        if (_state != State::Open) return "";
        const char *name = mbedtls_ssl_get_ciphersuite(&_impl->ssl);
        return name != nullptr ? name : "";
}

BMDS_NAMESPACE_END
