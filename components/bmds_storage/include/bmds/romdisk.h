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
#include <bmds/romeditor.h>
#include <bmds/rommanifest.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/rom`: the files that come with the firmware, read-only, in the
 *        flash partition named `rom` (`devplan/rom.md`).
 *
 * The partition is mapped through the flash cache and is the block device
 * `rom0`; its image is mounted at `/rom` for good, its files read where they
 * lie (`Vfs::map()`). `idf.py flash` writes the image the firmware was built
 * with. The tree is the newest good version for this firmware (§12e); its
 * system files are compared with the firmware's manifest at boot, and what
 * differs is logged, the image mounted anyway. A blank or damaged partition leaves no `/rom`, and the system runs
 * on without it.
 *
 * @par Thread Safety
 * `init()` once at boot; the rest from any task after it.
 */
namespace RomDisk {

        /** @brief Where it is mounted. */
        inline constexpr const char *Path = "/rom";

        /**
         * @brief Where `rom repack` leaves its request for recovery, which repacks the ROM at its
         *        boot and removes it (devplan/rom.md §12s): beside the update's intent.
         */
        inline constexpr const char *RepackPath = "/flash/update/rom-repack";

        /** @brief The partition's label. */
        inline constexpr const char *PartitionLabel = "rom";

        /**
         * @brief Boot stage: maps the partition and mounts its image at `/rom`.
         * @return `Ok` (also when there is no image: told in the log).
         */
        Error init();

        /** @brief Returns the image mounted, or null (none). */
        const RomImage *image();

        /** @brief Returns true if the tree's system files are the firmware's manifest's. */
        bool matchesFirmware();

        /** @brief Returns the tree booted compared with the manifest, counted (`manifest()`). */
        RomManifest::Counts manifestCounts();

        /** @brief Returns the system files the firmware was built with, sorted by path (§12f). */
        Span<const RomManifestEntry> manifest();

        /** @brief Returns this firmware's build id (its ELF's SHA-256), as a tree version names it. */
        Span<const uint8_t> firmware();

        /**
         * @brief Returns the whole partition as mapped (empty: none): what is in flash
         *        now, a tree written since boot included (the pending one, §12b).
         */
        ByteSpan mapped();

        /** @brief A change to the ROM's tree, run by `edit()` with an editor loaded. */
        using EditFn = Error (*)(RomEditor &editor, void *context);

        /** @brief The stack a change runs on (internal RAM: flash writes turn the caches off). */
        inline constexpr size_t EditStackBytes = 12 * 1024;

        /**
         * @brief Runs @p fn with an editor of the ROM's tree (`devplan/rom.md` §12b): one
         *        change at a time, on a thread with an internal stack, the booted tree never
         *        written. Each change is a pending tree the next boot takes. The caller waits
         *        first while flash writes are held (`FlashWrites`: audio playing).
         * @return `fn`'s result, `NoMedia` (no ROM mounted), or `NoMem` (no thread).
         * @blocking
         */
        Error edit(EditFn fn, void *context);

        /** @brief Returns the partition's size in bytes (0: no partition). */
        size_t partitionBytes();

} // namespace RomDisk

BMDS_NAMESPACE_END
