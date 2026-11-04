/**
 * @file      fonts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "fonts.h"
#include <stdio.h>

namespace preso {

        namespace {

                void nameOf(const FontKey &key, const char *family, char *out, size_t size) {
                        snprintf(out, size, "%s %u%s%s", family, unsigned(key.size),
                                 (key.style & FontKey::Bold) != 0 ? " bold" : "",
                                 (key.style & FontKey::Italic) != 0 ? " italic" : "");
                        return;
                }

                uint32_t characters(StringView text) {
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < text.length(); i++) {
                                if ((uint8_t(text[i]) & 0xC0) != 0x80) n++;
                        }
                        return n;
                }

        } // namespace

        FontCache::Entry &FontCache::get(const FontKey &key) {
                _clock++;
                for (uint32_t i = 0; i < _count; i++) {
                        if (_entries[i].key == key) {
                                _entries[i].used = _clock;
                                return _entries[i];
                        }
                }
                uint32_t slot = _count;
                if (_count == MaxOpen) {
                        slot = 0;
                        for (uint32_t i = 1; i < _count; i++) {
                                if (_entries[i].used < _entries[slot].used) slot = i;
                        }
                        if (_entries[slot].handle >= 0) _platform.closeFont(_entries[slot].handle);
                } else {
                        _count++;
                }
                Entry &e = _entries[slot];
                e = Entry();
                e.key = key;
                snprintf(e.family, sizeof e.family, "%s", key.family);
                e.key.family = e.family;
                e.used = _clock;
                char name[96];
                nameOf(key, key.family, name, sizeof name);
                e.handle = _platform.openFont(name);
                if (e.handle < 0) {
                        _failures++;
                        nameOf(key, "IBM Plex Sans", name, sizeof name);
                        e.handle = _platform.openFont(name);
                }
                if (e.handle >= 0) {
                        e.metrics = _platform.fontMetrics(e.handle);
                } else {
                        e.metrics.ascent = int32_t(key.size) * 4 / 5;
                        e.metrics.descent = int32_t(key.size) - e.metrics.ascent;
                        e.metrics.lineHeight = int32_t(key.size) * 5 / 4;
                }
                return e;
        }

        FontMetrics FontCache::metrics(const FontKey &key) {
                return get(key).metrics;
        }

        int32_t FontCache::width(const FontKey &key, StringView text) {
                Entry &e = get(key);
                if (e.handle < 0) return int32_t(characters(text) * key.size / 2);
                return _platform.textWidth(e.handle, text);
        }

        uint32_t FontCache::fit(const FontKey &key, StringView text, int32_t maxWidth) {
                Entry &e = get(key);
                if (e.handle >= 0) return _platform.textFit(e.handle, text, maxWidth);
                uint32_t room = maxWidth > 0 ? uint32_t(maxWidth) / (key.size / 2 + 1) : 0;
                uint32_t i = 0;
                for (uint32_t chars = 0; i < text.length(); i++) {
                        if ((uint8_t(text[i]) & 0xC0) != 0x80 && chars++ == room) break;
                }
                return i;
        }

        void FontCache::draw(Canvas &canvas, const FontKey &key, int32_t x, int32_t y, StringView text, Color color) {
                Entry &e = get(key);
                if (e.handle >= 0) _platform.drawText(canvas, e.handle, x, y, text, color);
                return;
        }

        void FontCache::clear() {
                for (uint32_t i = 0; i < _count; i++) {
                        if (_entries[i].handle >= 0) _platform.closeFont(_entries[i].handle);
                }
                _count = 0;
                return;
        }

} // namespace preso
