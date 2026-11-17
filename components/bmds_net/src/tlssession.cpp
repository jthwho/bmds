/**
 * @file      tlssession.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS client sessions (devplan/network.md §35): mbedTLS over a connected `Socket` (its BIO, on the
 * socket's own bytes), the server's certificate checked against IDF's bundle and any roots given,
 * its dates and the server's name; a client certificate, ALPN. And mbedTLS's allocations, through
 * `Memory` in PSRAM (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/memory.h>
#include <bmds/socket.h>
#include <bmds/tlsserverkeys.h>
#include <bmds/tlssession.h>

// The bundle's own check of a chain's first untrusted link against its roots
// (esp_crt_bundle.c; not in its header). Ours wraps it to see the server's certificate.
extern "C" int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);

// IDF's certificate bundle as built in (esp_crt_bundle.c's format: a u32 offset a root, then each
// root's subject length and key length, u16, little-endian, and its DER subject and public key).
extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

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
                mbedtls_x509_crt    roots; ///< `Config::ca`.
                mbedtls_x509_crt    cert;  ///< `Config::cert`.
                mbedtls_pk_context  key;   ///< `Config::key`.
                char                alpnNames[MaxAlpn][MaxAlpnName + 1] = {};
                const char         *alpn[MaxAlpn + 1] = {};
                Socket             *socket = nullptr;
                uint32_t            ioFlags = 0; ///< `Socket::DontWait` while stepping without waiting.
                Error               ioError;     ///< What the socket said, when mbedTLS failed through it.
                bool                ended = false; ///< `receive()` gave the end.

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

        uint16_t le16(const uint8_t *p) {
                return uint16_t(p[0] | (p[1] << 8));
        }

        // True if @p crt is one of the bundle's roots: the same subject and public key, whoever
        // signed this copy of it. A server may send a root cross-signed by an older one the
        // bundle no longer has (smtp.gmail.com: GTS Root R4 by "GlobalSign Root CA"); the
        // bundle's check looks only at the top certificate's issuer, and refuses it.
        bool isBundleRoot(const mbedtls_x509_crt *crt) {
                const size_t size = size_t(bundleEnd - bundleStart);
                if (size < 4) return false;
                uint32_t first;
                std::memcpy(&first, bundleStart, 4);
                if (first > size) return false;
                for (uint32_t i = 0; i < first / 4; ++i) {
                        uint32_t at;
                        std::memcpy(&at, bundleStart + 4 * i, 4);
                        if (at + 4 > size) return false;
                        const uint8_t *entry = bundleStart + at;
                        const size_t   nameLength = le16(entry);
                        const size_t   keyLength = le16(entry + 2);
                        if (at + 4 + nameLength + keyLength > size) return false;
                        if (nameLength == crt->subject_raw.len && keyLength == crt->pk_raw.len &&
                            std::memcmp(entry + 4, crt->subject_raw.p, nameLength) == 0 &&
                            std::memcmp(entry + 4 + nameLength, crt->pk_raw.p, keyLength) == 0)
                                return true;
                }
                return false;
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
                if ((*flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0 && isBundleRoot(crt))
                        *flags &= ~uint32_t(MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_BAD_MD);
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

        // A server's check of the client's certificate: mbedTLS checks it against the roots given;
        // ours only records it (depth 0).
        int verifyClient(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *) {
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

        bool isPem(ByteSpan data) {
                static constexpr char Begin[] = "-----BEGIN";
                const size_t          n = sizeof(Begin) - 1;
                for (size_t i = 0; i + n <= data.size(); ++i)
                        if (std::memcmp(data.data() + i, Begin, n) == 0) return true;
                return false;
        }

        // mbedTLS's PEM parsers want the text with its NUL, counted.
        template <typename Fn> int withPem(ByteSpan data, Fn &&parse) {
                auto *copy = static_cast<unsigned char *>(Memory::alloc(MemSpace::Psram, data.size() + 1));
                if (copy == nullptr) return MBEDTLS_ERR_SSL_ALLOC_FAILED;
                std::memcpy(copy, data.data(), data.size());
                copy[data.size()] = '\0';
                const int ret = parse(copy, data.size() + 1);
                Memory::free(copy);
                return ret;
        }

        // Certificates into @p chain: a PEM (one or more; any that fails fails it) or a DER.
        int parseCertificates(mbedtls_x509_crt &chain, ByteSpan data) {
                if (!isPem(data)) return mbedtls_x509_crt_parse_der(&chain, data.data(), data.size());
                return withPem(data, [&](const unsigned char *p, size_t n) {
                        const int ret = mbedtls_x509_crt_parse(&chain, p, n);
                        return ret > 0 ? MBEDTLS_ERR_X509_INVALID_FORMAT : ret;
                });
        }

        int parseKey(mbedtls_pk_context &key, ByteSpan data) {
                if (!isPem(data))
                        return mbedtls_pk_parse_key(&key, data.data(), data.size(), nullptr, 0, random, nullptr);
                return withPem(data, [&](const unsigned char *p, size_t n) {
                        return mbedtls_pk_parse_key(&key, p, n, nullptr, 0, random, nullptr);
                });
        }

} // namespace

int TlsSession::Impl::bioSend(void *arg, const unsigned char *data, size_t bytes) {
        Impl          &m = *static_cast<Impl *>(arg);
        const ByteSpan piece(data, bytes);
        auto [n, err] = m.socket->send(Span<const ByteSpan>(&piece, 1), nullptr, m.ioFlags | Socket::Underlying);
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
        auto [r, err] = m.socket->receive(Span<const WritableByteSpan>(&piece, 1), m.ioFlags | Socket::Underlying);
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
        const bool server = (config.flags & Server) != 0;
        const bool clientCert = (config.flags & ClientCert) != 0;
        if ((config.flags & ~uint32_t(NoVerify | Server | ClientCert)) != 0 ||
            (!server && config.serverName.isEmpty() && !noVerify) || (clientCert && !server) ||
            (clientCert && !noVerify && config.ca.isEmpty()) || config.cert.isEmpty() != config.key.isEmpty())
                return Result<TlsSession *>(nullptr, Error::Invalid);
        void *block = Memory::alloc(MemSpace::Psram, sizeof(Impl));
        if (block == nullptr) return Result<TlsSession *>(nullptr, Error::NoMem);
        Impl *m = new (block) Impl();
        m->session._impl = m;
        m->session._flags = config.flags;
        m->socket = &socket;
        mbedtls_ssl_init(&m->ssl);
        mbedtls_ssl_config_init(&m->conf);
        mbedtls_x509_crt_init(&m->roots);
        mbedtls_x509_crt_init(&m->cert);
        mbedtls_pk_init(&m->key);
        auto refuse = [&](Error err, const char *what) {
                if (what != nullptr) (void)why.append(what);
                destroy(&m->session);
                return Result<TlsSession *>(nullptr, err);
        };
        if (m->session._name.append(config.serverName).isError()) return refuse(Error::NoMem, nullptr);

        // ALPN: names 1-15 bytes, at most 8.
        size_t count = 0;
        for (size_t start = 0; start < config.alpn.size();) {
                size_t end = start;
                while (end < config.alpn.size() && config.alpn.data()[end] != ',') ++end;
                const size_t n = end - start;
                if (n == 0 || n > MaxAlpnName || count == MaxAlpn)
                        return refuse(Error::Invalid, "ALPN: 1-15 bytes a protocol, at most 8");
                std::memcpy(m->alpnNames[count], config.alpn.data() + start, n);
                m->alpn[count] = m->alpnNames[count];
                ++count;
                start = end + 1;
                if (end + 1 == config.alpn.size()) return refuse(Error::Invalid, "ALPN: an empty protocol");
        }
        for (const ByteSpan &roots : config.ca) {
                const int ret = parseCertificates(m->roots, roots);
                if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED || ret == MBEDTLS_ERR_X509_ALLOC_FAILED)
                        return refuse(Error::NoMem, nullptr);
                if (ret != 0) return refuse(Error::Invalid, "a trusted root doesn't parse (PEM or DER)");
        }
        if (!config.cert.isEmpty()) {
                if (parseCertificates(m->cert, config.cert) != 0)
                        return refuse(Error::Invalid, "the certificate doesn't parse (PEM or DER)");
                if (parseKey(m->key, config.key) != 0)
                        return refuse(Error::Invalid, "the key doesn't parse (PEM or DER, not encrypted)");
        } else if (server) {
                // The board's own (§37a S3), parsed for this session alone (mbedTLS's contexts are
                // never shared between sessions).
                String      certPem(MemSpace::Psram), keyPem(MemSpace::Psram);
                const Error err = TlsServerKeys::pem(certPem, keyPem, why);
                int         ret = err.isOk() ? 0 : -1;
                if (ret == 0)
                        ret = mbedtls_x509_crt_parse(&m->cert, reinterpret_cast<const unsigned char *>(certPem.cStr()),
                                                     certPem.size() + 1);
                if (ret == 0)
                        ret = mbedtls_pk_parse_key(&m->key, reinterpret_cast<const unsigned char *>(keyPem.cStr()),
                                                   keyPem.size() + 1, nullptr, 0, random, nullptr);
                if (!keyPem.isEmpty()) std::memset(const_cast<char *>(keyPem.cStr()), 0, keyPem.size());
                if (ret != 0) {
                        if (err.isOk()) (void)why.append("the board's TLS certificate doesn't parse");
                        return refuse(err.isOk() ? Error::IoError : err, nullptr);
                }
        }

        // The bundle's log says what `why` says, on the console in the middle of a command's output.
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);
        int ret = mbedtls_ssl_config_defaults(&m->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0 && server) {
                // A client certificate asked for only with ClientCert; with NoVerify any is taken
                // (required all the same: handshake() checks one came).
                mbedtls_ssl_conf_authmode(&m->conf, !clientCert ? MBEDTLS_SSL_VERIFY_NONE
                                                    : noVerify  ? MBEDTLS_SSL_VERIFY_OPTIONAL
                                                                : MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_rng(&m->conf, random, nullptr);
                if (clientCert && !config.ca.isEmpty()) mbedtls_ssl_conf_ca_chain(&m->conf, &m->roots, nullptr);
                mbedtls_ssl_conf_verify(&m->conf, verifyClient, &m->session._peer);
                mbedtls_ssl_conf_ciphersuites(&m->conf, Suites);
                ret = mbedtls_ssl_conf_own_cert(&m->conf, &m->cert, &m->key);
        } else if (ret == 0) {
                // `NoVerify`: the chain is still checked, so `verifyFlags()` says what passed.
                mbedtls_ssl_conf_authmode(&m->conf,
                                          noVerify ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_rng(&m->conf, random, nullptr);
                ret = esp_crt_bundle_attach(&m->conf) == ESP_OK ? 0 : MBEDTLS_ERR_SSL_ALLOC_FAILED;
        }
        if (ret == 0) {
                // Roots of our own in place of the bundle's placeholder chain: mbedTLS checks a chain
                // against them, and a chain that isn't theirs is left untrusted for the bundle's
                // check (verify()).
                if (!config.ca.isEmpty()) mbedtls_ssl_conf_ca_chain(&m->conf, &m->roots, nullptr);
                mbedtls_ssl_conf_verify(&m->conf, verify, &m->session._peer);
                mbedtls_ssl_conf_ciphersuites(&m->conf, Suites);
                if (!config.cert.isEmpty()) ret = mbedtls_ssl_conf_own_cert(&m->conf, &m->cert, &m->key);
        }
        if (ret == 0 && count > 0) ret = mbedtls_ssl_conf_alpn_protocols(&m->conf, m->alpn);
        if (ret == 0) ret = mbedtls_ssl_setup(&m->ssl, &m->conf);
        if (ret == 0 && !server && !m->session._name.isEmpty())
                ret = mbedtls_ssl_set_hostname(&m->ssl, m->session._name.cStr());
        if (ret != 0) return refuse(Error::NoMem, nullptr);
        mbedtls_ssl_set_bio(&m->ssl, m, Impl::bioSend, Impl::bioReceive, nullptr);
        return Result<TlsSession *>(&m->session, Error::Ok);
}

void TlsSession::destroy(TlsSession *session) {
        if (session == nullptr) return;
        Impl *m = session->_impl;
        mbedtls_ssl_free(&m->ssl);
        mbedtls_ssl_config_free(&m->conf);
        mbedtls_x509_crt_free(&m->roots);
        mbedtls_x509_crt_free(&m->cert);
        mbedtls_pk_free(&m->key);
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
        if (ret == MBEDTLS_ERR_SSL_INVALID_MAC) return Error::BadMessage;
        return Error::ProtocolError;
}

void TlsSession::abandon(Error err, const char *reason) {
        if (_state != State::Handshaking) return;
        _why.clear();
        (void)_why.appendf("%s: %s",
                           !_name.isEmpty()         ? _name.cStr()
                           : (_flags & Server) != 0 ? "the client"
                                                    : "the server",
                           reason);
        (void)fail(err);
        return;
}

Error TlsSession::handshake(uint32_t flags) {
        if (_state == State::Open) return Error::Ok;
        if (_state == State::Failed) return _failure;
        Impl      &m = *_impl;
        const bool server = (_flags & Server) != 0;
        const bool checks = (_flags & NoVerify) == 0 && (!server || (_flags & ClientCert) != 0);
        if (checks && !Clock::isSet()) {
                // A certificate's dates can't be checked: refused before a byte is sent.
                _verify = VerifyClock;
                _why.clear();
                (void)_why.appendf("%s: %s", _name.isEmpty() ? "the server" : _name.cStr(), verifyWords(VerifyClock));
                return fail(Error::KeyRejected);
        }
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

        const char    *name = !_name.isEmpty() ? _name.cStr() : server ? "the client" : "the server";
        const uint32_t flagsSeen = mbedtls_ssl_get_verify_result(&m.ssl);
        _verify = flagsSeen == uint32_t(-1) || (server && (_flags & ClientCert) == 0) ? 0 : verifyBits(flagsSeen);
        if (ret == 0 && server && (_flags & ClientCert) != 0 && !_peer.isKnown) {
                _why.clear();
                (void)_why.appendf("%s: gave no certificate", name);
                return fail(Error::KeyRejected);
        }
        // Only `NoVerify` gets this far without the clock: its dates were checked against 1970.
        if (_peer.isKnown && !Clock::isSet()) _verify |= VerifyClock;
        _why.clear();
        if (ret == 0) {
                if (_verify != 0 && (_flags & NoVerify) == 0) {
                        (void)_why.appendf("%s: %s", name, verifyWords(_verify));
                        return fail(Error::KeyRejected);
                }
                _state = State::Open;
                _bytesWanted = 1;
                return Error::Ok;
        }
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && _verify != 0) {
                (void)_why.appendf("%s: %s", name, verifyWords(_verify));
                return fail(Error::KeyRejected);
        }
        const Error err = ioError(ret);
        if (err == Error::ProtocolError || err == Error::BadMessage)
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
        // The rest of a record the socket took part of first: mbedTLS sends what is left of it
        // when called with nothing new.
        while (_flushPending) {
                const int ret = mbedtls_ssl_write(&m.ssl, data.data(), 0);
                if (ret >= 0) {
                        _flushPending = false;
                        break;
                }
                if (ret == MBEDTLS_ERR_SSL_WANT_WRITE && m.ioFlags != 0) {
                        _want = Want::Write;
                        return Result<size_t>(size_t(0), Error::WouldBlock);
                }
                if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) return Result<size_t>(size_t(0), fail(ioError(ret)));
        }
        if (data.isEmpty()) return Result<size_t>(size_t(0), Error::Ok);
        const int largest = mbedtls_ssl_get_max_out_record_payload(&m.ssl);
        if (largest <= 0) return Result<size_t>(size_t(0), fail(Error::ProtocolError));
        const size_t n = data.size() < size_t(largest) ? data.size() : size_t(largest);
        for (;;) {
                const int ret = mbedtls_ssl_write(&m.ssl, data.data(), n);
                if (ret >= 0) return Result<size_t>(size_t(ret), Error::Ok);
                if (ret == MBEDTLS_ERR_SSL_WANT_WRITE && m.ioFlags != 0) {
                        // The record is made: what the socket didn't take goes first next time
                        // (mbedTLS would otherwise want this same data again).
                        _flushPending = true;
                        _want = Want::Write;
                        return Result<size_t>(n, Error::Ok);
                }
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        if (m.ioFlags == 0) continue;
                        _want = Want::Read;
                        return Result<size_t>(size_t(0), Error::WouldBlock);
                }
                const Error err = ioError(ret);
                return Result<size_t>(size_t(0), err == Error::ProtocolError ? fail(err) : err);
        }
}

Result<size_t> TlsSession::receive(WritableByteSpan into, uint32_t flags) {
        if (Error err = handshake(flags); err.isError()) return Result<size_t>(size_t(0), err);
        Impl &m = *_impl;
        if (m.ended) return Result<size_t>(size_t(0), Error::Ok);
        m.ioFlags = flags & Socket::DontWait;
        _want = Want::Nothing;
        for (;;) {
                const int ret = mbedtls_ssl_read(&m.ssl, into.data(), into.size());
                if (ret > 0) {
                        _bytesWanted = 1;
                        return Result<size_t>(size_t(ret), Error::Ok);
                }
                if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF) {
                        // The server's close_notify, or the connection's end without it (servers do:
                        // a body that ends by closing must have sent its length to be trusted).
                        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) _cleanClose = true;
                        m.ended = true;
                        return Result<size_t>(size_t(0), Error::Ok);
                }
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        if (m.ioFlags == 0) continue;
                        _want = ret == MBEDTLS_ERR_SSL_WANT_READ ? Want::Read : Want::Write;
                        return Result<size_t>(size_t(0), Error::WouldBlock);
                }
                const Error err = ioError(ret);
                const bool  fatal = err == Error::BadMessage || err == Error::ProtocolError;
                return Result<size_t>(size_t(0), fatal ? fail(err) : err);
        }
}

void TlsSession::closeNotify() {
        if (_state != State::Open) return;
        _impl->ioFlags = Socket::DontWait;
        (void)mbedtls_ssl_close_notify(&_impl->ssl);
        return;
}

size_t TlsSession::pendingBytes() const {
        return _state == State::Open ? mbedtls_ssl_get_bytes_avail(&_impl->ssl) : 0;
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

void TlsSession::info(Info &out) const {
        out = Info{};
        out.state = _state;
        out.failure = _failure;
        out.verify = _verify;
        out.version = version();
        out.cipher = cipher();
        (void)std::snprintf(out.cipherName, sizeof(out.cipherName), "%s", cipherName());
        if (_state == State::Open) {
                const char *alpn = mbedtls_ssl_get_alpn_protocol(&_impl->ssl);
                if (alpn != nullptr) (void)std::snprintf(out.alpn, sizeof(out.alpn), "%s", alpn);
        }
        out.peer = _peer;
        out.cleanClose = _cleanClose;
        out.ended = _impl->ended;
        return;
}

BMDS_NAMESPACE_END
