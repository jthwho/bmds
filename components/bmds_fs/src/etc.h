/**
 * @file      etc.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the files of /etc (devplan/network.md §8, §18), built
 * from the settings by other components when opened (Vfs::addEtcFile).
 */

#pragma once

#include <cstddef>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace Etc {

        /** @brief Adds @p file (`Vfs::addEtcFile()`). */
        Error add(const Vfs::EtcFile &file);

        /** @brief Returns the files added so far. */
        size_t count();

        /** @brief Returns file @p index (below `count()`). */
        const Vfs::EtcFile &at(size_t index);

        /** @brief Returns the file named @p name, or null. */
        const Vfs::EtcFile *find(StringView name);

        /**
         * @brief Builds @p file and opens what it holds, read-only and seekable.
         *        Without the VFS's lock.
         * @return The handle, or the generator's error, `NoMem`.
         */
        Result<FileHandle::Ptr> open(const Vfs::EtcFile &file);

} // namespace Etc

BMDS_NAMESPACE_END
