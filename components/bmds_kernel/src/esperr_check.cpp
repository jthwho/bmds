/**
 * @file      esperr_check.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

// Compile-time check that the esp_err_t values duplicated in bmds_core
// (so it can stay free of IDF headers) match ESP-IDF. No code is generated.

#include <esp_err.h>
#include <bmds/detail/esperr.h>

namespace E = bmds::Detail::EspErr;

static_assert(E::Ok == ESP_OK);
static_assert(E::Fail == ESP_FAIL);
static_assert(E::NoMem == ESP_ERR_NO_MEM);
static_assert(E::InvalidArg == ESP_ERR_INVALID_ARG);
static_assert(E::InvalidState == ESP_ERR_INVALID_STATE);
static_assert(E::InvalidSize == ESP_ERR_INVALID_SIZE);
static_assert(E::NotFound == ESP_ERR_NOT_FOUND);
static_assert(E::NotSupported == ESP_ERR_NOT_SUPPORTED);
static_assert(E::Timeout == ESP_ERR_TIMEOUT);
static_assert(E::InvalidResponse == ESP_ERR_INVALID_RESPONSE);
static_assert(E::InvalidCrc == ESP_ERR_INVALID_CRC);
static_assert(E::InvalidVersion == ESP_ERR_INVALID_VERSION);
static_assert(E::InvalidMac == ESP_ERR_INVALID_MAC);
static_assert(E::NotFinished == ESP_ERR_NOT_FINISHED);
static_assert(E::NotAllowed == ESP_ERR_NOT_ALLOWED);
