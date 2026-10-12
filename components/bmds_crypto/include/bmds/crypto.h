/**
 * @file      crypto.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The chip's AES and SHA engines (devplan/network.md §26).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The AES and SHA engines, in register mode: the CPU writes each block and reads it back,
 *        so no DMA, no cache maintenance and no allocation.
 *
 * Stateless: each call brings its key and its state (an IV, a counter, a hash's chaining value)
 * and gets the state back, so the kernel keeps neither secrets nor handles. One lock serializes
 * the two engines (ESP-IDF's, shared with its own users); a call holds it for its blocks only.
 *
 * Thread-safe: any task may call; calls wait for each other.
 */
class Crypto {
        public:
                /** @brief What a call does. */
                enum class Algorithm : uint8_t {
                        AesEcbEncrypt, ///< AES, each block alone.
                        AesEcbDecrypt, ///< AES, each block alone, deciphered.
                        AesCbcEncrypt, ///< AES-CBC; the state is the IV, then the last block out.
                        AesCbcDecrypt, ///< AES-CBC deciphered; the state is the IV, then the last block in.
                        AesCtr,        ///< AES-CTR; the state is a 128-bit big-endian counter.
                        Sha1,          ///< SHA-1's compression of whole 64-byte blocks.
                        Sha256,        ///< SHA-256's (and so SHA-224's) of whole 64-byte blocks.
                        Sha512,        ///< SHA-512's (and so SHA-384's) of whole 128-byte blocks.
                };

                /** @brief An AES block and its CBC and CTR state: bytes. */
                static constexpr size_t AesBlockBytes = 16;

                /**
                 * @brief Runs AES over whole blocks with the engine.
                 * @param algorithm One of the `Aes...` algorithms.
                 * @param key The key: @p keyBytes bytes.
                 * @param keyBytes 16 or 32 (the engine has no AES-192).
                 * @param state CBC: the IV, replaced by what chains to the next call; CTR: the
                 *        first block's counter, replaced by the one after the last block; ECB:
                 *        unused (may be null).
                 * @param in The input: @p length bytes.
                 * @param out The output: @p length bytes; may be @p in.
                 * @param length Bytes: a multiple of `AesBlockBytes` (0 does nothing).
                 * @return `Error::Ok`, or `Error::Invalid` (a wrong algorithm, key size or length, a
                 *         missing pointer).
                 * @blocking Waits for the engines' lock; then about 1 µs per block.
                 */
                static Error aes(Algorithm algorithm, const uint8_t *key, size_t keyBytes, uint8_t *state,
                                 const uint8_t *in, uint8_t *out, size_t length);

                /**
                 * @brief Runs a SHA's compression over whole blocks from a chaining value.
                 *
                 * The padding and the initial values are the caller's, so SHA-224 and SHA-384 are
                 * SHA-256's and SHA-512's blocks from their own initial values.
                 * @param algorithm `Sha1`, `Sha256` or `Sha512`.
                 * @param state The chaining value as the digest is written: the words big-endian,
                 *        in order (20, 32 or 64 bytes); replaced by the value after the blocks.
                 * @param in The blocks: @p length bytes.
                 * @param length Bytes: a multiple of `shaBlockBytes()` (0 does nothing).
                 * @return `Error::Ok`, or `Error::Invalid` (a wrong algorithm or length, a missing
                 *         pointer).
                 * @blocking Waits for the engines' lock; then about 1.5 µs per 64 bytes.
                 */
                static Error sha(Algorithm algorithm, uint8_t *state, const uint8_t *in, size_t length);

                /** @brief A SHA's block in bytes (64, or 128 for SHA-512); 0 if not a SHA. */
                static constexpr size_t shaBlockBytes(Algorithm algorithm) {
                        return algorithm == Algorithm::Sha512                                   ? 128
                               : algorithm == Algorithm::Sha1 || algorithm == Algorithm::Sha256 ? 64
                                                                                                : 0;
                }

                /** @brief A SHA's chaining value in bytes (20, 32 or 64); 0 if not a SHA. */
                static constexpr size_t shaStateBytes(Algorithm algorithm) {
                        return algorithm == Algorithm::Sha1     ? 20
                               : algorithm == Algorithm::Sha256 ? 32
                               : algorithm == Algorithm::Sha512 ? 64
                                                                : 0;
                }
};

BMDS_NAMESPACE_END
