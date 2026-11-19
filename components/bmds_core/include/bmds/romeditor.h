/**
 * @file      romeditor.h
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
#include <bmds/list.h>
#include <bmds/result.h>
#include <bmds/romimage.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The ROM's partition as `RomEditor` writes it: its bytes as mapped,
 *        and the chip's erase, write and read.
 *
 * Flash semantics: `write()` only into erased bytes (0xff), `erase()` whole
 * 4 KB sectors. On the board, every call turns the caches off: the caller
 * runs on an internal-RAM stack.
 */
class RomFlash {
        public:
                virtual ~RomFlash() = default;

                /** @brief Returns the whole partition as mapped (reads see what was written). */
                virtual ByteSpan bytes() const = 0;

                /** @brief Erases @p bytes from @p offset (both whole sectors). */
                virtual Error erase(uint32_t offset, uint32_t bytes) = 0;

                /** @brief Writes @p data at @p offset (erased before). */
                virtual Error write(uint32_t offset, ByteSpan data) = 0;

                /** @brief Reads @p out from @p offset from the chip itself (to check a write). */
                virtual Error read(uint32_t offset, WritableByteSpan out) = 0;
};

/**
 * @brief Changes the ROM's tree (`devplan/rom.md` §12b, §12d, §12g): files
 *        added, replaced and removed, files moved together, an update's
 *        system files merged; each change one new tree version, which the
 *        next boot takes.
 *
 * Nothing the booted tree or the committed one (the newest, which a change
 * builds on) uses is ever written: new bytes go into space neither uses, are
 * written and read back, and only then is the version that points at them
 * written. A power cut at any point leaves the committed tree or the new
 * one. In recovery (no booted tree), space only the committed tree uses is
 * free as soon as a new version leaves it.
 *
 * The firmware a version is for: an update's names its firmware (`apply()`);
 * every other change's is for any, so no firmware passes over what the user
 * did (§12f).
 *
 * Space: files aligned to 64 KB or more take whole pages from the top of the
 * partition down; the others of one change are packed together (each at its
 * alignment) into one run of free sectors, the lowest that fits. A file
 * aligned to 4 KB or more owns its last 4 KB page, 0 after its end, so a
 * program may map it where it is (§6).
 *
 * @par Thread Safety
 * Not thread-safe: one editor, one task (the kernel serializes the ROM's).
 */
class RomEditor {
        public:
                /** @brief A new file's bytes, read once, in order. */
                class Source {
                        public:
                                virtual ~Source() = default;

                                /** @brief Returns its size. */
                                virtual uint64_t size() const = 0;

                                /** @brief Reads its next bytes into @p buf (0: none left). */
                                virtual Result<size_t> read(WritableByteSpan buf) = 0;
                };

                /** @brief Bytes in memory (or mapped flash) as a source. */
                class MemorySource final : public Source {
                        public:
                                explicit MemorySource(ByteSpan bytes) : _bytes(bytes) {}
                                uint64_t       size() const override { return _bytes.size(); }
                                Result<size_t> read(WritableByteSpan buf) override;

                        private:
                                ByteSpan _bytes;
                                size_t   _at = 0;
                };

                /** @brief One file to add (or replace). */
                struct Add {
                                StringView path;                   ///< Its path from the root, no leading `/`.
                                Source    *source = nullptr;       ///< Its bytes.
                                uint8_t    alignLog2 = 6;          ///< Its alignment, as log2 (64 bytes).
                                uint8_t    flags = RomImage::User; ///< Its flags (the user's, unless an update's).
                                Timestamp  modified;               ///< Its time.
                };

                /** @brief What a change did. */
                struct Report {
                                uint32_t sequence = 0;     ///< The version written (0: none was needed).
                                uint32_t written = 0;      ///< Files written.
                                uint64_t bytesWritten = 0; ///< Their bytes.
                                uint32_t kept = 0;         ///< Files kept where they were (an update's).
                                uint32_t removed = 0;      ///< Paths removed.
                                uint32_t moved = 0;        ///< Files moved (compaction).
                                uint32_t displaced = 0;    ///< The user's files an update's took the place of.
                };

                /** @brief Why a change didn't fit (`NoSpace`). */
                struct Shortfall {
                                uint64_t needed = 0;        ///< What the new bytes need (whole sectors and pages).
                                uint64_t freeNow = 0;       ///< Whole free sectors now.
                                uint64_t largestNow = 0;    ///< The largest run of them.
                                uint64_t freeAfterBoot = 0; ///< Whole free sectors once the booted tree's are free.
                };

                /** @brief Told the bytes written so far of a change's total. */
                using Progress = void (*)(void *context, uint64_t done, uint64_t total);

                /** @brief Told each of the user's files an update's file took the place of. */
                using Displaced = void (*)(void *context, StringView path);

                /**
                 * @brief An editor of @p flash's tree.
                 * @param booted The tree booted (its files and version are never written), or
                 *        null in recovery.
                 * @param firmware The build id whose trees are preferred (`RomImage::open()`).
                 */
                RomEditor(RomFlash &flash, const RomImage *booted, Span<const uint8_t> firmware);

                ~RomEditor();
                RomEditor(const RomEditor &) = delete;
                RomEditor &operator=(const RomEditor &) = delete;

                /**
                 * @brief Reads the committed tree (the newest good one, as the next boot
                 *        chooses it).
                 * @return `Ok`, or `CorruptData` (no good tree: nothing to change).
                 */
                Error load();

                /** @brief Returns the committed tree (after `load()`). */
                const RomImage &committed() const { return _committed; }

                /** @brief Returns true if the committed tree isn't the booted one. */
                bool hasPending() const;

                /** @brief Sets the time a new version says it was written. */
                void setTime(Timestamp now) { _now = now; }

                /** @brief Sets where progress is told (may be null). */
                void setProgress(Progress progress, void *context);

                /**
                 * @brief Adds (or replaces) @p adds, one batch: written, then one version.
                 * @return `Ok`, `Invalid` (a path refused, twice, or a file and a directory at
                 *         once: `failedPath()`), `NoSpace` (`shortfall()`), or the flash's error.
                 */
                Error install(Span<const Add> adds, Report &report);

                /**
                 * @brief Removes @p paths (a directory's files with @p recursive).
                 * @return `Ok`, `NotExist` or `IsDir` (`failedPath()`), or the flash's error.
                 */
                Error remove(Span<const StringView> paths, bool recursive, Report &report);

                /**
                 * @brief Drops the pending tree: a version of the booted one's files. `Ok` with
                 *        no version written if there is none.
                 */
                Error discard(Report &report);

                /**
                 * @brief Compacts the files (§12d). By default fills the holes: each run of free
                 *        sectors, from the bottom up, takes the files from the top down that fit it,
                 *        packed at their alignments (all but files on 64 KB pages); files never
                 *        move up, only what fills holes moves. @p full instead moves every file
                 *        after the first hole together into the lowest run that holds them all:
                 *        packed tight, but more moved (two boots in the main firmware if that run
                 *        is above them).
                 * @param more Set if files moved: their old places free at the next boot (in
                 *        recovery, at once), and another compaction may do more.
                 * @return `Ok` (`report.moved` 0: nothing to do), `NoSpace` (full: no run holds
                 *         them), or the flash's error.
                 */
                Error compact(Report &report, bool &more, bool full = false);

                /**
                 * @brief Merges an update's tree @p package (§12g): its system files written
                 *        where they differ, the system's files it lacks removed, the user's
                 *        kept unless the package's take their place; one version, for
                 *        @p firmware.
                 * @return `Ok`, `NoSpace` (nothing written), or the flash's error.
                 */
                Error apply(const RomImage &package, Span<const uint8_t> firmware, Displaced displaced, void *context,
                            Report &report);

                /**
                 * @brief Tells whether `apply()` of @p package would fit, writing nothing, and
                 *        which of the user's files it would displace (@p displaced, may be null:
                 *        to be backed up first).
                 * @return `Ok` or `NoSpace` (`shortfall()`).
                 */
                Error checkApply(const RomImage &package, Displaced displaced = nullptr, void *context = nullptr);

                /** @brief Returns why the last change didn't fit. */
                const Shortfall &shortfall() const { return _shortfall; }

                /** @brief Returns the path the last change refused (empty if none). */
                StringView failedPath() const { return _failedPath; }

        private:
                // A file of a change, as it will be in the new tree.
                struct Planned {
                                RomImage::NewEntry entry;
                                Source            *source = nullptr; // null: kept where it is
                                uint8_t            hash[RomImage::HashBytes] = {};
                };

                Error           buildUsage();
                static uint64_t taken(const RomImage::Entry &e);
                bool  isFree(uint32_t sector) const { return sector < _usedCount && _used[sector] == 0; }
                void  mark(uint64_t offset, uint64_t bytes);
                Error allocate(List<Planned> &planned, bool dryRun);
                Error writeFiles(List<Planned> &planned, Report &report);
                Error commit(List<Planned> &planned, Span<const uint8_t> firmware, Report &report);
                Error plan(const RomImage &package, List<Planned> &planned, Displaced displaced, void *context,
                           Report &report);
                Error eraseRun(uint64_t offset, uint64_t bytes);
                void  keep(List<Planned> &planned, uint32_t index);
                void  tell(uint64_t done, uint64_t total);

                RomFlash           &_flash;
                const RomImage     *_booted;
                Span<const uint8_t> _firmware;
                RomImage            _committed;
                bool                _loaded = false;
                Timestamp           _now;
                Progress            _progress = nullptr;
                void               *_progressContext = nullptr;
                uint8_t            *_used = nullptr; // a byte a sector: nonzero used
                uint32_t            _usedCount = 0;
                Shortfall           _shortfall;
                StringView          _failedPath;
};

BMDS_NAMESPACE_END
