/**
 * @file      ptpclock.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /dev/ptpN (devplan/network.md §31e): an Ethernet device's 1588 clock, read, compared with the
 * system clock, and (while the PTP service leaves it alone) set, stepped and tuned.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/ethernetdevice.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/ptpclock.h>
#include <bmds/vfs.h>

#include "ptpservice.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  MaxClocks = 2;
        constexpr int64_t PptPerPpb = 1000;

        struct Slot {
                        char      name[8] = {};
                        PtpClock *clock = nullptr;
        };

        constinit Slot slots[MaxClocks];

} // namespace

Error PtpClock::addFiles() {
        for (size_t i = 0; i < EthernetDevice::count() && i < MaxClocks; ++i) {
                auto [device, err] = EthernetDevice::device(i);
                if (err.isError()) return err;
                void *block = Memory::alloc(MemSpace::Psram, sizeof(PtpClock));
                if (block == nullptr) return Error::NoMem;
                Slot &s = slots[i];
                s.clock = ::new (block) PtpClock(*device, static_cast<uint32_t>(i));
                std::snprintf(s.name, sizeof(s.name), "ptp%u", static_cast<unsigned>(i));
                Vfs::DeviceFile file;
                file.name = s.name;
                file.arg = &s;
                file.isDevice = true;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        const uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                             ((flags & Vfs::Write) ? FileHandle::Write : 0);
                        if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                        return FileHandle::open(*static_cast<Slot *>(arg)->clock, mode);
                };
                err = Vfs::addDeviceFile(file);
                if (err.isError()) return err;
        }
        return Error::Ok;
}

int64_t PtpClock::timeNs() const {
        return _device.ptpTime().toNs();
}

int32_t PtpClock::ppsPin() const {
        const EthernetDevice::PpsStatus pps = _device.ppsStatus();
        return pps.running ? static_cast<int32_t>(pps.pin) : -1;
}

Error PtpClock::set(int64_t ns) {
        if (PtpService::runsOn(_device)) return Error::Busy;
        if (ns < 0) return Error::Invalid;
        return _device.setPtpTime(PtpTime::fromNs(ns));
}

Error PtpClock::step(int64_t ns) {
        if (PtpService::runsOn(_device)) return Error::Busy;
        return _device.stepPtpTime(ns);
}

int64_t PtpClock::frequencyPpt() const {
        const double rate = double(_device.ptpAddend()) / EthernetDevice::NominalPtpAddend - 1.0;
        return std::llround(rate * 1e12);
}

Error PtpClock::setFrequencyPpt(int64_t ppt) {
        if (PtpService::runsOn(_device)) return Error::Busy;
        const int64_t ppb = ppt >= 0 ? (ppt + PptPerPpb / 2) / PptPerPpb : -((-ppt + PptPerPpb / 2) / PptPerPpb);
        if (ppb > MaxAdjustPpb || ppb < -MaxAdjustPpb) return Error::OutOfRange;
        return _device.setPtpFrequency(static_cast<int32_t>(ppb));
}

BMDS_NAMESPACE_END
