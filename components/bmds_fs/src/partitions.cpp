/**
 * @file      partitions.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/partitions.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t SectorBytes = 512;

        // Most partitions registered at once, over all devices.
        constexpr size_t PoolSize = 8;

        // GPT tables have 128 entries by convention; a larger count is refused
        // rather than read (a crafted header could ask for millions).
        constexpr uint32_t MaxGptEntries = 128;

        uint32_t le32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t le64(const uint8_t *p) {
                return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
        }

        // A FAT or exFAT boot sector, as FatFs recognizes one.
        bool isBootSector(const uint8_t *s) {
                if (s[510] != 0x55 || s[511] != 0xaa) return false;
                if (s[0] != 0xeb && s[0] != 0xe9 && s[0] != 0xe8) return false;
                return std::memcmp(s + 3, "EXFAT   ", 8) == 0 || std::memcmp(s + 82, "FAT32   ", 8) == 0 ||
                       std::memcmp(s + 54, "FAT", 3) == 0;
        }

        bool fits(uint64_t first, uint64_t count, uint64_t total) {
                return count != 0 && first < total && count <= total - first;
        }

        /**
         * A window onto part of a block device. In a static pool; `valid`
         * goes false when its medium goes, and requests fail with NoMedia.
         */
        class PartitionDevice final : public BlockDevice {
                public:
                        constexpr PartitionDevice() = default;

                        void set(BlockDevice &parent, uint64_t first, uint64_t count) {
                                _parent = &parent;
                                _first = first;
                                _count = count;
                                _valid.store(true, std::memory_order_release);
                                return;
                        }

                        void invalidate() {
                                _valid.store(false, std::memory_order_release);
                                return;
                        }

                        bool         isFree() const { return _parent == nullptr; }
                        void         free() { _parent = nullptr; }
                        BlockDevice *parent() const { return _parent; }

                        uint32_t sectorSize() const override { return SectorBytes; }
                        uint64_t sectorCount() const override { return isPresent() ? _count : 0; }
                        bool     isPresent() const override {
                                return _valid.load(std::memory_order_acquire) && _parent->isPresent();
                        }
                        uint32_t eraseBlockSectors() const override {
                                return isPresent() ? _parent->eraseBlockSectors() : 1;
                        }
                        Error sync() override { return isPresent() ? _parent->sync() : Error(Error::NoMedia); }
                        Error trim(uint64_t first, uint64_t count) override {
                                if (!isPresent()) return Error::NoMedia;
                                if (!fits(first, count, _count)) return Error::OutOfRange;
                                return _parent->trim(_first + first, count);
                        }

                protected:
                        Error readSectors(uint64_t sector, WritableByteSpan buf) override {
                                return _parent->read(_first + sector, buf);
                        }
                        Error writeSectors(uint64_t sector, ByteSpan data) override {
                                return _parent->write(_first + sector, data);
                        }

                private:
                        BlockDevice      *_parent = nullptr;
                        uint64_t          _first = 0;
                        uint64_t          _count = 0;
                        std::atomic<bool> _valid = false;
        };

        constinit PartitionDevice pool[PoolSize];

        size_t readGpt(BlockDevice &device, uint8_t *sector, Span<PartitionInfo> out) {
                const uint64_t total = device.sectorCount();
                if (device.read(1, WritableByteSpan(sector, SectorBytes)).isError()) return 0;
                if (std::memcmp(sector, "EFI PART", 8) != 0) return 0;
                const uint64_t tableLba = le64(sector + 72);
                const uint32_t entryCount = le32(sector + 80);
                const uint32_t entryBytes = le32(sector + 84);
                if (entryBytes < 128 || entryBytes > SectorBytes || SectorBytes % entryBytes != 0 ||
                    entryCount > MaxGptEntries)
                        return 0;
                const uint32_t perSector = SectorBytes / entryBytes;
                const uint64_t tableSectors = (entryCount + perSector - 1) / perSector;
                if (!fits(tableLba, tableSectors, total)) return 0;
                size_t found = 0;
                for (uint32_t i = 0; i < entryCount && found < out.size(); ++i) {
                        if (i % perSector == 0 &&
                            device.read(tableLba + i / perSector, WritableByteSpan(sector, SectorBytes)).isError())
                                return found;
                        const uint8_t *e = sector + (i % perSector) * entryBytes;
                        bool           used = false;
                        for (int b = 0; b < 16; ++b) used = used || e[b] != 0; // the type GUID
                        const uint64_t first = le64(e + 32);
                        const uint64_t last = le64(e + 40);
                        if (!used || last < first || !fits(first, last - first + 1, total)) continue;
                        out[found++] = PartitionInfo{first, last - first + 1, 0, 1};
                }
                return found;
        }

} // namespace

Result<size_t> Partitions::read(BlockDevice &device, Span<PartitionInfo> out) {
        if (device.sectorSize() != SectorBytes) return makeError<size_t>(Error::NotSupported);
        const uint64_t total = device.sectorCount();
        if (total == 0) return makeError<size_t>(Error::NoMedia);
        auto *sector = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, SectorBytes));
        if (sector == nullptr) return makeError<size_t>(Error::NoMem);
        size_t found = 0;
        Error  err = device.read(0, WritableByteSpan(sector, SectorBytes));
        if (err.isOk()) {
                if (isBootSector(sector)) {
                        // No table: one volume over the whole medium.
                        if (!out.isEmpty()) out[found++] = PartitionInfo{0, total, 0, 0};
                } else if (sector[510] == 0x55 && sector[511] == 0xaa) {
                        PartitionInfo mbr[4];
                        size_t        mbrCount = 0;
                        bool          protective = false;
                        for (int i = 0; i < 4; ++i) {
                                const uint8_t *e = sector + 446 + i * 16;
                                const uint8_t  type = e[4];
                                if (type == 0xee) protective = true;
                                // Unused, and extended containers (logical partitions aren't read).
                                if (type == 0 || type == 0x05 || type == 0x0f || type == 0x85) continue;
                                const uint64_t first = le32(e + 8);
                                const uint64_t count = le32(e + 12);
                                if (fits(first, count, total)) mbr[mbrCount++] = PartitionInfo{first, count, type, 0};
                        }
                        if (protective) {
                                found = readGpt(device, sector, out);
                        } else {
                                for (size_t i = 0; i < mbrCount && found < out.size(); ++i) out[found++] = mbr[i];
                        }
                }
        }
        Memory::free(sector);
        if (err.isError()) return makeError<size_t>(err);
        return makeResult(found);
}

Result<size_t> Partitions::attach(BlockDevice &device) {
        char        name[BlockDevices::MaxName + 1];
        const Error named = BlockDevices::nameOf(device, name);
        if (named.isError()) return makeError<size_t>(named);
        PartitionInfo        parts[MaxPerDevice];
        const Result<size_t> found = read(device, Span<PartitionInfo>(parts));
        if (found.error().isError()) return found;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        size_t        added = 0;
        for (size_t i = 0; i < found.value(); ++i) {
                PartitionDevice *slot = nullptr;
                for (PartitionDevice &p : pool) {
                        if (p.isFree()) {
                                slot = &p;
                                break;
                        }
                }
                if (slot == nullptr) break;
                char partName[BlockDevices::MaxName + 12]; // "p" and up to 10 digits
                std::snprintf(partName, sizeof(partName), "%sp%u", name, static_cast<unsigned>(i));
                slot->set(device, parts[i].firstSector, parts[i].sectorCount);
                if (Fs::addDeviceLocked(*slot, partName, &device).isError()) {
                        slot->invalidate();
                        slot->free();
                        break;
                }
                ++added;
        }
        return makeResult(added);
}

void Partitions::detach(BlockDevice &device) {
        // Nothing may stay mounted on a partition device about to be reused.
        (void)Vfs::unmountDevice(device);
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (PartitionDevice &p : pool) {
                if (p.isFree() || p.parent() != &device) continue;
                p.invalidate();
                Fs::removeDeviceLocked(p);
                p.free();
        }
        return;
}

BMDS_NAMESPACE_END
