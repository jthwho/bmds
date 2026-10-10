/**
 * @file      blockdevices.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/blockdevices.h>
#include <bmds/memory.h>
#include <bmds/stats.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using BlockDevices::Entry;
        using BlockDevices::MaxDevices;
        using BlockDevices::MaxName;

        // Registration order; removal shifts the rest down.
        constinit Entry  entries[MaxDevices];
        constinit size_t entryCount = 0;

        // A device's counters (devplan/sysmon.md §6), storage.NAME: names are an interface once shipped.
        constexpr Stat::Desc DeviceStats[] = {
                {"reads", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Read requests"},
                {"writes", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Write requests"},
                {"read_bytes", Stat::Unit::Bytes, Stat::Kind::Total, Stat::KeepHistory, {}, "", "Bytes read"},
                {"written_bytes", Stat::Unit::Bytes, Stat::Kind::Total, Stat::KeepHistory, {}, "", "Bytes written"},
                {"errors",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {Stat::Rule::Zero, Stat::Health::Critical},
                 "",
                 "Requests the device failed (a medium removed isn't counted)"},
                {"io_us",
                 Stat::Unit::Micros,
                 Stat::Kind::Total,
                 0,
                 {},
                 "",
                 "Time in requests (a rate of 1,000,000 a second: busy all the time)"},
                {"erases", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Blocks erased (a flash: its wear)"},
        };

        void readDevice(void *arg, Span<int64_t> v);

        // The counters of the devices registered: slots in PSRAM, made with the first, so a group
        // never moves (the entries above do).
        struct DeviceCounters {
                        DeviceCounters()
                            : group(DeviceStats, sizeof(DeviceStats) / sizeof(DeviceStats[0]), readDevice, this) {}
                        BlockDevice *device = nullptr;
                        Stats::Group group;
        };

        constinit DeviceCounters *counters = nullptr; // MaxDevices of them, under the layer's lock

        void readDevice(void *arg, Span<int64_t> v) {
                const BlockDevice *d = static_cast<DeviceCounters *>(arg)->device;
                if (d == nullptr) return;
                const BlockDevice::IoStats s = d->ioStats();
                v[0] = s.reads;
                v[1] = s.writes;
                v[2] = int64_t(s.readBytes);
                v[3] = int64_t(s.writtenBytes);
                v[4] = s.errors;
                v[5] = int64_t(s.ioUs);
                v[6] = s.erases;
                return;
        }

        // Under the layer's lock. The registry's lock is never held while the layer's is taken (a
        // group's read takes no lock), so adding a group here is safe.
        void addCounters(BlockDevice &device, StringView name) {
                if (counters == nullptr) {
                        void *block = Memory::alloc(MemSpace::Psram, sizeof(DeviceCounters) * MaxDevices);
                        if (block == nullptr) return;
                        counters = static_cast<DeviceCounters *>(block);
                        for (size_t i = 0; i < MaxDevices; ++i) new (&counters[i]) DeviceCounters();
                }
                for (size_t i = 0; i < MaxDevices; ++i) {
                        DeviceCounters &c = counters[i];
                        if (c.device != nullptr) continue;
                        c.device = &device;
                        char full[Stats::MaxGroupName + 1];
                        std::snprintf(full, sizeof(full), "storage.%.*s", int(name.size()), name.data());
                        c.group.setName(full);
                        if (Stats::add(c.group).isError()) c.device = nullptr;
                        return;
                }
                return;
        }

        void removeCounters(BlockDevice &device) {
                if (counters == nullptr) return;
                for (size_t i = 0; i < MaxDevices; ++i) {
                        DeviceCounters &c = counters[i];
                        if (c.device != &device) continue;
                        Stats::remove(c.group);
                        c.device = nullptr;
                }
                return;
        }

        bool isValidName(StringView name) {
                if (name.isEmpty() || name.size() > MaxName || name[0] < 'a' || name[0] > 'z') return false;
                for (char c : name) {
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
                }
                return true;
        }

} // namespace

Error Fs::addDeviceLocked(BlockDevice &device, StringView name, BlockDevice *parent) {
        if (!isValidName(name)) return Error::Invalid;
        if (Fs::findEntryLocked(name) != nullptr || Fs::findEntryLocked(device) != nullptr) return Error::Exists;
        if (entryCount == MaxDevices) return Error::Full;
        Entry &e = entries[entryCount++];
        std::memcpy(e.name, name.data(), name.size());
        e.name[name.size()] = '\0';
        e.device = &device;
        e.parent = parent;
        addCounters(device, name);
        return Error();
}

void Fs::removeDeviceLocked(BlockDevice &device) {
        removeCounters(device);
        for (size_t i = 0; i < entryCount; ++i) {
                if (entries[i].device != &device) continue;
                for (size_t j = i + 1; j < entryCount; ++j) entries[j - 1] = entries[j];
                entries[--entryCount] = Entry();
                return;
        }
        return;
}

const Entry *Fs::findEntryLocked(const BlockDevice &device) {
        for (size_t i = 0; i < entryCount; ++i) {
                if (entries[i].device == &device) return &entries[i];
        }
        return nullptr;
}

const Entry *Fs::findEntryLocked(StringView name) {
        for (size_t i = 0; i < entryCount; ++i) {
                if (name == StringView(entries[i].name)) return &entries[i];
        }
        return nullptr;
}

size_t Fs::deviceCountLocked() {
        return entryCount;
}

const Entry &Fs::deviceAtLocked(size_t index) {
        return entries[index];
}

Error BlockDevices::addNumbered(BlockDevice &device, StringView prefix, char (&name)[MaxName + 1]) {
        // Leave room for two digits.
        if (prefix.size() + 2 > MaxName || !isValidName(prefix)) return Error::Invalid;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (unsigned n = 0; n < 100; ++n) {
                char candidate[MaxName + 1];
                std::snprintf(candidate, sizeof(candidate), "%.*s%u", static_cast<int>(prefix.size()), prefix.data(),
                              n);
                if (Fs::findEntryLocked(StringView(candidate)) != nullptr) continue;
                const Error err = Fs::addDeviceLocked(device, candidate, nullptr);
                if (err.isOk()) std::memcpy(name, candidate, sizeof(name));
                return err;
        }
        return Error::Full;
}

Error BlockDevices::add(BlockDevice &device, StringView name, BlockDevice *parent) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        return Fs::addDeviceLocked(device, name, parent);
}

void BlockDevices::remove(BlockDevice &device) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        Fs::removeDeviceLocked(device);
        return;
}

BlockDevice *BlockDevices::find(StringView name) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const Entry  *e = Fs::findEntryLocked(name);
        return e != nullptr ? e->device : nullptr;
}

Error BlockDevices::nameOf(const BlockDevice &device, char (&name)[MaxName + 1]) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const Entry  *e = Fs::findEntryLocked(device);
        if (e == nullptr) return Error::NotExist;
        std::memcpy(name, e->name, sizeof(name));
        return Error();
}

size_t BlockDevices::list(Span<Entry> out) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const size_t  n = entryCount < out.size() ? entryCount : out.size();
        for (size_t i = 0; i < n; ++i) out[i] = entries[i];
        return n;
}

BMDS_NAMESPACE_END
