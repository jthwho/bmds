/**
 * @file      icontheme.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/icontheme.h>
#include <bmds/inireader.h>
#include <bmds/memory.h>
#include <bmds/pngdecoder.h>
#include <bmds/programicons.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxFileBytes = 64 * 1024;

        // @p path's bytes, in PSRAM (freed by the caller), or null.
        uint8_t *readFile(const char *path, size_t &bytes) {
                Vfs::Context context;
                auto [file, err] = Vfs::open(StringView(path), Vfs::Read, context);
                if (err.isError()) return nullptr;
                auto [info, serr] = Vfs::stat(StringView(path), context);
                if (serr.isError() || info.size == 0 || info.size > MaxFileBytes) return nullptr;
                auto *data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size_t(info.size)));
                if (data == nullptr) return nullptr;
                size_t got = 0;
                while (got < info.size) {
                        auto [n, rerr] = file->read(WritableByteSpan(data + got, size_t(info.size) - got), WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (got != info.size) {
                        Memory::free(data);
                        return nullptr;
                }
                bytes = got;
                return data;
        }

} // namespace

IconTheme::~IconTheme() {
        clear();
        Memory::free(_packIcons);
}

void IconTheme::clear() {
        while (_first != nullptr) {
                Kept *k = _first;
                _first = k->next;
                Memory::free(k);
        }
        _bytes = 0;
        return;
}

void IconTheme::setTheme(StringView name) {
        const size_t n = name.size() < MaxTheme ? name.size() : MaxTheme;
        std::memcpy(_theme, name.data(), n);
        _theme[n] = '\0';
        _inherits[0] = '\0';
        _inheritsRead = false;
        clear();
        return;
}

IconTheme::Kept *IconTheme::lookup(StringView key, uint16_t side, bool fromProgram) {
        for (Kept *k = _first; k != nullptr; k = k->next) {
                if (k->side == side && k->fromProgram == fromProgram && StringView(k->key(), k->keyLength) == key) {
                        k->age = ++_clock;
                        return k;
                }
        }
        return nullptr;
}

void IconTheme::evictFor(size_t bytes) {
        while (_first != nullptr && _bytes + bytes > MaxBytes) {
                // The one used longest ago (few are kept: a walk).
                Kept **oldest = nullptr;
                for (Kept **at = &_first; *at != nullptr; at = &(*at)->next)
                        if (!(*at)->pinned && (oldest == nullptr || (*at)->age < (*oldest)->age)) oldest = at;
                if (oldest == nullptr) return; // all found by id: kept
                Kept *gone = *oldest;
                *oldest = gone->next;
                _bytes -= gone->bytes;
                Memory::free(gone);
        }
        return;
}

// @p png decoded and kept under @p key (not found: nothing kept, null).
const Icon *IconTheme::keep(StringView key, uint16_t side, bool fromProgram, const uint8_t *png, size_t bytes) {
        if (png == nullptr || key.size() > MaxName) return nullptr;
        auto [size, err] = Png::size(ByteSpan(png, bytes));
        if (err.isError() || size.width > MaxSide || size.height > MaxSide) return nullptr;
        const size_t head = (sizeof(Kept) + key.size() + 1 + 3) & ~size_t(3);
        const size_t pixels = size_t(size.width) * size.height;
        const size_t total = head + pixels * sizeof(uint32_t);
        evictFor(total);
        auto *mem = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, total));
        if (mem == nullptr) return nullptr;
        auto *k = ::new (mem) Kept();
        auto *pix = reinterpret_cast<uint32_t *>(mem + head);
        auto [got, derr] = Png::decode(ByteSpan(png, bytes), Span<uint32_t>(pix, pixels), MemSpace::Psram);
        if (derr.isError()) {
                Memory::free(mem);
                return nullptr;
        }
        char *name = reinterpret_cast<char *>(k + 1);
        std::memcpy(name, key.data(), key.size());
        name[key.size()] = '\0';
        k->keyLength = static_cast<uint8_t>(key.size());
        k->icon = Icon{got.width, got.height, pix};
        k->age = ++_clock;
        k->bytes = static_cast<uint32_t>(total);
        k->side = side;
        k->fromProgram = fromProgram;
        k->next = _first;
        _first = k;
        _bytes += total;
        return &k->icon;
}

// The theme's parent (index.theme's Inherits=, its first), read once the file is there.
void IconTheme::readInherits() {
        if (_inheritsRead) return;
        char path[96];
        std::snprintf(path, sizeof(path), "%s/%s/index.theme", ThemesDir, _theme);
        size_t   bytes = 0;
        uint8_t *text = readFile(path, bytes);
        if (text == nullptr) return; // not there (yet: the card): read again next time
        _inheritsRead = true;
        _inherits[0] = '\0';
        IniReader reader(StringView(reinterpret_cast<const char *>(text), bytes));
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                if (item.kind != IniReader::Kind::Entry || item.name != "Icon Theme" || item.key != "Inherits")
                        continue;
                const size_t     comma = item.value.indexOf(',');
                const StringView first = (comma == StringView::NPos ? item.value : item.value.left(comma)).trimmed();
                if (first.size() <= MaxTheme) {
                        std::memcpy(_inherits, first.data(), first.size());
                        _inherits[first.size()] = '\0';
                }
                break;
        }
        Memory::free(text);
        return;
}

bool IconTheme::isBmds() const {
        return std::strcmp(_theme, DefaultTheme) == 0;
}

const IconPack *IconTheme::pack() {
        if (!_packTried) {
                _packTried = true;
                auto [bytes, err] = Vfs::map(StringView(PackPath), Vfs::Context());
                if (err.isOk()) {
                        auto [opened, perr] = IconPack::open(bytes);
                        if (perr.isOk()) _pack = opened;
                }
        }
        return _pack.count() > 0 ? &_pack : nullptr;
}

const Icon *IconTheme::packIcon(uint32_t id, uint16_t side) {
        const IconPack *p = pack();
        if (p == nullptr || id >= p->count()) return nullptr;
        size_t s = 0;
        while (s < IconPack::MaxSizes && p->side(s) != side) ++s;
        if (s == IconPack::MaxSizes) return nullptr;
        if (_packIcons == nullptr) {
                const size_t n = size_t(p->count()) * IconPack::MaxSizes;
                _packIcons = static_cast<Icon *>(Memory::alloc(MemSpace::Psram, n * sizeof(Icon)));
                if (_packIcons == nullptr) return nullptr;
                for (size_t i = 0; i < n; ++i) ::new (&_packIcons[i]) Icon();
        }
        Icon &icon = _packIcons[size_t(id) * IconPack::MaxSizes + s];
        if (!icon.isValid()) {
                const uint32_t *pixels = p->pixels(id, side);
                if (pixels == nullptr) return nullptr;
                icon = Icon{side, side, pixels};
        }
        return &icon;
}

bool IconTheme::isPermanent(const Icon *icon) const {
        if (icon == nullptr || _pack.count() == 0) return false;
        const auto *p = reinterpret_cast<const uint8_t *>(icon->pixels);
        return p >= _pack.bytes().data() && p < _pack.bytes().data() + _pack.bytes().size();
}

// @p icon (the bmds theme's) kept under @p key, so a name finds it again without looking in
// the files first: the record only, its pixels where they are.
const Icon *IconTheme::keepMapped(StringView key, uint16_t side, const Icon *icon, bool pinned) {
        if (icon == nullptr || key.size() > MaxName) return icon;
        const size_t head = (sizeof(Kept) + key.size() + 1 + 3) & ~size_t(3);
        evictFor(head);
        auto *mem = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, head));
        if (mem == nullptr) return icon;
        auto *k = ::new (mem) Kept();
        char *name = reinterpret_cast<char *>(k + 1);
        std::memcpy(name, key.data(), key.size());
        name[key.size()] = '\0';
        k->keyLength = static_cast<uint8_t>(key.size());
        k->icon = *icon;
        k->age = ++_clock;
        k->bytes = static_cast<uint32_t>(head);
        k->side = side;
        k->pinned = pinned;
        k->next = _first;
        _first = k;
        _bytes += head;
        return icon;
}

// A PNG at @p path of @p side pixels (any size for a path), kept under @p key; null if not there.
const Icon *IconTheme::readKept(const char *path, StringView key, uint16_t side, bool anySize) {
        size_t   bytes = 0;
        uint8_t *png = readFile(path, bytes);
        if (png == nullptr) return nullptr;
        auto [size, err] = Png::size(ByteSpan(png, bytes));
        const Icon *icon = nullptr;
        if (err.isOk() && (anySize || (size.width == side && size.height == side)))
                icon = keep(key, side, false, png, bytes);
        Memory::free(png);
        return icon;
}

const Icon *IconTheme::findFile(StringView name, uint16_t side, bool userToo) {
        // The user's, the theme (on the card), what it inherits: never bmds's, which is the ROM's.
        char   dirs[3][80];
        size_t count = 0;
        if (userToo) std::snprintf(dirs[count++], sizeof(dirs[0]), "/cfg/icons/%ux%u/", side, side);
        if (!isBmds()) {
                readInherits();
                std::snprintf(dirs[count++], sizeof(dirs[0]), "%s/%s/%ux%u/", ThemesDir, _theme, side, side);
                if (_inherits[0] != '\0' && std::strcmp(_inherits, _theme) != 0 &&
                    std::strcmp(_inherits, DefaultTheme) != 0)
                        std::snprintf(dirs[count++], sizeof(dirs[0]), "%s/%s/%ux%u/", ThemesDir, _inherits, side, side);
        }
        for (size_t i = 0; i < count; ++i) {
                char path[512]; // a directory (under 80) and a name (MaxName) with .png
                std::snprintf(path, sizeof(path), "%s%.*s%s", dirs[i], int(name.size()), name.data(),
                              name.endsWith(StringView(".png")) ? "" : ".png");
                if (const Icon *icon = readKept(path, name, side, false); icon != nullptr) return icon;
        }
        return nullptr;
}

const Icon *IconTheme::find(StringView name, uint16_t side) {
        if (name.isEmpty() || name.size() > MaxName || side == 0 || side > MaxSide) return nullptr;
        if (Kept *k = lookup(name, side, false); k != nullptr) return &k->icon;
        if (name.startsWith('/')) {
                char path[MaxName + 1];
                std::snprintf(path, sizeof(path), "%.*s", int(name.size()), name.data());
                return readKept(path, name, side, true);
        }
        if (const Icon *icon = findFile(name, side, true); icon != nullptr) return icon;
        if (const IconPack *p = pack(); p != nullptr) {
                const uint32_t id = p->idOf(name);
                if (id != IconPack::NoIcon) {
                        if (const Icon *icon = packIcon(id, side); icon != nullptr)
                                return keepMapped(name, side, icon, false);
                }
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/hicolor/%ux%u/apps/%.*s.png", ThemesDir, side, side, int(name.size()),
                      name.data());
        return readKept(path, name, side, false);
}

const Icon *IconTheme::find(IconId iconId, uint16_t side) {
        const IconPack *p = pack();
        if (p == nullptr || side == 0 || side > MaxSide) return nullptr;
        const uint32_t id = static_cast<uint32_t>(iconId);
        if (isBmds()) return packIcon(id, side);
        // Another theme: its icon of the name first, found once and kept until setTheme().
        const StringView name = p->name(id);
        if (name.isEmpty()) return nullptr;
        if (Kept *k = lookup(name, side, false); k != nullptr) {
                k->pinned = true;
                return &k->icon;
        }
        if (const Icon *icon = findFile(name, side, false); icon != nullptr) {
                if (Kept *k = lookup(name, side, false); k != nullptr) k->pinned = true;
                return icon;
        }
        return keepMapped(name, side, packIcon(id, side), true);
}

const Icon *IconTheme::program(StringView path, uint16_t side) {
        if (path.isEmpty() || path.size() > MaxName || side == 0 || side > MaxSide) return nullptr;
        if (Kept *k = lookup(path, side, true); k != nullptr) return &k->icon;
        ProgramIcons::Image image;
        const Vfs::Context  context;
        const Error         err = ProgramIcons::read(path, context, side, MemSpace::Psram, image);
        const Icon         *icon = keep(path, side, true, err.isOk() ? image.png : nullptr, image.bytes);
        Memory::free(image.png);
        return icon;
}

const Icon *IconTheme::forProgram(StringView name, StringView path, uint16_t side) {
        const Icon *icon = find(name, side);
        if (icon == nullptr) icon = program(path, side);
        if (icon == nullptr) icon = find(IconId::ApplicationXExecutable, side);
        return icon;
}

BMDS_NAMESPACE_END
