/**
 * @file      fontlibrary.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cmath>
#include <cstring>
#include <new>
#include <bmds/bitmapfont.h>
#include <bmds/fontface.h>
#include <bmds/fontlibrary.h>
#include <bmds/glyphcache.h>

#include "fontrecord.h"

BMDS_NAMESPACE_BEGIN

namespace {

        bool sameText(StringView a, StringView b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i) {
                        char x = a[i], y = b[i];
                        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
                        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
                        if (x != y) return false;
                }
                return true;
        }

        bool isFontFile(StringView path) {
                if (path.size() < 5) return false;
                const StringView ext = path.mid(path.size() - 4);
                return sameText(ext, ".ttf") || sameText(ext, ".otf");
        }

        void copyText(char *out, size_t capacity, StringView text) {
                const size_t n = text.size() < capacity - 1 ? text.size() : capacity - 1;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        FontFace *newFace() {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(FontFace));
                return memory == nullptr ? nullptr : new (memory) FontFace;
        }

        void deleteFace(FontFace *face) {
                if (face == nullptr) return;
                face->~FontFace();
                Memory::free(face);
                return;
        }

} // namespace

FontLibrary::FontLibrary(GlyphCache &cache, FontFiles *files) : _cache(cache), _files(files) {}

FontLibrary::~FontLibrary() {
        for (Record *&r : _faces) {
                if (r == nullptr) continue;
                deleteFace(r->face);
                Memory::free(r->owned);
                r->~Record();
                Memory::free(r);
                r = nullptr;
        }
}

Error FontLibrary::addBitmap(const BitmapFont &font, StringView family, uint8_t ascent) {
        Mutex::Locker  lock(_mutex, WaitForever);
        const uint32_t n = _count.load(std::memory_order_relaxed);
        if (n == MaxFaces) return Error::Full;
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Record));
        if (memory == nullptr) return Error::NoMem;
        Record *r = new (memory) Record;
        copyText(r->family, sizeof(r->family), family);
        copyText(r->style, sizeof(r->style), "Regular");
        r->bitmap = &font;
        r->bitmapAscent = ascent;
        r->fixedPitch = true;
        r->glyphs = static_cast<uint16_t>(font.glyphCount() > 0xffff ? 0xffff : font.glyphCount());
        _faces[n] = r;
        if (_unscii == 0xffff) _unscii = static_cast<uint16_t>(n);
        _count.store(n + 1, std::memory_order_release);
        return Error::Ok;
}

void FontLibrary::addFile(StringView path) {
        if (!isFontFile(path) || path.size() > MaxPath) return;
        const uint32_t n = _count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i)
                if (_faces[i]->bitmap == nullptr && StringView(_faces[i]->path) == path) {
                        _faces[i]->seen = true;
                        return;
                }
        if (n == MaxFaces) {
                _full = true;
                return;
        }
        // A file in place stays open; another is read for its names, then let go until used.
        ByteSpan      bytes;
        Span<uint8_t> owned;
        auto [mapped, merr] = _files->map(path);
        if (merr.isOk()) {
                bytes = mapped;
        } else {
                auto [loaded, lerr] = _files->load(path);
                if (lerr.isError()) return;
                owned = loaded;
                bytes = ByteSpan(owned.data(), owned.size());
        }
        FontFace *face = newFace();
        if (face == nullptr || face->open(bytes).isError()) {
                deleteFace(face);
                Memory::free(owned.data());
                return;
        }
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Record));
        if (memory == nullptr) {
                deleteFace(face);
                Memory::free(owned.data());
                return;
        }
        Record *r = new (memory) Record;
        copyText(r->path, sizeof(r->path), path);
        copyText(r->family, sizeof(r->family), face->family());
        copyText(r->style, sizeof(r->style), face->style());
        r->weight = face->weight();
        r->italic = face->isItalic();
        r->fixedPitch = face->isFixedPitch();
        r->glyphs = face->glyphCount();
        r->seen = true;
        if (owned.data() == nullptr) {
                r->face = face; // mapped: open for good
                r->mapped = true;
        } else {
                deleteFace(face);
                Memory::free(owned.data());
        }
        _faces[n] = r;
        _count.store(n + 1, std::memory_order_release);
        return;
}

Error FontLibrary::scan(StringView folders) {
        if (_files == nullptr) return Error::Ok;
        Mutex::Locker  lock(_mutex, WaitForever);
        const uint32_t n = _count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) _faces[i]->seen = _faces[i]->bitmap != nullptr;
        _full = false;
        StringView rest = folders;
        while (!rest.isEmpty()) {
                const size_t     semi = rest.indexOf(';');
                const StringView dir = semi == StringView::NPos ? rest : rest.left(semi);
                rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                if (dir.isEmpty()) continue;
                (void)_files->list(
                        dir, [](void *arg, StringView path) { static_cast<FontLibrary *>(arg)->addFile(path); }, this);
        }
        const uint32_t now = _count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < now; ++i) {
                Record &r = *_faces[i];
                r.present = r.seen;
                // A face whose file has gone: its bytes let go once nothing uses it.
                if (!r.present && r.users.load() == 0 && !r.mapped && r.face != nullptr) {
                        deleteFace(r.face);
                        r.face = nullptr;
                        Memory::free(r.owned);
                        r.owned = nullptr;
                        r.ownedBytes = 0;
                        _cache.forget(static_cast<uint16_t>(i));
                }
        }
        // The fallback faces again: new files may give them.
        char names[MaxFallbacks][FontName::MaxFamily + 1];
        std::memcpy(names, _fallbackNames, sizeof(names));
        const uint8_t count = _fallbackNameCount;
        for (uint8_t i = 0; i < _fallbackCount; ++i) drop(static_cast<uint16_t>(_fallbacks[i]));
        _fallbackCount = 0;
        for (uint8_t i = 0; i < count; ++i) {
                bool      madeUp = false;
                const int face = findFamily(StringView(names[i]), 0, madeUp);
                if (face < 0 || load(*_faces[face]).isError()) continue;
                retain(static_cast<uint16_t>(face));
                _fallbacks[_fallbackCount++] = static_cast<int16_t>(face);
        }
        return _full ? Error::Full : Error::Ok;
}

size_t FontLibrary::faceCount() const {
        return _count.load(std::memory_order_acquire);
}

FontLibrary::FaceInfo FontLibrary::face(size_t index) const {
        FaceInfo info;
        if (index >= faceCount()) return info;
        Mutex::Locker lock(_mutex, WaitForever);
        const Record &r = *_faces[index];
        info.family = StringView(r.family);
        info.style = StringView(r.style);
        info.path = StringView(r.path);
        info.weight = r.weight;
        info.glyphs = r.glyphs;
        info.bitmapSize = r.bitmap != nullptr ? r.bitmap->height() : 0;
        info.italic = r.italic;
        info.fixedPitch = r.fixedPitch;
        info.present = r.present;
        info.loaded = r.bitmap != nullptr || r.face != nullptr;
        info.users = r.users.load();
        return info;
}

Error FontLibrary::setRole(FontName::Role role, const FontName &name) {
        if (role == FontName::Role::None || name.role != FontName::Role::None || name.familyLength == 0)
                return Error::Invalid;
        Mutex::Locker lock(_mutex, WaitForever);
        _roles[static_cast<size_t>(role)] = name;
        return Error::Ok;
}

FontName FontLibrary::role(FontName::Role role) const {
        Mutex::Locker lock(_mutex, WaitForever);
        return _roles[static_cast<size_t>(role)];
}

void FontLibrary::setFallback(StringView families) {
        Mutex::Locker lock(_mutex, WaitForever);
        _fallbackNameCount = 0;
        StringView rest = families;
        while (!rest.isEmpty() && _fallbackNameCount < MaxFallbacks) {
                size_t cut = rest.indexOf(';');
                if (const size_t comma = rest.indexOf(','); comma < cut) cut = comma;
                StringView one = cut == StringView::NPos ? rest : rest.left(cut);
                rest = cut == StringView::NPos ? StringView() : rest.mid(cut + 1);
                while (!one.isEmpty() && one[0] == ' ') one = one.mid(1);
                while (!one.isEmpty() && one[one.size() - 1] == ' ') one = one.left(one.size() - 1);
                if (one.isEmpty() || one.size() > FontName::MaxFamily) continue;
                copyText(_fallbackNames[_fallbackNameCount++], FontName::MaxFamily + 1, one);
        }
        for (uint8_t i = 0; i < _fallbackCount; ++i) drop(static_cast<uint16_t>(_fallbacks[i]));
        _fallbackCount = 0;
        for (uint8_t i = 0; i < _fallbackNameCount; ++i) {
                bool      madeUp = false;
                const int face = findFamily(StringView(_fallbackNames[i]), 0, madeUp);
                if (face < 0 || load(*_faces[face]).isError()) continue;
                retain(static_cast<uint16_t>(face));
                _fallbacks[_fallbackCount++] = static_cast<int16_t>(face);
        }
        return;
}

int FontLibrary::findFamily(StringView family, uint8_t style, bool &madeUpBold) const {
        const bool     italic = (style & FontName::Italic) != 0;
        const int      want = (style & FontName::Bold) != 0 ? 700 : 400;
        int            best = -1, bestScore = 0;
        const uint32_t n = _count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
                const Record &r = *_faces[i];
                if (!r.present || !sameText(StringView(r.family), family)) continue;
                const int score =
                        (r.italic != italic ? 10000 : 0) + (r.weight > want ? r.weight - want : want - r.weight);
                if (best < 0 || score < bestScore) {
                        best = int(i);
                        bestScore = score;
                }
        }
        madeUpBold = best >= 0 && want == 700 && _faces[best]->weight < 600;
        return best;
}

Error FontLibrary::load(Record &r) {
        if (r.bitmap != nullptr || r.face != nullptr) return Error::Ok;
        if (_files == nullptr) return Error::NotExist;
        auto [loaded, err] = _files->load(StringView(r.path));
        if (err.isError()) return err;
        FontFace *face = newFace();
        if (face == nullptr) {
                Memory::free(loaded.data());
                return Error::NoMem;
        }
        if (Error oerr = face->open(ByteSpan(loaded.data(), loaded.size())); oerr.isError()) {
                deleteFace(face);
                Memory::free(loaded.data());
                return Error::CorruptData;
        }
        r.owned = loaded.data();
        r.ownedBytes = loaded.size();
        r.face = face;
        return Error::Ok;
}

Result<Font> FontLibrary::open(StringView text) {
        auto [name, err] = FontName::parse(text);
        if (err.isError()) return makeError<Font>(err);
        return open(name);
}

Result<Font> FontLibrary::open(const FontName &name) {
        Mutex::Locker lock(_mutex, WaitForever);
        return openLocked(name, 0);
}

Result<Font> FontLibrary::openLocked(const FontName &name, int depth) {
        FontName want = name;
        if (want.role != FontName::Role::None) {
                const FontName &given = _roles[static_cast<size_t>(want.role)];
                FontName        resolved = given.familyLength != 0 ? given : FontName::make("unscii", 0, 0);
                if (want.size != 0) resolved.size = want.size;
                if (want.hasStyle) resolved.style = want.style;
                want = resolved;
        }
        bool madeUpBold = false;
        int  index = findFamily(want.familyName(), want.style, madeUpBold);
        if (index < 0 || load(*_faces[index]).isError()) {
                // A family it lacks (or can't read): the ui role's, then unscii.
                if (depth == 0 && name.role != FontName::Role::Ui) {
                        FontName ui;
                        ui.role = FontName::Role::Ui;
                        ui.size = want.size;
                        ui.style = want.style;
                        ui.hasStyle = true;
                        return openLocked(ui, 1);
                }
                if (_unscii == 0xffff) return makeError<Font>(Error::NotExist);
                index = _unscii;
                madeUpBold = (want.style & FontName::Bold) != 0;
        }
        const Record &r = *_faces[index];
        Font          f;
        f._library = this;
        f._face = static_cast<uint16_t>(index);
        f._madeUpBold = madeUpBold;
        if (r.bitmap != nullptr) {
                const unsigned height = r.bitmap->height();
                const unsigned size = want.size != 0 ? want.size : height;
                unsigned       scale = (size + height / 2) / height;
                if (scale < 1) scale = 1;
                if (scale > 8) scale = 8;
                f._scale = static_cast<uint8_t>(scale);
                f._size = static_cast<uint16_t>(height * scale);
                f._metrics.ascent = static_cast<int16_t>(r.bitmapAscent * scale);
                f._metrics.descent = static_cast<int16_t>((height - r.bitmapAscent) * scale);
                f._metrics.lineHeight = static_cast<int16_t>(height * scale);
                f._metrics.maxAdvance = static_cast<int16_t>(2 * r.bitmap->width() * scale + (madeUpBold ? 1 : 0));
        } else {
                const FontFace &face = *r.face;
                f._size = want.size != 0 ? want.size : DefaultSize;
                f._unitsTo64 = int32_t((int64_t(f._size) << 22) / face.unitsPerEm());
                const float s = face.scale(f._size);
                f._metrics.ascent = static_cast<int16_t>(std::ceil(float(face.metrics().ascent) * s));
                f._metrics.descent = static_cast<int16_t>(std::ceil(-float(face.metrics().descent) * s));
                f._metrics.lineHeight = static_cast<int16_t>(f._metrics.ascent + f._metrics.descent +
                                                             std::lround(float(face.metrics().lineGap) * s));
                f._metrics.maxAdvance =
                        static_cast<int16_t>(std::ceil(float(face.metrics().maxAdvance) * s) + (madeUpBold ? 1 : 0));
        }
        retain(f._face);
        return makeResult(static_cast<Font &&>(f));
}

void FontLibrary::retain(uint16_t face) const {
        _faces[face]->users.fetch_add(1, std::memory_order_relaxed);
        return;
}

void FontLibrary::drop(uint16_t face) {
        _faces[face]->users.fetch_sub(1, std::memory_order_acq_rel);
        return;
}

size_t FontLibrary::reclaim(MemSpace space, size_t bytes) {
        if (space != MemSpace::Psram && space != MemSpace::PsramDma) return 0;
        size_t freed = _cache.reclaim(space, bytes);
        if (freed >= bytes) return freed;
        // Then the faces read into memory that nothing uses. Not if the library is busy: this runs
        // inside an allocation, maybe one the library makes holding its lock (a scan, a load).
        if (_mutex.tryLock().isError()) return freed;
        const uint32_t n = _count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n && freed < bytes; ++i) {
                Record &r = *_faces[i];
                if (r.mapped || r.owned == nullptr || r.users.load() != 0) continue;
                deleteFace(r.face);
                r.face = nullptr;
                Memory::free(r.owned);
                r.owned = nullptr;
                freed += sizeof(FontFace) + r.ownedBytes;
                r.ownedBytes = 0;
                ++_reclaims;
        }
        _mutex.unlock();
        return freed;
}

Memory::Reclaimer::Usage FontLibrary::usage() const {
        const GlyphCache::Stats s = _cache.stats();
        Usage                   u;
        u.items = "glyphs";
        u.bytes = s.bytes;
        u.count = s.entries;
        u.reclaimable = s.bytes;
        u.hits = s.hits;
        u.misses = s.misses;
        u.reclaims = _reclaims;
        return u;
}

BMDS_NAMESPACE_END
