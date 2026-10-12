/**
 * @file      crypto.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The AES and SHA engines (devplan/network.md §26, §29), on ESP-IDF's low-level HAL, its clocks
 * and its lock. AES runs by DMA for calls of `DmaMinBytes` or more (the task sleeps while the
 * engine works), in register mode below; SHA in register mode. ESP-IDF's mbedtls port is not
 * used: its DMA path allocates on each call.
 */

#include <cstring>
#include <esp_cache.h>
#include <esp_crypto_lock.h>
#include <esp_crypto_periph_clk.h>
#include <esp_memory_utils.h>
#include <esp_private/gdma.h>
#include <hal/dma_types.h>
#include <hal/aes_ll.h>
#include <hal/aes_types.h>
#include <hal/sha_ll.h>
#include <hal/sha_types.h>
#include <soc/aes_reg.h>
#include <soc/hp_sys_clkrst_struct.h>
#include <bmds/crypto.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>

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

        // One block through the AES engine (the key and direction already set). The registers by
        // hand: aes_ll's copy each word through memcpy, and they were a quarter of AES-CTR's time.
        inline void aesBlock(const uint32_t in[4], uint32_t out[4]) {
                for (int i = 0; i < 4; ++i) REG_WRITE(AES_TEXT_IN_0_REG + 4 * i, in[i]);
                REG_WRITE(AES_TRIGGER_REG, 1);
                while (REG_READ(AES_STATE_REG) != ESP_AES_STATE_IDLE) {}
                for (int i = 0; i < 4; ++i) out[i] = REG_READ(AES_TEXT_OUT_0_REG + 4 * i);
                return;
        }

        // @p out = @p a ^ @p b; whole words when the bytes are aligned (SSH's packets are).
        inline void xorBlock(const uint8_t *a, const uint32_t b[4], uint8_t *out) {
                if (((reinterpret_cast<uintptr_t>(a) | reinterpret_cast<uintptr_t>(out)) & 3) == 0) {
                        const auto *aw = reinterpret_cast<const uint32_t *>(a);
                        auto       *ow = reinterpret_cast<uint32_t *>(out);
                        for (int i = 0; i < 4; ++i) ow[i] = aw[i] ^ b[i];
                        return;
                }
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

        // Adds @p n to a 128-bit big-endian counter.
        void advance(uint8_t counter[16], uint32_t n) {
                uint64_t carry = n;
                for (int i = 15; i >= 0 && carry != 0; --i) {
                        carry += counter[i];
                        counter[i] = static_cast<uint8_t>(carry);
                        carry >>= 8;
                }
                return;
        }

        // --- DMA ---------------------------------------------------------------------------

        // Calls this long or longer go by DMA (devplan/network.md §29: measured).
        constexpr size_t       DmaMinBytes = 1024;
        constexpr size_t       DescBytes = 3968;       // a descriptor's buffer: under 4096, whole lines and blocks
        constexpr size_t       InDescs = 7;            // a run's input: 27 KB
        constexpr size_t       OutDescs = InDescs + 2; // its output, with the head and tail bounce lines
        constexpr size_t       RunBytes = InDescs * DescBytes;
        constexpr size_t       Line = 128; // the largest cache line (L2)
        constexpr unsigned int DmaTimeoutMs = 1000;
        static_assert(DescBytes % Line == 0 && DescBytes % Crypto::AesBlockBytes == 0);

        using Desc = dma_descriptor_align8_t;

        // Our GDMA channel pair, connected to the AES engine at the first DMA call and kept (ESP-IDF's
        // mbedtls DMA, which BMDS doesn't use, can't connect to it after), the descriptors and
        // bounce lines in one block of internal DMA memory, and the end-of-frame wake.
        struct Dma {
                        gdma_channel_handle_t tx = nullptr;
                        gdma_channel_handle_t rx = nullptr;
                        Desc                 *in = nullptr;   // InDescs
                        Desc                 *out = nullptr;  // OutDescs
                        uint8_t              *head = nullptr; // a line each
                        uint8_t              *tail = nullptr;
                        size_t                blockBytes = 0; // descriptors and bounce lines, whole lines
                        bool                  tried = false;
                        bool                  ready = false;
        };
        constinit Dma                     dma;
        constinit StaticObject<Semaphore> dmaDone;

        bool IRAM_ATTR dmaEof(gdma_channel_handle_t, gdma_event_data_t *, void *) {
                dmaDone->releaseFromIsr();
                return false; // releaseFromIsr() switches at the interrupt's end
        }

        // Makes the channels (the crypto lock held). False if they couldn't be: register mode then.
        bool startDma() {
                if (dma.tried) return dma.ready;
                dma.tried = true;
                const size_t descBytes = (InDescs + OutDescs) * sizeof(Desc);
                const size_t total = (descBytes + Line - 1) / Line * Line + 2 * Line;
                auto        *block = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, total, Line));
                if (block == nullptr) return false;
                std::memset(block, 0, total);
                dma.in = reinterpret_cast<Desc *>(block);
                dma.out = dma.in + InDescs;
                dma.head = block + (descBytes + Line - 1) / Line * Line;
                dma.tail = dma.head + Line;
                dma.blockBytes = total;
                dmaDone.create(1u, 0u);
                gdma_channel_alloc_config_t txConfig = {};
                txConfig.direction = GDMA_CHANNEL_DIRECTION_TX;
                gdma_channel_alloc_config_t rxConfig = {};
                rxConfig.direction = GDMA_CHANNEL_DIRECTION_RX;
                if (gdma_new_axi_channel(&txConfig, &dma.tx) != ESP_OK) return false;
                if (gdma_new_axi_channel(&rxConfig, &dma.rx) != ESP_OK) return false;
                gdma_transfer_config_t transfer = {};
                transfer.max_data_burst_size = 16;
                transfer.access_ext_mem = true;
                if (gdma_config_transfer(dma.tx, &transfer) != ESP_OK) return false;
                transfer.max_data_burst_size = 0;
                if (gdma_config_transfer(dma.rx, &transfer) != ESP_OK) return false;
                if (gdma_connect(dma.tx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_AES, 0)) != ESP_OK) return false;
                if (gdma_connect(dma.rx, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_AES, 0)) != ESP_OK) return false;
                gdma_rx_event_callbacks_t callbacks = {};
                callbacks.on_recv_eof = &dmaEof;
                if (gdma_register_rx_event_callbacks(dma.rx, &callbacks, nullptr) != ESP_OK) return false;
                dma.ready = true;
                return true;
        }

        // Cache maintenance in whole lines (the neighbours' bytes written back too is harmless).
        void sync(const void *p, size_t bytes, int flags) {
                const auto   at = reinterpret_cast<uintptr_t>(p) & ~(Line - 1);
                const size_t len = (reinterpret_cast<uintptr_t>(p) + bytes - at + Line - 1) & ~(Line - 1);
                (void)esp_cache_msync(reinterpret_cast<void *>(at), len, flags);
                return;
        }

        // Chains @p bytes from @p p into @p descs (whole descriptors of up to DescBytes): returns
        // how many it used.
        size_t chain(Desc *descs, const uint8_t *p, size_t bytes) {
                size_t n = 0;
                for (size_t at = 0; at < bytes; at += DescBytes, ++n) {
                        const size_t len = bytes - at < DescBytes ? bytes - at : DescBytes;
                        Desc        &d = descs[n];
                        d.dw0.size = static_cast<uint32_t>(len);
                        d.dw0.length = static_cast<uint32_t>(len);
                        d.dw0.suc_eof = 0;
                        d.dw0.err_eof = 0;
                        d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
                        d.buffer = const_cast<uint8_t *>(p + at);
                        d.next = &descs[n + 1];
                }
                return n;
        }

        // One run through the engine (mode, key and IV set): @p bytes (at most RunBytes, whole
        // blocks) from @p in to @p out, the task asleep until the output is written.
        Error dmaRun(const uint8_t *in, uint8_t *out, size_t bytes) {
                // The output's whole lines are written in place, its partial lines at the ends
                // through the bounce lines (the DMA must not write a line the CPU may have dirty).
                const size_t lead = (Line - (reinterpret_cast<uintptr_t>(out) & (Line - 1))) & (Line - 1);
                const size_t head = lead < bytes ? lead : bytes;
                const size_t middle = (bytes - head) & ~(Line - 1);
                const size_t tail = bytes - head - middle;
                size_t       nIn = chain(dma.in, in, bytes);
                dma.in[nIn - 1].dw0.suc_eof = 1;
                dma.in[nIn - 1].next = nullptr;
                size_t nOut = 0;
                if (head > 0) nOut += chain(dma.out, dma.head, head);
                if (middle > 0) nOut += chain(dma.out + nOut, out + head, middle);
                if (tail > 0) nOut += chain(dma.out + nOut, dma.tail, tail);
                dma.out[nOut - 1].next = nullptr;
                // A cache operation costs 4-5 µs however short: the descriptors and bounce lines (one
                // block) in one, the data in one when it is done in place.
                sync(dma.in, dma.blockBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                sync(in, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                if (middle > 0 && in != out) sync(out + head, middle, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                (void)gdma_start(dma.rx, reinterpret_cast<intptr_t>(dma.out));
                (void)gdma_start(dma.tx, reinterpret_cast<intptr_t>(dma.in));
                aes_ll_set_num_blocks(bytes / Crypto::AesBlockBytes);
                aes_ll_start_transform();
                const Error err = dmaDone->acquire(DmaTimeoutMs);
                aes_ll_dma_exit();
                if (err.isError()) {
                        (void)gdma_reset(dma.tx);
                        (void)gdma_reset(dma.rx);
                        (void)dmaDone->tryAcquire(); // in case it ended just now
                        return Error::Timeout;
                }
                if (middle > 0) sync(out + head, middle, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                sync(dma.head, 2 * Line, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                std::memcpy(out, dma.head, head);
                std::memcpy(out + head + middle, dma.tail, tail);
                return Error::Ok;
        }

        // True if the DMA can read @p in and write @p out (internal DMA memory or PSRAM), word
        // aligned.
        bool dmaReaches(const uint8_t *in, const uint8_t *out, size_t bytes) {
                if (((reinterpret_cast<uintptr_t>(in) | reinterpret_cast<uintptr_t>(out)) & 3) != 0) return false;
                const auto reaches = [bytes](const uint8_t *p) {
                        return (esp_ptr_dma_capable(p) && esp_ptr_dma_capable(p + bytes - 1)) ||
                               (esp_ptr_dma_ext_capable(p) && esp_ptr_dma_ext_capable(p + bytes - 1));
                };
                return reaches(in) && reaches(out);
        }

        // The AES engine (lock held, mode and key set) by DMA, in runs; @p chain the IV or
        // counter, carried from run to run here (not read back from the engine).
        Error aesDma(Crypto::Algorithm algorithm, uint32_t chain[4], const uint8_t *in, uint8_t *out, size_t length) {
                using A = Crypto::Algorithm;
                const esp_aes_mode_t mode = algorithm == A::AesCtr ? ESP_AES_BLOCK_MODE_CTR
                                            : (algorithm == A::AesCbcEncrypt || algorithm == A::AesCbcDecrypt)
                                                    ? ESP_AES_BLOCK_MODE_CBC
                                                    : ESP_AES_BLOCK_MODE_ECB;
                aes_ll_set_block_mode(mode);
                REG_WRITE(AES_INC_SEL_REG, 1); // CTR: the 128-bit increment (SSH's), not INC32
                aes_ll_dma_enable(true);
                aes_ll_interrupt_enable(false);
                Error err = Error::Ok;
                for (size_t at = 0; at < length && err.isOk(); at += RunBytes) {
                        const size_t n = length - at < RunBytes ? length - at : RunBytes;
                        uint32_t     next[4];
                        if (algorithm == A::AesCbcDecrypt)
                                std::memcpy(next, in + at + n - Crypto::AesBlockBytes, sizeof(next));
                        if (mode != ESP_AES_BLOCK_MODE_ECB) aes_ll_set_iv(reinterpret_cast<const uint8_t *>(chain));
                        err = dmaRun(in + at, out + at, n);
                        if (algorithm == A::AesCtr)
                                advance(reinterpret_cast<uint8_t *>(chain),
                                        static_cast<uint32_t>(n / Crypto::AesBlockBytes));
                        else if (algorithm == A::AesCbcEncrypt)
                                std::memcpy(chain, out + at + n - Crypto::AesBlockBytes, sizeof(next));
                        else if (algorithm == A::AesCbcDecrypt)
                                std::memcpy(chain, next, sizeof(next));
                }
                aes_ll_dma_enable(false);
                return err;
        }

} // namespace

Error Crypto::aes(Algorithm algorithm, const uint8_t *key, size_t keyBytes, uint8_t *state, const uint8_t *in,
                  uint8_t *out, size_t length) {
        const bool ecb = algorithm == Algorithm::AesEcbEncrypt || algorithm == Algorithm::AesEcbDecrypt;
        const bool decrypt = algorithm == Algorithm::AesEcbDecrypt || algorithm == Algorithm::AesCbcDecrypt;
        if (algorithm > Algorithm::AesCtr) return Error::Invalid;
        if (keyBytes != 16 && keyBytes != 32) return Error::Invalid;
        if (length % Crypto::AesBlockBytes != 0) return Error::Invalid;
        if (key == nullptr || (!ecb && state == nullptr)) return Error::Invalid;
        if (length == 0) return Error::Ok;
        if (in == nullptr || out == nullptr) return Error::Invalid;
        Engine engine(true);
        aes_ll_dma_enable(false);
        aes_ll_set_mode(decrypt ? ESP_AES_DECRYPT : ESP_AES_ENCRYPT, static_cast<uint8_t>(keyBytes));
        (void)aes_ll_write_key(key, keyBytes / 4);
        uint32_t chain[4], block[4], result[4];
        if (!ecb) std::memcpy(chain, state, sizeof(chain));
        if (length >= DmaMinBytes && dmaReaches(in, out, length) && startDma()) {
                const Error err = aesDma(algorithm, chain, in, out, length);
                if (err.isError()) {
                        std::memset(out, 0, length);
                        return err;
                }
                if (!ecb) std::memcpy(state, chain, sizeof(chain));
                return Error::Ok;
        }
        for (size_t at = 0; at < length; at += Crypto::AesBlockBytes) {
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
