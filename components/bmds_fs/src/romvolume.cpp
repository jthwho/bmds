/**
 * @file      romvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <bmds/memory.h>
#include "romvolume.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit RomVolume volumes[RomVolume::MaxVolumes];

        /**
         * An open file: its bytes in the image and a position. Lives in Psram until its
         * FileHandle closes. Reads copy from the image, under the storage lock (a handle's
         * tasks share the position).
         */
        class RomFile final : public Stream {
                public:
                        RomFile(const RomVolume &volume, ByteSpan data)
                            : _volume(volume), _data(data), _generation(volume.generation()) {}

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                if (buf.isEmpty()) return makeResult(size_t(0));
                                if (_position >= _data.size()) return makeError<size_t>(Error::EndOfFile);
                                const uint64_t left = _data.size() - _position;
                                const size_t   n = buf.size() < left ? buf.size() : static_cast<size_t>(left);
                                std::memcpy(buf.data(), _data.data() + _position, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan, unsigned int) override {
                                return makeError<size_t>(Error::ReadOnly);
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<uint64_t>(Error::NoMedia);
                                int64_t base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(_position);
                                if (from == SeekFrom::End) base = static_cast<int64_t>(_data.size());
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                _position = static_cast<uint64_t>(base + offset);
                                return makeResult(_position);
                        }

                        Error truncate(uint64_t) override { return Error::ReadOnly; }

                        void handleClosed(uint8_t) override {
                                this->~RomFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        bool stale() const { return !_volume.isMounted() || _volume.generation() != _generation; }

                        const RomVolume &_volume;
                        ByteSpan         _data;
                        const uint32_t   _generation;
                        uint64_t         _position = 0;
        };

        // A file opened for plain reads (Volume::Reader).
        class RomReader final : public Volume::Reader {
                public:
                        explicit RomReader(ByteSpan data) : _data(data) {}

                        uint64_t size() const override { return _data.size(); }

                        Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) override {
                                if (offset >= _data.size()) return makeResult(size_t(0));
                                const uint64_t left = _data.size() - offset;
                                const size_t   n = buf.size() < left ? buf.size() : static_cast<size_t>(left);
                                std::memcpy(buf.data(), _data.data() + offset, n);
                                return makeResult(n);
                        }

                private:
                        ByteSpan _data;
        };

        // A directory's position: its children, one after another.
        class RomCursor final : public Volume::Cursor {
                public:
                        RomCursor(const RomVolume &volume, uint32_t first, uint32_t count)
                            : _volume(volume), _next(first), _end(first + count), _generation(volume.generation()) {}

                        Error next(FileInfo &info) override {
                                if (!_volume.isMounted() || _volume.generation() != _generation) return Error::NoMedia;
                                if (_next >= _end) return Error::EndOfFile;
                                return _volume.describe(_next++, info);
                        }

                private:
                        const RomVolume &_volume;
                        uint32_t         _next;
                        const uint32_t   _end;
                        const uint32_t   _generation;
        };

} // namespace

RomVolume *RomVolume::acquire() {
        for (RomVolume &v : volumes) {
                if (v._inUse) continue;
                v._inUse = true;
                return &v;
        }
        return nullptr;
}

Error RomVolume::mount(BlockDevice &device) {
        if (_mounted) return Error::InvalidState;
        Result<RomImage> opened = RomImage::open(device.mapped());
        if (opened.error().isError()) return opened.error();
        _image = opened.value();
        _device = &device;
        ++_generation;
        _mounted = true;
        return Error::Ok;
}

void RomVolume::release() {
        // Every file and directory still open is stale from now on.
        ++_generation;
        _mounted = false;
        _image = RomImage();
        _device = nullptr;
        _inUse = false;
        return;
}

Result<FsInfo> RomVolume::info() {
        if (!_mounted) return makeError<FsInfo>(Error::NoMedia);
        FsInfo info;
        info.type = "rom";
        info.totalBytes = _image.bytes().size();
        info.freeBytes = 0;
        info.clusterBytes = 1;
        return makeResult(info);
}

Result<uint32_t> RomVolume::find(StringView path, bool &noPath) const {
        noPath = false;
        if (!_mounted) return makeError<uint32_t>(Error::NoMedia);
        return _image.find(path, noPath);
}

Result<uint32_t> RomVolume::findFile(StringView path, bool &noPath) const {
        Result<uint32_t> found = find(path, noPath);
        if (found.error().isOk() && _image.entry(found.value()).directory) return makeError<uint32_t>(Error::IsDir);
        return found;
}

Error RomVolume::describe(uint32_t index, FileInfo &info) const {
        const RomImage::Entry e = _image.entry(index);
        info = FileInfo();
        const Error err = info.name.assign(e.name);
        if (err.isError()) return err;
        info.type = e.directory ? FileType::Directory : FileType::File;
        info.size = e.directory ? 0 : e.data.size();
        info.modified = _image.buildTime();
        info.attributes = FileInfo::ReadOnly | ((e.flags & RomImage::System) != 0 ? FileInfo::System : 0);
        return Error::Ok;
}

Error RomVolume::stat(StringView path, FileInfo &info, bool &noPath) {
        const Result<uint32_t> found = find(path, noPath);
        if (found.error().isError()) return found.error();
        return describe(found.value(), info);
}

Result<FileHandle::Ptr> RomVolume::open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                        String *link) {
        (void)identity;
        const Result<uint32_t> found = findFile(path, noPath);
        if (found.error() == Error::NotExist && (flags & Vfs::Create) != 0 && !noPath)
                return makeError<FileHandle::Ptr>(Error::ReadOnly);
        if (found.error().isError()) return makeError<FileHandle::Ptr>(found.error());
        if ((flags & (Vfs::Write | Vfs::Truncate | Vfs::Append)) != 0)
                return makeError<FileHandle::Ptr>(Error::ReadOnly);
        if ((flags & Vfs::Create) != 0 && (flags & Vfs::Exclusive) != 0)
                return makeError<FileHandle::Ptr>(Error::Exists);
        const RomImage::Entry e = _image.entry(found.value());
        // A System file may be an emulated link (devplan/storage.md §4).
        if (link != nullptr) {
                link->clear();
                const uint8_t attributes = (e.flags & RomImage::System) != 0 ? FileInfo::System : 0;
                if (Fs::isLinkCandidate(attributes, FileType::File, e.data.size()) &&
                    Fs::parseLink(e.data.data(), e.data.size(), *link).isOk())
                        return makeError<FileHandle::Ptr>(Error::Invalid);
                link->clear();
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(RomFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto                   *file = ::new (mem) RomFile(*this, e.data);
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, FileHandle::Read);
        if (handle.error().isError()) {
                file->~RomFile();
                Memory::free(mem);
        }
        return handle;
}

Result<Volume::Reader *> RomVolume::openReader(StringView path) {
        bool                   noPath = false;
        const Result<uint32_t> found = findFile(path, noPath);
        if (found.error().isError()) return makeError<Reader *>(found.error());
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(RomReader));
        if (mem == nullptr) return makeError<Reader *>(Error::NoMem);
        return makeResult<Reader *>(::new (mem) RomReader(_image.entry(found.value()).data));
}

void RomVolume::closeReader(Reader *reader) {
        if (reader == nullptr) return;
        auto *r = static_cast<RomReader *>(reader);
        r->~RomReader();
        Memory::free(r);
        return;
}

Error RomVolume::mkdir(StringView path, bool &noPath) {
        const Result<uint32_t> found = find(path, noPath);
        if (found.error().isOk()) return Error::Exists;
        return noPath ? found.error() : Error(Error::ReadOnly);
}

Error RomVolume::rmdir(StringView path) {
        (void)path;
        return Error::ReadOnly;
}

Error RomVolume::remove(StringView path) {
        (void)path;
        return Error::ReadOnly;
}

Error RomVolume::rename(StringView from, StringView to, Vfs::RenameMode mode, bool &noPath) {
        (void)from;
        (void)to;
        (void)mode;
        noPath = false;
        return Error::ReadOnly;
}

Error RomVolume::setModified(StringView path, Timestamp time) {
        (void)path;
        (void)time;
        return Error::ReadOnly;
}

Error RomVolume::writeLink(StringView path, ByteSpan contents, bool &noPath) {
        (void)path;
        (void)contents;
        noPath = false;
        return Error::ReadOnly;
}

Result<Volume::Cursor *> RomVolume::openDirectory(StringView path, bool &noPath) {
        const Result<uint32_t> found = find(path, noPath);
        if (found.error().isError()) return makeError<Cursor *>(found.error());
        const RomImage::Entry e = _image.entry(found.value());
        if (!e.directory) {
                // A file (perhaps a link): the VFS looks for a link there.
                noPath = true;
                return makeError<Cursor *>(Error::NotExist);
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(RomCursor));
        if (mem == nullptr) return makeError<Cursor *>(Error::NoMem);
        return makeResult<Cursor *>(::new (mem) RomCursor(*this, e.first, e.count));
}

void RomVolume::closeDirectory(Cursor *cursor) {
        if (cursor == nullptr) return;
        auto *c = static_cast<RomCursor *>(cursor);
        c->~RomCursor();
        Memory::free(c);
        return;
}

Result<ByteSpan> RomVolume::map(StringView path, bool &noPath) {
        const Result<uint32_t> found = findFile(path, noPath);
        if (found.error().isError()) return makeError<ByteSpan>(found.error());
        return makeResult(_image.entry(found.value()).data);
}

BMDS_NAMESPACE_END
