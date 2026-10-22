/**
 * @file      guiatoms.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "guiatoms.h"

#include <cstring>
#include <new>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Each name its own allocation (they never move), its length before it.
        struct Name {
                        const char *text;
                        uint32_t    length;
        };

        // Made by init(): no global constructors.
        struct Table {
                        Mutex      lock;
                        List<Name> names{MemSpace::Psram}; // the atom's number - 1
        };
        Table *table = nullptr;

        uint32_t findLocked(StringView name) {
                for (size_t i = 0; i < table->names.size(); ++i) {
                        const Name &n = table->names[i];
                        if (n.length == name.size() && std::memcmp(n.text, name.data(), name.size()) == 0)
                                return uint32_t(i + 1);
                }
                return 0;
        }

} // namespace

Error GuiAtoms::init() {
        if (table != nullptr) return Error::Ok;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Table));
        if (mem == nullptr) return Error::NoMem;
        table = ::new (mem) Table();
        return Error::Ok;
}

uint32_t GuiAtoms::find(StringView name) {
        if (table == nullptr) return 0;
        Mutex::Locker locked(table->lock, WaitForever);
        return findLocked(name);
}

uint32_t GuiAtoms::intern(StringView name) {
        if (table == nullptr || name.isEmpty() || name.size() > MaxName) return 0;
        Mutex::Locker  locked(table->lock, WaitForever);
        const uint32_t have = findLocked(name);
        if (have != 0) return have;
        auto *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, name.size()));
        if (text == nullptr) return 0;
        std::memcpy(text, name.data(), name.size());
        if (table->names.append(Name{text, uint32_t(name.size())}).isError()) {
                Memory::free(text);
                return 0;
        }
        return uint32_t(table->names.size());
}

StringView GuiAtoms::name(uint32_t atom) {
        if (table == nullptr) return StringView();
        Mutex::Locker locked(table->lock, WaitForever);
        if (atom == 0 || atom > table->names.size()) return StringView();
        const Name &n = table->names[atom - 1];
        return StringView(n.text, n.length);
}

BMDS_NAMESPACE_END
