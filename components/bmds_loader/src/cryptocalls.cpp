/**
 * @file      cryptocalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The crypto engines' system call (slot 112; devplan/network.md §26).
 */

#include <bmds/crypto.h>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Crypto;
using Algorithm = Crypto::Algorithm;

extern "C" int32_t bmds_k_crypto(const bmds_crypto_op_t *op) {
        bmds::KernelCall call;
        bmds_crypto_op_t o;
        if (const int32_t r = bmds::detail::copyIn(op, o); r != 0) return r;
        Algorithm algorithm;
        switch (o.algorithm) {
                case BMDS_CRYPTO_AES_ECB_ENCRYPT: algorithm = Algorithm::AesEcbEncrypt; break;
                case BMDS_CRYPTO_AES_ECB_DECRYPT: algorithm = Algorithm::AesEcbDecrypt; break;
                case BMDS_CRYPTO_AES_CBC_ENCRYPT: algorithm = Algorithm::AesCbcEncrypt; break;
                case BMDS_CRYPTO_AES_CBC_DECRYPT: algorithm = Algorithm::AesCbcDecrypt; break;
                case BMDS_CRYPTO_AES_CTR: algorithm = Algorithm::AesCtr; break;
                case BMDS_CRYPTO_SHA1: algorithm = Algorithm::Sha1; break;
                case BMDS_CRYPTO_SHA256: algorithm = Algorithm::Sha256; break;
                case BMDS_CRYPTO_SHA512: algorithm = Algorithm::Sha512; break;
                default: return -BMDS_ENOTSUP;
        }
        // A U-mode program's buffers where it could reach them (devplan/protection.md §16).
        using bmds::detail::canRead;
        using bmds::detail::canWrite;
        const bool   sha = Crypto::shaBlockBytes(algorithm) != 0;
        const bool   ecb = algorithm == Algorithm::AesEcbEncrypt || algorithm == Algorithm::AesEcbDecrypt;
        const size_t stateBytes = sha ? Crypto::shaStateBytes(algorithm) : ecb ? 0 : 16;
        if (!canRead(o.in, o.length) || (stateBytes != 0 && !canWrite(o.state, stateBytes)) ||
            (!sha && (!canRead(o.key, o.key_bytes) || !canWrite(o.out, o.length))))
                return -BMDS_EFAULT;
        const auto       *in = static_cast<const uint8_t *>(o.in);
        auto             *state = static_cast<uint8_t *>(o.state);
        const bmds::Error err = Crypto::shaBlockBytes(algorithm) != 0
                                        ? Crypto::sha(algorithm, state, in, o.length)
                                        : Crypto::aes(algorithm, static_cast<const uint8_t *>(o.key), o.key_bytes,
                                                      state, in, static_cast<uint8_t *>(o.out), o.length);
        return bmds::Process::errnoFor(err);
}
