/**
 * @file      blockdevices.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <bmds/blockdevices.h>

#include "fatlayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using BlockDevices::Entry;
        using BlockDevices::MaxDevices;
        using BlockDevices::MaxName;

        // Registration order; removal shifts the rest down.
        constinit Entry  entries[MaxDevices];
        constinit size_t entryCount = 0;

        bool isValidName(StringView name) {
                if (name.isEmpty() || name.size() > MaxName || name[0] < 'a' || name[0] > 'z') return false;
                for (char c : name) {
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
                }
                return true;
        }

} // namespace

Error Fat::addDeviceLocked(BlockDevice &device, StringView name, BlockDevice *parent) {
        if (!isValidName(name)) return Error::Invalid;
        if (Fat::findEntryLocked(name) != nullptr || Fat::findEntryLocked(device) != nullptr) return Error::Exists;
        if (entryCount == MaxDevices) return Error::Full;
        Entry &e = entries[entryCount++];
        std::memcpy(e.name, name.data(), name.size());
        e.name[name.size()] = '\0';
        e.device = &device;
        e.parent = parent;
        return Error();
}

void Fat::removeDeviceLocked(BlockDevice &device) {
        for (size_t i = 0; i < entryCount; ++i) {
                if (entries[i].device != &device) continue;
                for (size_t j = i + 1; j < entryCount; ++j) entries[j - 1] = entries[j];
                entries[--entryCount] = Entry();
                return;
        }
        return;
}

const Entry *Fat::findEntryLocked(const BlockDevice &device) {
        for (size_t i = 0; i < entryCount; ++i) {
                if (entries[i].device == &device) return &entries[i];
        }
        return nullptr;
}

const Entry *Fat::findEntryLocked(StringView name) {
        for (size_t i = 0; i < entryCount; ++i) {
                if (name == StringView(entries[i].name)) return &entries[i];
        }
        return nullptr;
}

size_t Fat::deviceCountLocked() {
        return entryCount;
}

const Entry &Fat::deviceAtLocked(size_t index) {
        return entries[index];
}

Error BlockDevices::addNumbered(BlockDevice &device, StringView prefix, char (&name)[MaxName + 1]) {
        // Leave room for two digits.
        if (prefix.size() + 2 > MaxName || !isValidName(prefix)) return Error::Invalid;
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (unsigned n = 0; n < 100; ++n) {
                char candidate[MaxName + 1];
                std::snprintf(candidate, sizeof(candidate), "%.*s%u", static_cast<int>(prefix.size()), prefix.data(),
                              n);
                if (Fat::findEntryLocked(StringView(candidate)) != nullptr) continue;
                const Error err = Fat::addDeviceLocked(device, candidate, nullptr);
                if (err.isOk()) std::memcpy(name, candidate, sizeof(name));
                return err;
        }
        return Error::Full;
}

Error BlockDevices::add(BlockDevice &device, StringView name, BlockDevice *parent) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        return Fat::addDeviceLocked(device, name, parent);
}

void BlockDevices::remove(BlockDevice &device) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        Fat::removeDeviceLocked(device);
        return;
}

BlockDevice *BlockDevices::find(StringView name) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        const Entry  *e = Fat::findEntryLocked(name);
        return e != nullptr ? e->device : nullptr;
}

Error BlockDevices::nameOf(const BlockDevice &device, char (&name)[MaxName + 1]) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        const Entry  *e = Fat::findEntryLocked(device);
        if (e == nullptr) return Error::NotExist;
        std::memcpy(name, e->name, sizeof(name));
        return Error();
}

size_t BlockDevices::list(Span<Entry> out) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        const size_t  n = entryCount < out.size() ? entryCount : out.size();
        for (size_t i = 0; i < n; ++i) out[i] = entries[i];
        return n;
}

BMDS_NAMESPACE_END
