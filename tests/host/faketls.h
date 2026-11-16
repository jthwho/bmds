/**
 * @file      faketls.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The host's TLS stand-in (faketls.cpp): the settings TlsService reads, for a test to change.
 */

#pragma once

#include <bmds/tlsservice.h>

BMDS_NAMESPACE_BEGIN

namespace FakeTls {

        /** @brief What `TlsService::settings()` returns. */
        extern TlsService::Settings settings;

} // namespace FakeTls

BMDS_NAMESPACE_END
