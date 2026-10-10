/**
 * @file      memoryblockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/memoryblockdevice.h>

BMDS_NAMESPACE_BEGIN

Error MemoryBlockDevice::readSectors(uint64_t sector, WritableByteSpan buf) {
        std::memcpy(buf.data(), _bytes.data() + sector * SectorBytes, buf.size());
        return Error::Ok;
}

Error MemoryBlockDevice::writeSectors(uint64_t sector, ByteSpan data) {
        (void)sector;
        (void)data;
        return Error::ReadOnly;
}

BMDS_NAMESPACE_END
