/**
 * @file      fatvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <bmds/memory.h>

#include "fatlayer.h"
#include "sectorcache.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // f_mkfs()'s work buffer: bigger writes the FAT in fewer, larger
        // requests. DMA-capable, so it goes straight to an SD card.
        constexpr size_t FormatBufferBytes = 32 * 1024;

        constinit FatVolume volumes[FF_VOLUMES];

        // A file open for plain reads (Psram).
        class FatReader final : public Volume::Reader {
                public:
                        explicit FatReader(FatVolume &volume) : _volume(volume) {}

                        FIL &fil() { return _fil; }

                        uint64_t size() const override { return f_size(&_fil); }

                        Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) override {
                                FRESULT result = f_lseek(&_fil, static_cast<FSIZE_t>(offset));
                                UINT    got = 0;
                                if (result == FR_OK)
                                        result = f_read(&_fil, buf.data(), static_cast<UINT>(buf.size()), &got);
                                if (result != FR_OK) return makeError<size_t>(Fat::toError(result, _volume));
                                return makeResult(static_cast<size_t>(got));
                        }

                private:
                        FIL        _fil = {};
                        FatVolume &_volume;
        };

        // An open directory (Psram).
        class FatCursor final : public Volume::Cursor {
                public:
                        explicit FatCursor(FatVolume &volume) : _volume(volume) {}

                        DIR &dir() { return _dir; }

                        Error next(FileInfo &info) override {
                                const FRESULT result = f_readdir(&_dir, &_entry);
                                if (result != FR_OK) return Fat::toError(result, _volume);
                                if (_entry.fname[0] == '\0') return Error::EndOfFile;
                                return Fat::toFileInfo(_entry, info);
                        }

                private:
                        DIR        _dir = {};
                        FILINFO    _entry = {};
                        FatVolume &_volume;
        };

} // namespace

FatVolume *FatVolume::acquire() {
        for (FatVolume &v : volumes) {
                if (v._inUse) continue;
                if (v._fs == nullptr) {
                        // Kept for good once allocated (see the class comment).
                        v._fs = static_cast<FATFS *>(Memory::alloc(MemSpace::Psram, sizeof(FATFS)));
                        if (v._fs == nullptr) return nullptr;
                        std::memset(v._fs, 0, sizeof(FATFS));
                }
                v._inUse = true;
                return &v;
        }
        return nullptr;
}

FatVolume &FatVolume::at(uint8_t drive) {
        return volumes[drive];
}

uint8_t FatVolume::drive() const {
        return static_cast<uint8_t>(this - volumes);
}

void FatVolume::root(char (&path)[3]) const {
        path[0] = static_cast<char>('0' + drive());
        path[1] = ':';
        path[2] = '\0';
        return;
}

void FatVolume::release() {
        unmount();
        _device = nullptr;
        _inUse = false;
        return;
}

// ---- The lookup cache (devplan/storage.md §3)
//
// FatFs walks a directory's entries for every lookup: 80 µs for a file in /sd0/p0/bin (139
// entries, 125 of them BusyBox's links), 235 for a name that isn't there, warm; several times that
// with the caches cold, as after a program's start. f_stat()'s results are kept, the missing ones
// too, until the volume changes: everything that would make f_stat() read differently goes
// through FatVolume or FatFile, and calls changed().

struct FatVolume::Lookup {
                uint32_t generation = 0; // 0: empty
                uint64_t size = 0;
                uint16_t fdate = 0;
                uint16_t ftime = 0;
                uint8_t  attributes = 0;
                uint8_t  result = FR_OK; // FR_OK, FR_NO_FILE or FR_NO_PATH
                uint8_t  pathBytes = 0;
                uint8_t  nameBytes = 0;
                char     path[94];
                char     name[64];
};

namespace {

        constexpr size_t LookupSlots = 64; // direct-mapped by the path's hash

        size_t lookupSlot(StringView path) {
                uint32_t h = 2166136261u;
                for (char c : path) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
                return h % LookupSlots;
        }

} // namespace

Error FatVolume::mount(BlockDevice &device) {
        if (_mounted) return Error::InvalidState;
        if (!device.isPresent()) return Error::NoMedia;
        _device = &device;
        // Whatever the drive held before is another filesystem's (or an
        // older copy of this one's: the card may have been in a PC).
        Fat::cache().discard(drive());
        char path[3];
        root(path);
        changed();
        if (_lookups == nullptr) {
                // Kept with the volume, as its FATFS is (11 KB; none: lookups aren't kept).
                _lookups = static_cast<Lookup *>(Memory::alloc(MemSpace::Psram, sizeof(Lookup) * LookupSlots));
                if (_lookups != nullptr)
                        for (size_t i = 0; i < LookupSlots; ++i) ::new (&_lookups[i]) Lookup();
        }
        const FRESULT result = f_mount(_fs, path, 1);
        if (result != FR_OK) {
                const Error err = Fat::toError(result, *this);
                (void)f_mount(nullptr, path, 0);
                _device = nullptr;
                return err;
        }
        _mounted = true;
        return Error();
}

void FatVolume::unmount() {
        if (!_mounted) return;
        char path[3];
        root(path);
        // What is held written first (lost, and logged, if the card is gone).
        if (_device != nullptr) (void)Fat::flushLocked(drive(), *_device);
        // Clears the volume's type, which every open file and directory on it
        // is checked against: they all fail from now on.
        (void)f_mount(nullptr, path, 0);
        Fat::cache().discard(drive());
        changed();
        _mounted = false;
        _device = nullptr;
        return;
}

Error FatVolume::format(BlockDevice &device, FsFormat format, bool partitionTable) {
        if (!device.isPresent()) return Error::NoMedia;
        FatVolume *v = acquire();
        if (v == nullptr) return Error::Full;
        auto *work = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, FormatBufferBytes));
        if (work == nullptr) {
                v->release();
                return Error::NoMem;
        }
        MKFS_PARM params = {};
        switch (format) {
                case FsFormat::Fat32: params.fmt = FM_FAT32; break;
                case FsFormat::ExFat: params.fmt = FM_EXFAT; break;
                default: params.fmt = FM_ANY; break;
        }
        // On a partition, the filesystem alone ("super floppy" within it).
        if (!partitionTable) params.fmt |= FM_SFD;
        // Everything else is FatFs's choice: cluster size by volume size, the
        // data area aligned to the device's erase block.
        v->_device = &device;
        char path[3];
        v->root(path);
        const FRESULT result = f_mkfs(path, &params, work, FormatBufferBytes);
        Fat::cache().discard(v->drive());
        const Error err = Fat::toError(result, *v);
        Memory::free(work);
        v->release();
        return err;
}

const char *FatVolume::typeName() const {
        if (!_mounted) return "";
        switch (_fs->fs_type) {
                case FS_FAT12: return "fat12";
                case FS_FAT16: return "fat16";
                case FS_FAT32: return "fat32";
                default: return "exfat";
        }
}

Result<FsInfo> FatVolume::info() {
        if (!_mounted) return makeError<FsInfo>(Error::NoMedia);
        char path[3];
        root(path);
        DWORD         freeClusters = 0;
        FATFS        *fs = nullptr;
        const FRESULT result = f_getfree(path, &freeClusters, &fs);
        if (result != FR_OK) return makeError<FsInfo>(Fat::toError(result, *this));
        FsInfo info;
        switch (fs->fs_type) {
                case FS_FAT12: info.type = "FAT12"; break;
                case FS_FAT16: info.type = "FAT16"; break;
                case FS_FAT32: info.type = "FAT32"; break;
                default: info.type = "exFAT"; break;
        }
        info.clusterBytes = static_cast<uint32_t>(fs->csize) * FF_MAX_SS;
        info.totalBytes = static_cast<uint64_t>(fs->n_fatent - 2) * info.clusterBytes;
        info.freeBytes = static_cast<uint64_t>(freeClusters) * info.clusterBytes;
        return makeResult(info);
}

Error FatVolume::fatPath(StringView path, String &out) const {
        char drive[3];
        root(drive);
        out.clear();
        Error err = out.reserve(sizeof(drive) + path.size() + 1);
        if (err.isOk()) err = out.append(StringView(drive));
        if (err.isOk()) err = out.append(path.isEmpty() ? StringView("/") : path);
        return err;
}

const FatVolume::Lookup *FatVolume::findLookup(StringView path) const {
        if (_lookups == nullptr) return nullptr;
        const Lookup &l = _lookups[lookupSlot(path)];
        if (l.generation != _generation || StringView(l.path, l.pathBytes) != path) return nullptr;
        return &l;
}

void FatVolume::keepLookup(StringView path, FRESULT result, const FILINFO &entry, uint32_t generation) {
        if (path.size() > sizeof(Lookup::path) || generation != _generation) return;
        if (result != FR_OK && result != FR_NO_FILE && result != FR_NO_PATH) return; // errors aren't kept
        const size_t nameBytes = result == FR_OK ? std::strlen(entry.fname) : 0;
        if (nameBytes > sizeof(Lookup::name) || _lookups == nullptr) return;
        Lookup &l = _lookups[lookupSlot(path)];
        l.generation = generation;
        l.result = static_cast<uint8_t>(result);
        l.size = result == FR_OK ? entry.fsize : 0;
        l.fdate = result == FR_OK ? entry.fdate : 0;
        l.ftime = result == FR_OK ? entry.ftime : 0;
        l.attributes = result == FR_OK ? entry.fattrib : 0;
        l.pathBytes = static_cast<uint8_t>(path.size());
        l.nameBytes = static_cast<uint8_t>(nameBytes);
        std::memcpy(l.path, path.data(), path.size());
        std::memcpy(l.name, entry.fname, nameBytes);
        return;
}

Error FatVolume::stat(StringView path, FileInfo &info, bool &noPath) {
        noPath = false;
        if (const Lookup *l = findLookup(path); l != nullptr) {
                noPath = l->result == FR_NO_PATH;
                if (l->result != FR_OK) return Fat::toError(static_cast<FRESULT>(l->result), *this);
                return Fat::toFileInfo(StringView(l->name, l->nameBytes), l->attributes, l->size, l->fdate, l->ftime,
                                       info);
        }
        String fat;
        Error  err = fatPath(path, fat);
        if (err.isError()) return err;
        auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
        if (entry == nullptr) return Error::NoMem;
        const uint32_t generation = _generation;
        const FRESULT  result = f_stat(fat.cStr(), entry);
        keepLookup(path, result, *entry, generation);
        noPath = result == FR_NO_PATH;
        err = Fat::toError(result, *this);
        if (err.isOk()) err = Fat::toFileInfo(*entry, info);
        Memory::free(entry);
        return err;
}

Result<FileHandle::Ptr> FatVolume::open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                        String *link) {
        noPath = false;
        String      fat;
        const Error err = fatPath(path, fat);
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        FRESULT                 raw = FR_OK;
        Result<FileHandle::Ptr> handle = Fat::openFile(*this, fat.cStr(), flags, identity, &raw, link);
        noPath = raw == FR_NO_PATH;
        return handle;
}

Result<Volume::Reader *> FatVolume::openReader(StringView path) {
        String      fat;
        const Error err = fatPath(path, fat);
        if (err.isError()) return makeError<Reader *>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(FatReader));
        if (mem == nullptr) return makeError<Reader *>(Error::NoMem);
        auto         *reader = ::new (mem) FatReader(*this);
        const FRESULT result = f_open(&reader->fil(), fat.cStr(), FA_READ);
        if (result != FR_OK) {
                reader->~FatReader();
                Memory::free(mem);
                return makeError<Reader *>(Fat::toError(result, *this));
        }
        return makeResult<Reader *>(reader);
}

void FatVolume::closeReader(Reader *reader) {
        if (reader == nullptr) return;
        auto *r = static_cast<FatReader *>(reader);
        // A stale file (its card went) just fails here.
        (void)f_close(&r->fil());
        r->~FatReader();
        Memory::free(r);
        return;
}

Error FatVolume::mkdir(StringView path, bool &noPath) {
        String fat;
        Error  err = fatPath(path, fat);
        if (err.isError()) return err;
        changed();
        const FRESULT result = f_mkdir(fat.cStr());
        noPath = result == FR_NO_PATH;
        return Fat::toError(result, *this);
}

Error FatVolume::rmdir(StringView path) {
        String fat;
        Error  err = fatPath(path, fat);
        changed();
        if (err.isOk()) err = Fat::toError(f_unlink(fat.cStr()), *this);
        // FatFs denies removing a directory that isn't empty.
        return err == Error::PermissionDenied ? Error(Error::NotEmpty) : err;
}

Error FatVolume::remove(StringView path) {
        String fat;
        Error  err = fatPath(path, fat);
        changed();
        if (err.isOk()) err = Fat::toError(f_unlink(fat.cStr()), *this);
        return err;
}

Error FatVolume::rename(StringView from, StringView to, Vfs::RenameMode mode, bool &noPath) {
        String fromFat;
        String toFat;
        Error  err = fatPath(from, fromFat);
        if (err.isOk()) err = fatPath(to, toFat);
        if (err.isError()) return err;
        changed();
        FRESULT result = f_rename(fromFat.cStr(), toFat.cStr());
        noPath = result == FR_NO_PATH;
        if (result != FR_EXIST || mode == Vfs::RenameMode::NoReplace) return Fat::toError(result, *this);
        // The name is taken, perhaps by the file itself under another case (FAT folds case). The
        // file goes aside first: if the name is then free it was its own, else what has it goes.
        auto *info = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
        if (info == nullptr) return Error::NoMem;
        result = f_stat(fromFat.cStr(), info);
        const bool fromDir = result == FR_OK && (info->fattrib & AM_DIR) != 0;
        // Aside: a free name in its directory (one left by a cut-off rename is passed over).
        String aside;
        for (char k = '0'; result == FR_OK && k <= '9'; ++k) {
                aside.clear();
                const size_t slash = fromFat.view().lastIndexOf('/');
                err = aside.append(fromFat.view().left(slash + 1));
                if (err.isOk()) err = aside.append(".bmds-rename-");
                if (err.isOk()) err = aside.append(k);
                if (err.isError()) result = FR_NOT_ENOUGH_CORE;
                if (result == FR_OK) result = f_rename(fromFat.cStr(), aside.cStr());
                if (result != FR_EXIST) break;
                result = k == '9' ? FR_DENIED : FR_OK; // all ten taken
        }
        if (result != FR_OK) {
                Memory::free(info);
                return Fat::toError(result, *this);
        }
        bool mismatch = false; // a file and a directory
        result = f_stat(toFat.cStr(), info);
        if (result == FR_OK) {
                mismatch = fromDir != ((info->fattrib & AM_DIR) != 0);
                if (!mismatch) result = f_unlink(toFat.cStr());
        } else if (result == FR_NO_FILE) {
                result = FR_OK; // the file's own name
        }
        Memory::free(info);
        if (result == FR_OK && !mismatch) result = f_rename(aside.cStr(), toFat.cStr());
        if (result == FR_OK && !mismatch) return Error::Ok;
        // Put back as it was.
        (void)f_rename(aside.cStr(), fromFat.cStr());
        if (mismatch) return fromDir ? Error::NotDir : Error::IsDir;
        // FatFs denies removing a directory that isn't empty.
        if (result == FR_DENIED && fromDir) return Error::NotEmpty;
        return Fat::toError(result, *this);
}

Error FatVolume::setModified(StringView path, Timestamp time) {
        String fat;
        Error  err = fatPath(path, fat);
        if (err.isError()) return err;
        auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
        if (entry == nullptr) return Error::NoMem;
        std::memset(entry, 0, sizeof(FILINFO));
        const DWORD packed = Fat::toFatTime(time);
        entry->fdate = static_cast<WORD>(packed >> 16);
        entry->ftime = static_cast<WORD>(packed & 0xffff);
        changed();
        err = Fat::toError(f_utime(fat.cStr(), entry), *this);
        Memory::free(entry);
        return err;
}

Error FatVolume::writeLink(StringView path, ByteSpan contents, bool &noPath) {
        noPath = false;
        String fat;
        Error  err = fatPath(path, fat);
        if (err.isError()) return err;
        auto *file = static_cast<FIL *>(Memory::alloc(MemSpace::Psram, sizeof(FIL)));
        if (file == nullptr) return Error::NoMem;
        std::memset(file, 0, sizeof(FIL));
        changed();
        FRESULT result = f_open(file, fat.cStr(), FA_WRITE | FA_CREATE_NEW);
        if (result == FR_OK) {
                UINT put = 0;
                result = f_write(file, contents.data(), static_cast<UINT>(contents.size()), &put);
                if (result == FR_OK && put < contents.size()) result = FR_DENIED;
                const FRESULT closed = f_close(file);
                if (result == FR_OK) result = closed;
                if (result == FR_OK) result = f_chmod(fat.cStr(), AM_SYS, AM_SYS);
                if (result != FR_OK) (void)f_unlink(fat.cStr());
        }
        Memory::free(file);
        noPath = result == FR_NO_PATH;
        return result == FR_INVALID_NAME ? Error(Error::Invalid) : Fat::toError(result, *this);
}

Result<Volume::Cursor *> FatVolume::openDirectory(StringView path, bool &noPath) {
        noPath = false;
        String      fat;
        const Error err = fatPath(path, fat);
        if (err.isError()) return makeError<Cursor *>(err);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(FatCursor));
        if (mem == nullptr) return makeError<Cursor *>(Error::NoMem);
        auto         *cursor = ::new (mem) FatCursor(*this);
        const FRESULT result = f_opendir(&cursor->dir(), fat.cStr());
        if (result != FR_OK) {
                cursor->~FatCursor();
                Memory::free(mem);
                noPath = result == FR_NO_PATH;
                return makeError<Cursor *>(Fat::toError(result, *this));
        }
        return makeResult<Cursor *>(cursor);
}

void FatVolume::closeDirectory(Cursor *cursor) {
        if (cursor == nullptr) return;
        auto *c = static_cast<FatCursor *>(cursor);
        // A stale directory (unmounted) just fails here.
        (void)f_closedir(&c->dir());
        c->~FatCursor();
        Memory::free(c);
        return;
}

BMDS_NAMESPACE_END
