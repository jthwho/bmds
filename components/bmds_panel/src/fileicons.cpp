/**
 * @file      fileicons.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's icons for file types (devplan/filebrowser.md §4h).
 */

#include <cstring>
#include <new>
#include <bmds/fileformat.h>
#include <bmds/memory.h>

#include "fileicons.h"

BMDS_NAMESPACE_BEGIN

void FileIcons::clear() {
        for (Kept *k : _kept) {
                Memory::free(k->pixels);
                Memory::free(k);
        }
        _kept.clear();
        return;
}

void FileIcons::forgetMissing() {
        for (size_t i = _kept.size(); i-- > 0;) {
                if (_kept[i]->pixels != nullptr) continue;
                Memory::free(_kept[i]);
                _kept.removeAt(i);
        }
        return;
}

const Icon *FileIcons::forType(StringView type, uint16_t side) {
        return find(type, side, false);
}

const Icon *FileIcons::named(StringView name, uint16_t side) {
        return find(name, side, true);
}

const Icon *FileIcons::find(StringView key, uint16_t side, bool isName) {
        if (key.isEmpty() || key.size() > MaxKey) return nullptr;
        for (const Kept *k : _kept)
                if (k->side == side && k->isName == isName && StringView(k->key) == key)
                        return k->pixels != nullptr ? &k->icon : nullptr;
        // Looked for once: the type's own, its generic one, unknown.
        const Icon *found = nullptr;
        if (isName) {
                found = _theme.find(key, side);
        } else {
                char name[FileFormat::MaxType + 1];
                found = _theme.find(FileFormat::iconName(key, Span<char>(name)), side);
                if (found == nullptr) found = _theme.find(FileFormat::genericIcon(key), side);
                if (found == nullptr) found = _theme.find("unknown", side);
        }
        if (_kept.reserve(_kept.size() + 1).isError()) return found; // not kept: valid for now
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Kept));
        if (mem == nullptr) return found;
        auto *k = ::new (mem) Kept();
        std::memcpy(k->key, key.data(), key.size());
        k->side = side;
        k->isName = isName;
        if (found != nullptr && found->isValid()) {
                const size_t n = size_t(found->width) * found->height;
                k->pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                if (k->pixels != nullptr) {
                        std::memcpy(k->pixels, found->pixels, n * sizeof(uint32_t));
                        k->icon = Icon{found->width, found->height, k->pixels};
                }
        }
        if (found != nullptr && k->pixels == nullptr) {
                // No memory for the copy: the theme's, for now, not kept.
                Memory::free(k);
                return found;
        }
        (void)_kept.append(k);
        return k->pixels != nullptr ? &k->icon : nullptr;
}

BMDS_NAMESPACE_END
