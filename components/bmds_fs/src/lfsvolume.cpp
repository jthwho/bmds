/**
 * @file      lfsvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * littlefs behind the Volume interface (devplan/storage.md §13d).
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/stream.h>

#include "lfsvolume.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit LittleFsVolume volumes[Vfs::MaxMounts];

        constexpr uint32_t ReadBytes = 16;
        constexpr uint32_t ProgramBytes = 16;
        constexpr int32_t  BlockCycles = 500; // erases before littlefs moves a metadata block (wear)

        // littlefs's block device: the flash in the configuration's context.
        FlashDevice &flashOf(const lfs_config *c) {
                return *static_cast<FlashDevice *>(c->context);
        }

        int flashRead(const lfs_config *c, lfs_block_t block, lfs_off_t offset, void *buffer, lfs_size_t size) {
                const Error err = flashOf(c).read(block * c->block_size + offset,
                                                  WritableByteSpan(static_cast<uint8_t *>(buffer), size));
                return err.isOk() ? 0 : LFS_ERR_IO;
        }

        int flashProgram(const lfs_config *c, lfs_block_t block, lfs_off_t offset, const void *buffer,
                         lfs_size_t size) {
                const Error err = flashOf(c).program(block * c->block_size + offset,
                                                     ByteSpan(static_cast<const uint8_t *>(buffer), size));
                return err.isOk() ? 0 : LFS_ERR_IO;
        }

        int flashErase(const lfs_config *c, lfs_block_t block) {
                return flashOf(c).erase(block).isOk() ? 0 : LFS_ERR_IO;
        }

        // A commit doesn't wait for the flash (a write cache may hold it): fsync does (LfsFile).
        int flashSync(const lfs_config *) {
                return 0;
        }

        size_t lookaheadBytes(const FlashDevice &flash) {
                return (flash.blockCount() + 63u) / 64u * 8u; // a bit per block, a multiple of 8 bytes
        }

        size_t bufferBytes(const FlashDevice &flash) {
                return 2 * LittleFsVolume::CacheBytes + lookaheadBytes(flash);
        }

        void configure(lfs_config &c, FlashDevice &flash, uint8_t *buffers) {
                c = lfs_config{};
                c.context = &flash;
                c.read = flashRead;
                c.prog = flashProgram;
                c.erase = flashErase;
                c.sync = flashSync;
                c.read_size = ReadBytes;
                c.prog_size = ProgramBytes;
                c.block_size = flash.blockBytes();
                c.block_count = flash.blockCount();
                c.block_cycles = BlockCycles;
                c.cache_size = LittleFsVolume::CacheBytes;
                c.lookahead_size = static_cast<lfs_size_t>(lookaheadBytes(flash));
                c.read_buffer = buffers;
                c.prog_buffer = buffers + LittleFsVolume::CacheBytes;
                c.lookahead_buffer = buffers + 2 * LittleFsVolume::CacheBytes;
                return;
        }

        // littlefs wants a NUL-terminated path: "/" for the root.
        Error cPath(StringView path, String &out) {
                out.clear();
                return out.append(path.isEmpty() ? StringView("/") : path);
        }

        // The open-file table's key: FNV-1a over the exact path (littlefs's names keep case).
        uint64_t keyOf(StringView path) {
                uint64_t key = 14695981039346656037ull;
                for (char c : path) key = (key ^ static_cast<uint8_t>(c)) * 1099511628211ull;
                return key;
        }

        int lfsFlags(uint8_t flags) {
                int mode = 0;
                if ((flags & (Vfs::Read | Vfs::Write)) == (Vfs::Read | Vfs::Write)) {
                        mode = LFS_O_RDWR;
                } else {
                        mode = (flags & Vfs::Write) ? LFS_O_WRONLY : LFS_O_RDONLY;
                }
                if (flags & Vfs::Create) mode |= LFS_O_CREAT;
                if ((flags & Vfs::Create) && (flags & Vfs::Exclusive)) mode |= LFS_O_EXCL;
                if (flags & Vfs::Truncate) mode |= LFS_O_TRUNC;
                if (flags & Vfs::Append) mode |= LFS_O_APPEND;
                return mode;
        }

        int64_t nowNs() {
                return Clock::now().ns;
        }

        /**
         * An open littlefs file as a Stream. Lives in Psram (with littlefs's
         * file and its buffer) until its FileHandle closes. Files don't block,
         * so the stream timeouts don't apply.
         */
        class LfsFile final : public Stream {
                public:
                        LfsFile(LittleFsVolume &volume, uint64_t key, uint8_t flags, const FileIdentity &identity)
                            : _volume(volume), _key(key), _identity(identity), _generation(volume.generation()),
                              _readable((flags & Vfs::Read) != 0), _writable((flags & Vfs::Write) != 0) {
                                _config.buffer = _buffer;
                                if (_writable) {
                                        // The new time goes into the commit that writes the data (at close or fsync).
                                        _time = nowNs();
                                        _attribute = lfs_attr{LittleFsVolume::TimeAttribute, &_time, sizeof(_time)};
                                        _config.attrs = &_attribute;
                                        _config.attr_count = 1;
                                }
                        }

                        lfs_file_t      &file() { return _file; }
                        lfs_file_config &config() { return _config; }

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                if (!_readable) return makeError<size_t>(Error::BadDescriptor);
                                const lfs_ssize_t n = lfs_file_read(&_volume.lfs(), &_file, buf.data(),
                                                                    static_cast<lfs_size_t>(buf.size()));
                                if (n < 0) return makeError<size_t>(LittleFsVolume::toError(int(n)));
                                if (n == 0 && !buf.isEmpty()) return makeError<size_t>(Error::EndOfFile);
                                return makeResult(static_cast<size_t>(n));
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                if (!_writable) return makeError<size_t>(Error::BadDescriptor);
                                const lfs_ssize_t n = lfs_file_write(&_volume.lfs(), &_file, data.data(),
                                                                     static_cast<lfs_size_t>(data.size()));
                                if (n < 0) return makeError<size_t>(LittleFsVolume::toError(int(n)));
                                if (size_t(n) < data.size()) return makeError<size_t>(Error::NoSpace);
                                return makeResult(static_cast<size_t>(n));
                        }

                        using Stream::write;

                        // fsync: committed, then on the flash (waited for without the lock).
                        Error flush(unsigned int timeoutMs) override {
                                FlashDevice *flash = nullptr;
                                {
                                        Mutex::Locker lock(Fs::lock(), WaitForever);
                                        if (stale()) return Error::NoMedia;
                                        if (_writable) {
                                                _time = nowNs();
                                                const int r = lfs_file_sync(&_volume.lfs(), &_file);
                                                if (r < 0) return LittleFsVolume::toError(r);
                                        }
                                        flash = &_volume.flash();
                                }
                                return flash->flush(timeoutMs);
                        }

                        // Past the end, a write fills the gap with zeros (littlefs does).
                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<uint64_t>(Error::NoMedia);
                                const int        whence = from == SeekFrom::Start     ? LFS_SEEK_SET
                                                          : from == SeekFrom::Current ? LFS_SEEK_CUR
                                                                                      : LFS_SEEK_END;
                                const lfs_soff_t at =
                                        lfs_file_seek(&_volume.lfs(), &_file, static_cast<lfs_soff_t>(offset), whence);
                                if (at < 0) return makeError<uint64_t>(LittleFsVolume::toError(int(at)));
                                return makeResult(static_cast<uint64_t>(at));
                        }

                        Error truncate(uint64_t size) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return Error::NoMedia;
                                if (!_writable) return Error::BadDescriptor;
                                // littlefs keeps the position, even past the new end.
                                return LittleFsVolume::toError(
                                        lfs_file_truncate(&_volume.lfs(), &_file, static_cast<lfs_off_t>(size)));
                        }

                        void handleClosed(uint8_t) override {
                                {
                                        Mutex::Locker lock(Fs::lock(), WaitForever);
                                        // A stale file (unmounted) is only freed.
                                        if (!stale()) {
                                                if (_writable) _time = nowNs();
                                                (void)lfs_file_close(&_volume.lfs(), &_file);
                                                _volume.unclaim(_key, _writable);
                                                if (_writable) Fs::writtenLocked(_identity);
                                        }
                                }
                                this->~LfsFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        bool stale() const { return _generation != _volume.generation(); }

                        lfs_file_t      _file = {};
                        lfs_file_config _config = {};
                        lfs_attr        _attribute = {};
                        int64_t         _time = 0;
                        uint8_t         _buffer[LittleFsVolume::CacheBytes];
                        LittleFsVolume &_volume;
                        const uint64_t  _key;
                        const FileIdentity _identity; // told at close after writing
                        const uint32_t  _generation;
                        const bool      _readable;
                        const bool      _writable;
        };

        // A file open for plain reads (Psram).
        class LfsReader final : public Volume::Reader {
                public:
                        explicit LfsReader(LittleFsVolume &volume) : _volume(volume), _generation(volume.generation()) {
                                _config.buffer = _buffer;
                        }

                        lfs_file_t      &file() { return _file; }
                        lfs_file_config &config() { return _config; }
                        bool             stale() const { return _generation != _volume.generation(); }

                        uint64_t size() const override { return _size; }
                        void     setSize(uint64_t size) { _size = size; }

                        Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) override {
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                lfs_t           &lfs = _volume.lfs();
                                const lfs_soff_t at =
                                        lfs_file_seek(&lfs, &_file, static_cast<lfs_soff_t>(offset), LFS_SEEK_SET);
                                if (at < 0) return makeError<size_t>(LittleFsVolume::toError(int(at)));
                                const lfs_ssize_t n =
                                        lfs_file_read(&lfs, &_file, buf.data(), static_cast<lfs_size_t>(buf.size()));
                                if (n < 0) return makeError<size_t>(LittleFsVolume::toError(int(n)));
                                return makeResult(static_cast<size_t>(n));
                        }

                private:
                        lfs_file_t      _file = {};
                        lfs_file_config _config = {};
                        uint8_t         _buffer[LittleFsVolume::CacheBytes];
                        LittleFsVolume &_volume;
                        const uint32_t  _generation;
                        uint64_t        _size = 0;
        };

        // An open directory (Psram), with its path for the entries' attributes.
        class LfsCursor final : public Volume::Cursor {
                public:
                        explicit LfsCursor(LittleFsVolume &volume)
                            : _volume(volume), _generation(volume.generation()) {}

                        lfs_dir_t &dir() { return _dir; }
                        String    &path() { return _path; }
                        bool       stale() const { return _generation != _volume.generation(); }

                        Error next(FileInfo &info) override {
                                if (stale()) return Error::NoMedia;
                                for (;;) {
                                        const int r = lfs_dir_read(&_volume.lfs(), &_dir, &_entry);
                                        if (r < 0) return LittleFsVolume::toError(r);
                                        if (r == 0) return Error::EndOfFile;
                                        if (std::strcmp(_entry.name, ".") != 0 && std::strcmp(_entry.name, "..") != 0)
                                                break;
                                }
                                info = FileInfo();
                                Error err = info.name.assign(StringView(_entry.name));
                                if (err.isError()) return err;
                                info.type = _entry.type == LFS_TYPE_DIR ? FileType::Directory : FileType::File;
                                info.size = info.type == FileType::File ? _entry.size : 0;
                                // The entry's path, for its time and attributes.
                                String child;
                                err = child.append(_path.view() == "/" ? StringView() : _path.view());
                                if (err.isOk()) err = child.append('/');
                                if (err.isOk()) err = child.append(StringView(_entry.name));
                                if (err.isOk()) _volume.readAttributes(child.cStr(), info);
                                return err;
                        }

                private:
                        lfs_dir_t       _dir = {};
                        lfs_info        _entry = {};
                        String          _path{MemSpace::Psram};
                        LittleFsVolume &_volume;
                        const uint32_t  _generation;
        };

} // namespace

LittleFsVolume *LittleFsVolume::acquire() {
        for (LittleFsVolume &v : volumes) {
                if (v._inUse) continue;
                v._inUse = true;
                return &v;
        }
        return nullptr;
}

Error LittleFsVolume::toError(int result) {
        if (result >= 0) return Error::Ok;
        switch (result) {
                case LFS_ERR_IO: return Error::IoError;
                case LFS_ERR_CORRUPT: return Error::CorruptData;
                case LFS_ERR_NOENT: return Error::NotExist;
                case LFS_ERR_EXIST: return Error::Exists;
                case LFS_ERR_NOTDIR: return Error::NotDir;
                case LFS_ERR_ISDIR: return Error::IsDir;
                case LFS_ERR_NOTEMPTY: return Error::NotEmpty;
                case LFS_ERR_BADF: return Error::BadDescriptor;
                case LFS_ERR_FBIG: return Error::NoSpace;
                case LFS_ERR_INVAL: return Error::Invalid;
                case LFS_ERR_NOSPC: return Error::NoSpace;
                case LFS_ERR_NOMEM: return Error::NoMem;
                case LFS_ERR_NOATTR: return Error::NotExist;
                case LFS_ERR_NAMETOOLONG: return Error::Invalid;
                default: return Error::LibraryFailure;
        }
}

Error LittleFsVolume::format(FlashDevice &flash) {
        // littlefs's state and buffers for the format only (Psram).
        const size_t buffers = bufferBytes(flash);
        auto        *mem = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, sizeof(lfs_t) + buffers));
        if (mem == nullptr) return Error::NoMem;
        auto      *lfs = ::new (mem) lfs_t();
        lfs_config config;
        configure(config, flash, mem + sizeof(lfs_t));
        const int r = lfs_format(lfs, &config);
        Memory::free(mem);
        return toError(r);
}

Error LittleFsVolume::mount(BlockDevice &device, FlashDevice &flash) {
        if (_mounted) return Error::InvalidState;
        _buffers = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bufferBytes(flash)));
        if (_buffers == nullptr) return Error::NoMem;
        configure(_config, flash, _buffers);
        _lfs = lfs_t{};
        const int r = lfs_mount(&_lfs, &_config);
        if (r < 0) {
                Memory::free(_buffers);
                _buffers = nullptr;
                // No superblock (or not littlefs's): nothing to mount.
                return r == LFS_ERR_CORRUPT || r == LFS_ERR_INVAL ? Error(Error::NoFilesystem) : toError(r);
        }
        _flash = &flash;
        _device = &device;
        ++_generation;
        _mounted = true;
        return Error::Ok;
}

void LittleFsVolume::unmount() {
        if (!_mounted) return;
        (void)lfs_unmount(&_lfs);
        Memory::free(_buffers);
        _buffers = nullptr;
        // Every file and directory still open is stale from now on.
        ++_generation;
        for (OpenFile &f : _open) f = OpenFile();
        _mounted = false;
        return;
}

void LittleFsVolume::release() {
        unmount();
        _flash = nullptr;
        _device = nullptr;
        _inUse = false;
        return;
}

bool LittleFsVolume::isOpen(uint64_t key) const {
        for (const OpenFile &f : _open)
                if (f.key == key && (f.readers != 0 || f.writer)) return true;
        return false;
}

Error LittleFsVolume::claim(uint64_t key, bool writing) {
        OpenFile *free = nullptr;
        for (OpenFile &f : _open) {
                if (f.readers == 0 && !f.writer) {
                        if (free == nullptr) free = &f;
                        continue;
                }
                if (f.key != key) continue;
                if (writing || f.writer) return Error::Busy;
                ++f.readers;
                return Error::Ok;
        }
        if (free == nullptr) return Error::TooManyOpen;
        free->key = key;
        free->writer = writing;
        free->readers = writing ? 0 : 1;
        return Error::Ok;
}

void LittleFsVolume::unclaim(uint64_t key, bool writing) {
        for (OpenFile &f : _open) {
                if (f.key != key || (f.readers == 0 && !f.writer)) continue;
                if (writing) {
                        f.writer = false;
                } else if (f.readers != 0) {
                        --f.readers;
                }
                return;
        }
        return;
}

void LittleFsVolume::readAttributes(const char *path, FileInfo &info) {
        int64_t time = 0;
        if (lfs_getattr(&_lfs, path, TimeAttribute, &time, sizeof(time)) == lfs_ssize_t(sizeof(time)))
                info.modified = Timestamp{time};
        uint8_t attributes = 0;
        if (lfs_getattr(&_lfs, path, AttributesAttribute, &attributes, 1) == 1) info.attributes = attributes;
        return;
}

Error LittleFsVolume::notFound(StringView path, bool &noPath) {
        noPath = false;
        const size_t slash = path.lastIndexOf('/');
        if (slash == StringView::NPos || slash == 0) return Error::NotExist;
        String parent;
        if (parent.append(path.left(slash)).isError()) return Error::NoMem;
        lfs_info info;
        noPath = lfs_stat(&_lfs, parent.cStr(), &info) < 0 || info.type != LFS_TYPE_DIR;
        return Error::NotExist;
}

Result<FsInfo> LittleFsVolume::info() {
        if (!_mounted) return makeError<FsInfo>(Error::NoMedia);
        const lfs_ssize_t used = lfs_fs_size(&_lfs);
        if (used < 0) return makeError<FsInfo>(toError(int(used)));
        FsInfo info;
        info.type = "littlefs";
        info.clusterBytes = _config.block_size;
        info.totalBytes = uint64_t(_config.block_count) * _config.block_size;
        const uint64_t usedBlocks = uint64_t(used) < _config.block_count ? uint64_t(used) : _config.block_count;
        info.freeBytes = (_config.block_count - usedBlocks) * _config.block_size;
        return makeResult(info);
}

Error LittleFsVolume::stat(StringView path, FileInfo &info, bool &noPath) {
        noPath = false;
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return err;
        lfs_info  entry;
        const int r = lfs_stat(&_lfs, p.cStr(), &entry);
        if (r == LFS_ERR_NOENT) return notFound(path, noPath);
        if (r == LFS_ERR_NOTDIR) {
                noPath = true;
                return Error::NotExist;
        }
        if (r < 0) return toError(r);
        info = FileInfo();
        err = info.name.assign(StringView(entry.name));
        if (err.isError()) return err;
        info.type = entry.type == LFS_TYPE_DIR ? FileType::Directory : FileType::File;
        info.size = info.type == FileType::File ? entry.size : 0;
        readAttributes(p.cStr(), info);
        return Error::Ok;
}

Result<FileHandle::Ptr> LittleFsVolume::open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                             String *link) {
        noPath = false;
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        const uint64_t key = keyOf(p.view());
        const bool     writing = (flags & Vfs::Write) != 0;
        err = claim(key, writing);
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(LfsFile));
        if (mem == nullptr) {
                unclaim(key, writing);
                return makeError<FileHandle::Ptr>(Error::NoMem);
        }
        auto     *file = ::new (mem) LfsFile(*this, key, flags, identity);
        const int r = lfs_file_opencfg(&_lfs, &file->file(), p.cStr(), lfsFlags(flags), &file->config());
        if (r < 0) {
                file->~LfsFile();
                Memory::free(mem);
                unclaim(key, writing);
                if (r == LFS_ERR_NOENT) return makeError<FileHandle::Ptr>(notFound(path, noPath));
                if (r == LFS_ERR_NOTDIR) {
                        noPath = true;
                        return makeError<FileHandle::Ptr>(Error::NotExist);
                }
                return makeError<FileHandle::Ptr>(toError(r));
        }
        // A System file may be an emulated link: read it here.
        uint8_t attributes = 0;
        if (link != nullptr && (flags & Vfs::Truncate) == 0 &&
            lfs_getattr(&_lfs, p.cStr(), AttributesAttribute, &attributes, 1) == 1 &&
            Fs::isLinkCandidate(attributes, FileType::File, uint64_t(lfs_file_size(&_lfs, &file->file()))) &&
            (flags & Vfs::Read) != 0) {
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Fs::MaxLinkFile));
                if (buf != nullptr) {
                        const lfs_ssize_t n =
                                lfs_file_read(&_lfs, &file->file(), buf, static_cast<lfs_size_t>(Fs::MaxLinkFile));
                        const bool isLink = n > 0 && Fs::parseLink(buf, size_t(n), *link).isOk();
                        Memory::free(buf);
                        if (isLink) {
                                (void)lfs_file_close(&_lfs, &file->file());
                                file->~LfsFile();
                                Memory::free(mem);
                                unclaim(key, writing);
                                return makeError<FileHandle::Ptr>(Error::Invalid);
                        }
                }
                link->clear();
                (void)lfs_file_seek(&_lfs, &file->file(), 0, LFS_SEEK_SET);
        }
        const uint8_t           mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) | (writing ? FileHandle::Write : 0);
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, mode);
        if (handle.error().isError()) {
                (void)lfs_file_close(&_lfs, &file->file());
                file->~LfsFile();
                Memory::free(mem);
                unclaim(key, writing);
        }
        return handle;
}

Result<Volume::Reader *> LittleFsVolume::openReader(StringView path) {
        String      p;
        const Error err = cPath(path, p);
        if (err.isError()) return makeError<Reader *>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(LfsReader));
        if (mem == nullptr) return makeError<Reader *>(Error::NoMem);
        auto     *reader = ::new (mem) LfsReader(*this);
        const int r = lfs_file_opencfg(&_lfs, &reader->file(), p.cStr(), LFS_O_RDONLY, &reader->config());
        if (r < 0) {
                reader->~LfsReader();
                Memory::free(mem);
                return makeError<Reader *>(toError(r));
        }
        reader->setSize(uint64_t(lfs_file_size(&_lfs, &reader->file())));
        return makeResult<Reader *>(reader);
}

void LittleFsVolume::closeReader(Reader *reader) {
        if (reader == nullptr) return;
        auto *r = static_cast<LfsReader *>(reader);
        if (!r->stale()) (void)lfs_file_close(&_lfs, &r->file());
        r->~LfsReader();
        Memory::free(r);
        return;
}

Error LittleFsVolume::mkdir(StringView path, bool &noPath) {
        noPath = false;
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return err;
        const int r = lfs_mkdir(&_lfs, p.cStr());
        if (r == LFS_ERR_NOENT) return notFound(path, noPath);
        if (r == LFS_ERR_NOTDIR) {
                noPath = true;
                return Error::NotExist;
        }
        if (r < 0) return toError(r);
        const int64_t time = nowNs();
        (void)lfs_setattr(&_lfs, p.cStr(), TimeAttribute, &time, sizeof(time));
        return Error::Ok;
}

Error LittleFsVolume::rmdir(StringView path) {
        String p;
        Error  err = cPath(path, p);
        if (err.isOk()) err = toError(lfs_remove(&_lfs, p.cStr()));
        return err;
}

Error LittleFsVolume::remove(StringView path) {
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return err;
        if (isOpen(keyOf(p.view()))) return Error::Busy;
        return toError(lfs_remove(&_lfs, p.cStr()));
}

Error LittleFsVolume::rename(StringView from, StringView to, bool &noPath) {
        noPath = false;
        String fromPath;
        String toPath;
        Error  err = cPath(from, fromPath);
        if (err.isOk()) err = cPath(to, toPath);
        if (err.isError()) return err;
        if (isOpen(keyOf(fromPath.view())) || isOpen(keyOf(toPath.view()))) return Error::Busy;
        // As on FAT: an existing name isn't replaced.
        lfs_info info;
        if (lfs_stat(&_lfs, toPath.cStr(), &info) == 0) return Error::Exists;
        const int r = lfs_rename(&_lfs, fromPath.cStr(), toPath.cStr());
        if (r == LFS_ERR_NOENT || r == LFS_ERR_NOTDIR) {
                bool fromBlocked = false;
                bool toBlocked = false;
                (void)notFound(from, fromBlocked);
                (void)notFound(to, toBlocked);
                noPath = r == LFS_ERR_NOTDIR || fromBlocked || toBlocked;
                return Error::NotExist;
        }
        return toError(r);
}

Error LittleFsVolume::setModified(StringView path, Timestamp time) {
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return err;
        const int64_t ns = time.ns;
        return toError(lfs_setattr(&_lfs, p.cStr(), TimeAttribute, &ns, sizeof(ns)));
}

Error LittleFsVolume::writeLink(StringView path, ByteSpan contents, bool &noPath) {
        noPath = false;
        String p;
        Error  err = cPath(path, p);
        if (err.isError()) return err;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(LfsReader));
        if (mem == nullptr) return Error::NoMem;
        // A reader's file and buffer, written once: the attributes go into the commit with the data.
        auto           *holder = ::new (mem) LfsReader(*this);
        int64_t         time = nowNs();
        uint8_t         attributes = FileInfo::System | FileInfo::Archive;
        lfs_attr        attrs[2] = {{TimeAttribute, &time, sizeof(time)}, {AttributesAttribute, &attributes, 1}};
        lfs_file_config config = holder->config();
        config.attrs = attrs;
        config.attr_count = 2;
        int r = lfs_file_opencfg(&_lfs, &holder->file(), p.cStr(), LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL, &config);
        if (r >= 0) {
                const lfs_ssize_t n = lfs_file_write(&_lfs, &holder->file(), contents.data(),
                                                     static_cast<lfs_size_t>(contents.size()));
                r = n < 0 ? int(n) : (size_t(n) < contents.size() ? LFS_ERR_NOSPC : 0);
                const int closed = lfs_file_close(&_lfs, &holder->file());
                if (r >= 0) r = closed;
                if (r < 0) (void)lfs_remove(&_lfs, p.cStr());
        }
        holder->~LfsReader();
        Memory::free(mem);
        if (r == LFS_ERR_NOENT) return notFound(path, noPath);
        if (r == LFS_ERR_NOTDIR) {
                noPath = true;
                return Error::NotExist;
        }
        return toError(r);
}

Result<Volume::Cursor *> LittleFsVolume::openDirectory(StringView path, bool &noPath) {
        noPath = false;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(LfsCursor));
        if (mem == nullptr) return makeError<Cursor *>(Error::NoMem);
        auto *cursor = ::new (mem) LfsCursor(*this);
        Error err = cPath(path, cursor->path());
        int   r = 0;
        if (err.isOk()) {
                r = lfs_dir_open(&_lfs, &cursor->dir(), cursor->path().cStr());
                if (r == LFS_ERR_NOENT) {
                        err = notFound(path, noPath);
                } else if (r == LFS_ERR_NOTDIR) {
                        // The directory itself is a file (perhaps a link), or one along the way is.
                        noPath = true;
                        err = Error::NotExist;
                } else {
                        err = toError(r);
                }
        }
        if (err.isError()) {
                cursor->~LfsCursor();
                Memory::free(mem);
                return makeError<Cursor *>(err);
        }
        return makeResult<Cursor *>(cursor);
}

void LittleFsVolume::closeDirectory(Cursor *cursor) {
        if (cursor == nullptr) return;
        auto *c = static_cast<LfsCursor *>(cursor);
        if (!c->stale()) (void)lfs_dir_close(&_lfs, &c->dir());
        c->~LfsCursor();
        Memory::free(c);
        return;
}

BMDS_NAMESPACE_END
