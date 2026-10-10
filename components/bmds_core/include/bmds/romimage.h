/**
 * @file      romimage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads a ROM image (`BMRO`, `devplan/rom.md` §3c; written by
 *        `tools/mkrom.py`): a read-only tree of files in memory, used where it
 *        lies (the flash mapped through the cache), nothing copied.
 *
 * Little-endian. An 80-byte header (`BMRO`, format 1, the header's size, the
 * image's size, the entry count, where the entries and names are, the
 * BLAKE2s-256 of everything after the header, the build time, a CRC-32 of the
 * header); then 16-byte entries; then the names; then the files' bytes.
 *
 * Entry 0 is the root directory. A directory's children are contiguous,
 * sorted by name (bytes, as `memcmp`), and come after it; the directories'
 * child ranges, taken in entry order, follow one another and cover every
 * entry but the root, so the entries are a tree. `open()` checks all of
 * this once, so nothing after it can read outside the image.
 *
 * @par Thread Safety
 * A value over memory it doesn't own (kept as long as the image is).
 */
class RomImage {
        public:
                /** @brief The header's size in format 1. */
                static constexpr size_t HeaderBytes = 80;

                /** @brief An entry's size. */
                static constexpr size_t EntryBytes = 16;

                /** @brief The content hash's size (BLAKE2s-256). */
                static constexpr size_t HashBytes = 32;

                /** @brief The longest name of an entry. */
                static constexpr size_t MaxName = 255;

                /** @brief The most entries an image may have. */
                static constexpr uint32_t MaxEntries = 1u << 20;

                /** @brief The alignment of a mappable file's bytes (a page, `devplan/rom.md` §6). */
                static constexpr uint32_t MapAlign = 4096;

                /** @brief No entry (`findChild()`). */
                static constexpr uint32_t NoEntry = UINT32_MAX;

                /** @brief An entry's flags. */
                enum Flag : uint8_t {
                        System = 0x01,   ///< FAT's System attribute (an emulated symbolic link, storage.md §4).
                        Mappable = 0x02, ///< Its bytes start on a `MapAlign` boundary.
                };

                /** @brief One entry, as found. */
                struct Entry {
                                StringView name;              ///< Its name ("" for the root).
                                bool       directory = false; ///< A directory, else a file.
                                uint8_t    flags = 0;         ///< `Flag` bits.
                                uint32_t   first = 0;         ///< A directory's first child.
                                uint32_t   count = 0;         ///< A directory's children.
                                ByteSpan   data;              ///< A file's bytes.
                };

                /** @brief An empty image (nothing found in it). */
                constexpr RomImage() = default;

                /** @brief True if @p bytes start like an image (its magic): worth `open()`. */
                static bool looksLike(ByteSpan bytes);

                /**
                 * @brief Checks the image at the start of @p bytes (which may run on
                 *        past it, as a partition does).
                 * @return The image, `CorruptData` (not an image, or damaged), or
                 *         `NotSupported` (a later format).
                 */
                static Result<RomImage> open(ByteSpan bytes);

                /** @brief Returns the number of entries (the root included). */
                uint32_t count() const { return _count; }

                /** @brief Returns entry @p index (less than `count()`). */
                Entry entry(uint32_t index) const;

                /** @brief Returns the child of directory @p dir named @p name, or `NoEntry`. */
                uint32_t findChild(uint32_t dir, StringView name) const;

                /**
                 * @brief Finds @p path ("" or "/" the root; "/a/b" or "a/b").
                 * @param noPath Set when a directory along the way is missing or is a
                 *        file (the VFS then looks for a link along it).
                 * @return The entry's index, or `NotExist`.
                 */
                Result<uint32_t> find(StringView path, bool &noPath) const;

                /** @brief Returns the image (its header to its last byte). */
                ByteSpan bytes() const { return _bytes; }

                /** @brief Returns the content hash the header gives. */
                Span<const uint8_t> hash() const { return _bytes.mid(32, HashBytes); }

                /** @brief Returns the build time (every file's time). */
                Timestamp buildTime() const { return _buildTime; }

                /**
                 * @brief Hashes the content and compares it with the header's.
                 * @return `Ok` or `CorruptData`.
                 */
                Error verify() const;

        private:
                const uint8_t *entryAt(uint32_t index) const { return _bytes.data() + _entries + index * EntryBytes; }
                StringView     nameOf(const uint8_t *e) const;

                ByteSpan  _bytes;
                uint32_t  _count = 0;
                uint32_t  _entries = 0; // offset of entry 0
                uint32_t  _names = 0;   // offset of the names
                Timestamp _buildTime;
};

BMDS_NAMESPACE_END
