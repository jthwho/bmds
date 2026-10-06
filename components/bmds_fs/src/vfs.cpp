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
#include <bmds/builtinprograms.h>
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
                        bool         pinned = false; // never unmounted (/boot)
        };

        // The mount table, in mount order; under the storage lock.
        constinit Mount  mounts[MaxMounts];
        constinit size_t mountCount = 0;
        constinit WaitListOf<4> mountChanges; // released after each mount and unmount

        constinit NullStream devNull;

        constinit Vfs::ChangeHook changeHook = nullptr; // under the storage lock

        // The fixed device nodes in /dev, listed before the block devices.
        constexpr const char *DevNames[] = {"console", "null"};

        // What a normalized path names.
        enum class Kind : uint8_t {
                Virtual,
                DevDir,
                DevNull,
                DevConsole,
                DevBlock,
                Fat,
                BinDir,     // /bin: the programs built in (devplan/programs.md §17)
                BinProgram, // /bin/NAME
        };

        // Where the programs built into the firmware are listed (read-only).
        constexpr StringView BinPath("/bin");

        struct Node {
                        Kind                kind = Kind::Virtual;
                        const ProgramEntry *program = nullptr; // BinProgram
                        FatVolume          *volume = nullptr;
                        BlockDevice *device = nullptr;
                        StringView   name; // The last component ("" for /).
                        StringView   rest; // Fat: within the volume, "" for its root, else "/dir/file".
                        uint32_t     mount = 0; // Fat: the mount slot + 1 (FileInfo::device).
        };

        // FileInfo::id: FNV-1a over the normalized path, ASCII letters folded
        // (FAT names are case-insensitive). The root hashes as "", so that a
        // directory's prefix and "/name" make its children's ids.
        constexpr uint64_t IdSeed = 14695981039346656037ull;

        uint64_t idAppend(uint64_t id, StringView text) {
                for (char c : text) {
                        uint8_t b = static_cast<uint8_t>(c);
                        if (b >= 'A' && b <= 'Z') b = static_cast<uint8_t>(b + ('a' - 'A'));
                        id = (id ^ b) * 1099511628211ull;
                }
                return id;
        }

        uint64_t pathId(StringView normalized) {
                return idAppend(IdSeed, normalized == "/" ? StringView() : normalized);
        }

        uint64_t objectId(const void *object) {
                return reinterpret_cast<uintptr_t>(object);
        }

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
                if (isAtOrBelow(path, BinPath)) {
                        if (path == BinPath) {
                                node.kind = Kind::BinDir;
                                return makeResult(node);
                        }
                        node.program =
                                path.lastIndexOf('/') == BinPath.size() ? BuiltinPrograms::find(node.name) : nullptr;
                        if (node.program == nullptr) return makeError<Node>(Error::NotExist);
                        node.kind = Kind::BinProgram;
                        return makeResult(node);
                }
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (isAtOrBelow(path, mp)) {
                                node.kind = Kind::Fat;
                                node.mount = static_cast<uint32_t>(i + 1);
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

        Result<FileInfo> statLocked(const Node &node, FRESULT *raw = nullptr) {
                FileInfo info;
                if (raw != nullptr) *raw = FR_OK;
                switch (node.kind) {
                        case Kind::Virtual:
                        case Kind::BinDir:
                        case Kind::DevDir: return makeResult(directoryInfo(node.name));
                        case Kind::BinProgram:
                                (void)info.name.assign(node.name);
                                info.type = FileType::File; // empty: run, not read
                                return makeResult(std::move(info));
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
                const FRESULT result = f_stat(fat.cStr(), entry);
                if (raw != nullptr) *raw = result;
                err = Fat::toError(result, *node.volume);
                if (err.isOk()) err = Fat::toFileInfo(*entry, info);
                Memory::free(entry);
                if (err.isError()) return makeError<FileInfo>(err);
                return makeResult(std::move(info));
        }

        // --- Symbolic links (devplan/storage.md §4) ------------------------------
        //
        // Cygwin's system-file links: the System attribute, "!<symlink>", then
        // FF FE and the target in UTF-16LE with a 16-bit NUL (or, read only,
        // the target in UTF-8). Looked for only where they can be: in a file
        // with the System attribute that a lookup found, or along a path that
        // FatFs says has a file where a directory should be (FR_NO_PATH).

        constexpr int    MaxLinks = 8;     // in a row, then TooManyLinks
        constexpr size_t MaxTarget = 1024; // bytes of UTF-8 (BMDS_PATH_MAX)
        constexpr char   Cookie[] = "!<symlink>";
        constexpr size_t CookieBytes = sizeof(Cookie) - 1;
        using Fat::MaxLinkFile;
        static_assert(MaxLinkFile == CookieBytes + 2 + 2 * MaxTarget + 2, "the link file's limit");

        bool linkCandidate(uint8_t attributes, FileType type, uint64_t size) {
                return (attributes & FileInfo::System) != 0 && type == FileType::File && size > CookieBytes &&
                       size <= MaxLinkFile;
        }

        // UTF-16LE (with surrogate pairs) to UTF-8.
        Error fromUtf16(const uint8_t *p, size_t units, String &out) {
                for (size_t i = 0; i < units; ++i) {
                        uint32_t c = static_cast<uint32_t>(p[2 * i] | p[2 * i + 1] << 8);
                        if (c == 0) break;
                        if (c >= 0xD800 && c < 0xDC00) {
                                if (i + 1 == units) return Error::Invalid;
                                const uint32_t low = static_cast<uint32_t>(p[2 * i + 2] | p[2 * i + 3] << 8);
                                if (low < 0xDC00 || low >= 0xE000) return Error::Invalid;
                                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                                ++i;
                        } else if (c >= 0xDC00 && c < 0xE000) {
                                return Error::Invalid;
                        }
                        char   b[4];
                        size_t n = 0;
                        if (c < 0x80) {
                                b[n++] = static_cast<char>(c);
                        } else if (c < 0x800) {
                                b[n++] = static_cast<char>(0xC0 | c >> 6);
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        } else if (c < 0x10000) {
                                b[n++] = static_cast<char>(0xE0 | c >> 12);
                                b[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        } else {
                                b[n++] = static_cast<char>(0xF0 | c >> 18);
                                b[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        }
                        const Error err = out.append(StringView(b, n));
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // UTF-8 to UTF-16LE into @p out; the number of bytes written, or 0 if invalid.
        size_t toUtf16(StringView text, uint8_t *out, size_t room) {
                size_t n = 0;
                for (size_t i = 0; i < text.size();) {
                        const uint8_t b = static_cast<uint8_t>(text[i]);
                        uint32_t      c = 0;
                        size_t        len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
                        if (len == 0 || i + len > text.size()) return 0;
                        c = len == 1 ? b : b & (0x7F >> len);
                        for (size_t k = 1; k < len; ++k) {
                                const uint8_t t = static_cast<uint8_t>(text[i + k]);
                                if ((t & 0xC0) != 0x80) return 0;
                                c = c << 6 | (t & 0x3F);
                        }
                        i += len;
                        if (c >= 0x10000) {
                                if (n + 4 > room) return 0;
                                const uint32_t v = c - 0x10000;
                                const uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
                                out[n++] = static_cast<uint8_t>(hi);
                                out[n++] = static_cast<uint8_t>(hi >> 8);
                                out[n++] = static_cast<uint8_t>(lo);
                                out[n++] = static_cast<uint8_t>(lo >> 8);
                        } else {
                                if (n + 2 > room || c == 0) return 0;
                                out[n++] = static_cast<uint8_t>(c);
                                out[n++] = static_cast<uint8_t>(c >> 8);
                        }
                }
                return n;
        }

        // The target of the link file at FatFs path @p fat; Invalid if it isn't one. Lock held.
        Error readLinkFile(const char *fat, String &target) {
                target.clear();
                auto *file = static_cast<FIL *>(Memory::alloc(MemSpace::Psram, sizeof(FIL) + MaxLinkFile));
                if (file == nullptr) return Error::NoMem;
                std::memset(file, 0, sizeof(FIL));
                uint8_t *buf = reinterpret_cast<uint8_t *>(file + 1);
                UINT     got = 0;
                FRESULT  result = f_open(file, fat, FA_READ);
                if (result == FR_OK) {
                        result = f_read(file, buf, MaxLinkFile, &got);
                        (void)f_close(file);
                }
                Error err = Fat::toError(result);
                if (err.isOk()) err = Fat::parseLink(buf, got, target);
                Memory::free(file);
                return err;
        }

        // Writes a new link file at FatFs path @p fat; the raw result. Lock held.
        FRESULT writeLinkFile(const char *fat, StringView target) {
                auto *file = static_cast<FIL *>(Memory::alloc(MemSpace::Psram, sizeof(FIL) + MaxLinkFile));
                if (file == nullptr) return FR_NOT_ENOUGH_CORE;
                std::memset(file, 0, sizeof(FIL));
                uint8_t *buf = reinterpret_cast<uint8_t *>(file + 1);
                std::memcpy(buf, Cookie, CookieBytes);
                buf[CookieBytes] = 0xFF;
                buf[CookieBytes + 1] = 0xFE;
                const size_t n = toUtf16(target, buf + CookieBytes + 2, 2 * MaxTarget);
                buf[CookieBytes + 2 + n] = 0;
                buf[CookieBytes + 3 + n] = 0;
                const UINT size = static_cast<UINT>(CookieBytes + 2 + n + 2);
                FRESULT    result = n == 0 ? FR_INVALID_NAME : f_open(file, fat, FA_WRITE | FA_CREATE_NEW);
                if (result == FR_OK) {
                        UINT put = 0;
                        result = f_write(file, buf, size, &put);
                        if (result == FR_OK && put < size) result = FR_DENIED;
                        const FRESULT closed = f_close(file);
                        if (result == FR_OK) result = closed;
                        if (result == FR_OK) result = f_chmod(fat, AM_SYS, AM_SYS);
                        if (result != FR_OK) (void)f_unlink(fat);
                }
                Memory::free(file);
                return result;
        }

        // The link @p link (a normalized path) points at, with @p rest after it.
        Error linkDestination(StringView link, StringView target, StringView rest, String &out) {
                String joined;
                Error  err = Error::Ok;
                if (!target.startsWith('/')) {
                        const size_t slash = link.lastIndexOf('/');
                        err = joined.append(slash == 0 ? StringView("/") : link.left(slash));
                        if (err.isOk()) err = joined.append('/');
                }
                if (err.isOk()) err = joined.append(target);
                if (err.isOk()) err = joined.append(rest);
                if (err.isError()) return err;
                Result<String> normalized = Vfs::normalize(joined, "/");
                if (normalized.error().isError()) return normalized.error();
                out = std::move(normalized).value();
                return Error::Ok;
        }

        // Finds the first link among the components of normalized @p storage
        // (the last too with @p last) and replaces @p storage with where it
        // leads. True if there was one. Lock held.
        bool followPathLocked(String &storage, bool last) {
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError() || found.value().kind != Kind::Fat) return false;
                const Node      &node = found.value();
                const StringView rest = node.rest;
                const size_t     mountLength = storage.size() - rest.size();
                char             root[3];
                node.volume->root(root);
                auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
                if (entry == nullptr) return false;
                bool   followed = false;
                String fat;
                for (size_t pos = 1; pos <= rest.size();) {
                        size_t     end = rest.indexOf('/', pos);
                        const bool isLast = end == StringView::NPos;
                        if (isLast) end = rest.size();
                        if (isLast && !last) break;
                        fat.clear();
                        if (fat.append(StringView(root)).isError() || fat.append(rest.left(end)).isError()) break;
                        if (f_stat(fat.cStr(), entry) != FR_OK) break;
                        if ((entry->fattrib & AM_DIR) == 0) {
                                String target;
                                if (linkCandidate(entry->fattrib, FileType::File, entry->fsize) &&
                                    readLinkFile(fat.cStr(), target).isOk()) {
                                        String next;
                                        const StringView link = storage.view().left(mountLength + end);
                                        if (linkDestination(link, target, rest.mid(end), next).isOk()) {
                                                storage = std::move(next);
                                                followed = true;
                                        }
                                }
                                break;
                        }
                        pos = end + 1;
                }
                Memory::free(entry);
                return followed;
        }

        // What normalized @p storage names, its links followed (the last one's
        // with @p followLast, else described as a link). @p storage ends as the
        // path of what was found; @p node resolves it. Lock held.
        Result<FileInfo> statFollowLocked(String &storage, bool followLast, Node &node) {
                for (int hops = 0;; ++hops) {
                        if (hops > MaxLinks) return makeError<FileInfo>(Error::TooManyLinks);
                        const Result<Node> found = resolveLocked(storage.view());
                        if (found.error().isError()) return makeError<FileInfo>(found.error());
                        node = found.value();
                        FRESULT          raw = FR_OK;
                        Result<FileInfo> info = statLocked(node, &raw);
                        if (info.error().isOk()) {
                                FileInfo &fi = info.value();
                                if (node.kind != Kind::Fat || !linkCandidate(fi.attributes, fi.type, fi.size)) return info;
                                String fat;
                                String target;
                                if (fatPath(node, fat).isError() || readLinkFile(fat.cStr(), target).isError()) return info;
                                if (!followLast) {
                                        fi.type = FileType::Link;
                                        fi.size = target.size();
                                        return info;
                                }
                                String next;
                                const Error err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileInfo>(err);
                                storage = std::move(next);
                                continue;
                        }
                        if (raw == FR_NO_PATH && followPathLocked(storage, false)) continue;
                        return info;
                }
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
                        if (index == 0) return StringView("bin");
                        if (index == 1) return StringView("dev");
                        emitted = 2;
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
                // Every file: the slots after this one moved.
                Fat::changedLocked(0, 0);
                mountChanges.notify();
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
                uint32_t   device = 0;
                uint64_t   idPrefix = IdSeed; // The directory's path hashed (children: + "/name").
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
                        info = directoryInfo(child);
                        info.id = idAppend(idAppend(s.idPrefix, "/"), child);
                        return makeResult(std::move(info));
                }
                case Kind::BinDir: {
                        const Span<const ProgramEntry> programs = BuiltinPrograms::all();
                        if (s.index >= programs.size()) return makeError<FileInfo>(Error::EndOfFile);
                        (void)info.name.assign(programs[s.index].name);
                        info.type = FileType::File;
                        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
                        ++s.index;
                        return makeResult(std::move(info));
                }
                case Kind::DevDir: {
                        constexpr size_t Fixed = sizeof(DevNames) / sizeof(DevNames[0]);
                        if (s.index < Fixed) {
                                (void)info.name.assign(DevNames[s.index]);
                                // The console's id is its stream's, unknown here.
                                if (info.name == "null") info.id = objectId(static_cast<Stream *>(&devNull));
                        } else if (s.index - Fixed < Fat::deviceCountLocked()) {
                                const BlockDevices::Entry &e = Fat::deviceAtLocked(s.index - Fixed);
                                (void)info.name.assign(e.name);
                                info.size = e.device->sectorCount() * e.device->sectorSize();
                                info.id = objectId(e.device);
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
        // A link by its attributes alone: reading every link to be sure made
        // listing a directory of BusyBox's applets take seconds (each read is
        // a lookup in that directory). System files Windows makes are also
        // Hidden; lstat() and readLink() read the cookie when it matters.
        if (linkCandidate(info.attributes, info.type, info.size) && (info.attributes & FileInfo::Hidden) == 0) {
                info.type = FileType::Link;
                info.size = (info.size - CookieBytes - 4) / 2; // the target's length, if ASCII
        }
        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
        info.device = s.device;
        return makeResult(std::move(info));
}

// --- Vfs: paths and mounts --------------------------------------------------

void Fat::changedLocked(uint32_t device, uint64_t id) {
        if (changeHook != nullptr) changeHook(device, id);
        return;
}

void Vfs::setChangeHook(ChangeHook hook) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        changeHook = hook;
        return;
}

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
        if (p == "/" || isAtOrBelow(p, "/dev") || isAtOrBelow(p, BinPath) || p.size() > MaxMountPath)
                return Error::Invalid;
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
        mountChanges.notify();
        return Error();
}

Error Vfs::pin(StringView path) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (path == StringView(mounts[i].path)) {
                        mounts[i].pinned = true;
                        return Error();
                }
        }
        return Error::NotExist;
}

bool Vfs::isMounted(StringView path) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i)
                if (path == StringView(mounts[i].path)) return true;
        return false;
}

WaitList &Vfs::mountWaiters() {
        return mountChanges;
}

Error Vfs::unmount(StringView path) {
        const Result<String> normalized = normalize(path, "/");
        if (normalized.error().isError()) return normalized.error();
        Mutex::Locker lock(Fat::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (normalized.value().view() == StringView(mounts[i].path)) {
                        if (mounts[i].pinned) return Error::Busy;
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
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        {
                Result<String> normalized = normalize(path, context.cwd);
                if (normalized.error().isError()) return makeError<FileHandle::Ptr>(normalized.error());
                storage = std::move(normalized).value();
        }
        const uint8_t mode = ((flags & Read) ? FileHandle::Read : 0) | ((flags & Write) ? FileHandle::Write : 0);
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return makeError<FileHandle::Ptr>(Error::TooManyLinks);
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error() == Error::NotExist && (flags & Create) != 0 && outsideFilesystemsLocked(storage))
                        return makeError<FileHandle::Ptr>(
                                isAtOrBelow(storage.view(), BinPath) ? Error::ReadOnly : Error::PermissionDenied);
                if (found.error().isError()) return makeError<FileHandle::Ptr>(found.error());
                const Node &node = found.value();
                switch (node.kind) {
                        case Kind::Virtual:
                        case Kind::BinDir:
                        case Kind::DevDir: return makeError<FileHandle::Ptr>(Error::IsDir);
                        case Kind::BinProgram: {
                                // Read-only, and empty: a built-in is run, not read.
                                if ((flags & (Write | Truncate | Append)) != 0)
                                        return makeError<FileHandle::Ptr>(Error::ReadOnly);
                                Result<FileHandle::Ptr> handle = FileHandle::open(devNull, mode);
                                if (handle.error().isOk()) handle.value()->setIdentity(pathId(storage.view()), 0);
                                return handle;
                        }
                        case Kind::DevNull: return FileHandle::open(devNull, mode);
                        case Kind::DevConsole:
                                if (context.terminal == nullptr) return makeError<FileHandle::Ptr>(Error::NotExist);
                                return FileHandle::open(*context.terminal, mode);
                        case Kind::DevBlock: return makeError<FileHandle::Ptr>(Error::NotSupported); // Raw access: later.
                        default: break;
                }
                if (node.rest.isEmpty()) return makeError<FileHandle::Ptr>(Error::IsDir);
                String fat;
                Error  err = fatPath(node, fat);
                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                // Truncating would cut a link file itself: look first.
                String target;
                if ((flags & Truncate) != 0) {
                        const Result<FileInfo> info = statLocked(node);
                        if (info.error().isOk() && linkCandidate(info.value().attributes, info.value().type, info.value().size) &&
                            readLinkFile(fat.cStr(), target).isOk()) {
                                String next;
                                err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                                storage = std::move(next);
                                continue;
                        }
                }
                FRESULT                 raw = FR_OK;
                Result<FileHandle::Ptr> handle = Fat::openFile(*node.volume, fat.cStr(), flags, node.mount,
                                                               pathId(storage.view()), &raw, &target);
                if (handle.error().isError()) {
                        // A link as the last component (openFile read it), or along the way.
                        if (!target.isEmpty()) {
                                String next;
                                err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                                storage = std::move(next);
                                continue;
                        }
                        if (raw == FR_NO_PATH && followPathLocked(storage, false)) continue;
                        return handle;
                }
                handle.value()->setIdentity(pathId(storage.view()), node.mount);
                return handle;
        }
}

namespace {

        // A FileInfo's identity: its path's, or a device node's stream.
        void setIdentity(FileInfo &fi, const Node &node, StringView storage, const Vfs::Context &context) {
                switch (node.kind) {
                        // A device node is its stream (or device), as its handles say.
                        case Kind::DevNull: fi.id = objectId(static_cast<Stream *>(&devNull)); break;
                        case Kind::DevConsole: fi.id = objectId(context.terminal); break;
                        case Kind::DevBlock: fi.id = objectId(node.device); break;
                        default:
                                fi.id = pathId(storage);
                                fi.device = node.mount;
                                break;
                }
                return;
        }

        Result<FileInfo> statAt(StringView path, const Vfs::Context &context, bool followLast) {
                Mutex::Locker  lock(Fat::lock(), WaitForever);
                Result<String> normalized = Vfs::normalize(path, context.cwd);
                if (normalized.error().isError()) return makeError<FileInfo>(normalized.error());
                String           storage = std::move(normalized).value();
                Node             node;
                Result<FileInfo> info = statFollowLocked(storage, followLast, node);
                if (info.error().isOk()) setIdentity(info.value(), node, storage.view(), context);
                return info;
        }

} // namespace

Result<FileInfo> Vfs::stat(StringView path, const Context &context) {
        return statAt(path, context, true);
}

Result<FileInfo> Vfs::lstat(StringView path, const Context &context) {
        return statAt(path, context, false);
}

Result<String> Vfs::readLink(StringView path, const Context &context) {
        Mutex::Locker  lock(Fat::lock(), WaitForever);
        Result<String> normalized = normalize(path, context.cwd);
        if (normalized.error().isError()) return normalized;
        String                 storage = std::move(normalized).value();
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, false, node);
        if (info.error().isError()) return makeError<String>(info.error());
        if (info.value().type != FileType::Link) return makeError<String>(Error::Invalid);
        String fat;
        String target;
        Error  err = fatPath(node, fat);
        if (err.isOk()) err = readLinkFile(fat.cStr(), target);
        if (err.isError()) return makeError<String>(err);
        return makeResult(std::move(target));
}

Error Vfs::symlink(StringView target, StringView path, const Context &context) {
        if (target.isEmpty() || target.size() > MaxTarget) return Error::Invalid;
        Mutex::Locker  lock(Fat::lock(), WaitForever);
        Result<String> normalized = normalize(path, context.cwd);
        if (normalized.error().isError()) return normalized.error();
        String storage = std::move(normalized).value();
        if (isAtOrBelow(storage.view(), BinPath)) return Error::ReadOnly; // the built-ins
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error() == Error::NotExist && outsideFilesystemsLocked(storage)) return Error::PermissionDenied;
                if (found.error().isOk() && (found.value().kind != Kind::Fat || found.value().rest.isEmpty()))
                        return Error::Exists;
                if (found.error().isError() && found.error() != Error::NotExist) return found.error();
                // resolveLocked() finds a Fat node whether or not the file exists.
                const Node &node = found.value();
                String      fat;
                Error       err = fatPath(node, fat);
                if (err.isError()) return err;
                const FRESULT raw = writeLinkFile(fat.cStr(), target);
                if (raw == FR_NO_PATH && followPathLocked(storage, false)) continue;
                if (raw == FR_INVALID_NAME) return Error::Invalid;
                return Fat::toError(raw, *node.volume);
        }
}

const ProgramEntry *Vfs::builtinProgram(StringView path, const Context &context) {
        Mutex::Locker  lock(Fat::lock(), WaitForever);
        Result<String> normalized = normalize(path, context.cwd);
        if (normalized.error().isError()) return nullptr;
        String                 storage = std::move(normalized).value();
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, true, node); // links followed
        if (info.error().isError() || node.kind != Kind::BinProgram) return nullptr;
        return node.program;
}

Result<String> Vfs::realPath(StringView path, const Context &context) {
        Mutex::Locker  lock(Fat::lock(), WaitForever);
        Result<String> normalized = normalize(path, context.cwd);
        if (normalized.error().isError()) return normalized;
        String                 storage = std::move(normalized).value();
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, true, node);
        if (info.error().isError()) return makeError<String>(info.error());
        return makeResult(std::move(storage));
}

Result<Directory> Vfs::openDirectory(StringView path, const Context &context) {
        Mutex::Locker  lock(Fat::lock(), WaitForever);
        Result<String> normalized = normalize(path, context.cwd);
        if (normalized.error().isError()) return makeError<Directory>(normalized.error());
        String storage = std::move(normalized).value();
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return makeError<Directory>(Error::TooManyLinks);
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError()) return makeError<Directory>(found.error());
                const Node &node = found.value();
                if (node.kind == Kind::DevNull || node.kind == Kind::DevConsole || node.kind == Kind::DevBlock ||
                    node.kind == Kind::BinProgram)
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
                state->device = node.mount;
                state->idPrefix = pathId(storage.view());
                if (node.kind == Kind::Virtual) {
                        const size_t n = storage.size() < MaxMountPath ? storage.size() : MaxMountPath;
                        std::memcpy(state->path, storage.data(), n);
                        state->path[n] = '\0';
                }
                if (node.kind == Kind::Fat) {
                        const FRESULT result = f_opendir(&state->dir, fat.cStr());
                        if (result != FR_OK) {
                                Error err = Fat::toError(result, *node.volume);
                                state->~State();
                                Memory::free(mem);
                                // A link along the way, or as the directory itself (FatFs: "no path").
                                if (result == FR_NO_PATH && followPathLocked(storage, true)) continue;
                                if (err == Error::NotExist) {
                                        const Result<FileInfo> info = statLocked(node);
                                        if (info.error().isOk() && !info.value().isDirectory()) err = Error::NotDir;
                                }
                                return makeError<Directory>(err);
                        }
                }
                return makeResult(VfsAccess::make(state));
        }
}

namespace {

        // Looks up normalized @p storage naming something inside a mounted
        // filesystem (not a mount point, not virtual), with its FatFs path;
        // other nodes give @p otherError. Lock held.
        Error fatTargetLocked(const String &storage, Error otherError, Node &node, String &fat) {
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError()) return found.error();
                node = found.value();
                if (node.kind != Kind::Fat || node.rest.isEmpty()) return otherError;
                return fatPath(node, fat);
        }

        Error normalizeInto(StringView path, const Vfs::Context &context, String &storage) {
                Result<String> normalized = Vfs::normalize(path, context.cwd);
                if (normalized.error().isError()) return normalized.error();
                storage = std::move(normalized).value();
                return Error::Ok;
        }

} // namespace

// The changes below act on the last component itself (a link is removed,
// renamed, not followed), following links in the directories along the way.

Error Vfs::mkdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isAtOrBelow(storage.view(), BinPath)) return Error::ReadOnly; // the built-ins
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                String fat;
                Node   node;
                err = fatTargetLocked(storage, Error::Exists, node, fat);
                if (err == Error::NotExist && outsideFilesystemsLocked(storage)) err = Error::PermissionDenied;
                if (err.isError()) return err;
                const FRESULT result = f_mkdir(fat.cStr());
                if (result == FR_NO_PATH && followPathLocked(storage, false)) continue;
                return Fat::toError(result, *node.volume);
        }
        return err;
}

Error Vfs::rmdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isAtOrBelow(storage.view(), BinPath)) return Error::ReadOnly; // the built-ins
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                String fat;
                Node   node;
                err = fatTargetLocked(storage, Error::Busy, node, fat);
                if (err == Error::Busy && node.kind != Kind::Fat) err = Error::PermissionDenied; // virtual, or /dev
                if (err.isError()) return err;
                FRESULT                raw = FR_OK;
                const Result<FileInfo> info = statLocked(node, &raw);
                if (raw == FR_NO_PATH && followPathLocked(storage, false)) continue;
                if (info.error().isError()) return info.error();
                if (!info.value().isDirectory()) return Error::NotDir;
                err = Fat::toError(f_unlink(fat.cStr()), *node.volume);
                // FatFs denies removing a directory that isn't empty.
                return err == Error::PermissionDenied ? Error(Error::NotEmpty) : err;
        }
        return err;
}

Error Vfs::remove(StringView path, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isAtOrBelow(storage.view(), BinPath)) return Error::ReadOnly; // the built-ins
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                String     fat;
                Node       node;
                err = fatTargetLocked(storage, Error::IsDir, node, fat);
                const bool deviceNode =
                        node.kind == Kind::DevNull || node.kind == Kind::DevConsole || node.kind == Kind::DevBlock;
                if (err == Error::IsDir && deviceNode) err = Error::PermissionDenied;
                if (err.isError()) return err;
                FRESULT                raw = FR_OK;
                const Result<FileInfo> info = statLocked(node, &raw);
                if (raw == FR_NO_PATH && followPathLocked(storage, false)) continue;
                if (info.error().isError()) return info.error();
                if (info.value().isDirectory()) return Error::IsDir;
                err = Fat::toError(f_unlink(fat.cStr()), *node.volume);
                if (err.isOk()) Fat::changedLocked(node.mount, pathId(storage.view()));
                return err;
        }
        return err;
}

Error Vfs::rename(StringView from, StringView to, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        fromStorage;
        String        toStorage;
        Error         err = normalizeInto(from, context, fromStorage);
        if (err.isOk()) err = normalizeInto(to, context, toStorage);
        if (err.isOk() && (isAtOrBelow(fromStorage.view(), BinPath) || isAtOrBelow(toStorage.view(), BinPath)))
                return Error::ReadOnly; // the built-ins
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                String fromFat;
                String toFat;
                Node   fromNode;
                Node   toNode;
                err = fatTargetLocked(fromStorage, Error::PermissionDenied, fromNode, fromFat);
                if (err.isOk()) {
                        err = fatTargetLocked(toStorage, Error::PermissionDenied, toNode, toFat);
                        // Outside every filesystem: not something a rename can do.
                        if (err == Error::NotExist || err == Error::PermissionDenied) err = Error::NotSupported;
                }
                if (err.isError()) return err;
                if (fromNode.volume != toNode.volume) return Error::NotSupported;
                const FRESULT result = f_rename(fromFat.cStr(), toFat.cStr());
                if (result == FR_NO_PATH && (followPathLocked(fromStorage, false) || followPathLocked(toStorage, false)))
                        continue;
                if (result == FR_OK) Fat::changedLocked(fromNode.mount, pathId(fromStorage.view()));
                return Fat::toError(result, *fromNode.volume);
        }
        return err;
}

Error Vfs::setModified(StringView path, Timestamp time, const Context &context) {
        Mutex::Locker lock(Fat::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isAtOrBelow(storage.view(), BinPath)) return Error::ReadOnly; // the built-ins
        if (err.isError()) return err;
        // The file a link leads to, as `touch` does.
        Node                   found;
        const Result<FileInfo> info = statFollowLocked(storage, true, found);
        if (info.error().isError() && info.error() != Error::NotExist) return info.error();
        String fat;
        Node   node;
        err = fatTargetLocked(storage, Error::PermissionDenied, node, fat);
        if (err.isError()) return err;
        auto *entry = static_cast<FILINFO *>(Memory::alloc(MemSpace::Psram, sizeof(FILINFO)));
        if (entry == nullptr) return Error::NoMem;
        std::memset(entry, 0, sizeof(FILINFO));
        const DWORD packed = Fat::toFatTime(time);
        entry->fdate = static_cast<WORD>(packed >> 16);
        entry->ftime = static_cast<WORD>(packed & 0xffff);
        err = Fat::toError(f_utime(fat.cStr(), entry), *node.volume);
        Memory::free(entry);
        if (err.isOk()) Fat::changedLocked(node.mount, pathId(storage.view()));
        return err;
}

Error Fat::parseLink(const uint8_t *data, size_t size, String &target) {
        target.clear();
        if (size <= CookieBytes || std::memcmp(data, Cookie, CookieBytes) != 0) return Error::Invalid;
        Error err;
        if (size >= CookieBytes + 2 && data[CookieBytes] == 0xFF && data[CookieBytes + 1] == 0xFE) {
                err = fromUtf16(data + CookieBytes + 2, (size - CookieBytes - 2) / 2, target);
        } else {
                // No byte order mark: UTF-8 (older Cygwin, links made by hand).
                size_t n = CookieBytes;
                while (n < size && data[n] != 0) ++n;
                err = target.append(StringView(reinterpret_cast<const char *>(data) + CookieBytes, n - CookieBytes));
        }
        if (err.isOk() && (target.isEmpty() || target.size() > MaxTarget)) err = Error::Invalid;
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
