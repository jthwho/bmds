/**
 * @file      romspace.h
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
#include <bmds/romimage.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief How the ROM's partition is used (`devplan/rom.md` §12d): its
 *        regions from start to end, each sector's state, and the totals.
 *
 * Built from the tree booted and, if there is one, the pending tree (a
 * newer version written since boot, which the next boot takes). Space
 * neither uses is usable now; space only the booted one uses is usable after
 * the next boot (§12b). Space is counted in whole 4 KB sectors (what can be
 * erased and written); the free bytes that aren't whole sectors are
 * padding.
 *
 * @par Thread Safety
 * A value its builder owns; `build()` allocates in PSRAM.
 */
class RomSpace {
        public:
                /** @brief Which trees use a file. */
                enum Tree : uint8_t {
                        Booted = 0x01,  ///< The tree booted.
                        Pending = 0x02, ///< The pending tree.
                };

                /** @brief What a region is. */
                enum class Kind : uint8_t {
                        Header,   ///< The partition's header (its sector).
                        Log,      ///< The log of tree versions.
                        Reserved, ///< Between the log and the files' space (unused).
                        File,     ///< A file's bytes.
                        Free,     ///< Bytes no tree uses.
                };

                /** @brief One region, in the order of the partition. */
                struct Region {
                                Kind            kind = Kind::Free;         ///< What it is.
                                uint32_t        offset = 0;                ///< Where it starts.
                                uint32_t        bytes = 0;                 ///< Its size.
                                uint8_t         trees = 0;                 ///< A file's `Tree` bits.
                                bool            overlaps = false;          ///< A file overlapping another of its tree.
                                const RomImage *image = nullptr;           ///< A file's tree (the booted one if both).
                                uint32_t        index = RomImage::NoEntry; ///< A file's entry in `image`.
                };

                /** @brief A sector's state bits. */
                enum SectorFlag : uint8_t {
                        UsedNow = 0x01,   ///< Used by a tree, or the header and log: not free now.
                        UsedAfter = 0x02, ///< Used after the next boot (the pending tree, else the booted).
                };

                /** @brief The totals. */
                struct Summary {
                                uint64_t partitionBytes = 0;  ///< The partition.
                                uint64_t reservedBytes = 0;   ///< The header, the log and anything before the files.
                                uint64_t filesBytes = 0;      ///< The booted tree's files, added up.
                                uint64_t freeNow = 0;         ///< Whole free sectors now.
                                uint64_t freeAfterBoot = 0;   ///< Whole free sectors after the next boot.
                                uint64_t paddingBytes = 0;    ///< Free bytes that aren't whole free sectors.
                                uint64_t holeBytes = 0;       ///< Free sectors now outside the largest run.
                                uint64_t largestNow = 0;      ///< The largest run of free sectors now.
                                uint64_t largestPagesNow = 0; ///< The largest run of free 64 KB pages now.
                                uint32_t files = 0;           ///< File regions.
                                uint32_t overlaps = 0;        ///< Files overlapping another (a damaged layout).
                };

                constexpr RomSpace() = default;
                ~RomSpace();
                RomSpace(const RomSpace &) = delete;
                RomSpace &operator=(const RomSpace &) = delete;

                /**
                 * @brief Lays out @p booted's partition, with @p pending (null: none).
                 * @return `Ok` or `NoMem`.
                 */
                Error build(const RomImage &booted, const RomImage *pending);

                /** @brief Returns the number of regions. */
                size_t regionCount() const { return _regionCount; }

                /** @brief Returns region @p index (less than `regionCount()`). */
                const Region &region(size_t index) const { return _regions[index]; }

                /** @brief Returns the number of whole sectors in the partition. */
                uint32_t sectorCount() const { return _sectorCount; }

                /** @brief Returns sector @p index's `SectorFlag` bits. */
                uint8_t sectorFlags(uint32_t index) const { return _sectors[index].flags; }

                /** @brief Returns how many files sector @p index holds part of (at most 255). */
                uint8_t sectorFiles(uint32_t index) const { return _sectors[index].files; }

                /** @brief Returns the totals. */
                const Summary &summary() const { return _summary; }

        private:
                struct Sector {
                                uint8_t flags;
                                uint8_t files;
                };

                void release();

                Region  *_regions = nullptr;
                size_t   _regionCount = 0;
                Sector  *_sectors = nullptr;
                uint32_t _sectorCount = 0;
                Summary  _summary;
};

BMDS_NAMESPACE_END
