/**
 * @file      bootdisk.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /boot: littlefs on the flash partition "boot", through an ordered write
 * cache (devplan/storage.md §13d).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sdkconfig.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <bmds/blockdevice.h>
#include <bmds/blockdevices.h>
#include <bmds/bootdisk.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

#include "bootdefaults.h"
#include "flashpartition.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *PartitionLabel = "boot";

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

        // The defaults installed (devplan/gui.md G14): each file of the repository's boot/ not
        // already on /boot and not listed in Manifest is written, and listed; so a new release's
        // defaults reach a /boot made before it, and a default deleted stays deleted.
        constexpr const char Manifest[] = "/boot/.defaults";
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

        void installDefaults() {
                char *manifest = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxManifest));
                if (manifest == nullptr) return;
                size_t       got = 0;
                Vfs::Context context;
                {
                        auto [file, err] = Vfs::open(Manifest, Vfs::Read, context);
                        while (err.isOk() && got < MaxManifest) {
                                auto [n, rerr] =
                                        file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(manifest) + got,
                                                                    MaxManifest - got),
                                                   WaitForever);
                                if (rerr.isError() || n == 0) break;
                                got += n;
                        }
                }
                const StringView had(manifest, got);
                String           added;
                for (size_t i = 0; i < BootDefaults::count; ++i) {
                        const BootDefaults::File &f = BootDefaults::files[i];
                        if (listed(had, StringView(f.path))) continue;
                        char path[MaxPath];
                        std::snprintf(path, sizeof(path), "/boot/%s", f.path);
                        makeParents(path);
                        // Not there yet: written (a file the user made is kept, and listed).
                        const Error err = writeFile(path, ByteSpan(f.data, f.size));
                        if (err.isOk()) {
                                bmdsInfo("BootDisk: installed %s", path);
                        } else if (err != Error::Exists) {
                                bmdsWarn("BootDisk: %s: %s", path, err.desc());
                                continue;
                        }
                        if (added.append(StringView(f.path)).isError() || added.append('\n').isError()) break;
                }
                Memory::free(manifest);
                if (added.isEmpty()) return;
                auto [file, err] = Vfs::open(Manifest, Vfs::Write | Vfs::Create | Vfs::Append, context);
                if (err.isOk()) {
                        auto [n, werr] = file->write(added.view(), WaitForever);
                        err = werr.isOk() && n != added.size() ? Error(Error::IoError) : werr;
                }
                if (err.isError()) bmdsWarn("BootDisk: %s: %s", Manifest, err.desc());
                return;
        }

        // The random seed kept across boots (devplan/random.md).
        constexpr const char SeedPath[] = "/boot/random-seed";
        constexpr size_t     SeedBytes = 64;

        // A new seed from the pool over the old one.
        Error writeSeed() {
                uint8_t seed[SeedBytes];
                Random::fill(seed, sizeof(seed));
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (err.isOk()) {
                        auto [n, werr] = file->write(ByteSpan(seed, sizeof(seed)), WaitForever);
                        err = werr.isOk() && n != sizeof(seed) ? Error(Error::IoError) : werr;
                }
                RandomPool::wipe(seed, sizeof(seed));
                return err; // the file closes (and /boot syncs) with its handle
        }

        constinit bool seedable = false; // /boot is mounted (set at boot)

        // Mixes the last boot's seed in. A new one is written a few seconds
        // later (refreshRandomSeed()), so a crash soon after boot can reuse
        // it; the boot's own noise (Random::init()) is mixed in regardless.
        void seedRandom() {
                uint8_t      seed[SeedBytes];
                size_t       got = 0;
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Read, context);
                while (err.isOk() && got < sizeof(seed)) {
                        auto [n, rerr] = file->read(WritableByteSpan(seed + got, sizeof(seed) - got), WaitForever);
                        if (rerr.isError()) break;
                        got += n;
                }
                file.reset();
                if (got != 0) Random::mix(seed, got);
                RandomPool::wipe(seed, sizeof(seed));
                seedable = true;
                bmdsInfo("Random: %s %s", got != 0 ? "seeded from" : "no seed yet in", SeedPath);
                return;
        }

        void saveSeedAtShutdown() {
                BootDisk::refreshRandomSeed();
                return;
        }

} // namespace

void BootDisk::refreshRandomSeed() {
        if (!seedable) return;
        const Error err = writeSeed();
        if (err.isError()) bmdsWarn("BootDisk: %s: %s", SeedPath, err.desc());
        return;
}

Error BootDisk::init() {
        Error err = Vfs::init();
        if (err.isError()) return err;
        const esp_partition_t *partition =
                esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PartitionLabel);
        if (partition == nullptr) {
                bmdsErr("BootDisk: no flash partition '%s'", PartitionLabel);
                return Error::NotExist;
        }
        FlashPartition &device = flash.create();
        err = device.open(*partition);
        char name[BlockDevices::MaxName + 1];
        if (err.isOk()) err = BlockDevices::addNumbered(device, "flash", name);
        if (err.isOk() && esp_register_shutdown_handler(flushOnRestart) != ESP_OK)
                bmdsWarn("BootDisk: no shutdown handler: a restart may lose the last writes");
        if (err.isError()) {
                bmdsErr("BootDisk: the flash partition '%s': %s", PartitionLabel, err.desc());
                return err;
        }
        bool formatted = false;
        if (isLittleFs(device)) {
                err = Vfs::mount(Path, device);
        } else {
                // A new board, or one from before littlefs (§13d): a new filesystem with the defaults.
                bmdsInfo("BootDisk: /dev/%s isn't littlefs: formatting it", name);
                err = Vfs::format(device, FsFormat::Auto);
                if (err.isOk()) err = Vfs::mount(Path, device);
                formatted = err.isOk();
        }
        if (err.isOk()) err = Vfs::pin(Path);
        if (err.isError()) {
                bmdsErr("BootDisk: /dev/%s on %s: %s", name, Path, err.desc());
                return err;
        }
        installDefaults();
        bmdsInfo("BootDisk: /dev/%s mounted on %s, %u KB%s", name, Path,
                 static_cast<unsigned>(device.sectorCount() * device.sectorSize() / 1024),
                 formatted ? " (formatted)" : "");
        seedRandom();
        const esp_err_t herr = esp_register_shutdown_handler(saveSeedAtShutdown);
        if (herr != ESP_OK)
                bmdsWarn("BootDisk: no shutdown handler for %s: %s", SeedPath, Error::fromEspErr(herr).desc());
        return Error::Ok;
}

BMDS_NAMESPACE_END
