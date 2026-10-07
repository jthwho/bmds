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
                Kept **oldest = &_first;
                for (Kept **at = &_first; *at != nullptr; at = &(*at)->next)
                        if ((*at)->age < (*oldest)->age) oldest = at;
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

const Icon *IconTheme::find(StringView name, uint16_t side) {
        if (name.isEmpty() || name.size() > MaxName || side == 0 || side > MaxSide) return nullptr;
        if (Kept *k = lookup(name, side, false); k != nullptr) return &k->icon;
        const bool isPath = name.startsWith('/');
        // Where it may be: a path as it is; else the user's, the theme, what it inherits, the
        // default theme, hicolor.
        char   dirs[5][80];
        size_t count = 0;
        if (isPath) {
                dirs[count++][0] = '\0';
        } else {
                readInherits();
                std::snprintf(dirs[count++], sizeof(dirs[0]), "/boot/icons/%ux%u/", side, side);
                const char *themes[] = {_theme, _inherits, DefaultTheme};
                for (size_t t = 0; t < 3; ++t) {
                        bool again = themes[t][0] == '\0';
                        for (size_t u = 0; u < t; ++u) again = again || std::strcmp(themes[u], themes[t]) == 0;
                        if (!again)
                                std::snprintf(dirs[count++], sizeof(dirs[0]), "%s/%s/%ux%u/", ThemesDir, themes[t],
                                              side, side);
                }
                std::snprintf(dirs[count++], sizeof(dirs[0]), "%s/hicolor/%ux%u/apps/", ThemesDir, side, side);
        }
        uint8_t *png = nullptr;
        size_t   bytes = 0;
        for (size_t i = 0; i < count && png == nullptr; ++i) {
                char path[512]; // a directory (under 80) and a name (MaxName) with .png
                std::snprintf(path, sizeof(path), "%s%.*s%s", dirs[i], int(name.size()), name.data(),
                              isPath || name.endsWith(StringView(".png")) ? "" : ".png");
                png = readFile(path, bytes);
                // A name's icon of another size isn't this size's (a path's is what it is): further on.
                auto [size, err] = Png::size(ByteSpan(png, png != nullptr ? bytes : 0));
                if (png != nullptr && !isPath && (err.isError() || size.width != side || size.height != side)) {
                        Memory::free(png);
                        png = nullptr;
                }
        }
        const Icon *icon = keep(name, side, false, png, bytes);
        Memory::free(png);
        return icon;
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
        if (icon == nullptr) icon = find("application-x-executable", side);
        return icon;
}

BMDS_NAMESPACE_END
