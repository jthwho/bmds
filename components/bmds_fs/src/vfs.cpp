/**
 * @file      vfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <utility>
#include <bmds/memory.h>
#include <bmds/memorystream.h>
#include <bmds/partitions.h>

#include "fatlayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Vfs::MaxMountPath;
        using Vfs::MaxMounts;

        struct Mount {
                        char         path[MaxMountPath + 1] = {};
                        FatVolume   *volume = nullptr;
                        BlockDevice *device = nullptr;
        };

        // The mount table, in mount order; under the storage lock.
        constinit Mount  mounts[MaxMounts];
        constinit size_t mountCount = 0;

        constinit NullStream devNull;

        // The fixed device nodes in /dev, listed before the block devices.
        constexpr const char *DevNames[] = {"console", "null"};

        // What a normalized path names.
        enum class Kind : uint8_t {
                Virtual,
                DevDir,
                DevNull,
                DevConsole,
                DevBlock,
                Fat
        };

        struct Node {
                        Kind         kind = Kind::Virtual;
                        FatVolume   *volume = nullptr;
                        BlockDevice *device = nullptr;
                        StringView   name; // The last component ("" for /).
                        StringView   rest; // Fat: within the volume, "" for its root, else "/dir/file".
        };

        // True if @p path is @p dir itself or below it.
        bool isAtOrBelow(StringView path, StringView dir) {
                if (dir == "/") return true;
                return path.startsWith(dir) && (path.size() == dir.size() || path[dir.size()] == '/');
        }

        // True if @p path is strictly below @p dir.
        bool isBelow(StringView path, StringView dir) {
                return path.size() > dir.size() && isAtOrBelow(path, dir);
        }

        // Resolves a normalized path. Lock held.
        Result<Node> resolveLocked(StringView path) {
                Node node;
                if (path == "/") return makeResult(node);
                node.name = path.mid(path.lastIndexOf('/') + 1);
                if (isAtOrBelow(path, "/dev")) {
                        if (path == "/dev") {
                                node.kind = Kind::DevDir;
                        } else if (path.lastIndexOf('/') != 4) {
                                return makeError<Node>(Error::NotExist);
                        } else if (node.name == "null") {
                                node.kind = Kind::DevNull;
                        } else if (node.name == "console") {
                                node.kind = Kind::DevConsole;
                        } else {
                                const BlockDevices::Entry *e = Fat::findEntryLocked(node.name);
                                if (e == nullptr) return makeError<Node>(Error::NotExist);
                                node.kind = Kind::DevBlock;
                                node.device = e->device;
                        }
                        return makeResult(node);
                }
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (isAtOrBelow(path, mp)) {
                                node.kind = Kind::Fat;
                                node.volume = mounts[i].volume;
                                node.device = mounts[i].device;
                                node.rest = path.mid(mp.size());
                                return makeResult(node);
                        }
                }
                // A directory leading to a mount is virtual.
                for (size_t i = 0; i < mountCount; ++i) {
                        if (isBelow(StringView(mounts[i].path), path)) return makeResult(node);
                }
                return makeError<Node>(Error::NotExist);
        }

        // Normalizes @p path into @p storage and resolves it. Lock held.
        Result<Node> lookupLocked(StringView path, const Vfs::Context &context, String &storage) {
                Result<String> normalized = Vfs::normalize(path, context.cwd);
                if (normalized.error().isError()) return makeError<Node>(normalized.error());
                storage = std::move(normalized).value();
                return resolveLocked(storage.view());
        }

        // True if normalized @p path, which doesn't exist, is in a virtual
        // directory (outside every filesystem), where nothing can be made.
        bool outsideFilesystemsLocked(StringView path) {
                if (path.isEmpty()) return false;
                const size_t       slash = path.lastIndexOf('/');
                const Result<Node> parent = resolveLocked(slash == 0 ? StringView("/") : path.left(slash));
                return parent.error().isOk() && parent.value().kind != Kind::Fat;
        }

        // The FatFs path of a node inside a volume: "N:/dir/file".
        Error fatPath(const Node &node, String &out) {
                char root[3];
                node.volume->root(root);
                out.clear();
                Error err = out.reserve(sizeof(root) + node.rest.size() + 1);
                if (err.isOk()) err = out.append(StringView(root));
                if (err.isOk()) err = out.append(node.rest.isEmpty() ? StringView("/") : node.rest);
                return err;
        }

        FileInfo directoryInfo(StringView name) {
                FileInfo info;
                (void)info.name.assign(name);
                info.type = FileType::Directory;
                return info;
        }

        Result<FileInfo> statLocked(const Node &node) {
                FileInfo info;
                switch (node.kind) {
                        case Kind::Virtual:
                        case Kind::DevDir: return makeResult(directoryInfo(node.name));
                        case Kind::DevNull:
                        case Kind::DevConsole:
                        case Kind::DevBlock:
                                (void)info.name.assign(node.name);
                                info.type = FileType::Device;
                                if (node.device != nullptr)
                                        info.size = node.device->sectorCount() * node.device->sectorSize();
                                return makeResult(std::move(info));
                        default: break;
                }
                if (node.rest.isEmpty()) return makeResult(directoryInfo(node.name));
                String fat;
                Error  err = fatPath(node, fat);
                if (err.isError()) return makeError<FileInfo>(err);
                auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
                if (entry == nullptr) return makeError<FileInfo>(Error::NoMem);
                err = Fat::toError(f_stat(fat.cStr(), entry), *node.volume);
                if (err.isOk()) err = Fat::toFileInfo(*entry, info);
                Memory::free(entry);
                if (err.isError()) return makeError<FileInfo>(err);
                return makeResult(std::move(info));
        }

        // The next component of mount path @p mp below virtual directory @p dir.
        StringView childOf(StringView mp, StringView dir) {
                const StringView tail = mp.mid(dir == "/" ? 1 : dir.size() + 1);
                return tail.left(tail.indexOf('/'));
        }

        // The child names of virtual directory @p dir, in order without
        // repeats: "dev" in /, then the next component of each mount below.
        // Returns child @p index, or an empty view past the end. Lock held.
        StringView virtualChild(StringView dir, size_t index) {
                size_t emitted = 0;
                if (dir == "/") {
                        if (index == 0) return StringView("dev");
                        emitted = 1;
                }
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (!isBelow(mp, dir)) continue;
                        const StringView child = childOf(mp, dir);
                        bool             repeat = false;
                        for (size_t j = 0; j < i && !repeat; ++j) {
                                const StringView other(mounts[j].path);
                                repeat = isBelow(other, dir) && childOf(other, dir) == child;
                        }
                        if (repeat) continue;
                        if (emitted == index) return child;
                        ++emitted;
                }
                return StringView();
        }

        void removeMountLocked(size_t index) {
                mounts[index].volume->release();
                for (size_t j = index + 1; j < mountCount; ++j) mounts[j - 1] = mounts[j];
                mounts[--mountCount] = Mount();
                return;
        }

        // True if mount @p index is on @p device or on a partition of it. Lock held.
        bool isMountOn(size_t index, const BlockDevice &device) {
                const BlockDevice         *mounted = mounts[index].device;
                const BlockDevices::Entry *e = Fat::findEntryLocked(*mounted);
                return mounted == &device || (e != nullptr && e->parent == &device);
        }

        // True if mount @p index shares sectors with @p device: the same
        // device, a partition of it, or the whole device it is part of. Lock held.
        bool overlaps(size_t index, const BlockDevice &device) {
                const BlockDevices::Entry *e = Fat::findEntryLocked(device);
                return isMountOn(index, device) || (e != nullptr && e->parent == mounts[index].device);
        }

} // namespace

// --- Directory -------------------------------------------------------------

// Holds the anonymous-namespace Kind as a plain value (a public class's
// member can't have an internal-linkage type).
struct Directory::State {
                uint8_t    kind = 0;
                uint8_t    index = 0; // Virtual directories: the next entry.
                FatVolume *volume = nullptr;
                char       path[MaxMountPath + 1] = {}; // Virtual directories.
                DIR        dir = {};
                FILINFO    entry = {};
};

class VfsAccess {
        public:
                static Directory make(Directory::State *state) {
                        Directory d;
                        d._state = state;
                        return d;
                }
};

Directory::~Directory() {
        close();
}

Directory::Directory(Directory &&other) : _state(std::exchange(other._state, nullptr)) {}

Directory &Directory::operator=(Directory &&other) {
        if (this != &other) {
                close();
                _state = std::exchange(other._state, nullptr);
        }
        return *this;
}

void Directory::close() {
        if (_state == nullptr) return;
        if (static_cast<Kind>(_state->kind) == Kind::Fat) {
                Mutex::Locker lock(Fat::lock(), WaitForever);
                // A stale directory (unmounted) just fails here.
                (void)f_closedir(&_state->dir);
        }
        _state->~State();
        Memory::free(_state);
        _state = nullptr;
        return;
}

Result<FileInfo> Directory::next() {
        if (_state == nullptr) return makeError<FileInfo>(Error::InvalidState);
        State        &s = *_state;
        FileInfo      info;
        Mutex::Locker lock(Fat::lock(), WaitForever);
        switch (static_cast<Kind>(s.kind)) {
                case Kind::Virtual: {
                        const StringView child = virtualChild(StringView(s.path), s.index);
                        if (child.isEmpty()) return makeError<FileInfo>(Error::EndOfFile);
                        ++s.index;
                        return makeResult(directoryInfo(child));
                }
                case Kind::DevDir: {
                        constexpr size_t Fixed = sizeof(DevNames) / sizeof(DevNames[0]);
                        if (s.index < Fixed) {
                                (void)info.name.assign(DevNames[s.index]);
                        } else if (s.index - Fixed < Fat::deviceCountLocked()) {
                                const BlockDevices::Entry &e = Fat::deviceAtLocked(s.index - Fixed);
                                (void)info.name.assign(e.name);
                                info.size = e.device->sectorCount() * e.device->sectorSize();
                        } else {
                                return makeError<FileInfo>(Error::EndOfFile);
                        }
                        ++s.index;
                        info.type = FileType::Device;
                        return makeResult(std::move(info));
                }
                default: break;
        }
        const FRESULT result = f_readdir(&s.dir, &s.entry);
        if (result != FR_OK) return makeError<FileInfo>(Fat::toError(result, *s.volume));
        if (s.entry.fname[0] == '\0') return makeError<FileInfo>(Error::EndOfFile);
        const Error err = Fat::toFileInfo(s.entry, info);
        if (err.isError()) return makeError<FileInfo>(err);
        return makeResult(std::move(info));
}

// --- Vfs: paths and mounts --------------------------------------------------

Error Vfs::init() {
        return Fat::init();
}

Result<String> Vfs::normalize(StringView path, StringView cwd) {
        if (path.isEmpty()) return makeError<String>(Error::Invalid);
        String out;
        Error  err = out.reserve(path.size() + (path[0] == '/' ? 0 : cwd.size()) + 1);
        // Walks @p text's components into `out`.
        auto walk = [&](StringView text) {
                size_t i = 0;
                while (err.isOk() && i < text.size()) {
                        size_t end = text.indexOf('/', i);
                        if (end == StringView::NPos) end = text.size();
                        const StringView part = text.mid(i, end - i);
                        i = end + 1;
                        if (part.isEmpty() || part == ".") continue;
                        if (part == "..") {
                                const size_t slash = out.view().lastIndexOf('/');
                                out.truncate(slash == StringView::NPos ? 0 : slash);
                                continue;
                        }
                        err = out.append('/');
                        if (err.isOk()) err = out.append(part);
                }
        };
        if (path[0] != '/') walk(cwd);
        walk(path);
        if (err.isOk() && out.isEmpty()) err = out.append('/');
        if (err.isError()) return makeError<String>(err);
        if (out.size() > MaxPath) return makeError<String>(Error::OutOfRange);
        return makeResult(std::move(out));
}

Error Vfs::mount(StringView path, BlockDevice &device) {
        const Result<String> normalized = normalize(path, "/");
        if (normalized.error().isError()) return normalized.error();
        const StringView p = normalized.value().view();
        if (p == "/" || isAtOrBelow(p, "/dev") || p.size() > MaxMountPath) return Error::Invalid;
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                const StringView mp(mounts[i].path);
                if (p == mp) return Error::Exists;
                // Not inside a mounted filesystem, nor over the path to one.
                if (isBelow(p, mp) || isBelow(mp, p)) return Error::Invalid;
                // One filesystem in two volumes would corrupt it.
                if (overlaps(i, device)) return Error::Busy;
        }
        if (mountCount == MaxMounts) return Error::Full;
        FatVolume *volume = FatVolume::acquire();
        if (volume == nullptr) return Error::Full;
        const Error err = volume->mount(device);
        if (err.isError()) {
                volume->release();
                return err;
        }
        Mount &m = mounts[mountCount++];
        std::memcpy(m.path, p.data(), p.size());
        m.path[p.size()] = '\0';
        m.volume = volume;
        m.device = &device;
        return Error();
}

Error Vfs::unmount(StringView path) {
        const Result<String> normalized = normalize(path, "/");
        if (normalized.error().isError()) return normalized.error();
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (normalized.value().view() == StringView(mounts[i].path)) {
                        removeMountLocked(i);
                        return Error();
                }
        }
        return Error::NotExist;
}

size_t Vfs::unmountDevice(BlockDevice &device) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        size_t        removed = 0;
        for (size_t i = 0; i < mountCount;) {
                if (isMountOn(i, device)) {
                        removeMountLocked(i);
                        ++removed;
                } else {
                        ++i;
                }
        }
        return removed;
}

size_t Vfs::listMounts(Span<MountInfo> out) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        const size_t  n = mountCount < out.size() ? mountCount : out.size();
        for (size_t i = 0; i < n; ++i) {
                MountInfo &info = out[i];
                info = MountInfo();
                std::memcpy(info.path, mounts[i].path, sizeof(info.path));
                const BlockDevices::Entry *e = Fat::findEntryLocked(*mounts[i].device);
                if (e != nullptr) std::memcpy(info.device, e->name, sizeof(info.device));
                info.type = mounts[i].volume->typeName();
        }
        return n;
}

BlockDevice *Vfs::findDevice(StringView path, const Context &context) {
        Mutex::Locker      lock(Fat::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError() || found.value().kind != Kind::DevBlock) return nullptr;
        return found.value().device;
}

Error Vfs::format(BlockDevice &device, FsFormat format) {
        bool hadPartitions = false;
        bool whole = true;
        {
                Mutex::Locker lock(Fat::lock(), WaitForever);
                for (size_t i = 0; i < mountCount; ++i) {
                        if (overlaps(i, device)) return Error::Busy;
                }
                const BlockDevices::Entry *e = Fat::findEntryLocked(device);
                whole = e == nullptr || e->parent == nullptr;
                for (size_t i = 0; i < Fat::deviceCountLocked(); ++i)
                        hadPartitions = hadPartitions || Fat::deviceAtLocked(i).parent == &device;
                const Error err = FatVolume::format(device, format, whole);
                if (err.isError()) return err;
        }
        // A new partition table: register the new partitions instead of the old.
        if (whole && hadPartitions) {
                Partitions::detach(device);
                const Result<size_t> attached = Partitions::attach(device);
                if (attached.error().isError()) return attached.error();
        }
        return Error();
}

// --- Vfs: files and directories ---------------------------------------------

Result<FileHandle::Ptr> Vfs::open(StringView path, uint8_t flags, const Context &context) {
        if ((flags & (Read | Write)) == 0 || ((flags & (Truncate | Append)) != 0 && (flags & Write) == 0))
                return makeError<FileHandle::Ptr>(Error::Invalid);
        Mutex::Locker      lock(Fat::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error() == Error::NotExist && (flags & Create) != 0 && outsideFilesystemsLocked(storage))
                return makeError<FileHandle::Ptr>(Error::PermissionDenied);
        if (found.error().isError()) return makeError<FileHandle::Ptr>(found.error());
        const Node   &node = found.value();
        const uint8_t mode = ((flags & Read) ? FileHandle::Read : 0) | ((flags & Write) ? FileHandle::Write : 0);
        switch (node.kind) {
                case Kind::Virtual:
                case Kind::DevDir: return makeError<FileHandle::Ptr>(Error::IsDir);
                case Kind::DevNull: return FileHandle::open(devNull, mode);
                case Kind::DevConsole:
                        if (context.terminal == nullptr) return makeError<FileHandle::Ptr>(Error::NotExist);
                        return FileHandle::open(*context.terminal, mode);
                case Kind::DevBlock: return makeError<FileHandle::Ptr>(Error::NotSupported); // Raw access: later.
                default: break;
        }
        if (node.rest.isEmpty()) return makeError<FileHandle::Ptr>(Error::IsDir);
        String      fat;
        const Error err = fatPath(node, fat);
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        return Fat::openFile(*node.volume, fat.cStr(), flags);
}

Result<FileInfo> Vfs::stat(StringView path, const Context &context) {
        Mutex::Locker      lock(Fat::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError()) return makeError<FileInfo>(found.error());
        return statLocked(found.value());
}

Result<Directory> Vfs::openDirectory(StringView path, const Context &context) {
        Mutex::Locker      lock(Fat::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError()) return makeError<Directory>(found.error());
        const Node &node = found.value();
        if (node.kind == Kind::DevNull || node.kind == Kind::DevConsole || node.kind == Kind::DevBlock)
                return makeError<Directory>(Error::NotDir);
        String fat;
        if (node.kind == Kind::Fat) {
                const Error err = fatPath(node, fat);
                if (err.isError()) return makeError<Directory>(err);
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Directory::State));
        if (mem == nullptr) return makeError<Directory>(Error::NoMem);
        auto *state = ::new (mem) Directory::State();
        state->kind = static_cast<uint8_t>(node.kind);
        state->volume = node.volume;
        if (node.kind == Kind::Virtual) {
                const size_t n = storage.size() < MaxMountPath ? storage.size() : MaxMountPath;
                std::memcpy(state->path, storage.data(), n);
                state->path[n] = '\0';
        }
        if (node.kind == Kind::Fat) {
                const FRESULT result = f_opendir(&state->dir, fat.cStr());
                if (result != FR_OK) {
                        Error err = Fat::toError(result, *node.volume);
                        // FatFs says "no path" for a file.
                        if (err == Error::NotExist) {
                                const Result<FileInfo> info = statLocked(node);
                                if (info.error().isOk() && !info.value().isDirectory()) err = Error::NotDir;
                        }
                        state->~State();
                        Memory::free(mem);
                        return makeError<Directory>(err);
                }
        }
        return makeResult(VfsAccess::make(state));
}

namespace {

        // Looks up a path naming something inside a mounted filesystem (not a
        // mount point, not virtual), with its FatFs path; other nodes give
        // @p otherError. Lock held.
        Error fatTargetLocked(StringView path, const Vfs::Context &context, Error otherError, String &storage,
                              Node &node, String &fat) {
                const Result<Node> found = lookupLocked(path, context, storage);
                if (found.error().isError()) return found.error();
                node = found.value();
                if (node.kind != Kind::Fat || node.rest.isEmpty()) return otherError;
                return fatPath(node, fat);
        }

} // namespace

Error Vfs::mkdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        String        fat;
        Node          node;
        Error         err = fatTargetLocked(path, context, Error::Exists, storage, node, fat);
        if (err == Error::NotExist && outsideFilesystemsLocked(storage)) err = Error::PermissionDenied;
        if (err.isError()) return err;
        return Fat::toError(f_mkdir(fat.cStr()), *node.volume);
}

Error Vfs::rmdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        String        fat;
        Node          node;
        Error         err = fatTargetLocked(path, context, Error::Busy, storage, node, fat);
        if (err == Error::Busy && node.kind != Kind::Fat) err = Error::PermissionDenied; // virtual, or /dev
        if (err.isError()) return err;
        const Result<FileInfo> info = statLocked(node);
        if (info.error().isError()) return info.error();
        if (!info.value().isDirectory()) return Error::NotDir;
        err = Fat::toError(f_unlink(fat.cStr()), *node.volume);
        // FatFs denies removing a directory that isn't empty.
        return err == Error::PermissionDenied ? Error(Error::NotEmpty) : err;
}

Error Vfs::remove(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        String        fat;
        Node          node;
        Error         err = fatTargetLocked(path, context, Error::IsDir, storage, node, fat);
        const bool    deviceNode =
                node.kind == Kind::DevNull || node.kind == Kind::DevConsole || node.kind == Kind::DevBlock;
        if (err == Error::IsDir && deviceNode) err = Error::PermissionDenied;
        if (err.isError()) return err;
        const Result<FileInfo> info = statLocked(node);
        if (info.error().isError()) return info.error();
        if (info.value().isDirectory()) return Error::IsDir;
        return Fat::toError(f_unlink(fat.cStr()), *node.volume);
}

Error Vfs::rename(StringView from, StringView to, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        fromStorage;
        String        toStorage;
        String        fromFat;
        String        toFat;
        Node          fromNode;
        Node          toNode;
        Error         err = fatTargetLocked(from, context, Error::PermissionDenied, fromStorage, fromNode, fromFat);
        if (err.isOk()) {
                err = fatTargetLocked(to, context, Error::PermissionDenied, toStorage, toNode, toFat);
                // Outside every filesystem: not something a rename can do.
                if (err == Error::NotExist || err == Error::PermissionDenied) err = Error::NotSupported;
        }
        if (err.isError()) return err;
        if (fromNode.volume != toNode.volume) return Error::NotSupported;
        return Fat::toError(f_rename(fromFat.cStr(), toFat.cStr()), *fromNode.volume);
}

Error Vfs::setModified(StringView path, Timestamp time, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        String        fat;
        Node          node;
        Error         err = fatTargetLocked(path, context, Error::PermissionDenied, storage, node, fat);
        if (err.isError()) return err;
        auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
        if (entry == nullptr) return Error::NoMem;
        std::memset(entry, 0, sizeof(FILINFO));
        const DWORD packed = Fat::toFatTime(time);
        entry->fdate = static_cast<WORD>(packed >> 16);
        entry->ftime = static_cast<WORD>(packed & 0xffff);
        err = Fat::toError(f_utime(fat.cStr(), entry), *node.volume);
        Memory::free(entry);
        return err;
}

Result<FsInfo> Vfs::statfs(StringView path, const Context &context) {
        Mutex::Locker      lock(Fat::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError()) return makeError<FsInfo>(found.error());
        if (found.value().kind != Kind::Fat) return makeError<FsInfo>(Error::NotSupported);
        return found.value().volume->info();
}

BMDS_NAMESPACE_END
