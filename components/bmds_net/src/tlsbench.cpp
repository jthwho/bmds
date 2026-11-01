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
 */

#include <cstring>
#include <aes/esp_aes_gcm.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>
#include <bmds/bench.h>
#include <bmds/ghash.h>
#include <bmds/memory.h>
#include <bmds/stopwatch.h>

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

} // namespace

BMDS_BENCH(netTlsAead, "net.tls.aead", runAead, BenchQuick,
           "A 16 KB TLS record encrypted and tagged: AES-128-GCM ESP-IDF's and with the 8-bit GHASH, "
           "ChaCha20-Poly1305, GHASH alone, ESP-IDF's whole-engine GCM from internal memory (MB/s)")

BMDS_NAMESPACE_END
