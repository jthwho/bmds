/**
 * @file      tlsgcm.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief AES-GCM for mbedTLS with the 8-bit GHASH (devplan/network.md §33): ESP-IDF's one-shot
 *        `esp_aes_gcm_crypt_and_tag()` and `esp_aes_gcm_auth_decrypt()` wrapped (`--wrap`).
 *
 * A record whose buffers the AES engine's DMA reaches (internal memory) goes to ESP-IDF's whole
 * hardware path as before; any other (mbedTLS's are in PSRAM) has its counter mode done by
 * ESP-IDF's engine call and its GHASH by `Ghash`, in place of ESP-IDF's 4-bit software one.
 *
 * @par Thread Safety
 * `init()` once at start; the wrapped calls from any task (the tables under a mutex).
 */
namespace TlsGcm {

        /** @brief Makes the tables' cache. Without it the wrapped calls are ESP-IDF's. @return `Ok`, `NoMem`. */
        Error init();

} // namespace TlsGcm

BMDS_NAMESPACE_END
