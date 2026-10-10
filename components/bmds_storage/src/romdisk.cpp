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

#include <cstring>
#include <esp_partition.h>
#include <bmds/blockdevice.h>
#include <bmds/blockdevices.h>
#include <bmds/log.h>
#include <bmds/romdisk.h>
#include <bmds/staticobject.h>
#include <bmds/vfs.h>

#include "romhash.h"

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
        constinit bool                       matches = false;
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
        err = Vfs::mount(Path, device);
        if (err.isOk()) err = Vfs::pin(Path);
        if (err.isError()) {
                bmdsWarn("RomDisk: /dev/%s on %s: %s", name, Path, err.desc());
                return Error::Ok;
        }
        auto [image, ierr] = RomImage::open(device.mapped());
        if (ierr.isError()) return Error::Ok; // mounted above, so it opens: never here
        mountedImage = image;
        mounted = true;
        matches = std::memcmp(image.hash().data(), RomContentHash, sizeof(RomContentHash)) == 0;
        char have[17];
        hex(image.hash().data(), 8, have);
        if (matches) {
                bmdsInfo("RomDisk: /dev/%s mounted on %s, %u KB, %u entries, content %s", name, Path,
                         static_cast<unsigned>(image.bytes().size() / 1024), static_cast<unsigned>(image.count()),
                         have);
        } else {
                char want[17];
                hex(RomContentHash, 8, want);
                bmdsWarn("RomDisk: /dev/%s mounted on %s, but its content (%s) isn't the firmware's (%s): "
                         "flash both (idf.py flash)",
                         name, Path, have, want);
        }
        return Error::Ok;
}

const RomImage *RomDisk::image() {
        return mounted ? &mountedImage : nullptr;
}

bool RomDisk::matchesFirmware() {
        return mounted && matches;
}

size_t RomDisk::partitionBytes() {
        return partitionSize;
}

BMDS_NAMESPACE_END
