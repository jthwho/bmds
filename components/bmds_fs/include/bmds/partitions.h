/**
 * @file      partitions.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/** @brief A partition as the partition table describes it. */
struct PartitionInfo {
                uint64_t firstSector = 0; ///< First sector on the device.
                uint64_t sectorCount = 0; ///< Length in sectors.
                uint8_t  mbrType = 0;     ///< MBR partition type; 0 for GPT and whole-device volumes.
                uint8_t  isGpt = 0;       ///< 1 if from a GPT.
};

/**
 * @brief Partition tables: reading them, and partitions as block devices.
 *
 * Read: MBR primary entries (extended partitions are skipped), GPT (when
 * the MBR is protective), or none: a medium whose first sector is a FAT or
 * exFAT boot sector ("superfloppy") is one partition covering it all.
 * Every field read from the medium is range-checked against its size.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace Partitions {

        /** @brief Most partitions read from one table. */
        inline constexpr size_t MaxPerDevice = 8;

        /**
         * @brief Reads @p device's partition table into @p out.
         * @return The number of partitions (0: no table, no filesystem), or
         *         `NotSupported` (sectors other than 512 bytes), `NoMem`, or the device's error.
         */
        Result<size_t> read(BlockDevice &device, Span<PartitionInfo> out);

        /**
         * @brief Reads the partition table of registered @p device and registers
         *        each partition as a block device, `<name>p<N>`, numbered from 0.
         * @return How many were registered, or the error (`NotExist`: @p device isn't registered).
         */
        Result<size_t> attach(BlockDevice &device);

        /**
         * @brief Unmounts and unregisters @p device's partitions; they report
         *        no medium from now on.
         */
        void detach(BlockDevice &device);

} // namespace Partitions

BMDS_NAMESPACE_END
