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
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class ProgramLauncher;

/** @brief The service's own (guiservice.cpp), for its connections. */
namespace GuiServicePriv {

        /** @brief A connection's slot is free again (its loop's thread, at its end). */
        void released(uint8_t slot);

        /** @brief The loader (`GuiService::init()`'s). */
        ProgramLauncher &launcher();

        /** @brief True if program @p path may use the `SHELL` extension (`gui/shell-clients`). */
        bool shellAllowed(StringView path);

        /** @brief A remote program's connection (@p end: its accepted socket) started (guiservice.cpp). */
        Error openRemote(FileHandle::Ptr end);

        /**
         * @brief True if a `Hello`'s authentication (@p name, @p data) is the user's cookie
         *        (`gui/cookie`, guilistener.cpp), compared in constant time.
         */
        bool cookieMatches(StringView name, ByteSpan data);

} // namespace GuiServicePriv

BMDS_NAMESPACE_END
