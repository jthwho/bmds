/**
 * @file      fontface.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/fontface.h>
#include <bmds/memory.h>
#include <bmds/utf8.h>
#include <stb_truetype.h>

// stb_truetype's memory (stbtruetype.c): Internal for speed, Psram if that is short. A failure is
// noted in the render's context, since stb_truetype gives up without saying why.
struct BmdsFontAllocContext {
                bool failed;
};

extern "C" void *bmds_font_alloc(size_t bytes, void *context) {
        void *p = bmds::Memory::alloc(bmds::MemSpace::Internal, bytes);
        if (p == nullptr) p = bmds::Memory::alloc(bmds::MemSpace::Psram, bytes);
        if (p == nullptr && context != nullptr) static_cast<BmdsFontAllocContext *>(context)->failed = true;
        return p;
}

extern "C" void bmds_font_free(void *p, void *context) {
        (void)context;
        bmds::Memory::free(p);
        return;
}

BMDS_NAMESPACE_BEGIN

namespace {

        static_assert(sizeof(stbtt_fontinfo) <= 192, "FontFace::StbBytes is too small");
        static_assert(alignof(stbtt_fontinfo) <= alignof(void *), "FontFace::_stb is not aligned enough");

        constexpr uint32_t tag(const char (&t)[5]) {
                return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 |
                       uint8_t(t[3]);
        }

        // The deepest nesting of compound glyphs (which also stops a glyph containing itself), and
        // the most points a glyph may have with its parts: what stb_truetype allocates for.
        constexpr unsigned MaxCompoundDepth = 8;
        constexpr uint32_t MaxGlyphPoints = 16384;

        // The largest glyph box drawn, in pixels on a side: a damaged box is refused, not drawn.
        constexpr uint32_t MaxBoxSide = 4 * FontFace::MaxPixelSize;

        // Big-endian reads inside a table: [base, base + length). Reads past its end give false,
        // so a damaged offset ends a lookup instead of reading outside the file.
        struct Table {
                        const uint8_t *p = nullptr;
                        uint32_t       length = 0;

                        bool has(uint32_t at, uint32_t bytes) const { return at <= length && bytes <= length - at; }

                        bool u16(uint32_t at, uint16_t &out) const {
                                if (!has(at, 2)) return false;
                                out = static_cast<uint16_t>(p[at] << 8 | p[at + 1]);
                                return true;
                        }

                        bool u32(uint32_t at, uint32_t &out) const {
                                if (!has(at, 4)) return false;
                                out = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 |
                                      p[at + 3];
                                return true;
                        }

                        Table sub(uint32_t at) const {
                                if (at > length) return Table{};
                                return Table{p + at, length - at};
                        }
        };

        uint16_t be16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] << 8 | p[1]);
        }

        uint32_t be32(const uint8_t *p) {
                return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }

        // A GPOS coverage table: @p glyph's index in it, or -1.
        int coverageIndex(const Table &t, uint16_t glyph) {
                uint16_t format = 0, count = 0;
                if (!t.u16(0, format) || !t.u16(2, count)) return -1;
                if (format == 1) {
                        if (!t.has(4, uint32_t(count) * 2)) return -1;
                        int lo = 0, hi = int(count) - 1;
                        while (lo <= hi) {
                                const int      mid = (lo + hi) / 2;
                                const uint16_t g = be16(t.p + 4 + mid * 2);
                                if (glyph < g)
                                        hi = mid - 1;
                                else if (glyph > g)
                                        lo = mid + 1;
                                else
                                        return mid;
                        }
                } else if (format == 2) {
                        if (!t.has(4, uint32_t(count) * 6)) return -1;
                        int lo = 0, hi = int(count) - 1;
                        while (lo <= hi) {
                                const int      mid = (lo + hi) / 2;
                                const uint8_t *r = t.p + 4 + mid * 6;
                                if (glyph < be16(r))
                                        hi = mid - 1;
                                else if (glyph > be16(r + 2))
                                        lo = mid + 1;
                                else
                                        return be16(r + 4) + (glyph - be16(r));
                        }
                }
                return -1;
        }

        // A GPOS class definition: @p glyph's class (0 if it has none).
        uint16_t glyphClass(const Table &t, uint16_t glyph) {
                uint16_t format = 0;
                if (!t.u16(0, format)) return 0;
                if (format == 1) {
                        uint16_t start = 0, count = 0, value = 0;
                        if (!t.u16(2, start) || !t.u16(4, count) || glyph < start || glyph - start >= count) return 0;
                        return t.u16(6 + uint32_t(glyph - start) * 2, value) ? value : 0;
                }
                if (format == 2) {
                        uint16_t count = 0;
                        if (!t.u16(2, count) || !t.has(4, uint32_t(count) * 6)) return 0;
                        int lo = 0, hi = int(count) - 1;
                        while (lo <= hi) {
                                const int      mid = (lo + hi) / 2;
                                const uint8_t *r = t.p + 4 + mid * 6;
                                if (glyph < be16(r))
                                        hi = mid - 1;
                                else if (glyph > be16(r + 2))
                                        lo = mid + 1;
                                else
                                        return be16(r + 4);
                        }
                }
                return 0;
        }

        // A value record's size, and where its XAdvance is (-1: none).
        uint32_t valueBytes(uint16_t format) {
                return 2u * static_cast<uint32_t>(__builtin_popcount(format & 0xff));
        }

        int xAdvanceAt(uint16_t format) {
                if ((format & 4) == 0) return -1;
                return 2 * __builtin_popcount(format & 3);
        }

        // One PairPos subtable: true if it applies to the pair (and @p out gets its XAdvance).
        bool pairAdjustment(const Table &t, uint16_t left, uint16_t right, int &out) {
                uint16_t format = 0, coverage = 0, vf1 = 0, vf2 = 0;
                if (!t.u16(0, format) || !t.u16(2, coverage) || !t.u16(4, vf1) || !t.u16(6, vf2)) return false;
                const int index = coverageIndex(t.sub(coverage), left);
                if (index < 0) return false;
                const uint32_t size1 = valueBytes(vf1), size2 = valueBytes(vf2);
                const int      xAdvance = xAdvanceAt(vf1);
                if (format == 1) {
                        uint16_t setCount = 0, setOffset = 0, pairCount = 0;
                        if (!t.u16(8, setCount) || index >= setCount || !t.u16(10 + uint32_t(index) * 2, setOffset))
                                return false;
                        const Table    set = t.sub(setOffset);
                        const uint32_t recordBytes = 2 + size1 + size2;
                        if (!set.u16(0, pairCount) || !set.has(2, pairCount * recordBytes)) return false;
                        int lo = 0, hi = int(pairCount) - 1;
                        while (lo <= hi) {
                                const int      mid = (lo + hi) / 2;
                                const uint8_t *r = set.p + 2 + uint32_t(mid) * recordBytes;
                                const uint16_t g = be16(r);
                                if (right < g)
                                        hi = mid - 1;
                                else if (right > g)
                                        lo = mid + 1;
                                else {
                                        out = xAdvance < 0 ? 0 : int16_t(be16(r + 2 + xAdvance));
                                        return true;
                                }
                        }
                        return false; // not this subtable's pair: the next one may have it
                }
                if (format == 2) {
                        uint16_t class1Def = 0, class2Def = 0, class1Count = 0, class2Count = 0;
                        if (!t.u16(8, class1Def) || !t.u16(10, class2Def) || !t.u16(12, class1Count) ||
                            !t.u16(14, class2Count))
                                return false;
                        const uint16_t c1 = glyphClass(t.sub(class1Def), left);
                        const uint16_t c2 = glyphClass(t.sub(class2Def), right);
                        if (c1 >= class1Count || c2 >= class2Count) return false;
                        const uint64_t at =
                                16 + (uint64_t(c1) * class2Count + c2) * (size1 + size2) + uint64_t(xAdvance);
                        uint16_t value = 0;
                        out = xAdvance < 0 || at > UINT32_MAX || !t.u16(uint32_t(at), value) ? 0 : int16_t(value);
                        return true; // covered: this subtable decides, even with 0
                }
                return false;
        }

        // Appends @p codepoint as UTF-8 if it fits in @p max bytes.
        void append(char *out, uint8_t &length, size_t max, uint32_t codepoint) {
                char         bytes[4];
                const size_t n = Utf8::encode(codepoint, bytes);
                if (length + n > max) return;
                std::memcpy(out + length, bytes, n);
                length = static_cast<uint8_t>(length + n);
                return;
        }

} // namespace

FontFace::FontFace() = default;

Error FontFace::open(ByteSpan data) {
        if (isOpen()) return Error::InvalidState;
        const Table file{data.data(), data.size() > UINT32_MAX ? UINT32_MAX : uint32_t(data.size())};
        uint32_t    version = 0;
        uint16_t    tableCount = 0;
        if (!file.u32(0, version) || !file.u16(4, tableCount)) return Error::CorruptData;
        if (version == tag("OTTO") || version == tag("ttcf")) return Error::NotSupported;
        if (version != 0x00010000 && version != tag("true")) return Error::CorruptData;
        if (!file.has(12, uint32_t(tableCount) * 16)) return Error::CorruptData;

        // Every table inside the file; the first of each tag, as stb_truetype finds them.
        struct Found {
                        uint32_t tag;
                        uint32_t at = 0, length = 0;
        };
        Found tables[] = {{tag("cmap")}, {tag("head")}, {tag("hhea")}, {tag("hmtx")}, {tag("loca")}, {tag("glyf")},
                          {tag("maxp")}, {tag("name")}, {tag("post")}, {tag("GPOS")}, {tag("kern")}};
        enum {
                Cmap,
                Head,
                Hhea,
                Hmtx,
                Loca,
                Glyf,
                Maxp,
                Name,
                Post,
                Gpos,
                Kern
        };
        for (uint32_t i = 0; i < tableCount; ++i) {
                const uint8_t *r = file.p + 12 + i * 16;
                const uint32_t t = be32(r), at = be32(r + 8), length = be32(r + 12);
                if (!file.has(at, length)) return Error::CorruptData;
                for (Found &f : tables)
                        if (f.tag == t && f.at == 0) {
                                f.at = at;
                                f.length = length;
                        }
        }
        for (int required : {Cmap, Head, Hhea, Hmtx, Maxp})
                if (tables[required].at == 0) return Error::CorruptData;
        if (tables[Glyf].at == 0 || tables[Loca].at == 0) return Error::NotSupported; // CFF or bitmap-only

        const Table head{file.p + tables[Head].at, tables[Head].length};
        const Table hhea{file.p + tables[Hhea].at, tables[Hhea].length};
        const Table maxp{file.p + tables[Maxp].at, tables[Maxp].length};
        uint16_t    unitsPerEm = 0, locaFormat = 0, glyphCount = 0, hMetricCount = 0, a = 0, d = 0, g = 0, m = 0;
        if (!head.u16(18, unitsPerEm) || !head.u16(50, locaFormat) || !maxp.u16(4, glyphCount) || !hhea.u16(4, a) ||
            !hhea.u16(6, d) || !hhea.u16(8, g) || !hhea.u16(10, m) || !hhea.u16(34, hMetricCount))
                return Error::CorruptData;
        if (unitsPerEm < 16 || unitsPerEm > 16384 || locaFormat > 1 || glyphCount == 0 || hMetricCount == 0 ||
            hMetricCount > glyphCount)
                return Error::CorruptData;
        if (tables[Hmtx].length < uint32_t(hMetricCount) * 4 + uint32_t(glyphCount - hMetricCount) * 2)
                return Error::CorruptData;

        // loca: every glyph's bytes inside glyf, in order.
        const bool     longLoca = locaFormat == 1;
        const uint32_t locaBytes = (uint32_t(glyphCount) + 1) * (longLoca ? 4 : 2);
        if (tables[Loca].length < locaBytes) return Error::CorruptData;
        uint32_t previous = 0;
        for (uint32_t i = 0; i <= glyphCount; ++i) {
                const uint8_t *e = file.p + tables[Loca].at + i * (longLoca ? 4 : 2);
                const uint32_t offset = longLoca ? be32(e) : uint32_t(be16(e)) * 2;
                if (offset < previous || offset > tables[Glyf].length) return Error::CorruptData;
                previous = offset;
        }

        // cmap: a Unicode subtable, format 12 over format 4; stb_truetype needs one to open.
        const Table cmap{file.p + tables[Cmap].at, tables[Cmap].length};
        uint16_t    encodings = 0;
        if (!cmap.u16(2, encodings) || !cmap.has(4, uint32_t(encodings) * 8)) return Error::CorruptData;
        uint32_t best = 0;
        uint16_t bestFormat = 0;
        for (uint32_t i = 0; i < encodings; ++i) {
                const uint8_t *r = cmap.p + 4 + i * 8;
                const uint16_t platform = be16(r), encoding = be16(r + 2);
                const uint32_t at = be32(r + 4);
                const bool     unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
                uint16_t       format = 0;
                if (!unicode || !cmap.u16(at, format)) continue;
                if (format == 4) {
                        uint16_t segX2 = 0;
                        if (!cmap.u16(at + 6, segX2) || segX2 == 0 || (segX2 & 1) != 0 ||
                            !cmap.has(at + 14, uint32_t(segX2) * 4 + 2))
                                continue;
                } else if (format == 12) {
                        uint32_t groups = 0;
                        if (!cmap.u32(at + 12, groups) || groups > (cmap.length - at) / 12 ||
                            !cmap.has(at + 16, groups * 12))
                                continue;
                } else {
                        continue;
                }
                if (format > bestFormat) {
                        best = at;
                        bestFormat = format;
                }
        }
        if (bestFormat == 0) return Error::NotSupported; // no Unicode map we read

        if (stbtt_InitFont(reinterpret_cast<stbtt_fontinfo *>(_stb), file.p, 0) == 0) return Error::CorruptData;

        _data = data.mid(0, file.length);
        _glyf = tables[Glyf].at;
        _glyfLength = tables[Glyf].length;
        _loca = tables[Loca].at;
        _hmtx = tables[Hmtx].at;
        _cmap = tables[Cmap].at + best;
        _cmapEnd = tables[Cmap].at + tables[Cmap].length;
        _cmapFormat = bestFormat;
        _name = tables[Name].at;
        _nameLength = tables[Name].length;
        _gpos = tables[Gpos].at;
        _gposLength = tables[Gpos].length;
        _kern = tables[Kern].at;
        _kernLength = tables[Kern].length;
        _unitsPerEm = unitsPerEm;
        _hMetricCount = hMetricCount;
        _longLoca = longLoca;
        _metrics.ascent = int16_t(a);
        _metrics.descent = int16_t(d);
        _metrics.lineGap = int16_t(g);
        _metrics.maxAdvance = m;
        uint32_t fixedPitch = 0;
        _fixedPitch = Table{file.p + tables[Post].at, tables[Post].length}.u32(12, fixedPitch) && fixedPitch != 0;

        // GPOS: the lookups the `kern` features name, each once, in order (types 2 and 9).
        const Table gpos{file.p + _gpos, _gposLength};
        uint16_t    major = 0, featureList = 0, lookupList = 0, featureCount = 0, lookupCount = 0;
        if (_gpos != 0 && gpos.u16(0, major) && major == 1 && gpos.u16(6, featureList) && gpos.u16(8, lookupList) &&
            gpos.u16(featureList, featureCount) && gpos.u16(lookupList, lookupCount)) {
                bool wanted[MaxKernLookups * 8] = {}; // lookups below 128
                for (uint32_t i = 0; i < featureCount; ++i) {
                        uint32_t       t = 0;
                        uint16_t       offset = 0, count = 0;
                        const uint32_t r = featureList + 2u + i * 6u;
                        if (!gpos.u32(r, t) || !gpos.u16(r + 4, offset) || t != tag("kern")) continue;
                        const uint32_t feature = featureList + uint32_t(offset);
                        if (!gpos.u16(feature + 2, count)) continue;
                        for (uint32_t k = 0; k < count; ++k) {
                                uint16_t index = 0;
                                if (gpos.u16(feature + 4 + k * 2, index) && index < lookupCount &&
                                    index < sizeof(wanted))
                                        wanted[index] = true;
                        }
                }
                for (uint16_t i = 0; i < sizeof(wanted) && _kernLookupCount < MaxKernLookups; ++i)
                        if (wanted[i]) _kernLookups[_kernLookupCount++] = i;
        }
        if (_kernLookupCount == 0) _gpos = 0; // no kerning there: the kern table, if any

        _glyphCount = glyphCount;
        for (uint32_t c = 0; c < 256; ++c) _latin[c] = cmapLookup(c);
        if (!readName(16, _family, _familyLength)) (void)readName(1, _family, _familyLength);
        if (!readName(17, _style, _styleLength)) (void)readName(2, _style, _styleLength);
        return Error::Ok;
}

uint16_t FontFace::glyphIndex(uint32_t codepoint) const {
        if (codepoint < 256) return _latin[codepoint];
        return cmapLookup(codepoint);
}

uint16_t FontFace::cmapLookup(uint32_t codepoint) const {
        const Table t{_data.data() + _cmap, _cmapEnd - _cmap};
        uint32_t    glyph = 0;
        if (_cmapFormat == 4) {
                if (codepoint > 0xffff) return 0;
                uint16_t segX2 = 0;
                (void)t.u16(6, segX2);
                const uint32_t segs = segX2 / 2u;
                // The first segment whose end is at or past the codepoint (ends are sorted).
                uint32_t lo = 0, hi = segs;
                while (lo < hi) {
                        const uint32_t mid = (lo + hi) / 2;
                        if (be16(t.p + 14 + mid * 2) < codepoint)
                                lo = mid + 1;
                        else
                                hi = mid;
                }
                if (lo == segs) return 0;
                const uint32_t startAt = 16 + segX2 + lo * 2, deltaAt = startAt + segX2, rangeAt = deltaAt + segX2;
                const uint16_t start = be16(t.p + startAt), delta = be16(t.p + deltaAt), range = be16(t.p + rangeAt);
                if (codepoint < start) return 0;
                if (range == 0) {
                        glyph = (codepoint + delta) & 0xffff;
                } else {
                        uint16_t g = 0;
                        if (!t.u16(rangeAt + range + (codepoint - start) * 2, g) || g == 0) return 0;
                        glyph = (uint32_t(g) + delta) & 0xffff;
                }
        } else {
                uint32_t groups = 0;
                (void)t.u32(12, groups);
                uint32_t lo = 0, hi = groups;
                while (lo < hi) {
                        const uint32_t mid = (lo + hi) / 2;
                        const uint8_t *g = t.p + 16 + mid * 12;
                        if (codepoint < be32(g))
                                hi = mid;
                        else if (codepoint > be32(g + 4))
                                lo = mid + 1;
                        else {
                                glyph = be32(g + 8) + (codepoint - be32(g));
                                break;
                        }
                }
        }
        return glyph < _glyphCount ? static_cast<uint16_t>(glyph) : 0;
}

int FontFace::advance(uint16_t glyph) const {
        if (glyph >= _glyphCount) return 0;
        const uint16_t metric = glyph < _hMetricCount ? glyph : static_cast<uint16_t>(_hMetricCount - 1);
        return be16(_data.data() + _hmtx + uint32_t(metric) * 4);
}

int FontFace::kerning(uint16_t left, uint16_t right) const {
        if (left >= _glyphCount || right >= _glyphCount) return 0;
        if (_gpos != 0) return gposKerning(left, right);
        if (_kern != 0) return kernTableKerning(left, right);
        return 0;
}

int FontFace::gposKerning(uint16_t left, uint16_t right) const {
        const Table gpos{_data.data() + _gpos, _gposLength};
        uint16_t    lookupList = 0;
        (void)gpos.u16(8, lookupList);
        const Table lookups = gpos.sub(lookupList);
        int         total = 0;
        // Every lookup adds its part; within one, the first subtable that applies decides.
        for (uint8_t i = 0; i < _kernLookupCount; ++i) {
                uint16_t offset = 0, type = 0, count = 0;
                if (!lookups.u16(2 + uint32_t(_kernLookups[i]) * 2, offset)) continue;
                const Table lookup = lookups.sub(offset);
                if (!lookup.u16(0, type) || !lookup.u16(4, count) || (type != 2 && type != 9)) continue;
                for (uint32_t s = 0; s < count; ++s) {
                        uint16_t at = 0;
                        if (!lookup.u16(6 + s * 2, at)) break;
                        Table subtable = lookup.sub(at);
                        if (type == 9) { // an extension: the real subtable further on
                                uint16_t real = 0;
                                uint32_t far = 0;
                                if (!subtable.u16(2, real) || real != 2 || !subtable.u32(4, far)) continue;
                                subtable = subtable.sub(far);
                        }
                        int value = 0;
                        if (pairAdjustment(subtable, left, right, value)) {
                                total += value;
                                break;
                        }
                }
        }
        return total;
}

int FontFace::kernTableKerning(uint16_t left, uint16_t right) const {
        const Table kern{_data.data() + _kern, _kernLength};
        uint16_t    version = 0, count = 0;
        if (!kern.u16(0, version) || version != 0 || !kern.u16(2, count)) return 0;
        const uint32_t key = uint32_t(left) << 16 | right;
        uint32_t       at = 4;
        int            total = 0;
        for (uint32_t i = 0; i < count; ++i) {
                uint16_t length = 0, coverage = 0, pairs = 0;
                if (!kern.u16(at + 2, length) || !kern.u16(at + 4, coverage) || length < 6) break;
                // Format 0, horizontal, kerning values (not minimums), not cross-stream.
                if ((coverage & 0xff07) == 0x0001 && kern.u16(at + 6, pairs) && kern.has(at + 14, pairs * 6u)) {
                        uint32_t lo = 0, hi = pairs;
                        while (lo < hi) {
                                const uint32_t mid = (lo + hi) / 2;
                                const uint32_t k = be32(kern.p + at + 14 + mid * 6);
                                if (k < key)
                                        lo = mid + 1;
                                else
                                        hi = mid;
                        }
                        if (lo < pairs && be32(kern.p + at + 14 + lo * 6) == key)
                                total += int16_t(be16(kern.p + at + 14 + lo * 6 + 4));
                }
                at += length;
        }
        return total;
}

bool FontFace::readName(uint16_t id, char *out, uint8_t &length) const {
        if (_name == 0) return false;
        const Table name{_data.data() + _name, _nameLength};
        uint16_t    count = 0, strings = 0;
        if (!name.u16(2, count) || !name.u16(4, strings) || !name.has(6, uint32_t(count) * 12)) return false;
        // Windows Unicode, US English first; then any Windows Unicode; then Macintosh Roman.
        int found = -1, rank = 0;
        for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *r = name.p + 6 + i * 12;
                if (be16(r + 6) != id) continue;
                const uint16_t platform = be16(r), encoding = be16(r + 2), language = be16(r + 4);
                int            score = 0;
                if (platform == 3 && (encoding == 1 || encoding == 10))
                        score = language == 0x409 ? 3 : 2;
                else if (platform == 1 && encoding == 0)
                        score = 1;
                if (score > rank) {
                        rank = score;
                        found = int(i);
                }
        }
        if (found < 0) return false;
        const uint8_t *r = name.p + 6 + uint32_t(found) * 12;
        const uint32_t bytes = be16(r + 8), at = uint32_t(strings) + be16(r + 10);
        if (!name.has(at, bytes)) return false;
        const uint8_t *s = name.p + at;
        length = 0;
        if (rank == 1) {
                for (uint32_t i = 0; i < bytes; ++i) append(out, length, MaxName, s[i] < 0x80 ? s[i] : '?');
        } else {
                for (uint32_t i = 0; i + 1 < bytes; i += 2) {
                        uint32_t c = be16(s + i);
                        if (c >= 0xd800 && c < 0xdc00 && i + 3 < bytes && be16(s + i + 2) >= 0xdc00 &&
                            be16(s + i + 2) < 0xe000) {
                                c = 0x10000 + ((c - 0xd800) << 10) + (be16(s + i + 2) - 0xdc00);
                                i += 2;
                        }
                        append(out, length, MaxName, c);
                }
        }
        out[length] = '\0';
        return length != 0;
}

bool FontFace::glyphRange(uint16_t glyph, uint32_t &start, uint32_t &end) const {
        if (glyph >= _glyphCount) return false;
        const uint8_t *loca = _data.data() + _loca;
        if (_longLoca) {
                start = be32(loca + uint32_t(glyph) * 4);
                end = be32(loca + uint32_t(glyph) * 4 + 4);
        } else {
                start = uint32_t(be16(loca + uint32_t(glyph) * 2)) * 2;
                end = uint32_t(be16(loca + uint32_t(glyph) * 2 + 2)) * 2;
        }
        return true;
}

// Walks @p glyph's data as stb_truetype will read it: every byte it reads inside the glyph's own,
// its contours' ends in order, its parts real glyphs nested not too deep, its points few enough.
Error FontFace::checkGlyph(uint16_t glyph, unsigned depth, uint32_t &points) const {
        uint32_t start = 0, end = 0;
        if (!glyphRange(glyph, start, end)) return Error::OutOfRange;
        if (start == end) return Error::Ok; // no outline
        const Table g{_data.data() + _glyf + start, end - start};
        uint16_t    contours16 = 0;
        if (!g.has(0, 10) || !g.u16(0, contours16)) return Error::CorruptData;
        const int16_t contours = int16_t(contours16);
        if (contours > 0) {
                uint32_t n = 0;
                for (int16_t c = 0; c < contours; ++c) {
                        uint16_t e = 0;
                        if (!g.u16(10 + uint32_t(c) * 2, e) || (c > 0 && e < n)) return Error::CorruptData;
                        // A last contour of one point would have stb_truetype read past its points.
                        if (c == contours - 1 && c > 0 && e == n) return Error::CorruptData;
                        n = uint32_t(e) + 1;
                }
                if (contours == 1 && n == 1) return Error::CorruptData;
                uint16_t       instructions = 0;
                const uint32_t insAt = 10 + uint32_t(contours) * 2;
                if (!g.u16(insAt, instructions)) return Error::CorruptData;
                uint32_t at = insAt + 2 + instructions, xBytes = 0, yBytes = 0;
                for (uint32_t i = 0; i < n;) {
                        if (!g.has(at, 1)) return Error::CorruptData;
                        const uint8_t flags = g.p[at++];
                        uint32_t      repeat = 1;
                        if ((flags & 8) != 0) {
                                if (!g.has(at, 1)) return Error::CorruptData;
                                repeat += g.p[at++];
                        }
                        if (repeat > n - i) repeat = n - i;
                        xBytes += repeat * ((flags & 2) != 0 ? 1 : (flags & 16) != 0 ? 0 : 2);
                        yBytes += repeat * ((flags & 4) != 0 ? 1 : (flags & 32) != 0 ? 0 : 2);
                        i += repeat;
                }
                if (!g.has(at, xBytes + yBytes)) return Error::CorruptData;
                points += n + 2u * uint32_t(contours);
                return points > MaxGlyphPoints ? Error::CorruptData : Error::Ok;
        }
        if (contours == 0) return Error::Ok;
        // A compound glyph: its parts, each placed by an offset (not by matching points).
        if (depth >= MaxCompoundDepth) return Error::CorruptData;
        uint32_t at = 10;
        for (;;) {
                uint16_t flags = 0, part = 0;
                if (!g.u16(at, flags) || !g.u16(at + 2, part)) return Error::CorruptData;
                if ((flags & 2) == 0) return Error::CorruptData;
                at += 4 + ((flags & 1) != 0 ? 4 : 2);
                if ((flags & 8) != 0)
                        at += 2;
                else if ((flags & 0x40) != 0)
                        at += 4;
                else if ((flags & 0x80) != 0)
                        at += 8;
                if (!g.has(0, at)) return Error::CorruptData;
                if (Error err = checkGlyph(part, depth + 1, points); err.isError())
                        return err == Error::OutOfRange ? Error::CorruptData : err;
                if ((flags & 0x20) == 0) break;
        }
        return Error::Ok;
}

Result<FontFace::GlyphBox> FontFace::box(uint16_t glyph, unsigned pixelSize) const {
        if (glyph >= _glyphCount || pixelSize < MinPixelSize || pixelSize > MaxPixelSize)
                return makeError<GlyphBox>(Error::OutOfRange);
        uint32_t points = 0;
        if (Error err = checkGlyph(glyph, 0, points); err.isError()) return makeError<GlyphBox>(err);
        const float s = scale(pixelSize);
        int         x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(reinterpret_cast<const stbtt_fontinfo *>(_stb), glyph, s, s, 0.0f, 0.0f, &x0,
                                        &y0, &x1, &y1);
        if (x1 < x0 || y1 < y0 || uint32_t(x1 - x0) > MaxBoxSide || uint32_t(y1 - y0) > MaxBoxSide ||
            x0 < -int(MaxBoxSide) || y0 < -int(MaxBoxSide) || x0 > int(MaxBoxSide) || y0 > int(MaxBoxSide))
                return makeError<GlyphBox>(Error::CorruptData);
        GlyphBox b;
        if (x1 > x0 && y1 > y0) {
                b.x = int16_t(x0);
                b.y = int16_t(y0);
                b.width = uint16_t(x1 - x0);
                b.height = uint16_t(y1 - y0);
        }
        return makeResult(b);
}

Error FontFace::render(uint16_t glyph, unsigned pixelSize, Depth depth, const GlyphBox &box, Span<uint8_t> out) const {
        if (glyph >= _glyphCount || pixelSize < MinPixelSize || pixelSize > MaxPixelSize) return Error::OutOfRange;
        if (box.width == 0 || box.height == 0) return Error::Ok;
        if (out.size() < box.workBytes()) return Error::BufferTooSmall;
        uint32_t points = 0;
        if (Error err = checkGlyph(glyph, 0, points); err.isError()) return err;

        // A copy of the face's state whose allocations report to this call alone.
        BmdsFontAllocContext context{false};
        stbtt_fontinfo       info = *reinterpret_cast<const stbtt_fontinfo *>(_stb);
        info.userdata = &context;
        const float   s = scale(pixelSize);
        stbtt_vertex *vertices = nullptr;
        const int     count = stbtt_GetGlyphShape(&info, glyph, &vertices);
        if (context.failed) {
                stbtt_FreeShape(&info, vertices);
                return Error::NoMem;
        }
        uint8_t *pixels = out.data();
        std::memset(pixels, 0, box.workBytes());
        stbtt__bitmap bitmap{box.width, box.height, box.width, pixels};
        if (count > 0) stbtt_Rasterize(&bitmap, 0.35f, vertices, count, s, s, 0.0f, 0.0f, box.x, box.y, 1, &context);
        stbtt_FreeShape(&info, vertices);
        if (context.failed) return Error::NoMem;

        // Packed in place, row by row: each row's packed bytes never pass its unread ones.
        const size_t stride = box.stride(depth);
        for (uint32_t y = 0; y < box.height; ++y) {
                const uint8_t *src = pixels + size_t(y) * box.width;
                uint8_t       *dst = pixels + size_t(y) * stride;
                if (depth == Depth::Mono) {
                        for (uint32_t x = 0; x < box.width; x += 8) {
                                uint8_t bits = 0;
                                for (uint32_t k = 0; k < 8 && x + k < box.width; ++k)
                                        if (src[x + k] >= 128) bits = static_cast<uint8_t>(bits | 0x80 >> k);
                                dst[x / 8] = bits;
                        }
                } else {
                        for (uint32_t x = 0; x < box.width; x += 2) {
                                const uint32_t hi = (uint32_t(src[x]) * 15 + 127) / 255;
                                const uint32_t lo = x + 1 < box.width ? (uint32_t(src[x + 1]) * 15 + 127) / 255 : 0;
                                dst[x / 2] = static_cast<uint8_t>(hi << 4 | lo);
                        }
                }
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
