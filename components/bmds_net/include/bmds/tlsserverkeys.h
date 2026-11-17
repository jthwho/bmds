/**
 * @file      tlsserverkeys.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The board's own TLS certificate (devplan/network.md §37a S3): an ECDSA P-256 key and a
 *        self-signed certificate for the host name, PEM files in the folder `net/tls/server-keys`
 *        names (`server-key.pem`, `server-cert.pem`), made the first time a server session wants
 *        them. Programs never read the key: a TLS server that gives no certificate of its own gets
 *        these, handed to its session by the kernel.
 *
 * @par Thread Safety
 * Thread-safe: making them is serialized; reads see whole files (each written under a temporary
 * name and renamed).
 */
namespace TlsServerKeys {

        /** @brief A certificate's description (`describe()`). */
        struct Description {
                        char    subject[128] = {};
                        uint8_t sha256[32] = {}; ///< Of its DER: what a client pins.
                        int64_t notBefore = 0;   ///< Unix seconds.
                        int64_t notAfter = 0;
                        char    folder[128] = {}; ///< Where the files are.
        };

        /**
         * @brief The certificate and key as PEM, made first if either file is missing.
         * @return `Ok`, or the file system's errors, `NoMem`, `IoError` (a file that doesn't parse
         *         or couldn't be made): @p why says which.
         * @blocking Making them: about a second at most (a key, a signature, two files).
         */
        Error pem(String &cert, String &key, String &why);

        /**
         * @brief Makes a new key and certificate in place of the old (clients that pinned the old
         *        one will refuse the new until told).
         * @blocking
         */
        Error renew(String &why);

        /** @brief Describes the certificate, made first if missing. @blocking */
        Error describe(Description &out, String &why);

} // namespace TlsServerKeys

BMDS_NAMESPACE_END
