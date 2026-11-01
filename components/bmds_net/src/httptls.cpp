/**
 * @file      httptls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS for fetching (devplan/packages.md §23): mbedTLS over a connected `Socket` (its BIO), the
 * server's certificate checked against IDF's bundle and the host's name. And mbedTLS's
 * allocations, through `Memory` in PSRAM (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`): fetching is its only
 * user at run time.
 */

#include <cstring>
#include <new>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <bmds/clock.h>
#include <bmds/httpclient.h>
#include <bmds/memory.h>

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

namespace {

        struct Session {
                        mbedtls_ssl_context ssl;
                        mbedtls_ssl_config  conf;
                        Socket             *socket = nullptr;
                        Error               ioError; ///< What the socket said, when mbedTLS failed through it.
        };

        int random(void *, unsigned char *out, size_t bytes) {
                esp_fill_random(out, bytes);
                return 0;
        }

        int bioSend(void *arg, const unsigned char *data, size_t bytes) {
                Session       &s = *static_cast<Session *>(arg);
                const ByteSpan piece(data, bytes);
                auto [n, err] = s.socket->send(Span<const ByteSpan>(&piece, 1), nullptr, 0);
                if (err.isError()) {
                        s.ioError = err == Error::WouldBlock ? Error::Timeout : err;
                        return MBEDTLS_ERR_NET_SEND_FAILED;
                }
                return int(n);
        }

        int bioReceive(void *arg, unsigned char *into, size_t bytes) {
                Session               &s = *static_cast<Session *>(arg);
                const WritableByteSpan piece(into, bytes);
                auto [r, err] = s.socket->receive(Span<const WritableByteSpan>(&piece, 1), 0);
                if (err == Error::EndOfFile) return 0;
                if (err.isError()) {
                        s.ioError = err == Error::WouldBlock ? Error::Timeout : err;
                        return MBEDTLS_ERR_NET_RECV_FAILED;
                }
                return int(r.bytes);
        }

        // An mbedTLS failure as an `Error`: the socket's own when it came from there.
        Error errorOf(const Session &s, int ret) {
                if (ret == MBEDTLS_ERR_NET_SEND_FAILED || ret == MBEDTLS_ERR_NET_RECV_FAILED) return s.ioError;
                if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED) return Error::NoMem;
                return Error::CorruptData;
        }

        void destroy(Session *s) {
                mbedtls_ssl_free(&s->ssl);
                mbedtls_ssl_config_free(&s->conf);
                s->~Session();
                Memory::free(s);
                return;
        }

        Result<size_t> tlsSend(void *state, ByteSpan data) {
                Session &s = *static_cast<Session *>(state);
                for (;;) {
                        const int ret = mbedtls_ssl_write(&s.ssl, data.data(), data.size());
                        if (ret >= 0) return Result<size_t>(size_t(ret), Error::Ok);
                        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                                return Result<size_t>(size_t(0), errorOf(s, ret));
                }
        }

        Result<size_t> tlsReceive(void *state, WritableByteSpan into) {
                Session &s = *static_cast<Session *>(state);
                for (;;) {
                        const int ret = mbedtls_ssl_read(&s.ssl, into.data(), into.size());
                        if (ret >= 0) return Result<size_t>(size_t(ret), Error::Ok);
                        // The server's close_notify, or the connection's end without it (a server
                        // that ends its body by closing must have sent a length to be trusted).
                        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF)
                                return Result<size_t>(size_t(0), Error::Ok);
                        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                                return Result<size_t>(size_t(0), errorOf(s, ret));
                }
        }

        void tlsClose(void *state) {
                Session *s = static_cast<Session *>(state);
                (void)mbedtls_ssl_close_notify(&s->ssl);
                destroy(s);
                return;
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

        // A refused certificate, in words.
        void certificateWhy(uint32_t flags, String &out) {
                if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
                        (void)out.append("its certificate has expired");
                else if (flags & MBEDTLS_X509_BADCERT_FUTURE)
                        (void)out.append("its certificate isn't valid yet (is the clock right?)");
                else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
                        (void)out.append("its certificate is for another name");
                else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
                        (void)out.append("its certificate isn't from an authority this system trusts");
                else
                        (void)out.appendf("its certificate was refused (0x%x)", unsigned(flags));
                return;
        }

} // namespace

Error Http::openTls(Socket &socket, StringView host, String &why, Tls &out) {
        why.clear();
        String name;
        Error  err = name.append(host);
        if (err.isError()) return err;
        if (!Clock::isSet()) {
                (void)why.appendf(
                        "%s: the clock isn't set yet (no time from the network): HTTPS checks a certificate's "
                        "dates against it",
                        name.cStr());
                return Error::NotReady;
        }
        void *block = Memory::alloc(MemSpace::Psram, sizeof(Session));
        if (block == nullptr) return Error::NoMem;
        Session *s = new (block) Session();
        s->socket = &socket;
        mbedtls_ssl_init(&s->ssl);
        mbedtls_ssl_config_init(&s->conf);
        // The bundle's log says what `why` says, on the console in the middle of a command's output.
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);
        int ret = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0) {
                mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
                mbedtls_ssl_conf_rng(&s->conf, random, nullptr);
                ret = esp_crt_bundle_attach(&s->conf) == ESP_OK ? 0 : MBEDTLS_ERR_SSL_ALLOC_FAILED;
        }
        if (ret == 0) mbedtls_ssl_conf_ciphersuites(&s->conf, Suites);
        if (ret == 0) ret = mbedtls_ssl_setup(&s->ssl, &s->conf);
        if (ret == 0) ret = mbedtls_ssl_set_hostname(&s->ssl, name.cStr());
        if (ret == 0) {
                mbedtls_ssl_set_bio(&s->ssl, s, bioSend, bioReceive, nullptr);
                do {
                        ret = mbedtls_ssl_handshake(&s->ssl);
                } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        }
        if (ret != 0) {
                uint32_t flags = mbedtls_ssl_get_verify_result(&s->ssl);
                // The bundle's callback, refusing a chain without a root it holds, ends the
                // verification with a fatal error and no flags.
                if (ret == MBEDTLS_ERR_X509_FATAL_ERROR && flags == uint32_t(-1))
                        flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
                if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (flags != 0 && flags != uint32_t(-1))) {
                        String reason;
                        certificateWhy(flags, reason);
                        (void)why.appendf("%s: %s", name.cStr(), reason.cStr());
                        err = Error::PermissionDenied;
                } else {
                        err = errorOf(*s, ret);
                        if (err == Error::CorruptData)
                                (void)why.appendf("%s: the TLS handshake failed (mbedTLS -0x%04x)", name.cStr(),
                                                  unsigned(-ret));
                        else
                                (void)why.appendf("%s: %s", name.cStr(), err.desc());
                }
                destroy(s);
                return err;
        }
        out.state = s;
        out.send = tlsSend;
        out.receive = tlsReceive;
        out.close = tlsClose;
        return Error::Ok;
}

BMDS_NAMESPACE_END
