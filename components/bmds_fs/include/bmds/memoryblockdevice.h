/**
 * @file      memoryblockdevice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A read-only block device over memory the caller keeps, read in place
 *        (`mapped()`): a ROM image (`devplan/rom.md` §3d) held in memory, as the
 *        host tests mount one.
 *
 * Its sectors are `SectorBytes`; a last part sector isn't one of them, but
 * `mapped()` has every byte. Writes are `ReadOnly`.
 *
 * @par Thread Safety
 * Thread-safe (nothing changes).
 */
class MemoryBlockDevice final : public BlockDevice {
        public:
                /** @brief Its sector size. */
                static constexpr uint32_t SectorBytes = 512;

                /** @brief A device over @p bytes (kept by the caller while it is used). */
                explicit constexpr MemoryBlockDevice(ByteSpan bytes) : _bytes(bytes) {}

                MemoryBlockDevice(const MemoryBlockDevice &) = delete;
                MemoryBlockDevice &operator=(const MemoryBlockDevice &) = delete;

                uint32_t sectorSize() const override { return SectorBytes; }
                uint64_t sectorCount() const override { return _bytes.size() / SectorBytes; }
                bool     isMemory() const override { return true; }
                Error    sync() override { return Error::Ok; }
                ByteSpan mapped() const override { return _bytes; }

        protected:
                Error readSectors(uint64_t sector, WritableByteSpan buf) override;
                Error writeSectors(uint64_t sector, ByteSpan data) override;

        private:
                ByteSpan _bytes;
};

BMDS_NAMESPACE_END
