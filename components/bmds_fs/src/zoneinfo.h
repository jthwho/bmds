/**
 * @file      zoneinfo.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: the time zone database behind /share/zoneinfo and
 * `Vfs::zoneRules()` (devplan/time.md §9). Everything here expects the
 * storage lock.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/zonebundle.h>

BMDS_NAMESPACE_BEGIN

namespace Zones {

        /** @brief What a name below `/share/zoneinfo` is. */
        struct Found {
                        ZoneBundle::Entry entry;             ///< A zone: its data.
                        bool              directory = false; ///< A directory of names (or the root).
                        Timestamp         modified;          ///< The database's.
        };

        /**
         * @brief Looks @p name ("Europe/Berlin", "America", "" for the root)
         *        up. The root always exists (empty without a database).
         * @return `Ok` or `NotExist`.
         */
        Error findLocked(StringView name, Found &found);

        /** @brief Bytes of a child record: its type (`d` or `f`), its size (u32), its name, a NUL. */
        constexpr size_t RecordHeader = 5;

        /**
         * @brief The names directly below directory @p name ("" for the root)
         *        as records (`RecordHeader`) in @p out.
         * @return `Ok` (none without a database), `NoMem`, or an error reading.
         */
        Error listLocked(StringView name, String &out);

        /** @brief Opens a zone (its data's place in the database): its bytes in a read-only, seekable stream. */
        Result<FileHandle::Ptr> openLocked(uint32_t offset, uint32_t length);

        /** @brief A file changed (`Fat::changedLocked()`): forgets a lookup of the database. */
        void changedLocked(uint32_t device, uint64_t id);

        /** @brief A filesystem was mounted or unmounted: forgets the lookup. */
        void mountsChangedLocked();

        /** @brief Makes `Vfs::zoneRules()` the kernel's zone resolver (`Vfs::init()`; not locked). */
        void installResolver();

} // namespace Zones

BMDS_NAMESPACE_END
