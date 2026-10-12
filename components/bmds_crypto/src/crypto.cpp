/**
 * @file      crypto.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The AES and SHA engines in register mode (devplan/network.md §26), on ESP-IDF's low-level
 * HAL, its clocks and its lock. ESP-IDF's mbedtls port is not used: its DMA path allocates on
 * each call, and register mode is as fast for SSH's packet sizes.
 */

#include <cstring>
#include <esp_crypto_lock.h>
#include <esp_crypto_periph_clk.h>
#include <hal/aes_ll.h>
#include <hal/aes_types.h>
#include <hal/sha_ll.h>
#include <hal/sha_types.h>
#include <soc/hp_sys_clkrst_struct.h>
#include <bmds/crypto.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Holds an engine: ESP-IDF's lock (both engines share it, with ESP-IDF's own users), and
        // its clock on. The clock stays on after: switching it on (which also resets the engine)
        // and off took 3.8 µs of a 12.6 µs call, reading its bit 0.09 µs; ESP-IDF's users switch it
        // off when they finish, so it is checked each time.
        class Engine {
                public:
                        explicit Engine(bool aes) {
                                esp_crypto_sha_aes_lock_acquire();
                                if (aes && HP_SYS_CLKRST.peri_clk_ctrl25.reg_crypto_aes_clk_en == 0) {
                                        esp_crypto_aes_enable_periph_clk(true);
                                } else if (!aes && HP_SYS_CLKRST.peri_clk_ctrl25.reg_crypto_sha_clk_en == 0) {
                                        esp_crypto_sha_enable_periph_clk(true);
                                }
                        }

                        ~Engine() { esp_crypto_sha_aes_lock_release(); }

                        Engine(const Engine &) = delete;
                        Engine &operator=(const Engine &) = delete;
        };

        // One block through the AES engine (the key and direction already set).
        inline void aesBlock(const uint32_t in[4], uint32_t out[4]) {
                aes_ll_write_block(in);
                aes_ll_start_transform();
                while (aes_ll_get_state() != ESP_AES_STATE_IDLE) {}
                aes_ll_read_block(out);
                return;
        }

        inline void xorBlock(const uint8_t *a, const uint32_t b[4], uint8_t *out) {
                uint32_t w[4];
                std::memcpy(w, a, sizeof(w));
                for (int i = 0; i < 4; ++i) w[i] ^= b[i];
                std::memcpy(out, w, sizeof(w));
                return;
        }

        // Adds one to a 128-bit big-endian counter.
        inline void increment(uint8_t counter[16]) {
                for (int i = 15; i >= 0; --i)
                        if (++counter[i] != 0) break;
                return;
        }

} // namespace

Error Crypto::aes(Algorithm algorithm, const uint8_t *key, size_t keyBytes, uint8_t *state, const uint8_t *in,
                  uint8_t *out, size_t length) {
        const bool ecb = algorithm == Algorithm::AesEcbEncrypt || algorithm == Algorithm::AesEcbDecrypt;
        const bool decrypt = algorithm == Algorithm::AesEcbDecrypt || algorithm == Algorithm::AesCbcDecrypt;
        if (algorithm > Algorithm::AesCtr) return Error::Invalid;
        if (keyBytes != 16 && keyBytes != 32) return Error::Invalid;
        if (length % AesBlockBytes != 0) return Error::Invalid;
        if (key == nullptr || (!ecb && state == nullptr)) return Error::Invalid;
        if (length == 0) return Error::Ok;
        if (in == nullptr || out == nullptr) return Error::Invalid;
        Engine engine(true);
        aes_ll_dma_enable(false);
        aes_ll_set_mode(decrypt ? ESP_AES_DECRYPT : ESP_AES_ENCRYPT, static_cast<uint8_t>(keyBytes));
        (void)aes_ll_write_key(key, keyBytes / 4);
        uint32_t chain[4], block[4], result[4];
        if (!ecb) std::memcpy(chain, state, sizeof(chain));
        for (size_t at = 0; at < length; at += AesBlockBytes) {
                switch (algorithm) {
                        case Algorithm::AesEcbEncrypt:
                        case Algorithm::AesEcbDecrypt:
                                std::memcpy(block, in + at, sizeof(block));
                                aesBlock(block, result);
                                std::memcpy(out + at, result, sizeof(result));
                                break;
                        case Algorithm::AesCbcEncrypt:
                                std::memcpy(block, in + at, sizeof(block));
                                for (int i = 0; i < 4; ++i) block[i] ^= chain[i];
                                aesBlock(block, chain);
                                std::memcpy(out + at, chain, sizeof(chain));
                                break;
                        case Algorithm::AesCbcDecrypt:
                                std::memcpy(block, in + at, sizeof(block)); // before out (it may be in)
                                aesBlock(block, result);
                                for (int i = 0; i < 4; ++i) result[i] ^= chain[i];
                                std::memcpy(out + at, result, sizeof(result));
                                std::memcpy(chain, block, sizeof(chain));
                                break;
                        case Algorithm::AesCtr:
                                aesBlock(chain, result);
                                xorBlock(in + at, result, out + at);
                                increment(reinterpret_cast<uint8_t *>(chain));
                                break;
                        default: break;
                }
        }
        if (!ecb) std::memcpy(state, chain, sizeof(chain));
        return Error::Ok;
}

Error Crypto::sha(Algorithm algorithm, uint8_t *state, const uint8_t *in, size_t length) {
        const size_t block = shaBlockBytes(algorithm);
        const size_t stateBytes = shaStateBytes(algorithm);
        if (block == 0 || state == nullptr || length % block != 0) return Error::Invalid;
        if (length == 0) return Error::Ok;
        if (in == nullptr) return Error::Invalid;
        const esp_sha_type type = algorithm == Algorithm::Sha1     ? SHA1
                                  : algorithm == Algorithm::Sha256 ? SHA2_256
                                                                   : SHA2_512;
        // The engine's registers hold the chaining value as the digest is written, so the
        // caller's bytes go in and come out as they are.
        uint32_t words[32];
        Engine   engine(false);
        sha_ll_set_mode(type);
        std::memcpy(words, state, stateBytes);
        sha_ll_write_digest(type, words, stateBytes / 4);
        for (size_t at = 0; at < length; at += block) {
                const uint8_t *p = in + at;
                if ((reinterpret_cast<uintptr_t>(p) & 3) != 0) {
                        std::memcpy(words, p, block);
                        p = reinterpret_cast<const uint8_t *>(words);
                }
                while (sha_ll_busy()) {} // the previous block done before its successor goes in
                sha_ll_fill_text_block(p, block / 4);
                sha_ll_continue_block(type);
        }
        while (sha_ll_busy()) {}
        sha_ll_read_digest(type, words, stateBytes / 4);
        std::memcpy(state, words, stateBytes);
        return Error::Ok;
}

BMDS_NAMESPACE_END
