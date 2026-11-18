/**
 * @file      romdisk.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /rom: the ROM image in the flash partition "rom", mapped and mounted
 * (devplan/rom.md §3b, §3d).
 */

#include <algorithm>
#include <cstring>
#include <esp_app_desc.h>
#include <esp_partition.h>
#include <bmds/blockdevice.h>
#include <bmds/blockdevices.h>
#include <bmds/log.h>
#include <bmds/romdisk.h>
#include <bmds/rommanifest.h>
#include <bmds/staticobject.h>
#include <bmds/vfs.h>

#include "romfiles.h"

BMDS_NAMESPACE_BEGIN

namespace {

        /**
         * The partition as a read-only block device whose bytes are mapped through the
         * cache, for good: what a ROM image is mounted on.
         */
        class RomPartition final : public BlockDevice {
                public:
                        static constexpr uint32_t SectorBytes = 512;

                        RomPartition() = default;
                        RomPartition(const RomPartition &) = delete;
                        RomPartition &operator=(const RomPartition &) = delete;

                        Error open(const esp_partition_t &partition) {
                                const void                 *mapped = nullptr;
                                esp_partition_mmap_handle_t handle = 0;
                                const esp_err_t             err = esp_partition_mmap(&partition, 0, partition.size,
                                                                                     ESP_PARTITION_MMAP_DATA, &mapped, &handle);
                                if (err != ESP_OK) return Error::fromEspErr(err);
                                _bytes = ByteSpan(static_cast<const uint8_t *>(mapped), partition.size);
                                return Error::Ok;
                        }

                        uint32_t sectorSize() const override { return SectorBytes; }
                        uint64_t sectorCount() const override { return _bytes.size() / SectorBytes; }
                        bool     isMemory() const override { return true; }
                        Error    sync() override { return Error::Ok; }
                        ByteSpan mapped() const override { return _bytes; }

                protected:
                        Error readSectors(uint64_t sector, WritableByteSpan buf) override {
                                std::memcpy(buf.data(), _bytes.data() + sector * SectorBytes, buf.size());
                                return Error::Ok;
                        }

                        Error writeSectors(uint64_t, ByteSpan) override { return Error::ReadOnly; }

                private:
                        ByteSpan _bytes;
        };

        constinit StaticObject<RomPartition> partition;
        constinit RomImage                   mountedImage;
        constinit bool                       mounted = false;
        constinit RomManifest::Counts        counts;
        constinit size_t                     partitionSize = 0;

        void hex(const uint8_t *bytes, size_t n, char *out) {
                for (size_t i = 0; i < n; ++i) {
                        out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
                        out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 0xf];
                }
                out[2 * n] = '\0';
                return;
        }

} // namespace

Span<const uint8_t> RomDisk::firmware() {
        return Span<const uint8_t>(esp_app_get_description()->app_elf_sha256, RomImage::HashBytes);
}

Span<const RomManifestEntry> RomDisk::manifest() {
        return Span<const RomManifestEntry>(RomManifestFiles, RomManifestCount);
}

Error RomDisk::init() {
        Error err = Vfs::init();
        if (err.isError()) return err;
        const esp_partition_t *found =
                esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PartitionLabel);
        if (found == nullptr) {
                bmdsWarn("RomDisk: no flash partition '%s': no %s", PartitionLabel, Path);
                return Error::Ok;
        }
        partitionSize = found->size;
        RomPartition &device = partition.create();
        err = device.open(*found);
        char name[BlockDevices::MaxName + 1] = {};
        if (err.isOk()) err = BlockDevices::addNumbered(device, "rom", name);
        if (err.isError()) {
                bmdsWarn("RomDisk: the flash partition '%s': %s: no %s", PartitionLabel, err.desc(), Path);
                return Error::Ok;
        }
        if (!RomImage::looksLike(device.mapped())) {
                bmdsWarn("RomDisk: /dev/%s holds no ROM image (flash it: idf.py flash): no %s", name, Path);
                return Error::Ok;
        }
        // The tree for this firmware if the log has one (devplan/rom.md §12e): the volume and
        // this choose alike.
        Vfs::setRomFirmware(firmware());
        err = Vfs::mount(Path, device);
        if (err.isOk()) err = Vfs::pin(Path);
        if (err.isError()) {
                bmdsWarn("RomDisk: /dev/%s on %s: %s", name, Path, err.desc());
                return Error::Ok;
        }
        auto [image, ierr] = RomImage::open(device.mapped(), firmware());
        if (ierr.isError()) return Error::Ok; // mounted above, so it opens: never here
        mountedImage = image;
        mounted = true;
        counts = RomManifest::compare(image, manifest(), nullptr, nullptr);
        const RomImage::Version &v = image.version();
        const bool any = std::all_of(v.firmware.begin(), v.firmware.end(), [](uint8_t b) { return b == 0; });
        if (!any && std::memcmp(v.firmware.data(), firmware().data(), RomImage::HashBytes) != 0) {
                char needs[17];
                hex(v.firmware.data(), 8, needs);
                bmdsWarn("RomDisk: tree version %u is for firmware %s, not this one, and the log has none "
                         "for this one: used anyway",
                         static_cast<unsigned>(v.sequence), needs);
        }
        if (counts.matches()) {
                bmdsInfo("RomDisk: /dev/%s mounted on %s, %u KB, %u files, tree version %u, the system's %u "
                         "files as built",
                         name, Path, static_cast<unsigned>(image.filesBytes() / 1024),
                         static_cast<unsigned>(image.count()), static_cast<unsigned>(v.sequence),
                         static_cast<unsigned>(counts.good));
        } else {
                bmdsWarn("RomDisk: /dev/%s mounted on %s, tree version %u, but its system files aren't the "
                         "firmware's: %u changed, %u replaced, %u missing, %u extra (rom diff)",
                         name, Path, static_cast<unsigned>(v.sequence), static_cast<unsigned>(counts.changed),
                         static_cast<unsigned>(counts.replaced), static_cast<unsigned>(counts.missing),
                         static_cast<unsigned>(counts.extra));
        }
        return Error::Ok;
}

const RomImage *RomDisk::image() {
        return mounted ? &mountedImage : nullptr;
}

ByteSpan RomDisk::mapped() {
        return partition.isCreated() ? partition->mapped() : ByteSpan();
}

bool RomDisk::matchesFirmware() {
        return mounted && counts.matches();
}

RomManifest::Counts RomDisk::manifestCounts() {
        return counts;
}

size_t RomDisk::partitionBytes() {
        return partitionSize;
}

BMDS_NAMESPACE_END
