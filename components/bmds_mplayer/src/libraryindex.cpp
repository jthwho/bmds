/**
 * @file      libraryindex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "libraryindex.h"

#include <initializer_list>
#include <utility>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Header = "#bmds-library 1";
        constexpr size_t     Fields = 10;
        constexpr size_t     CompactAfter = 16384;

        // @p field without its escapes, into @p out.
        Error unescape(StringView field, String &out) {
                out.clear();
                for (size_t i = 0; i < field.size(); ++i) {
                        char c = field[i];
                        if (c == '\\' && i + 1 < field.size()) {
                                const char e = field[++i];
                                c = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
                        }
                        const Error err = out.append(c);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        Error escape(StringView text, String &out) {
                Error err = Error::Ok;
                for (size_t i = 0; err.isOk() && i < text.size(); ++i) {
                        const char c = text[i];
                        if (c == '\t') {
                                err = out.append("\\t");
                        } else if (c == '\n') {
                                err = out.append("\\n");
                        } else if (c == '\r') {
                                err = out.append("\\r");
                        } else if (c == '\\') {
                                err = out.append("\\\\");
                        } else {
                                err = out.append(c);
                        }
                }
                return err;
        }

        // A decimal number (a '-' first if @p signedOk); false if it isn't one.
        bool parseNumber(StringView s, bool signedOk, int64_t &out) {
                bool negative = false;
                if (signedOk && s.startsWith('-')) {
                        negative = true;
                        s = s.mid(1);
                }
                if (s.isEmpty() || s.size() > 18) return false;
                int64_t v = 0;
                for (const char c : s) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + (c - '0');
                }
                out = negative ? -v : v;
                return true;
        }

} // namespace

LibraryIndex::LibraryIndex(MemSpace space) : _records(space), _texts(space) {}

Error LibraryIndex::store(StringView s, Text &out) {
        if (s.size() > UINT16_MAX) return Error::Invalid;
        const size_t at = _texts.size();
        for (const char c : s) {
                if (_texts.append(c).isError()) {
                        _texts.truncate(at);
                        return Error::NoMem;
                }
        }
        out.at = static_cast<uint32_t>(at);
        out.length = static_cast<uint16_t>(s.size());
        return Error::Ok;
}

void LibraryIndex::release(const Stored &s) {
        _unused += s.path.length + s.format.length + s.title.length + s.artist.length + s.album.length;
        return;
}

void LibraryIndex::compact() {
        List<char> texts(_texts.space());
        if (texts.reserve(_texts.size() - _unused).isError()) return;
        for (Stored &s : _records) {
                for (Text *t : {&s.path, &s.format, &s.title, &s.artist, &s.album}) {
                        const uint32_t at = static_cast<uint32_t>(texts.size());
                        for (size_t i = 0; i < t->length; ++i) (void)texts.append(_texts[t->at + i]); // reserved
                        t->at = at;
                }
        }
        _texts = std::move(texts);
        _unused = 0;
        return;
}

size_t LibraryIndex::lowerBound(StringView path) const {
        size_t low = 0;
        size_t high = _records.size();
        while (low < high) {
                const size_t mid = (low + high) / 2;
                if (view(_records[mid].path).toStdStringView() < path.toStdStringView()) {
                        low = mid + 1;
                } else {
                        high = mid;
                }
        }
        return low;
}

int LibraryIndex::find(StringView path) const {
        const size_t i = lowerBound(path);
        return i < _records.size() && view(_records[i].path) == path ? static_cast<int>(i) : -1;
}

LibraryIndex::Record LibraryIndex::record(size_t index) const {
        const Stored &s = _records[index];
        Record        r;
        r.path = view(s.path);
        r.size = s.size;
        r.modified = s.modified;
        r.lengthMs = s.lengthMs;
        r.format = view(s.format);
        r.title = view(s.title);
        r.artist = view(s.artist);
        r.album = view(s.album);
        r.track = s.track;
        r.year = s.year;
        return r;
}

Error LibraryIndex::set(const Record &record) {
        if (record.path.isEmpty()) return Error::Invalid;
        const size_t i = lowerBound(record.path);
        const bool   replacing = i < _records.size() && view(_records[i].path) == record.path;
        if (!replacing && _records.size() >= MaxRecords) return Error::Full;
        Stored       s;
        const size_t mark = _texts.size();
        Error        err = replacing ? Error::Ok : _records.reserve(_records.size() + 1);
        if (err.isOk()) err = store(record.path, s.path);
        if (err.isOk()) err = store(record.format, s.format);
        if (err.isOk()) err = store(record.title, s.title);
        if (err.isOk()) err = store(record.artist, s.artist);
        if (err.isOk()) err = store(record.album, s.album);
        if (err.isError()) {
                _texts.truncate(mark);
                return err;
        }
        s.size = record.size;
        s.modified = record.modified;
        s.lengthMs = record.lengthMs;
        s.track = record.track;
        s.year = record.year;
        if (replacing) {
                release(_records[i]);
                _records[i] = s;
        } else {
                (void)_records.insert(i, std::move(s)); // reserved
        }
        if (_unused > CompactAfter && _unused > _texts.size() / 2) compact();
        return Error::Ok;
}

void LibraryIndex::remove(StringView path) {
        const int i = find(path);
        if (i < 0) return;
        release(_records[static_cast<size_t>(i)]);
        _records.removeAt(static_cast<size_t>(i));
        if (_records.isEmpty()) {
                _texts.clear();
                _unused = 0;
        } else if (_unused > CompactAfter && _unused > _texts.size() / 2) {
                compact();
        }
        return;
}

void LibraryIndex::clear() {
        _records.clear();
        _texts.clear();
        _unused = 0;
        return;
}

Result<size_t> LibraryIndex::read(StringView text) {
        size_t     bad = 0;
        size_t     at = 0;
        StringView fields[Fields];
        String     texts[5] = {String(MemSpace::Psram), String(MemSpace::Psram), String(MemSpace::Psram),
                               String(MemSpace::Psram), String(MemSpace::Psram)};
        while (at < text.size()) {
                const size_t start = at;
                while (at < text.size() && text[at] != '\n') ++at;
                StringView line = text.mid(start, at - start);
                if (at < text.size()) ++at;
                if (line.endsWith('\r')) line = line.left(line.size() - 1);
                if (line.isEmpty() || line.startsWith('#')) continue;
                size_t n = 0;
                size_t from = 0;
                for (size_t i = 0; i <= line.size() && n < Fields; ++i) {
                        if (i == line.size() || line[i] == '\t') {
                                fields[n++] = line.mid(from, i - from);
                                from = i + 1;
                        }
                }
                int64_t size = 0;
                int64_t modified = 0;
                int64_t length = 0;
                int64_t track = 0;
                int64_t year = 0;
                if (n < Fields || !parseNumber(fields[1], false, size) || !parseNumber(fields[2], true, modified) ||
                    !parseNumber(fields[3], false, length) || length > UINT32_MAX ||
                    !parseNumber(fields[8], false, track) || track > UINT16_MAX ||
                    !parseNumber(fields[9], false, year) || year > UINT16_MAX) {
                        ++bad;
                        continue;
                }
                // path, format, title, artist, album
                const size_t which[5] = {0, 4, 5, 6, 7};
                for (size_t t = 0; t < 5; ++t) {
                        const Error err = unescape(fields[which[t]], texts[t]);
                        if (err.isError()) return makeError<size_t>(err);
                }
                Record r;
                r.path = texts[0].view();
                r.size = static_cast<uint64_t>(size);
                r.modified = modified;
                r.lengthMs = static_cast<uint32_t>(length);
                r.format = texts[1].view();
                r.title = texts[2].view();
                r.artist = texts[3].view();
                r.album = texts[4].view();
                r.track = static_cast<uint16_t>(track);
                r.year = static_cast<uint16_t>(year);
                const Error err = set(r);
                if (err == Error::Invalid) {
                        ++bad;
                } else if (err.isError()) {
                        return makeError<size_t>(err);
                }
        }
        return makeResult(bad);
}

Error LibraryIndex::write(String &out) const {
        out.clear();
        Error err = out.append(Header);
        if (err.isOk()) err = out.append('\n');
        for (size_t i = 0; err.isOk() && i < _records.size(); ++i) {
                const Record r = record(i);
                err = escape(r.path, out);
                if (err.isOk())
                        err = out.appendf("\t%llu\t%lld\t%lu\t", static_cast<unsigned long long>(r.size),
                                          static_cast<long long>(r.modified), static_cast<unsigned long>(r.lengthMs));
                if (err.isOk()) err = escape(r.format, out);
                if (err.isOk()) err = out.append('\t');
                if (err.isOk()) err = escape(r.title, out);
                if (err.isOk()) err = out.append('\t');
                if (err.isOk()) err = escape(r.artist, out);
                if (err.isOk()) err = out.append('\t');
                if (err.isOk()) err = escape(r.album, out);
                if (err.isOk())
                        err = out.appendf("\t%u\t%u\n", static_cast<unsigned>(r.track), static_cast<unsigned>(r.year));
        }
        if (err.isError()) out.clear();
        return err;
}

BMDS_NAMESPACE_END
