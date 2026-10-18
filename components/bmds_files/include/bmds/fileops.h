/**
 * @file      fileops.h
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
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a file manager does to files (`devplan/filebrowser.md` §4f):
 *        copy, move, remove, trash, rename and make new ones.
 *
 * The long ones (copies, moves across filesystems, removing trees) take a
 * `Progress`: told of each item and the bytes as they go, asked whether to
 * stop between chunks, and asked what to do when a name is taken. A failed
 * item is told to it and the others go on; the result is then the first
 * error. Paths are relative to the context's cwd.
 *
 * @par Thread Safety
 * Reentrant: each call stands alone (the worker's thread, or the loop's for
 * the quick ones). The VFS serializes the filesystems.
 */
namespace FileOps {

        /** @brief What to do when a name is taken at the target. */
        enum class Resolution : uint8_t {
                Replace,  ///< The one there removed first (a folder: merged into).
                KeepBoth, ///< The new one under a copy's name (`copyName()`).
                Skip,     ///< This item left as it is.
                Cancel,   ///< The operation stopped.
        };

        /**
         * @brief The caller's view of a long operation. The defaults: no
         *        cancelling, nothing shown, a taken name skipped.
         */
        class Progress {
                public:
                        virtual ~Progress() = default;

                        /** @brief Returns true to stop (asked between chunks and items). */
                        virtual bool isCancelled() { return false; }

                        /** @brief Item @p path (a file, folder or link) is begun; a file is @p bytes long. */
                        virtual void began(StringView path, uint64_t bytes) {
                                (void)path;
                                (void)bytes;
                                return;
                        }

                        /** @brief @p bytes more of the item were copied. */
                        virtual void advanced(uint64_t bytes) {
                                (void)bytes;
                                return;
                        }

                        /** @brief Item @p path failed with @p err (the others go on). */
                        virtual void failed(StringView path, Error err) {
                                (void)path;
                                (void)err;
                                return;
                        }

                        /**
                         * @brief @p target is taken by something (a folder: @p isDirectory)
                         *        where @p source goes. @return What to do.
                         */
                        virtual Resolution conflict(StringView source, StringView target, bool isDirectory) {
                                (void)source;
                                (void)target;
                                (void)isDirectory;
                                return Resolution::Skip;
                        }
        };

        /** @brief What `measure()` counts. */
        struct Totals {
                        uint64_t items = 0;   ///< Files, folders and links.
                        uint64_t folders = 0; ///< The folders among them.
                        uint64_t bytes = 0;   ///< The files' bytes.
        };

        /** @brief What `makeNew()` makes. */
        enum class NewKind : uint8_t {
                Folder,       ///< "New Folder".
                TextDocument, ///< "New Text Document.txt", empty.
                EmptyFile,    ///< "New File".
        };

        /** @brief The deepest tree walked. */
        inline constexpr int MaxDepth = 64;

        /**
         * @brief Counts the items and bytes of @p paths and the trees under them,
         *        each item told to `began()` as it is counted.
         * @return The totals, or the first error (`Interrupted` if cancelled).
         */
        Result<Totals> measure(Span<const StringView> paths, const Vfs::Context &context, Progress &progress);

        /**
         * @brief Copies @p from (a file, a link, or a folder and its tree) into
         *        folder @p into under its own name (a taken name asks).
         * @param made [out] The path made (may be null; empty if skipped).
         * @return `Ok` (skipped items too), `Interrupted` (cancelled), `Invalid`
         *         (into itself), or the first item's error.
         */
        Error copy(StringView from, StringView into, const Vfs::Context &context, Progress &progress,
                   String *made = nullptr);

        /**
         * @brief Moves @p from into folder @p into: a rename on one filesystem,
         *        else a copy and then the source removed (only if all of it
         *        was copied).
         * @return As `copy()`.
         */
        Error move(StringView from, StringView into, const Vfs::Context &context, Progress &progress,
                   String *made = nullptr);

        /**
         * @brief Removes @p path, a folder with everything in it (a link itself,
         *        never what it leads to).
         * @return `Ok`, `Interrupted`, or the first error.
         */
        Error remove(StringView path, const Vfs::Context &context, Progress &progress);

        /** @brief Moves @p path to its filesystem's trash (`Trash::move()`). */
        Error trash(StringView path, const Vfs::Context &context);

        /**
         * @brief Renames @p path to @p name in the same folder.
         * @return `Ok`, `Invalid` (an empty name, `.`, `..`, or one holding `/`),
         *         `Exists`, or the VFS's error.
         */
        Error rename(StringView path, StringView name, const Vfs::Context &context, String *made = nullptr);

        /**
         * @brief Makes a new folder or file in @p directory under the first free
         *        name ("New Folder", "New Folder (2)"...).
         * @return Its path, or the error.
         */
        Result<String> makeNew(StringView directory, NewKind kind, const Vfs::Context &context);

        /**
         * @brief Returns @p name if it is free in @p directory, else the first
         *        free copy's name (`CopyName`: "NAME (copy).EXT", "(copy 2)"...).
         * @return The name, `Full` (none free), or the error.
         */
        Result<String> copyName(StringView directory, StringView name, const Vfs::Context &context);

        /** @brief Returns the last component of @p path. */
        StringView baseName(StringView path);

} // namespace FileOps

BMDS_NAMESPACE_END
