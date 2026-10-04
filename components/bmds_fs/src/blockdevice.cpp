/**
 * @file      blockdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/blockdevice.h>

BMDS_NAMESPACE_BEGIN

Error BlockDevice::checkRequest(uint64_t sector, size_t bytes) const {
        if (!isPresent()) return Error::NoMedia;
        const uint32_t size = sectorSize();
        if (bytes == 0 || size == 0 || bytes % size != 0) return Error::Invalid;
        const uint64_t count = bytes / size;
        const uint64_t total = sectorCount();
        if (sector >= total || count > total - sector) return Error::OutOfRange;
        return Error();
}

Error BlockDevice::read(uint64_t sector, WritableByteSpan buf) {
        const Error err = checkRequest(sector, buf.size());
        if (err.isError()) return err;
        return readSectors(sector, buf);
}

Error BlockDevice::write(uint64_t sector, ByteSpan data) {
        const Error err = checkRequest(sector, data.size());
        if (err.isError()) return err;
        return writeSectors(sector, data);
}

Error BlockDevice::trim(uint64_t, uint64_t) {
        return Error::NotSupported;
}

BMDS_NAMESPACE_END
