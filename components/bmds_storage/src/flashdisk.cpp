/**
 * @file      flashdisk.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /flash: littlefs on the flash partition "flash", through an ordered write
 * cache (devplan/storage.md §13d); and the defaults installed (devplan/gui.md G14).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sdkconfig.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <bmds/blockdevice.h>
#include <bmds/blockdevices.h>
#include <bmds/flashdisk.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

#include "flashdefaults.h"
#include "flashpartition.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *PartitionLabel = "flash";

        constinit StaticObject<FlashPartition> flash;

        // A restart (reboot, esp_restart()) writes the queued operations first.
        void flushOnRestart() {
                if (flash.isCreated()) (void)flash->flush(WaitForever);
                return;
        }

        // littlefs's superblock: its name at offset 8 of block 0 or block 1 (the anchor pair).
        bool isLittleFs(FlashPartition &device) {
                for (uint32_t block = 0; block < 2; ++block) {
                        uint8_t magic[8];
                        if (device.read(block * FlashPartition::BlockBytes + 8, WritableByteSpan(magic)).isOk() &&
                            std::memcmp(magic, "littlefs", sizeof(magic)) == 0)
                                return true;
                }
                return false;
        }

        constexpr StringView CfgPrefix("cfg/");
        constexpr size_t     MaxManifest = 4096;
        constexpr size_t     MaxPath = 128;

        Error writeFile(const char *path, ByteSpan bytes) {
                Vfs::Context context;
                auto [file, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Exclusive, context);
                if (err.isError()) return err;
                auto [n, werr] = file->write(bytes, WaitForever);
                if (werr.isOk() && n != bytes.size()) werr = Error::IoError;
                return werr;
        }

        // Every directory above @p path made (those there already are fine).
        void makeParents(const char *path) {
                char         dir[MaxPath];
                Vfs::Context context;
                for (const char *slash = std::strchr(path + 1, '/'); slash != nullptr;
                     slash = std::strchr(slash + 1, '/')) {
                        const size_t n = static_cast<size_t>(slash - path);
                        if (n >= sizeof(dir)) return;
                        std::memcpy(dir, path, n);
                        dir[n] = '\0';
                        (void)Vfs::mkdir(StringView(dir), context);
                }
                return;
        }

        bool listed(StringView manifest, StringView path) {
                while (!manifest.isEmpty()) {
                        const size_t     end = manifest.indexOf('\n');
                        const StringView line = end == StringView::NPos ? manifest : manifest.left(end);
                        if (line.trimmed() == path) return true;
                        manifest = end == StringView::NPos ? StringView() : manifest.mid(end + 1);
                }
                return false;
        }

} // namespace

void FlashDefaults::install(StringView prefix, StringView root) {
        char manifestPath[MaxPath];
        std::snprintf(manifestPath, sizeof(manifestPath), "%.*s/.defaults", int(root.size()), root.data());
        char *manifest = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxManifest));
        if (manifest == nullptr) return;
        size_t       got = 0;
        Vfs::Context context;
        {
                auto [file, err] = Vfs::open(manifestPath, Vfs::Read, context);
                while (err.isOk() && got < MaxManifest) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(manifest) + got, MaxManifest - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
        }
        const StringView had(manifest, got);
        String           added;
        for (size_t i = 0; i < count; ++i) {
                const File      &f = files[i];
                const StringView full(f.path);
                const bool       inCfg = full.startsWith(CfgPrefix);
                if (prefix.isEmpty() ? inCfg : !full.startsWith(prefix)) continue;
                const StringView rel = full.mid(prefix.size());
                if (listed(had, rel)) continue;
                char path[MaxPath];
                std::snprintf(path, sizeof(path), "%.*s/%.*s", int(root.size()), root.data(), int(rel.size()),
                              rel.data());
                makeParents(path);
                // Not there yet: written (a file the user made is kept, and listed).
                const Error err = writeFile(path, ByteSpan(f.data, f.size));
                if (err.isOk()) {
                        bmdsInfo("Defaults: installed %s", path);
                } else if (err != Error::Exists) {
                        bmdsWarn("Defaults: %s: %s", path, err.desc());
                        continue;
                }
                if (added.append(rel).isError() || added.append('\n').isError()) break;
        }
        Memory::free(manifest);
        if (added.isEmpty()) return;
        auto [file, err] = Vfs::open(manifestPath, Vfs::Write | Vfs::Create | Vfs::Append, context);
        if (err.isOk()) {
                auto [n, werr] = file->write(added.view(), WaitForever);
                err = werr.isOk() && n != added.size() ? Error(Error::IoError) : werr;
        }
        if (err.isError()) bmdsWarn("Defaults: %s: %s", manifestPath, err.desc());
        return;
}

Error FlashDisk::init() {
        Error err = Vfs::init();
        if (err.isError()) return err;
        const esp_partition_t *partition =
                esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PartitionLabel);
        if (partition == nullptr) {
                bmdsErr("FlashDisk: no flash partition '%s'", PartitionLabel);
                return Error::NotExist;
        }
        FlashPartition &device = flash.create();
        err = device.open(*partition);
        char name[BlockDevices::MaxName + 1];
        if (err.isOk()) err = BlockDevices::addNumbered(device, "flash", name);
        if (err.isOk() && esp_register_shutdown_handler(flushOnRestart) != ESP_OK)
                bmdsWarn("FlashDisk: no shutdown handler: a restart may lose the last writes");
        if (err.isError()) {
                bmdsErr("FlashDisk: the flash partition '%s': %s", PartitionLabel, err.desc());
                return err;
        }
        bool formatted = false;
        if (isLittleFs(device)) {
                err = Vfs::mount(Path, device);
        } else {
                // A new board, or one from before littlefs (§13d): a new filesystem.
                bmdsInfo("FlashDisk: /dev/%s isn't littlefs: formatting it", name);
                err = Vfs::format(device, FsFormat::Auto);
                if (err.isOk()) err = Vfs::mount(Path, device);
                formatted = err.isOk();
        }
        if (err.isOk()) err = Vfs::pin(Path);
        if (err.isError()) {
                bmdsErr("FlashDisk: /dev/%s on %s: %s", name, Path, err.desc());
                return err;
        }
        FlashDefaults::install(StringView(), StringView(Path));
        bmdsInfo("FlashDisk: /dev/%s mounted on %s, %u KB%s", name, Path,
                 static_cast<unsigned>(device.sectorCount() * device.sectorSize() / 1024),
                 formatted ? " (formatted)" : "");
        return Error::Ok;
}

BMDS_NAMESPACE_END
