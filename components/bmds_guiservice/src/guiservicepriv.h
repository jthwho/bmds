/**
 * @file      guiservicepriv.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/** @brief The service's own (guiservice.cpp), for its connections. */
namespace GuiServicePriv {

        /** @brief A connection's slot is free again (its loop's thread, at its end). */
        void released(uint8_t slot);

} // namespace GuiServicePriv

BMDS_NAMESPACE_END
