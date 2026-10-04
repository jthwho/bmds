/**
 * @file      automount.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <bmds/blockdevices.h>
#include <bmds/partitions.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

Result<size_t> Vfs::automount(BlockDevice &device) {
        char        name[BlockDevices::MaxName + 1];
        const Error named = BlockDevices::nameOf(device, name);
        if (named.isError()) return makeError<size_t>(named);
        const Result<size_t> partitions = Partitions::attach(device);
        if (partitions.error().isError()) return partitions;
        size_t mounted = 0;
        for (size_t i = 0; i < partitions.value(); ++i) {
                char partName[BlockDevices::MaxName + 12]; // "p" and up to 10 digits
                char path[MaxMountPath + 1];
                std::snprintf(partName, sizeof(partName), "%sp%u", name, static_cast<unsigned>(i));
                std::snprintf(path, sizeof(path), "/%s/p%u", name, static_cast<unsigned>(i));
                BlockDevice *partition = BlockDevices::find(partName);
                // Partitions without a FAT filesystem (Linux, swap, ...) stay unmounted.
                if (partition != nullptr && mount(path, *partition).isOk()) ++mounted;
        }
        return makeResult(mounted);
}

void Vfs::eject(BlockDevice &device) {
        // Partitions first (that unmounts them), then mounts of the whole device.
        Partitions::detach(device);
        (void)unmountDevice(device);
        return;
}

BMDS_NAMESPACE_END
