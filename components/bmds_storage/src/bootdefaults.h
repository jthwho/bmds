/**
 * @file      bootdefaults.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_storage: the files of the repository's boot/ directory,
 * embedded by tools/embedfiles.py, that BootDisk installs on /boot
 * (devplan/gui.md G14).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace BootDefaults {

        /** @brief A default file: its path below /boot and its bytes. */
        struct File {
                        const char    *path;
                        const uint8_t *data;
                        uint32_t       size;
        };

        /** @brief The files, sorted by path. */
        extern const File files[];

        /** @brief How many. */
        extern const size_t count;

} // namespace BootDefaults

BMDS_NAMESPACE_END
