/**
 * @file      tmpvolume.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: a filesystem in memory, a tmpfs (devplan/storage.md §15).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief A tmpfs: a tree of files and directories in `Psram`, without a
 *        block device, gone when unmounted.
 *
 * Names are any bytes but `/` and NUL (at most `MaxName`), case kept and
 * significant. A file up to `PageBytes` is one block sized to it; a larger
 * one is pages through a table, and a page never written is a hole that
 * reads as zeros. What nodes, names and data take counts against the limit;
 * past it writes fail with `NoSpace`.
 *
 * A file removed while open loses its name at once and its data with its
 * last handle, also when a rename replaces it. Files and directories still
 * open when it is unmounted go stale (`NoMedia`, a generation number tells
 * them).
 *
 * A static pool of `MaxVolumes`; everything expects the storage lock.
 */
class TmpVolume final : public Volume {
        public:
                /** @brief Tmpfs mounts at once. */
                static constexpr size_t MaxVolumes = 2;

                /** @brief A page of a large file; also the largest single-block file. */
                static constexpr uint32_t PageBytes = 4096;

                /** @brief The longest name, in bytes. */
                static constexpr size_t MaxName = 255;

                struct Node; ///< A file or directory (tmpvolume.cpp).

                constexpr TmpVolume() = default;
                TmpVolume(const TmpVolume &) = delete;
                TmpVolume &operator=(const TmpVolume &) = delete;

                /** @brief Takes a free volume; null if all are in use. */
                static TmpVolume *acquire();

                /** @brief Starts empty, holding at most @p limitBytes (`NoMem`: no root). */
                Error mount(uint64_t limitBytes);

                /** @brief Changes the limit (what is held is kept). */
                void setLimit(uint64_t limitBytes) { _limit = limitBytes; }

                /** @brief Returns the bytes its nodes, names and data take. */
                uint64_t usedBytes() const { return _used; }

                /** @brief Returns the mount's generation: a file's must match, else it is stale. */
                uint32_t generation() const { return _generation; }

                /** @brief Reads @p node's bytes at @p offset into @p buf; returns how many (0 at the end). */
                size_t readAt(const Node &node, uint64_t offset, WritableByteSpan buf) const;

                /**
                 * @brief Writes @p data at @p offset into file @p node, growing it.
                 * @return The bytes written (fewer if space ran out), or `NoSpace`.
                 */
                Result<size_t> writeAt(Node &node, uint64_t offset, ByteSpan data);

                /** @brief Makes file @p node @p size bytes long (`NoSpace` if it can't grow). */
                Error resize(Node &node, uint64_t size);

                /** @brief Returns file @p node's size. */
                static uint64_t sizeOf(const Node &node);

                /** @brief A handle of @p node went: freed if it was its last and the node is removed. */
                void unref(Node &node);

                void                    release() override;
                BlockDevice            *device() const override { return nullptr; }
                const char             *typeName() const override { return "tmpfs"; }
                Result<FsInfo>          info() override;
                Error                   stat(StringView path, FileInfo &info, bool &noPath) override;
                Result<FileHandle::Ptr> open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                             String *link) override;
                Result<Reader *>        openReader(StringView path) override;
                void                    closeReader(Reader *reader) override;
                Error                   mkdir(StringView path, bool &noPath) override;
                Error                   rmdir(StringView path) override;
                Error                   remove(StringView path) override;
                Error rename(StringView from, StringView to, Vfs::RenameMode mode, bool &noPath) override;
                Error                   setModified(StringView path, Timestamp time) override;
                Error                   writeLink(StringView path, ByteSpan contents, bool &noPath) override;
                Result<Cursor *>        openDirectory(StringView path, bool &noPath) override;
                void                    closeDirectory(Cursor *cursor) override;

                class DirCursor; ///< An open directory (tmpvolume.cpp).

        private:
                // Finds @p path ("" is the root): `NotExist` with @p noPath as `Volume` says.
                Result<Node *> find(StringView path, bool &noPath) const;
                // Finds the directory @p path's last name is in, and that name.
                Error findParent(StringView path, Node *&parent, StringView &name, bool &noPath) const;
                // A new node named @p name in directory @p parent (`NoSpace`, `NoMem`, `Invalid` name).
                Result<Node *> create(Node &parent, StringView name, FileType type);
                // Takes @p node out of its directory: freed, or kept while it has handles.
                void unlink(Node &node);
                // Frees @p node (no entries, no handles) and what it holds.
                void destroy(Node *node);
                // Frees every node: the tree and the removed ones still open.
                void destroyAll();
                // Turns small file @p node into pages (`NoSpace`).
                Error makePaged(Node &node);
                // Counts @p bytes against the limit, then allocates them; null when over or out.
                void *take(size_t bytes);
                // Frees @p p, @p bytes of what take() gave.
                void give(void *p, size_t bytes);

                Node      *_root = nullptr;
                Node      *_removed = nullptr; // removed while open: freed with their last handle
                DirCursor *_cursors = nullptr; // open directories, moved past entries removed under them
                uint64_t   _limit = 0;
                uint64_t   _used = 0;
                uint32_t   _generation = 0;
                bool       _inUse = false;
};

BMDS_NAMESPACE_END
