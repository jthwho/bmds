/**
 * @file      fileicons.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Icons for file types (devplan/filebrowser.md §4h).
 */

#include <cstring>
#include <new>
#include <bmds/fileformat.h>
#include <bmds/fileicons.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

void FileIcons::clear() {
        for (Kept *k : _kept) {
                Memory::free(k->pixels);
                Memory::free(k->dimPixels);
                Memory::free(k);
        }
        _kept.clear();
        _wanted.clear();
        return;
}

void FileIcons::forgetMissing() {
        for (size_t i = _kept.size(); i-- > 0;) {
                if (_kept[i]->icon.isValid()) continue;
                Memory::free(_kept[i]);
                _kept.removeAt(i);
        }
        return;
}

const Icon *FileIcons::dimmed(const Icon *icon) {
        for (Kept *k : _kept) {
                if (icon != &k->icon || !k->icon.isValid()) continue;
                if (k->dimPixels == nullptr) {
                        const size_t n = size_t(icon->width) * icon->height;
                        k->dimPixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                        if (k->dimPixels == nullptr) return icon;
                        for (size_t i = 0; i < n; ++i) k->dimPixels[i] = halfAlpha(icon->pixels[i]);
                        k->dim = Icon{icon->width, icon->height, k->dimPixels};
                }
                return &k->dim;
        }
        return icon;
}

const Icon *FileIcons::forType(StringView type, uint16_t side) {
        return find(type, side, Kind::Type);
}

const Icon *FileIcons::named(StringView name, uint16_t side) {
        return find(name, side, Kind::Name);
}

const Icon *FileIcons::forApp(StringView folder, uint16_t side) {
        return find(folder, side, Kind::App);
}

const FileIcons::Kept *FileIcons::kept(StringView key, uint16_t side, Kind kind) const {
        for (const Kept *k : _kept)
                if (k->side == side && k->kind == kind && StringView(k->key) == key) return k;
        return nullptr;
}

const Icon *FileIcons::loadedForType(StringView type, uint16_t side) {
        return loaded(type, side, Kind::Type);
}

const Icon *FileIcons::loadedNamed(StringView name, uint16_t side) {
        return loaded(name, side, Kind::Name);
}

const Icon *FileIcons::loadedForApp(StringView folder, uint16_t side) {
        return loaded(folder, side, Kind::App);
}

const Icon *FileIcons::loaded(StringView key, uint16_t side, Kind kind) {
        if (key.isEmpty() || key.size() > MaxKey) return nullptr;
        if (const Kept *k = kept(key, side, kind); k != nullptr) return k->icon.isValid() ? &k->icon : nullptr;
        for (const Wanted &w : _wanted)
                if (w.side == side && w.kind == kind && StringView(w.key) == key) return nullptr; // asked
        Wanted w;
        std::memcpy(w.key, key.data(), key.size());
        w.side = side;
        w.kind = kind;
        if (_wanted.append(w).isError()) return nullptr; // asked again at the next draw
        if (_asked != nullptr) _asked(_askedArg);
        return nullptr;
}

bool FileIcons::loadAsked() {
        const bool any = !_wanted.isEmpty();
        for (const Wanted &w : _wanted) (void)find(StringView(w.key), w.side, w.kind);
        _wanted.clear();
        return any;
}

const Icon *FileIcons::find(StringView key, uint16_t side, Kind kind) {
        if (key.isEmpty() || key.size() > MaxKey) return nullptr;
        if (const Kept *k = kept(key, side, kind); k != nullptr) return k->icon.isValid() ? &k->icon : nullptr;
        // Looked for once: the type's own, its generic one, unknown; an application's, its own.
        const Icon *found = nullptr;
        if (kind == Kind::Name) {
                found = _theme.find(key, side);
        } else if (kind == Kind::App) {
                found = _theme.forAppFolder(key, side);
        } else {
                char name[FileFormat::MaxType + 1];
                found = _theme.find(FileFormat::iconName(key, Span<char>(name)), side);
                if (found == nullptr) found = _theme.find(FileFormat::genericIcon(key), side);
                if (found == nullptr) found = _theme.find(IconId::Unknown, side);
        }
        if (_kept.reserve(_kept.size() + 1).isError()) return found; // not kept: valid for now
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Kept));
        if (mem == nullptr) return found;
        auto *k = ::new (mem) Kept();
        std::memcpy(k->key, key.data(), key.size());
        k->side = side;
        k->kind = kind;
        if (found != nullptr && found->isValid() && _theme.isPermanent(found)) {
                k->icon = *found; // the ROM's: used where it is
        } else if (found != nullptr && found->isValid()) {
                const size_t n = size_t(found->width) * found->height;
                k->pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, n * sizeof(uint32_t)));
                if (k->pixels != nullptr) {
                        std::memcpy(k->pixels, found->pixels, n * sizeof(uint32_t));
                        k->icon = Icon{found->width, found->height, k->pixels};
                }
        }
        if (found != nullptr && !k->icon.isValid()) {
                // No memory for the copy: the theme's, for now, not kept.
                Memory::free(k);
                return found;
        }
        (void)_kept.append(k);
        return k->icon.isValid() ? &k->icon : nullptr;
}

BMDS_NAMESPACE_END
