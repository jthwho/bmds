/**
 * @file      tlsgcm.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * AES-GCM's one-shot calls with the 8-bit GHASH (tlsgcm.h, devplan/network.md §33). ESP-IDF's
 * GHASH is static in esp_aes_gcm.c, so the whole one-shot is wrapped: its starts (H from the
 * engine, J0), the counter mode by esp_aes_crypt_ctr(), the tag the same way, as its own partial
 * path does them.
 */

#include <cstring>
#include <aes/esp_aes.h>
#include <aes/esp_aes_gcm.h>
#include <esp_memory_utils.h>
#include <mbedtls/gcm.h>
#include <bmds/ghash.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>

#include "tlsgcm.h"

extern "C" {
int __real_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                                     size_t iv_len, const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output, size_t tag_len,
                                     unsigned char *tag);
int __real_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len, const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output);
int __wrap_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                                     size_t iv_len, const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output, size_t tag_len,
                                     unsigned char *tag);
int __wrap_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len, const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output);
}

BMDS_NAMESPACE_BEGIN

namespace {

        // A TLS session has two keys (one each way); a second session's take turns.
        constexpr size_t Tables = 2;

        struct Cache {
                        Mutex   lock;
                        uint8_t h[Tables][Ghash::BlockBytes] = {};
                        bool    valid[Tables] = {};
                        size_t  next = 0; // replaced next on a miss
                        Ghash   ghash[Tables];
        };

        PsramObject<Cache> cache;

        // The whole of it in the engine (ESP-IDF's): when its DMA reaches every buffer.
        bool engineTakes(size_t length, const unsigned char *aad, size_t aad_len, const unsigned char *input,
                         const unsigned char *output) {
                return length > 0 && esp_ptr_dma_capable(input) && esp_ptr_dma_capable(output) &&
                       (aad_len == 0 || esp_ptr_dma_capable(aad));
        }

        const Ghash &tablesFor(Cache &c, const uint8_t (&h)[Ghash::BlockBytes]) {
                for (size_t i = 0; i < Tables; ++i)
                        if (c.valid[i] && std::memcmp(c.h[i], h, sizeof(h)) == 0) return c.ghash[i];
                const size_t i = c.next;
                c.next = (c.next + 1) % Tables;
                c.ghash[i].setKey(h);
                std::memcpy(c.h[i], h, sizeof(h));
                c.valid[i] = true;
                return c.ghash[i];
        }

        // J0's last 32 bits plus one: the first block's counter.
        void increment32(uint8_t (&block)[16]) {
                for (int i = 15; i >= 12; --i)
                        if (++block[i] != 0) break;
                return;
        }

        int crypt(esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv, size_t iv_len,
                  const unsigned char *aad, size_t aad_len, const unsigned char *input, unsigned char *output,
                  size_t tag_len, unsigned char *tag) {
                if (tag_len < 4 || tag_len > 16 || (aad_len > 0 && aad == nullptr) ||
                    (length > 0 && (input == nullptr || output == nullptr)))
                        return MBEDTLS_ERR_GCM_BAD_INPUT;
                // H (from the engine, once a context) and J0.
                int ret = esp_aes_gcm_starts(ctx, mode, iv, iv_len);
                if (ret != 0) return ret;
                uint8_t y[Ghash::BlockBytes] = {};
                uint8_t j0[16];
                std::memcpy(j0, ctx->ori_j0, sizeof(j0));
                {
                        Mutex::Locker lock(cache->lock, WaitForever);
                        const Ghash  &g = tablesFor(*cache, ctx->H);
                        g.update(y, aad, aad_len);
                        if (mode == MBEDTLS_GCM_DECRYPT) g.update(y, input, length);
                }
                uint8_t counter[16];
                std::memcpy(counter, j0, sizeof(counter));
                increment32(counter);
                size_t  offset = 0;
                uint8_t stream[16] = {};
                if (length > 0) {
                        ret = esp_aes_crypt_ctr(&ctx->aes_ctx, length, &offset, counter, stream, input, output);
                        if (ret != 0) return ret;
                }
                uint8_t        lengths[16];
                const uint64_t aadBits = uint64_t(aad_len) * 8, textBits = uint64_t(length) * 8;
                for (int i = 0; i < 8; ++i) {
                        lengths[7 - i] = static_cast<uint8_t>(aadBits >> (8 * i));
                        lengths[15 - i] = static_cast<uint8_t>(textBits >> (8 * i));
                }
                {
                        Mutex::Locker lock(cache->lock, WaitForever);
                        const Ghash  &g = tablesFor(*cache, ctx->H);
                        if (mode == MBEDTLS_GCM_ENCRYPT) g.update(y, output, length);
                        g.update(y, lengths, sizeof(lengths));
                }
                // The tag: E(J0) ^ GHASH, cut to tag_len.
                offset = 0;
                return esp_aes_crypt_ctr(&ctx->aes_ctx, tag_len, &offset, j0, stream, y, tag);
        }

} // namespace

Error TlsGcm::init() {
        return cache.create() != nullptr ? Error::Ok : Error::NoMem;
}

BMDS_NAMESPACE_END

using namespace bmds;

int __wrap_esp_aes_gcm_crypt_and_tag(esp_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                                     size_t iv_len, const unsigned char *aad, size_t aad_len,
                                     const unsigned char *input, unsigned char *output, size_t tag_len,
                                     unsigned char *tag) {
        if (ctx == nullptr || ctx->ctx_soft != nullptr || !cache.isCreated() ||
            engineTakes(length, aad, aad_len, input, output))
                return __real_esp_aes_gcm_crypt_and_tag(ctx, mode, length, iv, iv_len, aad, aad_len, input, output,
                                                        tag_len, tag);
        return crypt(ctx, mode, length, iv, iv_len, aad, aad_len, input, output, tag_len, tag);
}

int __wrap_esp_aes_gcm_auth_decrypt(esp_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                                    const unsigned char *aad, size_t aad_len, const unsigned char *tag, size_t tag_len,
                                    const unsigned char *input, unsigned char *output) {
        if (ctx == nullptr || ctx->ctx_soft != nullptr || !cache.isCreated() ||
            engineTakes(length, aad, aad_len, input, output))
                return __real_esp_aes_gcm_auth_decrypt(ctx, length, iv, iv_len, aad, aad_len, tag, tag_len, input,
                                                       output);
        uint8_t   check[16];
        const int ret =
                crypt(ctx, MBEDTLS_GCM_DECRYPT, length, iv, iv_len, aad, aad_len, input, output, tag_len, check);
        if (ret != 0) return ret;
        // Compared in constant time; a forgery's text is not given out.
        uint8_t diff = 0;
        for (size_t i = 0; i < tag_len; ++i) diff |= static_cast<uint8_t>(tag[i] ^ check[i]);
        if (diff != 0) {
                std::memset(output, 0, length);
                return MBEDTLS_ERR_GCM_AUTH_FAILED;
        }
        return 0;
}
