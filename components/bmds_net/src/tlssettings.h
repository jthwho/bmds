/**
 * @file      tlssettings.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TlsService's platform half (tlsservice.h): the firmware's declares and reads `net/tls/...` in
 * sysconf (tlssettings.cpp); the host tests' stands in (tests/host/faketls.cpp). Each also
 * defines `TlsService::settings()` and `TlsService::systemRoots()`.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

namespace TlsService {

        /** @brief Declares the settings (`init()` calls it once). */
        Error declareSettings();

} // namespace TlsService

BMDS_NAMESPACE_END
