/**
 * @file      ramdisk.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <sdkconfig.h>
#include <bmds/blockdevices.h>
#include <bmds/log.h>
#include <bmds/ramblockdevice.h>
#include <bmds/ramdisk.h>
#include <bmds/staticobject.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t SectorBytes = 512;

        constinit StaticObject<RamBlockDevice> disk;

} // namespace

Error RamDisk::init() {
        if (CONFIG_BMDS_RAMDISK_MB == 0) return Error::Ok;
        Error err = Vfs::init();
        if (err.isError()) return err;
        RamBlockDevice &device = disk.create(SectorBytes);
        err = device.allocate(uint64_t(CONFIG_BMDS_RAMDISK_MB) * 1024 * 1024 / SectorBytes, MemSpace::Psram);
        char name[BlockDevices::MaxName + 1];
        if (err.isOk()) err = BlockDevices::addNumbered(device, "ram", name);
        if (err.isOk()) err = Vfs::format(device, FsFormat::Auto);
        if (err.isOk()) err = Vfs::automount(device).error();
        if (err.isError()) {
                bmdsErr("RamDisk: %s", err.desc());
                return err;
        }
        bmdsInfo("RamDisk: /%s/p0, %u MB (%u KB in use)", name, static_cast<unsigned>(CONFIG_BMDS_RAMDISK_MB),
                 static_cast<unsigned>(device.usedBytes() / 1024));
        return Error::Ok;
}

BMDS_NAMESPACE_END
