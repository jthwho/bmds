/**
 * @file      zoneinfo.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The time zone database (devplan/time.md §9): one file, zoneinfo.bin,
 * shown as /share/zoneinfo and used for zone names by the kernel and the
 * shell.
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/stream.h>
#include <bmds/vfs.h>

#include "fslayer.h"
#include "zoneinfo.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The database's path ("" for none) and its identity; under the storage lock.
        constinit char     databasePath[Vfs::MaxZoneDatabasePath + 1] = {};
        constinit uint64_t databaseId = 0;

        // The last answer of Vfs::zoneRules() (a zone, or "not in the
        // database"); under the storage lock.
        struct Answer {
                        char     name[ZoneBundle::MaxName + 1] = {};
                        TimeZone rules;
                        bool     valid = false;
                        bool     found = false;
        };

        constinit Answer answer;

        // The database file, open for reading while this lives. Lock held.
        class Database final : public ZoneBundle::Source {
                public:
                        Database() = default;
                        Database(const Database &) = delete;
                        Database &operator=(const Database &) = delete;

                        ~Database() {
                                if (_reader != nullptr) _volume->closeReader(_reader);
                        }

                        // Opens it (the ROM's while none is at the path) and reads its header.
                        Error open(Timestamp *modified = nullptr) {
                                if (databasePath[0] == '\0') return Error::NotExist;
                                Error err = Fs::openReaderLocked(StringView(databasePath), _volume, _reader, modified);
                                if ((err == Error::NotExist || err == Error::NoMedia || err == Error::NotDir) &&
                                    StringView(databasePath) != Vfs::RomZoneDatabase) {
                                        const Error rom =
                                                Fs::openReaderLocked(Vfs::RomZoneDatabase, _volume, _reader, modified);
                                        if (rom.isOk()) {
                                                err = rom;
                                                _fromRom = true;
                                        }
                                }
                                if (err.isError()) return err;
                                Result<ZoneBundle> bundle = ZoneBundle::open(*this, _reader->size());
                                if (bundle.error().isError()) return bundle.error();
                                _bundle = bundle.value();
                                return Error();
                        }

                        const ZoneBundle &bundle() const { return _bundle; }

                        bool isFromRom() const { return _fromRom; }

                        Error readAt(uint32_t offset, WritableByteSpan buf) override {
                                auto [got, err] = _reader->readAt(offset, buf);
                                if (err.isError()) return err;
                                return got == buf.size() ? Error() : Error(Error::CorruptData);
                        }

                private:
                        Volume::Reader *_reader = nullptr;
                        Volume         *_volume = nullptr;
                        bool            _fromRom = false;
                        ZoneBundle      _bundle;
        };

        // One zone's bytes: read-only and seekable, so fstat() tells its
        // size and a program can map it. Lives in Psram, its bytes after it,
        // until its handle closes.
        class ZoneStream final : public Stream {
                public:
                        explicit ZoneStream(uint32_t size) : _size(size) {}

                        uint8_t *bytes() { return reinterpret_cast<uint8_t *>(this + 1); }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever); // a handle's tasks share the position
                                if (buf.isEmpty()) return makeResult(size_t(0));
                                if (_position >= _size) return makeError<size_t>(Error::EndOfFile);
                                const uint64_t left = _size - _position;
                                const size_t   n = buf.size() < left ? buf.size() : static_cast<size_t>(left);
                                std::memcpy(buf.data(), bytes() + _position, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan, unsigned int) override {
                                return makeError<size_t>(Error::ReadOnly);
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                int64_t       base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(_position);
                                if (from == SeekFrom::End) base = _size;
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                _position = static_cast<uint64_t>(base + offset);
                                return makeResult(_position);
                        }

                        void handleClosed(uint8_t) override {
                                this->~ZoneStream();
                                Memory::free(this);
                                return;
                        }

                private:
                        const uint32_t _size;
                        uint64_t       _position = 0;
        };

        // The longest name, and the separator after it.
        using Key = char[ZoneBundle::MaxName + 2];

        // @p name and "/" in @p key; false if too long to be in a database.
        bool directoryKey(StringView name, Key &key) {
                if (name.size() >= ZoneBundle::MaxName) return false;
                std::memcpy(key, name.data(), name.size());
                key[name.size()] = '/';
                key[name.size() + 1] = '\0';
                return true;
        }

        Error resolveZone(StringView name, TimeZone &rules) {
                const Result<TimeZone> found = Vfs::zoneRules(name);
                if (found.error().isError()) return found.error();
                rules = found.value();
                return Error();
        }

        void forgetAnswerLocked() {
                answer.valid = false;
                return;
        }

} // namespace

Error Zones::findLocked(StringView name, Found &found) {
        found = Found();
        Database    db;
        const Error opened = db.open(&found.modified);
        if (name.isEmpty()) {
                found.directory = true; // the root, with or without a database
                return Error();
        }
        if (opened.isError()) return Error::NotExist;
        Error err = db.bundle().find(db, name, found.entry);
        if (err != Error::NotExist) return err;
        // A directory: a name starts with it and a "/".
        Key key;
        if (!directoryKey(name, key)) return Error::NotExist;
        const Result<uint32_t> at = db.bundle().lowerBound(db, StringView(key));
        if (at.error().isError()) return at.error();
        if (at.value() == db.bundle().count()) return Error::NotExist;
        ZoneBundle::Entry next;
        err = db.bundle().entry(db, at.value(), next);
        if (err.isError()) return err;
        if (!next.nameView().startsWith(StringView(key))) return Error::NotExist;
        found.entry = ZoneBundle::Entry();
        found.directory = true;
        return Error();
}

Error Zones::listLocked(StringView name, String &out) {
        out.clear();
        Database db;
        if (db.open().isError()) return Error(); // empty without a database
        Key key = {};
        if (!name.isEmpty() && !directoryKey(name, key)) return Error();
        const StringView       prefix(key);
        const Result<uint32_t> at = db.bundle().lowerBound(db, prefix);
        if (at.error().isError()) return at.error();
        char last[ZoneBundle::MaxName + 1] = {};
        for (uint32_t i = at.value(); i < db.bundle().count(); ++i) {
                ZoneBundle::Entry e;
                Error             err = db.bundle().entry(db, i, e);
                if (err.isError()) return err;
                if (!e.nameView().startsWith(prefix)) break;
                const StringView rest = e.nameView().mid(prefix.size());
                const size_t     slash = rest.indexOf('/');
                const StringView child = slash == StringView::NPos ? rest : rest.left(slash);
                // Names in a directory are together: one record for each.
                if (child == StringView(last)) continue;
                std::memcpy(last, child.data(), child.size());
                last[child.size()] = '\0';
                const uint32_t size = slash == StringView::NPos ? e.length : 0;
                const char     header[RecordHeader] = {slash == StringView::NPos ? 'f' : 'd',
                                                   static_cast<char>(size & 0xff), static_cast<char>(size >> 8 & 0xff),
                                                   static_cast<char>(size >> 16 & 0xff),
                                                   static_cast<char>(size >> 24 & 0xff)};
                err = out.append(StringView(header, sizeof(header)));
                if (err.isOk()) err = out.append(child);
                if (err.isOk()) err = out.append('\0');
                if (err.isError()) return err;
        }
        return Error();
}

Result<FileHandle::Ptr> Zones::openLocked(uint32_t offset, uint32_t length) {
        Database db;
        Error    err = db.open();
        if (err.isError()) return makeError<FileHandle::Ptr>(err == Error::NoMem ? err : Error::NotExist);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(ZoneStream) + length);
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *stream = ::new (mem) ZoneStream(length);
        err = db.readAt(offset, WritableByteSpan(stream->bytes(), length));
        Result<FileHandle::Ptr> handle =
                err.isOk() ? FileHandle::open(*stream, FileHandle::Read) : makeError<FileHandle::Ptr>(err);
        if (handle.error().isError()) {
                stream->~ZoneStream();
                Memory::free(mem);
        }
        return handle;
}

void Zones::changedLocked(uint32_t device, uint64_t id) {
        if ((device == 0 && id == 0) || id == databaseId) forgetAnswerLocked();
        return;
}

void Zones::mountsChangedLocked() {
        forgetAnswerLocked();
        return;
}

Error Vfs::setZoneDatabase(StringView path) {
        String normalized;
        if (!path.isEmpty()) {
                if (path[0] != '/') return Error::Invalid;
                Result<String> n = normalize(path, "/");
                if (n.error().isError()) return n.error();
                normalized = std::move(n).value();
                if (normalized.size() > MaxZoneDatabasePath) return Error::Invalid;
        }
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                std::memcpy(databasePath, normalized.data(), normalized.size());
                databasePath[normalized.size()] = '\0';
                databaseId = normalized.isEmpty() ? 0 : Fs::pathId(normalized.view());
                forgetAnswerLocked();
        }
        Clock::retryZoneName();
        return Error();
}

Result<String> Vfs::zoneDatabase() {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        path;
        const Error   err = path.append(StringView(databasePath));
        if (err.isError()) return makeError<String>(err);
        return makeResult(std::move(path));
}

Result<Vfs::ZoneDatabaseInfo> Vfs::zoneDatabaseInfo() {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        Database      db;
        const Error   err = db.open();
        if (err.isError()) return makeError<ZoneDatabaseInfo>(err);
        ZoneDatabaseInfo info;
        const StringView version = db.bundle().version();
        std::memcpy(info.version, version.data(), version.size());
        info.count = db.bundle().count();
        info.fromRom = db.isFromRom();
        return makeResult(info);
}

Result<TimeZone> Vfs::zoneRules(StringView name) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        if (answer.valid && name == StringView(answer.name)) {
                if (!answer.found) return makeError<TimeZone>(Error::NotExist);
                return makeResult(answer.rules);
        }
        Database db;
        if (db.open().isError()) return makeError<TimeZone>(Error::NotReady);
        ZoneBundle::Entry entry;
        const Error       err = db.bundle().find(db, name, entry);
        if (err.isError() && err != Error::NotExist) return makeError<TimeZone>(Error::NotReady);
        TimeZone rules;
        if (err.isOk()) {
                const Result<TimeZone> found = ZoneBundle::rules(db, entry);
                if (found.error().isError()) return makeError<TimeZone>(Error::NotReady);
                rules = found.value();
        }
        if (name.size() <= ZoneBundle::MaxName) {
                std::memcpy(answer.name, name.data(), name.size());
                answer.name[name.size()] = '\0';
                answer.rules = rules;
                answer.found = err.isOk();
                answer.valid = true;
        }
        if (err.isError()) return makeError<TimeZone>(err);
        return makeResult(rules);
}

void Zones::installResolver() {
        Clock::setZoneResolver(resolveZone);
        return;
}

BMDS_NAMESPACE_END
