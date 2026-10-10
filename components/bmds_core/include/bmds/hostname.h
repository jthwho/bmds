/**
 * @file      hostname.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's host name, pure (`devplan/notifications.md` §2): the
 *        configured one (`system/hostname`), or `bmds-` and the serial number's
 *        last six characters when it is empty.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace HostName {

        /** @brief The longest host name, in bytes (one DNS label). */
        constexpr size_t MaxBytes = 63;

        /** @brief What the default puts before the serial number. */
        constexpr const char *DefaultPrefix = "bmds-";

        /** @brief How much of the serial number the default takes: its end. */
        constexpr size_t DefaultSerialChars = 6;

        /**
         * @brief Returns true if @p name may be configured: empty (the default), or
         *        1 to `MaxBytes` letters, digits and `-`, not starting or ending
         *        with `-` (RFC 1123).
         */
        bool isValid(StringView name);

        /**
         * @brief Writes the host name into @p out: @p configured if it isn't empty,
         *        else `DefaultPrefix` and the last `DefaultSerialChars` of @p serial's
         *        letters and digits, in lower case (a board's serial may have others);
         *        just `bmds` without a serial.
         * @return Its length.
         */
        size_t resolve(StringView configured, StringView serial, char (&out)[MaxBytes + 1]);

} // namespace HostName

BMDS_NAMESPACE_END
