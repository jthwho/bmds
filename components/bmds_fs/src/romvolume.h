/**
 * @file      romvolume.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_fs: a ROM image mounted (devplan/rom.md §3d).
 */

#pragma once

#include <cstddef>
#include <bmds/romimage.h>
#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief A ROM image while mounted: a read-only tree whose files are read
 *        where they lie (`BlockDevice::mapped()`), nothing copied.
 *
 * A static pool of `MaxVolumes`. Every file has its entry's time and the
 * ReadOnly attribute (System too where the image says so: an emulated link);
 * a directory has the newest time under it. The tree is the one found at
 * mount: a change to the ROM takes effect at the next mount (devplan/rom.md
 * §12b). Files and directories still open when it is unmounted go stale:
 * they fail with `NoMedia` (a generation number tells them).
 */
class RomVolume final : public Volume {
        public:
                /** @brief ROM images mounted at once. */
                static constexpr size_t MaxVolumes = 2;

                constexpr RomVolume() = default;
                RomVolume(const RomVolume &) = delete;
                RomVolume &operator=(const RomVolume &) = delete;

                /** @brief Sets the firmware build id whose tree a mount prefers (`Vfs::setRomFirmware()`). */
                static void setFirmware(Span<const uint8_t> buildId);

                /** @brief Takes a free volume; null if all are in use. */
                static RomVolume *acquire();

                /**
                 * @brief Mounts the image in @p device's mapped bytes.
                 * @return `Ok`, `CorruptData` (damaged), `NotSupported` (a later format).
                 */
                Error mount(BlockDevice &device);

                /** @brief Returns the image. */
                const RomImage &image() const { return _image; }

                /** @brief Returns the mount's generation: a file's must match, else it is stale. */
                uint32_t generation() const { return _generation; }

                /** @brief Returns true while mounted. */
                bool isMounted() const { return _mounted; }

                void                    release() override;
                BlockDevice            *device() const override { return _device; }
                const char             *typeName() const override { return "rom"; }
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
                Result<ByteSpan>        map(StringView path, bool &noPath) override;

                /** @brief Fills @p info from @p node (a file or a directory) named @p name. */
                Error describe(const RomImage::Node &node, StringView name, FileInfo &info) const;

        private:
                // Finds @p path: `Ok` and what it is, else `NotExist` (with @p noPath).
                Result<RomImage::Node> find(StringView path, bool &noPath) const;
                // True if entry @p index can be read (stored as it is).
                bool isReadable(uint32_t index) const;
                // Finds file @p path: its entry (`IsDir` for a directory).
                Result<uint32_t> findFile(StringView path, bool &noPath) const;

                RomImage     _image;
                BlockDevice *_device = nullptr;
                uint32_t     _generation = 0;
                bool         _inUse = false;
                bool         _mounted = false;
};

BMDS_NAMESPACE_END
