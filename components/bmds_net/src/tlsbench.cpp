/**
 * @file      tlsbench.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `bench net.tls.aead` (devplan/network.md §33): a TLS record's encryption and tag, 16 KB, as
 * mbedTLS makes them: AES-GCM ESP-IDF's way and with the 8-bit GHASH (tlsgcm.h), ChaCha20-Poly1305,
 * GHASH alone; and ESP-IDF's whole-engine GCM, which takes only buffers in internal memory.
 *
 * `bench net.tls.handshake` (§35f step 1): a `TlsSession`'s handshake with a real server, each on
 * a thread of its own, to size the `tls` task's stack: the time, the stack it reached, and an open
 * session's PSRAM.
 */

#include <cstdio>
#include <cstring>
#include <aes/esp_aes_gcm.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/bench.h>
#include <bmds/ghash.h>
#include <bmds/memory.h>
#include <bmds/netstack.h>
#include <bmds/socket.h>
#include <bmds/stopwatch.h>
#include <bmds/thread.h>
#include <bmds/tlssession.h>

extern "C" int __real_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                                                size_t iv_len, const unsigned char *aad, size_t aad_len,
                                                const unsigned char *input, unsigned char *output, size_t tag_len,
                                                unsigned char *tag);

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t RecordBytes = 16384;
        constexpr int    Records = 64; // 1 MB a run

        struct Buffers {
                        uint8_t *in = nullptr;
                        uint8_t *out = nullptr;
                        ~Buffers() {
                                Memory::free(in);
                                Memory::free(out);
                        }
        };

        Error allocate(Buffers &b, MemSpace space) {
                b.in = static_cast<uint8_t *>(Memory::alloc(space, RecordBytes));
                b.out = static_cast<uint8_t *>(Memory::alloc(space, RecordBytes));
                if (b.in == nullptr || b.out == nullptr) return Error::NoMem;
                for (size_t i = 0; i < RecordBytes; ++i) b.in[i] = static_cast<uint8_t>(i * 7);
                return Error::Ok;
        }

        // MB/s of @p one(), a record each call.
        template <typename F> Error mbPerS(BenchContext &ctx, const char *metric, F &&one) {
                return ctx.measure(metric, "MB/s", BenchBetter::Higher, [&](double &out) {
                        Stopwatch watch;
                        for (int i = 0; i < Records; ++i)
                                if (one() != 0) return Error(Error::IoError);
                        out = double(RecordBytes) * Records / double(watch.elapsedUs());
                        return Error(Error::Ok);
                });
        }

        Error gcm(BenchContext &ctx, const char *metric, MemSpace space, bool espIdf) {
                Buffers b;
                if (Error err = allocate(b, space); err.isError()) return err;
                static const uint8_t Key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
                uint8_t              iv[12] = {}, aad[13] = {}, tag[16];
                esp_gcm_context      gcm;
                esp_aes_gcm_init(&gcm);
                Error err = esp_aes_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, Key, 128) == 0 ? Error::Ok : Error::IoError;
                if (err.isOk()) {
                        err = mbPerS(ctx, metric, [&] {
                                ++iv[11];
                                return espIdf ? __real_esp_aes_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, RecordBytes,
                                                                                 iv, sizeof(iv), aad, sizeof(aad), b.in,
                                                                                 b.out, sizeof(tag), tag)
                                              : esp_aes_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, RecordBytes, iv,
                                                                          sizeof(iv), aad, sizeof(aad), b.in, b.out,
                                                                          sizeof(tag), tag);
                        });
                }
                esp_aes_gcm_free(&gcm);
                return err;
        }

        Error runAead(BenchContext &ctx) {
                // Records in PSRAM, as mbedTLS's are here.
                if (Error err = gcm(ctx, "aes128_gcm_esp_idf", MemSpace::Psram, true); err.isError()) return err;
                if (Error err = gcm(ctx, "aes128_gcm_ghash8", MemSpace::Psram, false); err.isError()) return err;
                {
                        Buffers b;
                        if (Error err = allocate(b, MemSpace::Psram); err.isError()) return err;
                        static const uint8_t       Key[32] = {1, 2, 3};
                        uint8_t                    nonce[12] = {}, aad[13] = {}, tag[16];
                        mbedtls_chachapoly_context cc;
                        mbedtls_chachapoly_init(&cc);
                        (void)mbedtls_chachapoly_setkey(&cc, Key);
                        const Error err = mbPerS(ctx, "chacha20_poly1305", [&] {
                                ++nonce[11];
                                return mbedtls_chachapoly_encrypt_and_tag(&cc, RecordBytes, nonce, aad, sizeof(aad),
                                                                          b.in, b.out, tag);
                        });
                        mbedtls_chachapoly_free(&cc);
                        if (err.isError()) return err;
                }
                {
                        Buffers b;
                        if (Error err = allocate(b, MemSpace::Psram); err.isError()) return err;
                        auto *g = static_cast<Ghash *>(Memory::alloc(MemSpace::Psram, sizeof(Ghash)));
                        if (g == nullptr) return Error::NoMem;
                        const uint8_t h[16] = {0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
                                               0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e};
                        g->setKey(h);
                        uint8_t     y[16] = {};
                        const Error err = mbPerS(ctx, "ghash8", [&] {
                                g->update(y, b.in, RecordBytes);
                                return 0;
                        });
                        Memory::free(g);
                        if (err.isError()) return err;
                }
                // ESP-IDF's whole-engine GCM: the records in internal memory (TLS's aren't).
                return gcm(ctx, "aes128_gcm_engine_internal", MemSpace::InternalDma, true);
        }


        // A handshake's stack: generous, so the deepest is seen rather than overflowed.
        constexpr size_t HandshakeStackBytes = 32 * 1024;

        struct Handshake {
                        uint32_t   ipv4 = 0;
                        uint16_t   port = 0;
                        StringView host;
                        // Out:
                        Error    err;
                        String   why;
                        double   ms = 0;
                        size_t   stackBytes = 0;
                        size_t   sessionBytes = 0;
                        uint16_t version = 0;
                        char     cipher[48] = {};
                        char     subject[128] = {};
                        char     issuer[128] = {};
        };

        // One connection and handshake, on its own thread (`Handshake`'s fields out).
        void handshakeThread(void *arg) {
                Handshake &h = *static_cast<Handshake *>(arg);
                auto [handle, err] = Socket::create(Socket::Type::Stream, 0);
                if (err.isOk()) {
                        Socket *socket = handle->stream().socket();
                        (void)socket->setOption(Socket::Option::SendTimeoutMs, 30000);
                        (void)socket->setOption(Socket::Option::ReceiveTimeoutMs, 30000);
                        (void)socket->setOption(Socket::Option::NoDelay, 1);
                        err = socket->connect(SocketAddress{h.ipv4, h.port}, 0);
                        if (err.isOk()) {
                                const size_t       before = Memory::stats(MemSpace::Psram).freeBytes;
                                TlsSession::Config config;
                                config.serverName = h.host;
                                auto [s, cerr] = TlsSession::create(*socket, config, h.why);
                                err = cerr;
                                if (err.isOk()) {
                                        Stopwatch watch;
                                        err = s->handshake(0);
                                        h.ms = double(watch.elapsedUs()) / 1000.0;
                                        const size_t after = Memory::stats(MemSpace::Psram).freeBytes;
                                        h.sessionBytes = before > after ? before - after : 0;
                                        if (err.isError()) {
                                                (void)h.why.append(s->why());
                                        } else {
                                                h.version = s->version();
                                                (void)std::snprintf(h.cipher, sizeof(h.cipher), "%s", s->cipherName());
                                                (void)std::snprintf(h.subject, sizeof(h.subject), "%s",
                                                                    s->peer().subject);
                                                (void)std::snprintf(h.issuer, sizeof(h.issuer), "%s", s->peer().issuer);
                                                s->closeNotify();
                                        }
                                        TlsSession::destroy(s);
                                }
                        }
                }
                h.err = err;
                h.stackBytes = HandshakeStackBytes - size_t(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
                return;
        }

        constexpr BenchParam HandshakeParams[] = {
                {"host", "github.com", "The server (its certificate must check out)"},
                {"port", "443", "Its port"},
        };

        Error runHandshake(BenchContext &ctx) {
                uint64_t port = 0;
                if (ctx.paramUnsigned("port", port).isError() || port == 0 || port > 65535) return Error::Invalid;
                Handshake h;
                h.host = ctx.param("host");
                h.port = uint16_t(port);
                if (Error err = NetStack::resolve(h.host, h.ipv4, 10000); err.isError()) {
                        ctx.note("%.*s: %s", int(h.host.size()), h.host.data(), err.desc());
                        return err == Error::NotReady ? Error::NotReady : err;
                }
                const uint32_t total = ctx.warmup() + ctx.runs();
                double         ms[32];
                double         stack[32];
                double         session[32];
                uint32_t       kept = 0;
                for (uint32_t i = 0; i < total && kept < 32; ++i) {
                        if (ctx.isCancelled()) return Error::Cancelled;
                        Thread         thread;
                        Thread::Config config;
                        config.name = "tlsbench";
                        config.stackBytes = HandshakeStackBytes;
                        config.priority = unsigned(uxTaskPriorityGet(nullptr));
                        config.kernel = true;
                        h.why.clear();
                        if (Error err = thread.start(config, handshakeThread, &h); err.isError()) return err;
                        thread.join();
                        if (h.err.isError()) {
                                ctx.note("%s", h.why.isEmpty() ? h.err.desc() : h.why.cStr());
                                return h.err;
                        }
                        if (i < ctx.warmup()) continue;
                        ms[kept] = h.ms;
                        stack[kept] = double(h.stackBytes);
                        session[kept] = double(h.sessionBytes);
                        ++kept;
                }
                ctx.note("%.*s: TLS 0x%04x, %s; %s, from %s", int(h.host.size()), h.host.data(), unsigned(h.version),
                         h.cipher, h.subject, h.issuer);
                ctx.reportSamples("handshake", "ms", BenchBetter::Lower, Span<double>(ms, kept));
                ctx.reportSamples("stack", "bytes", BenchBetter::Lower, Span<double>(stack, kept));
                ctx.reportSamples("session_psram", "bytes", BenchBetter::Lower, Span<double>(session, kept));
                return Error::Ok;
        }

} // namespace

BMDS_BENCH(netTlsAead, "net.tls.aead", runAead, BenchQuick,
           "A 16 KB TLS record encrypted and tagged: AES-128-GCM ESP-IDF's and with the 8-bit GHASH, "
           "ChaCha20-Poly1305, GHASH alone, ESP-IDF's whole-engine GCM from internal memory (MB/s)")

BMDS_BENCH_WITH(netTlsHandshake, "net.tls.handshake", runHandshake, BenchPeer, HandshakeParams,
                "A TLS handshake with a real server on a thread of its own: its time, the stack it reached and an "
                "open session's PSRAM")

BMDS_NAMESPACE_END
