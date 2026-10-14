/**
 * @file      flashdefaults.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_storage: the files of the repository's flash/ directory, embedded by
 * tools/embedfiles.py (devplan/gui.md G14): those below cfg/ are installed in /cfg (Cfg), the
 * others in /flash (FlashDisk).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace FlashDefaults {

        /** @brief A default file: its path below the repository's flash/ and its bytes. */
        struct File {
                        const char    *path;
                        const uint8_t *data;
                        uint32_t       size;
        };

        /** @brief The files, sorted by path. */
        extern const File files[];

        /** @brief How many. */
        extern const size_t count;

        /**
         * @brief Installs the files whose path starts with @p prefix ("cfg/"), or (@p prefix
         *        empty) every file not below `cfg/`, below @p root: each one not there and not
         *        listed in @p root's `.defaults` is written, and listed, so a new release's
         *        defaults reach an older directory, and a default deleted stays deleted.
         */
        void install(StringView prefix, StringView root);

} // namespace FlashDefaults

BMDS_NAMESPACE_END
