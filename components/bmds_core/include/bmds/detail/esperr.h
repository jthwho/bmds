/**
 * @file      esperr.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Numeric values of the ESP-IDF `esp_err_t` codes that `Error::fromEspErr()`
 * maps. Duplicated here so bmds_core stays free of IDF headers; the kernel
 * build static_asserts every value against `esp_err.h`.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace Detail::EspErr {

        inline constexpr int32_t Ok = 0;
        inline constexpr int32_t Fail = -1;
        inline constexpr int32_t NoMem = 0x101;
        inline constexpr int32_t InvalidArg = 0x102;
        inline constexpr int32_t InvalidState = 0x103;
        inline constexpr int32_t InvalidSize = 0x104;
        inline constexpr int32_t NotFound = 0x105;
        inline constexpr int32_t NotSupported = 0x106;
        inline constexpr int32_t Timeout = 0x107;
        inline constexpr int32_t InvalidResponse = 0x108;
        inline constexpr int32_t InvalidCrc = 0x109;
        inline constexpr int32_t InvalidVersion = 0x10A;
        inline constexpr int32_t InvalidMac = 0x10B;
        inline constexpr int32_t NotFinished = 0x10C;
        inline constexpr int32_t NotAllowed = 0x10D;

} // namespace Detail::EspErr

BMDS_NAMESPACE_END
