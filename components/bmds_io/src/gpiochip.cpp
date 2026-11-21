/**
 * @file      gpiochip.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `/dev/gpiochip0` (devplan/io.md §3.9).
 */

#include <new>
#include <bmds/gpiochip.h>
#include <bmds/io.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

Error GpioChip::addFiles() {
        if (!Io::isReady()) return Error::NotReady;
        static GpioChip *chip = nullptr;
        if (chip != nullptr) return Error::Exists;
        void *block = Memory::alloc(MemSpace::Psram, sizeof(GpioChip));
        if (block == nullptr) return Error::NoMem;
        chip = ::new (block) GpioChip(0);
        Vfs::DeviceFile file;
        file.name = "gpiochip0";
        file.arg = chip;
        file.isDevice = true;
        file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                return FileHandle::open(*static_cast<GpioChip *>(arg), mode);
        };
        return Vfs::addDeviceFile(file);
}

BMDS_NAMESPACE_END
