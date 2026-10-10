/**
 * @file      romdisk.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/romimage.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/rom`: the files that come with the firmware, read-only, in the
 *        flash partition named `rom` (`devplan/rom.md`).
 *
 * The partition is mapped through the flash cache and is the block device
 * `rom0`; its image is mounted at `/rom` for good, its files read where they
 * lie (`Vfs::map()`). `idf.py flash` writes the image the firmware was built
 * with; its content hash is compared with the firmware's at boot, and a
 * mismatch (one flashed from another build) is logged, the image mounted
 * anyway. A blank or damaged partition leaves no `/rom`, and the system runs
 * on without it.
 *
 * @par Thread Safety
 * `init()` once at boot; the rest from any task after it.
 */
namespace RomDisk {

        /** @brief Where it is mounted. */
        inline constexpr const char *Path = "/rom";

        /** @brief The partition's label. */
        inline constexpr const char *PartitionLabel = "rom";

        /**
         * @brief Boot stage: maps the partition and mounts its image at `/rom`.
         * @return `Ok` (also when there is no image: told in the log).
         */
        Error init();

        /** @brief Returns the image mounted, or null (none). */
        const RomImage *image();

        /** @brief Returns true if the image's content hash is the one the firmware was built with. */
        bool matchesFirmware();

        /** @brief Returns the partition's size in bytes (0: no partition). */
        size_t partitionBytes();

} // namespace RomDisk

BMDS_NAMESPACE_END
