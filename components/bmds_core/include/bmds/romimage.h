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
 * @brief Reads the ROM's partition (`BMRO` format 2, `devplan/rom.md` §12c;
 *        written by `tools/mkrom.py`): files at fixed places in the partition,
 *        used where they lie (the flash mapped through the cache), nothing
 *        copied.
 *
 * Little-endian. The partition is:
 *
 * - **A header** (64 bytes, at 0): `BMRO`, the format (2), where the log of
 *   tree versions is and how large, where the files' space starts, a CRC-32.
 * - **The log** (whole 4 KB sectors): tree versions, each starting on a
 *   sector. A version is a header (`BMRT`: its sequence number, when it was
 *   written, the firmware it needs), its entries, the paths, and a CRC-32 of
 *   all of them. The newest version whose CRC and contents are good is the
 *   tree; the others are older trees or a change cut short.
 * - **The files' space:** each file's bytes at an offset aligned to its own
 *   alignment (a power of two: 64 bytes, a 4 KB page, the MMU's 64 KB page).
 *
 * The entries are files only, sorted by their whole path (bytes, as
 * `memcmp`). Directories are implied: one exists while a path is under it,
 * so finding a path is a binary search, and a directory's files are one run
 * of the table. `open()` checks the chosen version whole, so nothing after it
 * reads outside the partition.
 *
 * `writeHeader()` and `writeVersion()` make the partition's header and a tree
 * version (as `mkrom.py` does, byte for byte), for recovery and the main
 * firmware to write.
 *
 * @par Thread Safety
 * A value over memory it doesn't own (kept as long as the partition is).
 */
class RomImage {
        public:
                /** @brief The partition header's size. */
                static constexpr size_t HeaderBytes = 64;

                /** @brief A tree version's header size. */
                static constexpr size_t VersionHeaderBytes = 80;

                /** @brief An entry's size. */
                static constexpr size_t EntryBytes = 64;

                /** @brief A file's hash size (BLAKE2s-256). */
                static constexpr size_t HashBytes = 32;

                /** @brief The longest path of a file (from the root, without a leading `/`). */
                static constexpr size_t MaxPath = 255;

                /** @brief The flash's erase sector: the log's unit, where each version starts. */
                static constexpr uint32_t SectorBytes = 4096;

                /** @brief The MMU's page for flash. */
                static constexpr uint32_t PageBytes = 65536;

                /** @brief The largest alignment a file may ask for, as its log2 (16 MB). */
                static constexpr uint8_t MaxAlignLog2 = 24;

                /** @brief No entry (`nextChild()`'s end). */
                static constexpr uint32_t NoEntry = UINT32_MAX;

                /** @brief An entry's flags. */
                enum Flag : uint8_t {
                        System = 0x01, ///< FAT's System attribute (an emulated symbolic link, storage.md §4).
                        User = 0x02,   ///< The user's file (installed with `rom install`); else the system's.
                };

                /** @brief One file, as found. */
                struct Entry {
                                StringView          path;          ///< Its path from the root, no leading `/`.
                                StringView          name;          ///< Its last component.
                                uint8_t             flags = 0;     ///< `Flag` bits.
                                uint8_t             alignLog2 = 0; ///< Its offset's alignment, as log2.
                                uint32_t            offset = 0;    ///< Where its bytes are in the partition.
                                ByteSpan            data;          ///< Its bytes.
                                Timestamp           modified;      ///< Its time.
                                Span<const uint8_t> hash;          ///< Its bytes' BLAKE2s-256.
                };

                /**
                 * @brief A path found: a file, or a directory and its files.
                 *
                 * A directory's files (all under it, at any depth) are entries
                 * `[first, end)`; `prefixBytes` is its path's length with the `/`
                 * after it (0 for the root).
                 */
                struct Node {
                                bool     directory = false; ///< A directory, else a file.
                                uint32_t index = 0;         ///< A file's entry.
                                uint32_t first = 0;         ///< A directory's first entry.
                                uint32_t end = 0;           ///< A directory's entry after its last.
                                uint32_t prefixBytes = 0;   ///< A directory's path and its `/`.
                };

                /** @brief One child of a directory (`nextChild()`). */
                struct Child {
                                StringView name; ///< Its name.
                                Node       node; ///< What it is (a file's entry, or a directory's run).
                };

                /** @brief The tree version in use. */
                struct Version {
                                uint32_t            offset = 0;   ///< Where it is in the partition.
                                uint32_t            bytes = 0;    ///< Its size (header, entries, paths, CRC).
                                uint32_t            sequence = 0; ///< Its sequence number (newer is larger).
                                Timestamp           written;      ///< When it was written.
                                Span<const uint8_t> firmware;     ///< The firmware build it needs (zeros: any).
                };

                /** @brief What a sector of the log holds (`logRecord()`). */
                struct LogRecord {
                                /** @brief What it is. */
                                enum class Kind : uint8_t {
                                        Version, ///< A version, whole and good.
                                        Wrong,   ///< A version whole (its CRC) but with entries refused.
                                        Damaged, ///< A version's start, not whole (cut short or damaged).
                                        Erased,  ///< An erased sector (0xff), free for the next version.
                                        Other,   ///< Anything else (a damaged version's later sectors).
                                };

                                Kind                kind = Kind::Other; ///< What it is.
                                uint32_t            offset = 0;         ///< Where it starts in the partition.
                                uint32_t            bytes = 0;          ///< A version's size, else a sector.
                                uint32_t            sequence = 0;       ///< A version's sequence (as its header says).
                                uint32_t            count = 0;          ///< A whole version's file count.
                                Timestamp           written;            ///< A whole version's time.
                                Span<const uint8_t> firmware;           ///< A whole version's firmware (zeros: any).
                };

                /** @brief One file of a tree version to write (`writeVersion()`). */
                struct NewEntry {
                                StringView          path;          ///< Its path from the root, no leading `/`.
                                uint8_t             flags = 0;     ///< `Flag` bits.
                                uint8_t             alignLog2 = 6; ///< Its offset's alignment, as log2.
                                uint32_t            offset = 0;    ///< Where its bytes are in the partition.
                                uint32_t            size = 0;      ///< Its size.
                                Timestamp           modified;      ///< Its time (whole seconds are kept).
                                Span<const uint8_t> hash;          ///< Its bytes' BLAKE2s-256 (`HashBytes`).
                };

                /** @brief An empty image (nothing found in it). */
                constexpr RomImage() = default;

                /**
                 * @brief Writes a partition header: the log at @p logOffset, @p logBytes
                 *        long, the files' space from @p dataOffset.
                 * @return `Ok`, `Invalid` (not whole sectors, or overlapping), or
                 *         `BufferTooSmall` (@p out under `HeaderBytes`).
                 */
                static Error writeHeader(uint32_t logOffset, uint32_t logBytes, uint32_t dataOffset,
                                         WritableByteSpan out);

                /** @brief Returns the size of a tree version holding @p entries. */
                static size_t versionBytes(Span<const NewEntry> entries);

                /**
                 * @brief Writes a tree version of @p entries (sorted by path, as
                 *        `memcmp`, each once) into @p out.
                 *
                 * Every entry is checked as `open()` checks it, but for its place in
                 * the partition (which this doesn't know): its path, the order, no path
                 * both a file and a directory, its flags, its offset at its alignment.
                 *
                 * @param sequence Its sequence number (larger than every version before).
                 * @param written When it is written.
                 * @param firmware The firmware build it needs (`HashBytes`), or empty for any.
                 * @return Its size, `Invalid` (an entry or argument refused), or
                 *         `BufferTooSmall`.
                 */
                static Result<size_t> writeVersion(Span<const NewEntry> entries, uint32_t sequence, Timestamp written,
                                                   Span<const uint8_t> firmware, WritableByteSpan out);

                /** @brief True if @p bytes start like a ROM partition (its magic): worth `open()`. */
                static bool looksLike(ByteSpan bytes);

                /**
                 * @brief Checks the partition @p bytes and finds its tree: the
                 *        newest version that is whole and good.
                 *
                 * With @p firmware (a build id, `HashBytes`), the newest among the
                 * versions that need no firmware or that one, if any is good; else the
                 * newest of all (an update's tree for a firmware that didn't make it
                 * is passed over while an older one is there, §12e).
                 *
                 * @return The image, `CorruptData` (not a ROM, damaged, or no good
                 *         version), or `NotSupported` (another format).
                 */
                static Result<RomImage> open(ByteSpan bytes, Span<const uint8_t> firmware = {});

                /**
                 * @brief Reads what starts at sector @p at of the log (start with
                 *        `logOffset()`): a version (good, or whole but wrong), the start
                 *        of a damaged one, an erased sector, or anything else.
                 * @return Where the next starts; `logOffset() + logBytes()` at the end.
                 */
                uint32_t logRecord(uint32_t at, LogRecord &record) const;

                /** @brief Returns the number of files. */
                uint32_t count() const { return _count; }

                /** @brief Returns file @p index (less than `count()`). */
                Entry entry(uint32_t index) const;

                /**
                 * @brief Finds @p path ("" or "/" the root; "/a/b" or "a/b"; repeated
                 *        `/` are one).
                 * @param noPath Set when a directory along the way is missing or is a
                 *        file (the VFS then looks for a link along it).
                 * @return The file or directory, or `NotExist`.
                 */
                Result<Node> find(StringView path, bool &noPath) const;

                /**
                 * @brief Lists directory @p dir: the child at @p at (start with
                 *        `dir.first`), each directory under it once.
                 * @return Where the next child is, or `NoEntry` when there are no more.
                 */
                uint32_t nextChild(const Node &dir, uint32_t at, Child &child) const;

                /** @brief Returns the newest time of entries `[first, end)` (the version's if none). */
                Timestamp newest(uint32_t first, uint32_t end) const;

                /** @brief Returns the tree version in use. */
                const Version &version() const { return _version; }

                /** @brief Returns the whole partition. */
                ByteSpan partition() const { return _partition; }

                /** @brief Returns where the log of tree versions is. */
                uint32_t logOffset() const { return _logOffset; }

                /** @brief Returns the log's size. */
                uint32_t logBytes() const { return _logBytes; }

                /** @brief Returns where the files' space starts. */
                uint32_t dataOffset() const { return _dataOffset; }

                /** @brief Returns the files' bytes, added up. */
                uint64_t filesBytes() const;

                /**
                 * @brief Hashes file @p index and compares it with its entry's.
                 * @return `Ok` or `CorruptData`.
                 */
                Error verifyFile(uint32_t index) const;

                /**
                 * @brief Hashes every file (`verifyFile()`).
                 * @return `Ok`, or `CorruptData` at the first that differs.
                 */
                Error verify() const;

        private:
                const uint8_t *entryAt(uint32_t index) const {
                        return _partition.data() + _entries + index * EntryBytes;
                }
                StringView pathOf(uint32_t index) const;
                uint32_t   lowerBound(StringView key) const;
                bool       isDirectory(StringView path) const;
                Error      checkEntries() const;
                void       useVersion(uint32_t at);

                ByteSpan _partition;
                uint32_t _logOffset = 0;
                uint32_t _logBytes = 0;
                uint32_t _dataOffset = 0;
                uint32_t _count = 0;
                uint32_t _entries = 0;    // entry 0's offset in the partition
                uint32_t _paths = 0;      // the paths' offset in the partition
                uint32_t _pathsBytes = 0; // their size
                Version  _version;
};

BMDS_NAMESPACE_END
