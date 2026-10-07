/**
 * @file      programicons.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A program's own icon (`devplan/gui.md` §4j), as Windows keeps one in
 *        an `.exe`: a built-in program's from its `/bin` entry
 *        (`BMDS_PROGRAM_ICONS`), a program file's from its `BMDS_NOTE_ICON`
 *        notes. Nothing needs installing beside the program.
 *
 * @par Thread Safety
 * Reentrant (the VFS's rules for the file).
 */
namespace ProgramIcons {

        /** @brief An icon's image as read: a PNG in memory the caller frees (`Memory::free`). */
        struct Image {
                        uint8_t *png = nullptr;
                        size_t   bytes = 0;
                        uint16_t width = 0;
                        uint16_t height = 0;
        };

        /** @brief The largest image read. */
        constexpr size_t MaxImageBytes = 64 * 1024;

        /**
         * @brief Reads the icon of the program at @p path (as the launcher runs
         *        it: a path, symbolic links followed) nearest @p side pixels (that
         *        size, else the next bigger, else the biggest) into memory from
         *        @p space.
         * @return `Ok`, `NotFound` (no program there, or it has no icon),
         *         `NoMem`, or an error reading.
         */
        Error read(StringView path, const Vfs::Context &context, uint16_t side, MemSpace space, Image &out);

} // namespace ProgramIcons

BMDS_NAMESPACE_END
