/**
 * @file      fatvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/memory.h>

#include "fatlayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // f_mkfs()'s work buffer: bigger writes the FAT in fewer, larger
        // requests. DMA-capable, so it goes straight to an SD card.
        constexpr size_t FormatBufferBytes = 32 * 1024;

        constinit FatVolume volumes[FF_VOLUMES];

} // namespace

FatVolume *FatVolume::acquire() {
        for (FatVolume &v : volumes) {
                if (v._inUse) continue;
                if (v._fs == nullptr) {
                        // Kept for good once allocated (see the class comment).
                        v._fs = static_cast<FATFS *>(Memory::alloc(MemSpace::Psram, sizeof(FATFS)));
                        if (v._fs == nullptr) return nullptr;
                        std::memset(v._fs, 0, sizeof(FATFS));
                }
                v._inUse = true;
                return &v;
        }
        return nullptr;
}

FatVolume &FatVolume::at(uint8_t drive) {
        return volumes[drive];
}

uint8_t FatVolume::drive() const {
        return static_cast<uint8_t>(this - volumes);
}

void FatVolume::root(char (&path)[3]) const {
        path[0] = static_cast<char>('0' + drive());
        path[1] = ':';
        path[2] = '\0';
        return;
}

void FatVolume::release() {
        unmount();
        _device = nullptr;
        _inUse = false;
        return;
}

Error FatVolume::mount(BlockDevice &device) {
        if (_mounted) return Error::InvalidState;
        if (!device.isPresent()) return Error::NoMedia;
        _device = &device;
        char path[3];
        root(path);
        const FRESULT result = f_mount(_fs, path, 1);
        if (result != FR_OK) {
                const Error err = Fat::toError(result, *this);
                (void)f_mount(nullptr, path, 0);
                _device = nullptr;
                return err;
        }
        _mounted = true;
        return Error();
}

void FatVolume::unmount() {
        if (!_mounted) return;
        char path[3];
        root(path);
        // Clears the volume's type, which every open file and directory on it
        // is checked against: they all fail from now on.
        (void)f_mount(nullptr, path, 0);
        _mounted = false;
        _device = nullptr;
        return;
}

Error FatVolume::format(BlockDevice &device, FsFormat format, bool partitionTable) {
        if (!device.isPresent()) return Error::NoMedia;
        FatVolume *v = acquire();
        if (v == nullptr) return Error::Full;
        auto *work = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, FormatBufferBytes));
        if (work == nullptr) {
                v->release();
                return Error::NoMem;
        }
        MKFS_PARM params = {};
        switch (format) {
                case FsFormat::Fat32: params.fmt = FM_FAT32; break;
                case FsFormat::ExFat: params.fmt = FM_EXFAT; break;
                default: params.fmt = FM_ANY; break;
        }
        // On a partition, the filesystem alone ("super floppy" within it).
        if (!partitionTable) params.fmt |= FM_SFD;
        // Everything else is FatFs's choice: cluster size by volume size, the
        // data area aligned to the device's erase block.
        v->_device = &device;
        char path[3];
        v->root(path);
        const FRESULT result = f_mkfs(path, &params, work, FormatBufferBytes);
        const Error   err = Fat::toError(result, *v);
        Memory::free(work);
        v->release();
        return err;
}

const char *FatVolume::typeName() const {
        if (!_mounted) return "";
        switch (_fs->fs_type) {
                case FS_FAT12: return "fat12";
                case FS_FAT16: return "fat16";
                case FS_FAT32: return "fat32";
                default: return "exfat";
        }
}

Result<FsInfo> FatVolume::info() {
        if (!_mounted) return makeError<FsInfo>(Error::NoMedia);
        char path[3];
        root(path);
        DWORD         freeClusters = 0;
        FATFS        *fs = nullptr;
        const FRESULT result = f_getfree(path, &freeClusters, &fs);
        if (result != FR_OK) return makeError<FsInfo>(Fat::toError(result, *this));
        FsInfo info;
        switch (fs->fs_type) {
                case FS_FAT12: info.type = "FAT12"; break;
                case FS_FAT16: info.type = "FAT16"; break;
                case FS_FAT32: info.type = "FAT32"; break;
                default: info.type = "exFAT"; break;
        }
        info.clusterBytes = static_cast<uint32_t>(fs->csize) * FF_MAX_SS;
        info.totalBytes = static_cast<uint64_t>(fs->n_fatent - 2) * info.clusterBytes;
        info.freeBytes = static_cast<uint64_t>(freeClusters) * info.clusterBytes;
        return makeResult(info);
}

BMDS_NAMESPACE_END
